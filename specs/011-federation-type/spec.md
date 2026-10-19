# Spec 011: the `hugr` type - a platform as a source of another

- **Status**: draft
- **Date**: 2026-10-19
- **Author**: Claude, at the owner's ask. The decisions are the owner's: design/019 (duckdb-acl's local `design/`,
  2026-10-01 and 2026-10-09), design/017 §3.3 and §7 items 13-14, and design/082's F1 (duckdb-acl spec 135). The open
  ones are at the end, with recommendations.

## Summary

Platform A takes platform B as a source. A node of A runs `ATTACH 'b.example:443' AS b (TYPE hugr, SECRET b_client)`.
`b` is then B's virtual catalog as B shows it to A's node role. A's admin maps its objects into A's virtual catalogs
(`ADD TABLE b.main.orders AS sales.orders`), and A's users read them with A's RLS and masks on top of B's.

- **To B, A is one ordinary principal.** A's node logs in with client credentials, and the secret is in A's tresor.
  No user of A reaches B as themselves.
- **The client is quack's**, compiled into hugr_node at the quack commit acl's embedded server uses. It has a type name
  of its own and hidden symbols.
- **Each attached catalog has a pool of connections.**
  - A connection is lent per remote read.
  - Connections retire when idle and after a maximum lifetime, and borrowers wait in a FIFO queue.
  - A connection that holds nothing pinned reconnects and retries once when B says "connect again" (duckdb-acl spec
    135). Reconnecting also refreshes the token.
- **Three stages.** Stage 1 is this spec's code: the type, the pool, the reconnect and read-only scans. Stage 2 is
  pushdown, and stage 3 is lineage.

## Problem

The owner's requirement (design/019 §0): a connection from A to B never falls off - not when a node of B dies or
drains, and not when a token expires.

A stock quack client can attach B's door today. duckdb-acl's door tests do that, and read a role's slice. It falls
short:

- **One connection per catalog, running one query at a time.** A new PREPARE supersedes the running query, and a
  second scan in one query is refused ("Multiple streaming scans … not currently supported").
- **No reconnect.** When a node goes away or the lease lapses, the attachment is dead until DETACH and ATTACH.
- **A token refreshed by hand**, with `CREATE OR REPLACE SECRET` (spec 061).
- **Pushdown always on.**
  - `QuackCatalog` claims `IS_REMOTE`, `EXECUTE_QUERY_NODE`, `EXECUTE_STATEMENT` and `CONNECT`, and says yes to every
    `SupportsPushdown` except `query()`.
  - So A's rewritten query goes to B whole, with A's RLS constants in it.
  - A function that B denies A's role fails the query there: `RemoteExecute` cannot decline (mssql-extension spec 079).
- **The type name `quack`** collides with acl's embedded server and with a stock quack loaded on the same node
  (design/019 §4).

The owner decided (design/017 §7 item 14; 019 §1) that the client and its type, `hugr`, are our own, in hugr_node
(BUSL). The pool, the reconnect and the routing belong to the fleet, and acl stays a single node.

## Design

### The ATTACH

- **The path** is B's quack door, `host[:port]` in quack's URI form. On a platform that is B's front.
  - Any node serving duckdb-acl's quack door can be B; it need not be a hugr_node.
  - TLS is always on. `disable_ssl` is accepted only for a loopback host (the tests).
- **`SECRET`** names an `oidc_client` secret in A's tresor (duckdb-acl spec 095: `CLIENT_ID`, `CLIENT_SECRET`;
  design/019 §2). The grant's IdP and scope are pending Q8.
- **The token.**
  - It is minted by client credentials at every connect, through duckdb-ext-common's OIDC core, compiled under
    hugr_node's namespace (R13).
  - It is reused until shortly before its `exp`.
  - B's default `acl_session_token_binding = 'connect'` (spec 059) judges `exp` only when a session opens. A live
    connection therefore outlives its token, and reconnecting is the refresh (design/019 §2).
- **Who attaches**: the operator only - the bootstrap, a hot profile item (`ACL CLUSTER ATTACH … (TYPE hugr,
  SECRET s)`), or `ACL NATIVE`. A principal never does.
- **Options**, per link. The defaults are proposed, except those marked 019.

