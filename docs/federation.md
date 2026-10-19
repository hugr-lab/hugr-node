# Another platform as a source

A node of platform A attaches platform B as a catalog of type `hugr` (spec 011):

```sql
ATTACH 'b.example:443' AS b (TYPE hugr, SECRET b_link);
```

`b` is then B's virtual catalog **as B shows it to A's node role**: the objects B grants that role, with B's row
filters and masks applied by B. A's operator maps them into A's own virtual catalogs, and A's users read them with A's
policy on top:

```sql
ACL ADMIN CREATE VIRTUAL TABLE sales.orders AS b.main.orders RLS (region = acl_claim('region'));
```

To B, A is one principal. No user of A reaches B as themselves, and B's grants to A's role are the ceiling for every
user of A. A's users never name `b` - it is a physical source on A, like a database A attaches.

## The secret

The token B verifies comes from the secret the ATTACH names. It is read **as the node** every time a connection is
made, never as the user whose statement made it.

On a platform node the secret lives in A's secrets service. B's operator gives A's operator a description of the
link - B's door, and the audience and scope B accepts for A - and A's operator stores it with a token tresor mints
for its caller:

```sql
CREATE PERSISTENT SECRET b_link IN corp (TYPE hugr, SCOPE 'hugr://b.example:443',
    TOKEN 'mint+exchange://api://hugr-b?scope=api://hugr-b/.default');
CALL corp.grant_secret('b_link', 'role:nodes', ['use']);
```

tresor exchanges the node's own token for one meant for B (RFC 8693) at every read and caches it until shortly before
it expires. So no client secret is stored where the identity provider federates the node's identity, and a reconnect
is the token's refresh. A `hugr` secret has one parameter, `token`, and it is redacted wherever secrets are listed.

**The secret's `SCOPE` names B's door**, and is required: the token goes only to the door its secret names. An ATTACH
of another door with that secret is refused, so whoever may write an ATTACH - the cluster profile's `cluster` bundle
among them - cannot send the node's token for B anywhere else.

## On B

B's side is configuration only:

- an issuer B trusts for the token tresor mints (spec 095), and a client with its audience whose mapping reaches A's
  role;
- that role's grants, and the function categories its listings need (B's catalog is read with `duckdb_schemas()` and
  `duckdb_columns()`);
- a resource group for the role (spec 085), on nodes of its own if wanted (spec 096);
- a served quack door.

## Options

| option | default | what |
| --- | --- | --- |
| `SECRET` | - | the `hugr` secret holding the token (required) |
| `pool_size` | 4 | connections to B at most, per node of A |
| `pool_wait` | 25 s | how long a read waits for a connection |
| `idle_timeout` | 600 s | a connection idle this long is closed - below B's session idle timeout (900 s) |
| `max_lifetime` | 1800 s | a connection is closed when it comes back after this, so the pool spreads over B's nodes |
| `read_ahead` | 8 | FETCHes in flight per read |
| `catalog_ttl` | 300 s | when B's catalog is read again |
| `heartbeat_timeout` | 60 s | the lease a connection asks B for |
| `disable_ssl` | false | cleartext - a loopback B only |

The cluster profile attaches B like any source: `ACL CLUSTER ATTACH 'b.example:443' AS b (TYPE hugr, SECRET b_link)`.
A `hugr` source without a `SECRET` is refused, and nothing is installed for it - the type is built into hugr_node.

## What it does

- **Read-only.** A write through `b` is refused. A read sends B `SELECT <the columns it needs> FROM <the object>` -
  never a filter, a user, a claim value or a statement of A's.
- **TLS** to B is verified against the node's `ca_cert_file`; no redirect is followed. An `http` secret reaches B only
  when it is scoped to `hugr://<door>`.
- **The catalog** is a snapshot of B's listings, read at ATTACH and again after `catalog_ttl`, or after B says an
  object or column is gone. A transaction reads one snapshot. A table and a view of B are both read as a table. An
  object with a type the node cannot spell is left out, with a warning in the log.
- **The pool.** Each read borrows a connection (one acl session on B) until B's answer ends; two reads that run at
  once borrow two. Borrowers are served in arrival order. A read that waits longer than `pool_wait` fails with the
  pool's state; B's last answer is in `hugr_pools()`.
- **B's shape.** Columns are asked for by name, and every answer is checked against the catalog A read: when B changed
  an object since (a type, a column), the statement fails with `… changed on B …` and the next one reads B's catalog
  again.
- **Reconnects.** When B says a connection is gone - its node no longer knows it, its lease ran out, its session
  ended, or B does not answer - the read is retried once on a new connection, before any row reached A. A connection
  of a node B no longer knows takes the other connections of that node with it. Once rows have flowed, a failure
  fails the statement.
- **B's capacity.** When B refuses a new connection (at capacity, draining, another resource group), the pool does
  not grow for a while (0.5 s, doubling to 8 s) and reads wait for a connection in use.

## Watching it

- `hugr_pools()`: one row per pool - B, its size, connections busy / idle / connecting, reads waiting, B's last
  refusal, and per connection its node label, age and idle time. Never a connection id: its random part is the
  connection's secret. The operator's; refused under a principal.
- Metrics, each with the link's `catalog`: `hugr.pool.connections{state}`, `hugr.pool.waiting`,
  `hugr.pool.reconnects{reason}`, `hugr.pool.retries`, `hugr.pool.refused{reason}`, `hugr.pool.wait_timeouts`.

## Not yet

B's functions are not in `b`; filters and whole queries are not sent to B (stage 2, with limits B sets per consumer);
lineage across the link is stage 3; nothing is written to B.