| option | default | what |
| --- | --- | --- |
| `pool_size` | 4 | connections at most, per A node |
| `pool_wait` | 25 s | how long a loan may wait (spec 080's queue bound) |
| `idle_timeout` | 600 s (019 §5.3) | below B's `acl_session_idle_timeout` (900 s) |
| `max_lifetime` | 1800 s (019 §5.3) | a connection retires at its next return after this |
| `read_ahead` | 8 (019 §5.1) | FETCHes in flight per scan - pending Q2 |
| `catalog_ttl` | 300 s | when the catalog is read again |

**On B**, the setup is configuration only:

- a `CLIENT` for A's node (spec 095), with A's role reached through the client's own mapping;
- that role's grants;
- a resource group for the role (spec 085), on nodes of its own if wanted (spec 096);
- a served quack door.

### The client, from quack's client sources

- **The source** is `duckdb-acl/third_party/quack`, the commit acl's embedded server is built from (`e7a9be3` at the
  duckdb-acl pin `0c3415c`). Client and server speak one protocol version; quack's CONNECT refuses any other.
- **The copies.** `src/hugr_type/sync.py` makes them the way duckdb-acl's `src/quack_embed/sync.py` does:
  - duckdb's quack patches first;
  - unique basenames, `hugr_client_*.cpp`;
  - exact literals renamed: the catalog type, setting names and function names;
  - duckdb/duckdb-quack#297 carried as a patch (the owner, 2026-10-18) until quack's pin has it. It sends a cookie and
    `X-Quack-Connection-Id` on every request, so the front routes by header.

| quack `src/` | taken |
| --- | --- |
| `quack_client.cpp` / `.hpp` | `HttpsQuackClient` (POST `/quack` through duckdb's `HTTPUtil`); `QuackClientConnection` (the id, the heartbeat lease, cached HTTP clients, `CancelQuery`); `QuackClientWrapper` |
| `quack_scan.cpp`, `quack_fetch_ahead.cpp` | the table scan: `QuackScanInitGlobal` (PREPARE `SELECT #i, … FROM <path>`, first rows inline); `QuackFetcher` (read-ahead on the ASYNC pool, `StopAndDrain`). It takes its connection from the pool. |
| `storage/quack_catalog.cpp`, `quack_schema.cpp`, `quack_table.cpp`, `quack_catalog_set.cpp` | `QuackCatalog` (loaded whole at ATTACH; `Refresh`), `QuackSchemaSet`, `QuackTableSet` (each table's `CREATE TABLE` text, bound on A) |
| `storage/quack_transaction*.cpp` | the transaction and its manager; stage 1 begins none on B |
| `quack_storage.cpp` | the attach and its options, kept by hand as `HugrStorageExtension` |

- **Not taken:**
  - `PinnedHttpsQuackClient`: the trust anchor is B's URL and TLS, as for all of the node's https (the owner,
    2026-10-14);
  - the secret type, `SEND_DATA` and inserts, `quack_query`, the cancel function, the entry point and the server;
  - `storage/quack_optimizer.cpp`: its "second scan" refusal goes away (pending Q1).
- **The protocol TUs are acl's.** acl's embed already compiles `quack_message.cpp`, `serialize_quack_message.cpp`,
  `quack_uri.cpp`, `quack_log.cpp` and `quack_random.cpp` into hugr_node, and a second copy would duplicate their
  definitions. So the type needs the embed, which is on by default.
- **The embed's compile options**: the httplib shim and `-fvisibility=hidden -fvisibility-inlines-hidden`. Without them,
  a stock quack loaded beside hugr_node on Linux binds its calls to our copies (duckdb-acl spec 063).
- **HTTP goes through duckdb's client.** httpfs is linked in (spec 006), so `ca_cert_file` and spec 010's server
  verification apply.

### No state between loans

A pooled connection carries only what the type composes: the catalog's listing queries and `SELECT <columns> FROM
<path>`.

- **`Supports()` claims nothing.** duckdb then never forwards a statement verbatim (`CONNECT`), and in stage 1 never
  pushes one (`EXECUTE_*`).
- **The catalog refuses writes**: schema DDL, CTAS, INSERT, UPDATE, DELETE and indexes fail with
  `hugr: the catalog b is read-only`. Writes are pending Q4.
- **No function takes SQL text.** quack binds a remote view as `FROM quack_query_by_name(<catalog>, 'FROM <view>')`.
  The type instead describes a view from B's `duckdb_columns()` at load, and scans it like a table.

No SET, USE, temp table or transaction reaches B through `hugr`, though B would admit some on a session of the
client's own (specs 068, 114, 050). Spec 135 left the replay of the client's context to this spec. The type keeps
none, so a reconnect has nothing to replay.

### The pool

**The unit** is one quack connection: one acl session on B, one connection id, one query at a time.

- All connections are A's one role, so any of them serves any user of A.
- Each A node keeps one pool per attached catalog. DETACH ends it.
- A retired connection sends DISCONNECT, which frees its seats on B at once (spec 079).

**Capacity.**

- On B, a connection takes:
  - a place in `acl_max_sessions`;
  - a place in its group's `max_sessions` (spec 085);
  - `acl_quack_client_depth` door seats (spec 079). That is 64 by default, so a default node seats 16 quack clients in
    all - pending Q7.
- `pool_size` bounds the pool. B bounds it further: `at_capacity`, `max_sessions`, `draining` or
  `wrong_resource_group` at connect means "no new connection now".
  - The pool does not grow.
  - The loan waits for a connection in use (design/019 §5.1: wait, do not storm).
  - An empty pool retries with backoff.
- design/019 §5.1 also has the pool keep under the `max_sessions` that B declares, which B's load report shows
  (`sessions.by_group`). That report needs an `observe` bearer (duckdb-acl spec 097), so reading it is pending Q3.
  Until then the pool learns B's limit from B's refusals.
- A connection keeps at most `read_ahead` FETCHes in flight, and caches that many HTTP clients (quack caches one per
  async thread).

**A loan** serves one remote read (one scan), from its PREPARE to its end.

- Two scans of B in one A query run on two loans in parallel, and see B at two moments (pending Q1).
- On an early stop (a LIMIT, a cancel, an error on A), the type stops the read-ahead and sends CANCEL. The connection
  returns only when the remote query is over (design/019 §5.4). Otherwise the next borrower's PREPARE would supersede a
  query whose FETCH answers are still arriving.
- A transaction on B keeps its loan until A's commit or rollback. Stage 1 opens none.

**Lifecycle** (design/019 §5.3).

- **Idle.** A connection idle for `idle_timeout` retires. B ends a session that runs no statement for 900 s, even
  while quack's heartbeat keeps the lease alive.
- **Lifetime.** A connection retires at its first return after `max_lifetime`. Its replacement goes through B's front,
  which places each new connection on a less loaded node. So the pool spreads over B, and a drained B node loses A's
  connections within one lifetime, without a kill.
- **Health.** quack's heartbeat renews the lease about every 20 s. Its last result is the cheap check before a loan
  (design/019 §5.3): a connection whose heartbeat failed is replaced silently before it is lent. One that fails anyway
  is caught by the PREPARE's retry (below).

**Failures and the reconnect (F1).**

| B's answer | the pool |
| --- | --- |
| `acl: this connection is not known on this node - connect again` (spec 135; also the answer behind a front that re-routed because the label's node is gone) | reconnect and retry once. Also drop every connection whose id starts with the same `<label>.` (spec 134; 019 §5.4): idle ones at once, busy ones when they return. Anything more about B's nodes: pending Q3. |
| `acl: this connection's lease ran out on this node - connect again` | reconnect and retry once |
| no answer: refused, reset, timed out | reconnect and retry once |
| `Authorization failed` (the session has ended on B) | pending Q9 |
| at connect: `acl: node at capacity …`, `max_sessions`, `draining`, `wrong_resource_group` | no new connection: wait (Capacity) |
| B's database invalidated (`must_invalidate`) | drop the connection |
| the statement's own error (binder, access, function) | the statement fails and the connection returns. A binder error naming an object or column also refreshes the catalog. |

The retry rules:

- **Transparent only before any row, and only unpinned.** No row may have reached A's pipeline (it is the PREPARE), and
  the connection may hold no open transaction on B. The retry runs once, on a new connection.
- **Once rows have flowed, the statement fails** (design/019 §5.4).
- **A connection holding a transaction fails its statement**: `hugr: the connection to b lost its open transaction on
  B - ROLLBACK to go on`, and A's transaction is aborted.
  - Nothing of the B part was committed, unless the COMMIT itself was cut. Then the outcome is unknown, and the message
    says so.
  - A write is never retried.

**Fairness.**

- Loans wait in a FIFO queue for up to `pool_wait` (spec 080's pattern). After that the loan fails with
  `hugr: no connection to b within 25 s (4 of 4 in use, 3 waiting)`.
- A statement takes its loans as its scans start, so two statements that each hold part of the pool end in that
  timeout, not a hang.
- A role's share needs `max_remote_connections` in resource groups (design/019 §5.5), a duckdb-acl dependency. Until
  it exists, the queue is the only fairness.

**Visibility.**

- Metrics go to the registry acl-otel reads: `hugr.pool.connections{catalog, state}`, `hugr.pool.waiting`,
  `hugr.pool.reconnects{reason}`, `hugr.pool.retries`, `hugr.pool.refused{reason}` and `hugr.pool.wait_timeouts`.
- `hugr_pools()` is the operator's view:
  - per pool: its B, size, in use, idle and waiting;
  - per connection: its label, age and last error class.
- It never shows a connection id: the id's random part is its secret (spec 134).

### The catalog

- **The snapshot.** At ATTACH and at each refresh, the type reads B's listings as A's role: `duckdb_schemas()`,
  `duckdb_tables()` (each `CREATE TABLE` bound on A) and `duckdb_columns()` for views. B answers them as the
  principal's own, with masked and exposed types (duckdb-acl specs 025, 026, 046, 099). So `b` is what B shows A's
  role, never B's physical objects (design/019 §3).
- **The layout** is quack's. B's MAIN catalog's schemas sit at the top (`b.main.orders`). Each other catalog of the
  role becomes a schema holding its schemas (`b.hr.main.staff`).
- **The refresh** runs after `catalog_ttl`, or after an error from B that names an object or column (design/019 §3).
  - The snapshot is swapped whole; a statement bound to the old one finishes on it.
  - The failed statement is not retried, because its plan was bound to the old shape.
- **B's table functions and scalars are not in the catalog.** quack loads tables and views only; functions come with
  stage 2.

### Stages

1. **This spec's code.**
   - The type, the identity, the catalog, the pool and the reconnect.
   - Read-only scans that push the projection only, as quack's scan does (`filter_pushdown` is off). Filters are
     pending Q6.
   - `hugr` in the profile, the metrics and the tests.
2. **Pushdown** (design/019 §6), in its own spec.
   - The type claims `IS_REMOTE | EXECUTE_QUERY_NODE`.
   - `SupportsPushdown` is a dry run against what B lets A's role call. That list is `duckdb_functions()` under a
     principal (duckdb-acl spec 098, done), read with the catalog.
   - It vetoes by default, using mssql-extension spec 079's tree walks. The writer is `ToString()`: B gets text,
     never a serialized plan, and B's acl parses it again.
   - The vehicle is a table function in the never set.
   - It has the per-link settings of design/019 §7 (pending Q5), and measures what `IS_REMOTE` costs.
3. **Lineage** (design/019 §11).
   - hugr_node provides the lineage identity of `hugr` sources (`acl_lineage_sources`): B's cluster namespace, read
     from B's discovery.
   - A statement's trace and lineage context travel as request headers, never as a SET.

Writes through `hugr` are pending Q4.

### Dependencies

- **duckdb-acl `6750f3a` or later** (spec 135): the two "connect again" answers. Spec 134's label is in the pin
  already.
- **duckdb-acl**: `max_remote_connections` in resource groups (spec 085), readable by hugr_node for a running
  statement's session (design/019 §8).
- **duckdb-acl, for stage 2**: the never set must name the type's vehicle (design/019 §4). It has `quack_*` but not
  `hugr_*`.
- **duckdb-acl, for stage 3**: B's discovery serves the cluster's lineage namespace.
- **Already done**: the principal's function list (duckdb-acl spec 098) and the label in connection ids (spec 134).
- **Here**: `acl_cluster.cpp` adds `hugr` to `TYPES_NEEDING_SECRET`, and `ExtensionOfType("hugr")` answers `''` because
  the type is built in. design/019 §8 put this in acl, before spec 127 moved the profile code here.
- **duckdb-ext-common, for stage 2**: the tree walks, shared out of mssql-extension.

## Enforcement / security

- **Two policies stack; neither bypasses the other** (design/019 §6). A rewrites before bind (its names become
  `b.<path>`, with A's RLS and masks), and B enforces its own policy for A's role. B's grants to that role are the
  ceiling for every user of A.
- **No delegation.** B sees and audits A's node role only. A's users are in A's audit.
- **Stage 1 sends B nothing of A's**: no predicate, no claim value, no user - only `SELECT <columns> FROM <path>` and
  the listing queries. The trust question of design/019 §7 starts with stage 2.
- **Credentials.**
  - The client secret is only in A's tresor.
  - The token is in memory only, never logged or written.
  - A profile item `TYPE hugr` without `SECRET` is refused, and its options are linted for credential keys.
- **TLS** is verified against the node's `ca_cert_file` (specs 006, 010). Cleartext is allowed to loopback only.
- **No borrower can set B's state for the next one.** A's users share one role on B, and nothing more.
- **Retries** cover reads only: once, before any row, without pinned state. A write is never retried.
- **Revoking A.** Under B's `connect` binding, a revoked client keeps its live connections for at most `max_lifetime`.
  Each reconnect needs a fresh token, so B's operator removes A by revoking A's client or its mapping (and see Q9).
- **The type's own name and hidden symbols** let it load beside acl's embedded server and a stock quack.
- **`hugr_pools()` and every other `hugr_*` function** are refused to A's principals by the function gate, because
  they are uncategorized (spec 072).

## Testing

- **sqllogictest** (`test/sql/federation/hugr_type.test`, no B needed):
  - ATTACH is refused without `SECRET`, with an unknown option, and with `disable_ssl` to a host that is not loopback;
  - `ACL CLUSTER ATTACH … (TYPE hugr)` is refused without `SECRET`, and with one it plans no extension install;
  - `hugr_pools()` is refused under a principal.
- **C++** (`make test-cpp`):
  - `test_hugr_pool.cpp` tests the pool's decisions, kept in a pure `hugr_pool.hpp` (as `hugr_node_tick.hpp` is):
    - the FIFO queue and its timeout;
    - retirement when idle and after the lifetime;
    - each answer in the table;
    - one retry, only before a row and only unpinned;
    - the label sweep;
    - no growth after `at_capacity`;
    - the token's reuse.
  - `test_hugr_type_coload.cpp` loads hugr_node and a stock quack in one process. Both attach, and our copies'
    symbols are local (`nm`).
- **e2e** (`test/e2e/federation/run.sh`, in `make test-e2e`).
  - The setup:
    - Keycloak and tresor-server, as in `test/e2e/bootstrap`;
    - two B nodes, `b1` and `b2`, on one policy catalog, with labels and quack doors;
    - a Python stand-in for the front, which routes by the label in `X-Quack-Connection-Id` and places new connections
      where it is told;
    - A, with its client secret in tresor-server.
  - The checks:
    1. A reads exactly B's slice for A's role, and a column B hides is not in `b`.
    2. A's principal reads through A's virtual table with A's RLS on top, and cannot name `b.…`.
    3. A join of two B tables runs on two loans.
    4. B's door restarts between two statements: the second reconnects, succeeds, and is counted.
    5. The stand-in forgets `b1`: `b2` answers "not known", and A continues on `b2`.
    6. A is stopped (SIGSTOP) for longer than its lease: its next statement succeeds through a reconnect.
    7. B is killed during a long scan: that statement fails, and the next works.
    8. With a short token lifetime, live connections keep working, and new ones mint new tokens.
    9. `acl_drain()` on `b1` with a short `max_lifetime`: A's reads continue on `b2`, and `b1` holds none of A's
       sessions after one lifetime.
    10. One statement more than `pool_size`: it waits, then fails with the message, and `hugr_pools()` shows the queue.
        With B's door capacity lowered, B's refusal counter stays bounded.
    11. SET, CREATE TABLE and INSERT through `b` are refused.
- **CI**: the linux job, like the other e2e.

## Alternatives

- **Stock quack (`TYPE quack`) with spec 061's provider secret.** It has one connection, no reconnect, pushdown that
  cannot be turned off, and a manual token refresh (Problem).
- **Views bound through a function that takes SQL, as quack binds them.** It stays closer to quack's code, but any SQL
  that reached the function would run on a pooled connection - a SET under `ACL NATIVE`, for one.
- **A connection per A session instead of a pool.** B seats 16 quack clients per node by default, so B would bound A's
  users.
- **Flight SQL as the transport.** duckdb has no Flight catalog client, and quack is duckdb's own protocol, already
  served by B.
- **Not done** (design/019 §10): delegating A's users to B, routing per statement, moving quack sessions between nodes,
  serialized plans, and two-phase commit.

## Open questions for the owner

**Q1. One snapshot of B per A query?** Two scans of B in one A query run on two connections and see B at two moments
(design/019 §5.2). *Recommendation:* accept it and document it. B's own sources are live, and B gives no snapshot
across them either. One snapshot would need the scans to run one after another in a single B transaction, or quack's
refusal of the second scan.

**Q2. Read-ahead: fixed in the client, or declared by B?** *Recommendation:* fixed for now, as the option `read_ahead`
with 8 as the default (design/019 §5.1). Switch to B's value once B announces one in its discovery (the
server-announced read-ahead proposed on duckdb-quack#277). The pool does not change either way.

**Q3. Does the pool need B's node information?** *Recommendation:* no, not in v1. The label in each connection id
(spec 134) is enough for the sweep, and B's front with the maximum lifetime spreads and drains the pool. B's load report
needs an `observe` bearer (spec 097), a privileged scope that A's role should not hold.

**Q4. Writes to B through `hugr` in v1?** *Recommendation:* read-only in v1. A write brings pinned state (one loan per
A transaction), a statement that is never retried, and an outcome A cannot know after a lost COMMIT. The use the owner
described is long-lived and mostly reads (design/082 F1). Writes can come in a stage of their own, after pushdown.

**Q5. Re-filter A's RLS locally by default?** *Recommendation:* on by default, per link. It costs one filter over rows
that are arriving anyway, and it catches a B that evaluates A's predicate differently (a type, a collation). Turning it
off for a trusted B is the operator's choice. Masks cannot be re-checked this way (design/019 §7).

**Q6. Filters in stage 1's scans?** (found in quack) quack's scan pushes only the projection: `filter_pushdown` is off
at `e7a9be3`. Each scan of B therefore transfers the role's whole slice of the table. *Recommendation:* keep stage 1
projection-only, so B never sees an A predicate or claim value. Bring the scan's filters in stage 2, under the same
per-link setting as whole-query pushdown.

**Q7. How many of B's seats does a `hugr` connection take?** (found in duckdb-acl spec 079) B's door charges each
quack connection `acl_quack_client_depth` seats (64) at connect, before it knows who is connecting. So a connection with
a read-ahead of 8 still costs 64, and a default B node seats 16 connections across all its quack clients.
*Recommendation:* in v1, a B that serves federation places A on nodes of A's own (spec 096). There, a profile setting
for that group sets the depth to A's read-ahead. Later, the client declares its depth at CONNECT and B charges that:
one protocol change, together with Q2's announcement.

**Q8. Where do the grant's IdP and scope come from?** (found in duckdb-acl spec 095) An `oidc_client` secret holds
`CLIENT_ID`, `CLIENT_SECRET` and `AUDIENCES`, and no issuer. *Recommendation:* A's operator names both, as `issuer` and
`scope` options of the ATTACH. These are pointers, not credentials, so the profile may carry them. Never take them from
B's door discovery: that way A's client secret goes only to an IdP that A's operator wrote down.

**Q9. Reconnect on `Authorization failed`?** (found in quack's server) B's door answers this when the connection's
session has ended there: idle past `acl_session_idle_timeout`, killed, or past its token's `exp` under
`acl_session_token_binding = 'every_use'`. *Recommendation:* yes, treat it like the two "connect again" answers: once,
before any row. The connection is useless either way, and the new one is judged afresh (token, drain, placement). An
operator who wants A off B revokes A's client or its mapping, and the reconnect then gets no token.

## Follow-ups

- The specs for stages 2 and 3, and a stage for writes if Q4 allows one.
- The front's demo (hugr-platform 002), with a federated pair.
- Drop the #297 patch at the quack pin that carries it. Revisit `read_ahead` and the seats once duckdb-quack#277's
  announcement exists.
- B's virtual functions in `b`, with stage 2.
