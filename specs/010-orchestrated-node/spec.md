# Spec 010: the node in an orchestrated cluster - the entry, the tick, the heartbeat

- **Status**: implemented (accepted by the owner, 2026-10-18)
- **Date**: 2026-10-18
- **Author**: Claude (with the owner). The node's side of hugr-platform spec 001 (the operator), settled the same day

## Summary

hugr-platform spec 001 makes the orchestrator a Kubernetes operator. It deploys a group's pods, answers a node's entry
with a bootstrap document, and records each node's heartbeat in a Lease. This spec is the node's side of that.

1. **The entry, orchestrator first.** The pod's environment holds the orchestrator's address and two projected
   ServiceAccount tokens, and nothing else of the platform. The node fetches document v2 with the first token. The
   document names the group, the node's label, tresor and the node's identity in the IdP, the repository and the
   catalog. With the second token the node then logs in to tresor. From there on spec 006's order runs unchanged.
2. **The engine's defaults from the pod.** Memory, threads and the spill volume follow from the pod's own resources.
   A profile item wins, but never above the pod.
3. **The tick.** Every interval the node:
   - applies the profile's **hot** items it does not have yet;
   - reports **restart** and **drain** items as pending;
   - drains itself when its resource group is gone;
   - sends the heartbeat.
4. **The label.** `acl_node_label` (duckdb-acl spec 134) is set from the document, so the node's sessions carry its
   name for the front.

Without an orchestrator, spec 006's entry stays as it is: tresor first, then a document v1 from `HUGR_CONTROL_PLANE`.

## Problem

- **Only the writing node applies a hot item.** Spec 006 left it this way: "only the node that writes a hot item
  applies it". The other nodes of its scope take a new source or extension at their next start only, and nothing
  reports what a node has applied.
- **The entry asks tresor first for a token to the control plane.** In an orchestrated cluster the pod's own
  ServiceAccount token is that proof, checked by TokenReview (spec 001). The tresor login and the node's IdP identity
  then come from the document, so they leave the pod's environment.
- **The engine does not know the pod.**
  - DuckDB sizes its memory limit and thread count from the machine, and spills to its temp directory up to 90% of the
    free disk.
  - In a pod, the first two can exceed the container's limits and get it OOM-killed.
  - The third fills the node's ephemeral storage, so kubelet evicts the pod and every session on it dies.
- **A node whose resource group is dropped keeps running.** It already refuses new sessions (duckdb-acl spec 096), but
  nothing says it is draining, so the front cannot tell.
- **Nothing sends the heartbeat** the operator records.

## Design

### The entry (with `HUGR_ORCHESTRATOR`)

**The environment.** No value in it is a credential.

| variable | what |
| --- | --- |
| `HUGR_ORCHESTRATOR` | the operator's in-cluster base URL (https, no userinfo) |
| `HUGR_ORCHESTRATOR_TOKEN_FILE` | the projected ServiceAccount token, audience `hugr-platform`: the entry and the heartbeat |
| `HUGR_IDENTITY_ASSERTION_FILE` | the projected ServiceAccount token with the IdP's federation audience: tresor's FLOW `federated` (spec 006) |
| `HUGR_FLIGHT`, `HUGR_FLIGHT_CERT`, `HUGR_FLIGHT_KEY`, `HUGR_QUACK`, … | the doors, as in spec 006 |
| `HUGR_DRAIN_DEADLINE`, `HUGR_CA_FILE` | as in spec 006 |
| `HUGR_POD_MEMORY`, `HUGR_POD_CPU` | the container's memory limit and its CPU request, through the Downward API (`resourceFieldRef: limits.memory`, `requests.cpu`, divisor 1: bytes and whole cores - a size sets no CPU limit, which would only throttle) |
| `HUGR_TEMP_DIRECTORY`, `HUGR_TEMP_SIZE` | the spill volume's mount and its size - bytes, a duckdb size (`600GiB`) or the `sizeLimit` as Kubernetes writes it (`600Gi`); the operator sets both |
| `HUGR_IDENTITY_PRIVATE_KEY_FILE`, `HUGR_TRESOR_INSECURE_HTTP` | **dev only**, as in spec 006 |

`HUGR_TRESOR`, `HUGR_IDENTITY_ISSUER`, `HUGR_IDENTITY_CLIENT_ID` and `HUGR_IDENTITY_SCOPE` move into the document. When
`HUGR_ORCHESTRATOR` is set, they are refused in the environment, and so is `HUGR_CONTROL_PLANE`: one source per value.

The pod's numbers are checked as read: a size below 1 MiB, or more cores than the machine has, is a Downward API
divisor other than 1 and is refused. The engine's values apply without an orchestrator too, whenever the environment
names them. No URL of the environment or the document reaches an error or a log line (a URL may carry userinfo): a
refusal names the variable or the field.

**The order.** Only steps 2, 3 and 5 change.

1. **Hardening**, as in spec 006, plus `temp_directory` from `HUGR_TEMP_DIRECTORY` and
   `enable_server_cert_verification = true` (whichever HTTP client serves a request verifies its server; httplib's own
   default does not).
2. **The document.** `GET <HUGR_ORCHESTRATOR>/v1/bootstrap` with `Authorization: Bearer <the token file's content>`.
   - It goes through duckdb's HTTP client, so `ca_cert_file` applies, and never follows a redirect: the bearer goes to
     the orchestrator only. The server is verified by the hardening: step 1 turns httplib's verification on, curl's is
     on by default, and neither the client nor its verification is a profile item.
   - The token is read from its file at each call, because kubelet renews it. It is one line of printable ASCII of at
     most 64 KiB; anything else is refused before it reaches a header.
   - It is never written into a secret, a setting or a log.
3. **tresor**, as in spec 006, with the document's `tresor.host` and `tresor.identity` (issuer, client id, scope) and
   `HUGR_IDENTITY_ASSERTION_FILE`.
4. **The repository**, as in spec 006.
5. **The policy catalog**, as in spec 006, plus two settings: `SET GLOBAL acl_node_group` from `node_group`, and
   `SET GLOBAL acl_node_label` from `node_label`.
6. **The engine, then the profile.**
   - First the defaults:
     - `memory_limit` at 80% of `HUGR_POD_MEMORY`, DuckDB's own ratio of the machine;
     - `threads` at `HUGR_POD_CPU`;
     - `max_temp_directory_size` at 90% of `HUGR_TEMP_SIZE`.
   - Then the profile, as in spec 006. An item for one of those three settings is applied cut to its ceiling, and the
     cut is reported as drift (`cut`). The ceilings:
     - `memory_limit` at 90% of the pod's memory;
     - `threads` at the pod's CPU;
     - `max_temp_directory_size` at 90% of the volume.
   - The ceiling fails closed: a value that is no finite amount asks for everything and is cut to the ceiling. That
     covers unlimited (`-1`, `none`), a percentage (`95%`, `90% of available disk space`) and `1e3` threads, all of
     which duckdb itself would take for the whole machine.
7. **Lock**, as in spec 006.
8. **Serve**, as in spec 006, then the tick (below).

**Document v2** (hugr-platform spec 001). An unknown `version` is refused, and unknown fields are ignored. Required:
`node_group`, `node_label` (a DNS label, `[a-z0-9-]` of at most 63 - what the node's sessions carry, duckdb-acl spec
134), `tresor.host`, `tresor.identity.issuer` and `client_id`, and `repository.prefix` (https, no userinfo).
`heartbeat.interval_seconds` is a whole number in 1..3600, 15 when absent.

```json
{"version": 2,
 "node_group": "etl", "node_label": "hugr-node-etl-7f9c-x2k8p",
 "tresor": {"host": "tresor.example:443",
            "identity": {"issuer": "https://login.microsoftonline.com/<tenant>/v2.0",
                         "client_id": "<the group's workload identity>", "scope": "api://tresor/.default"}},
 "repository": {"name": "hugr", "prefix": "https://kista.example/<tenant>/<channel>"},
 "catalog": {"secret": "hugr_policy_catalog", "schema": "acl"},
 "heartbeat": {"interval_seconds": 15}}
```

**The catalog's type comes from its secret.**
- The document names only the reserved secret (the owner's decision of 2026-10-17), with no type. The secret's own
  TYPE gives the catalog's: tresor's `duckdb_secrets()` lists a descriptor with its type before any ATTACH.
- The node reads the secret's type and attaches the catalog as that type, installing the type's extension from the
  repository when the build does not link it.
- A secret of a type other than `postgres`, `mysql` or `mssql` (the policy catalog's) is refused.

### The tick

The runner's wait loop (spec 006: a poll every 250 ms on the PRAGMA's own thread, never a worker) runs one tick every
`heartbeat.interval_seconds`, the first one as soon as the doors are open. A tick that fails is logged and the next one
tries again; the node never stops serving because of one. Without an orchestrator (spec 006's entry) the tick runs
too, every 15 s: the convergence and the resource group, without the heartbeat.

1. **Convergence.**
   - The node reads the profile's version and its effective items for its scope (`ClusterEffective`), and what is in
     effect on it (`ClusterDrift`). It also remembers what it **took** from the profile, with the spec it applied: at
     its bootstrap, on a tick, or by its own profile write (the writing node applies a hot item before its commit; the
     write notes it, and the next tick takes it). Nothing else is taken: a database attached by hand that the drift
     reads as `same` (it compares a source by its type) stays drift.
   - Every **hot** item not yet in effect is applied with the statements its write already uses
     (`ClusterItemApplyStatements`, spec 093): an `INSTALL`/`LOAD` from the repository, an `ATTACH` of a new source.
     A source the node took and the profile dropped is detached; one in use goes with a drain. A database attached on
     the node by hand (`ACL NATIVE`, break-glass) is never touched: it is drift, shown.
   - Every **restart** or **drain** item the node cannot apply live becomes `needs_restart`, and is listed in
     `pending` with its kind, name and class:
     - a setting that differs, or one the node took and the profile dropped (`RESET`) - the configuration is locked;
     - an extension at another version, written again (its spec is not the one taken), or removed;
     - a source re-pointed: to another type (the drift says `differs`) or to the same type (the drift cannot tell;
       its spec is not the one the node attached).
     A restart item withdrawn before the restart ran waits no more.
   - Once every hot item of the version is in effect, `acl_cluster_applied(version)` records it.
   - A hot item that fails is listed in `pending` with its error class (never its text, which may carry a path). The
     rest still apply, the version is not recorded, and the next tick tries it again.
   - An extension is one item under its canonical name (`postgres` is written as `postgres_scanner`, as duckdb loads
     it). The policy catalog's own extension is the bootstrap's: step 5 loads it before the profile can be read, so an
     item for it is never pending (a restart would load it the same way).
   - A setting duckdb renders differently from how it is written is the same value when it reads as the same boolean
     (`yes`, `true`) or number (`1e3`, `1000`; `2`, `2.0`). A thread count is never set on the drift's scratch
     instance: a profile's `1e6` would start a million threads there on every tick.
2. **The resource group.**
   - A node of a group the policy lacks already refuses new sessions (spec 096, fail closed).
   - The tick makes it explicit: it calls `acl_drain()`, so the load report and the heartbeat say `draining`, and logs
     why.
   - When the group is back, the tick calls `acl_resume()`, but only for a drain it made itself. A drain an operator
     ordered (`DRAIN NODE`) before the group went is left alone.
   - A known limit: a drain an operator orders while the node is already drained this way cannot be told from it,
     and ends with it when the group comes back.
3. **The heartbeat.** `POST <HUGR_ORCHESTRATOR>/v1/heartbeat` with the same bearer and through the same client as the
   document, in one attempt: the connection within 5 s, and a transfer stalled for 5 s is cut, so a stalled orchestrator
   does not hold the tick, nor a SIGTERM behind it.
   The body is spec 001's contract:
   - `node_id`: the document's `node_label`, the pod's name;
   - `image`, `duckdb`: the build's version and duckdb's source id (its commit - the node tracks a branch head);
   - `config`: `target`, `applied`, `needs_restart`, `pending`;
   - `drift`: the counts of `platform.drift` by state, with `cut` for a setting held at the pod's ceiling;
   - `load`: `acl_node_load()`.

   A heartbeat that fails is logged (an answer by its status, a 409 says another pod beats as this node; no answer by
   its error's class) and the next tick sends again. The ticks keep the interval's own clock, so a slow one does not
   push the later beats back. The operator counts a node lost after three intervals; the node keeps serving.

### Stopping

As in spec 006. The tick stops first, so that no statement of its own runs while the doors drain.

### Changes to acl_cluster.cpp

- `HARDENING_SETTINGS` gains `temp_directory`. Where a node spills is the bootstrap's, from the pod, and a profile item
  could otherwise point every node's temp files somewhere else.
- It also gains the bearer path's two (found in review): `httpfs_client_implementation` (httplib verifies a server by
  its own setting) and `redact_http_logs` (off, an HTTP log shows an `Authorization` header as written).
- `ClusterDrift` compares an extension by its canonical name, a setting also as a boolean or a number, marks the
  policy catalog's extension `bootstrap`, and never sets a thread count on its scratch. `ClusterExtension` writes an
  extension under its canonical name. A hot item a write applies on the writing node is noted for that node's tick
  (`NoteAppliedHere` / `TakeAppliedHere`, per instance in its object cache).
- The notes of drain and restart items say what happens now: each node reports the item pending, and the orchestrator
  rolls the nodes.

The clamp rule (a setting's ceiling) is one function in `hugr_node_engine.hpp`, shared by the bootstrap's step 6 and
the tick's report; the tick's decision is `hugr_node_tick.hpp`. Both are pure.

### Pins

duckdb-acl moves to main (`0c3415c`, spec 134: `acl_node_label`), which also brings mssql 986fd19. tresor moves to
`d7f2a1e` (its spec 036: `enable_external_access = false` stops its own HTTP too; a node does not disable it, so
nothing changes here).

## Enforcement / security

- **No credential is written anywhere.**
  - The two tokens stay in kubelet's files and are read per call.
  - The document holds pointers and names.
  - The catalog's secret comes from tresor, as before.
- **The document reveals a group's pointers only to its pods.** The operator binds the token to a pod of the group by
  TokenReview (spec 001).
- **The node speaks only for itself.** Its heartbeat's `node_id` is its pod's name, and the operator refuses another.
- **The tick applies only what a profile write already applies**, from the same statements, under the same hardening
  and the same trusted repository. A restart item never runs live: the configuration stays locked after the
  bootstrap.
- **The bearer goes to the orchestrator only**: https without userinfo, a verified server, no redirect, and no profile
  item can change the HTTP client, its verification, or unredact its log (only `ACL NATIVE` can, on one node: the
  break-glass).
- **The pod's ceiling fails closed**: a value the node cannot read as an amount is cut to the ceiling, never applied
  as written.
- **Nothing listens before the policy is loaded and the configuration is locked**, as in spec 006.
- **A dropped resource group closes the node to new sessions** (spec 096). The tick only says so, and reopens only what
  it closed.

## Testing

- **sqllogictest** (`test/sql/agent/hugr_node_bootstrap.test`, `test/sql/cluster/*.test`):
  - `hugr_node_bootstrap_plan()` with a v2 document and an orchestrated environment: the new order, `acl_node_group`
    and `acl_node_label` from the document, the engine's defaults, `temp_directory`, the server verification, and the
    catalog type taken from the secret's;
  - the legacy tresor variables and `HUGR_CONTROL_PLANE` refused when `HUGR_ORCHESTRATOR` is set; an orchestrator URL
    over http or with userinfo; a pod size below 1 MiB, a fractional CPU; a `600Gi` spill size read like `600GiB`;
  - v2 without `node_group`, without a `node_label` or with one that is no DNS label, without tresor's identity, with a
    heartbeat interval out of range or not a whole number; an unknown version; a loopback tresor host with userinfo;
  - `hugr_node_profile_statements(environment := …)`: an engine item cut to the pod, one under it as written, and
    `-1`, `none`, `95%`, `90% of available disk space`, `1e2` threads all cut to the ceiling;
  - `temp_directory`, `httpfs_client_implementation`, `redact_http_logs` refused as profile items; an extension
    alias written under its canonical name;
  - `platform.drift` (`node_cluster.test`): an extension's setting written `1e3` and shown `1000` is `same`, and a
    profile `threads = '1e6'` is read without starting a thread.
- **C++** (`make test-cpp`, the pure parts):
  - `test_hugr_node_engine.cpp`: the defaults, the ceilings, the clamp (a value that is no amount cut);
  - `test_hugr_node_tick.cpp`: the decision from (effective items, drift, what the node took) - the hot items to
    apply, the pending list with classes, `needs_restart`, a re-point to the same type, a removed extension and a
    RESET setting, adoption of the node's own write and the detach it enables, a database attached by hand never
    adopted, the policy catalog's extension never pending, what is forgotten; and the resource group's decision -
    drained by the node, resumed only when it drained itself, an operator's drain never taken nor resumed.
- **e2e** `test/e2e/bootstrap/run.sh`, extended with an orchestrated mode. A fake orchestrator over https serves the
  v2 document to the token file it knows, records the heartbeats and answers 409 to a node beating for another. Two
  nodes run on one catalog, and the checks are:
  1. both enter through the orchestrator, log in to tresor with the document's identity, take the catalog's type
     from its secret, and serve;
  2. the engine's defaults follow `HUGR_POD_*`, and a profile `memory_limit` above the pod is cut - `cut` in the
     heartbeat's drift, nothing pending;
  3. a hot `ACL CLUSTER ATTACH` written on the catalog (by an operator's process, which applies it itself) is in effect
     on both within a tick, `applied` at the new version; re-pointed to the same type it is pending a drain on both,
     and dropped it is detached by both;
  4. a hot item failing on one node (a file the other node holds) is listed there with its error's class, and the rest
     of the write applies;
  5. a `SET … IN GROUP g` is reported by both as `needs_restart` with the item in `pending`; withdrawn before a
     restart it waits no more, while a `RESET` of a setting both took at their bootstrap waits for the restart;
  6. a `DROP RESOURCE GROUP g` drains both by themselves (the heartbeat says `draining`), and recreating it resumes
     them;
  7. the heartbeat's `node_id` is the document's label, and the load report carries it.
  The cookie `acl_node=<label>` on both doors is duckdb-acl spec 134's, tested there.
- **Not automated**: `Converge` and `HeartbeatBody` themselves run against a live node only in the e2e; the
  heartbeat's 5 s bound and a token file over 64 KiB are by review.
- **CI**: as spec 006's e2e, in the linux job.

## Alternatives

- **The operator computing the engine's values and passing them in env.** The Downward API gives the node the
  container's own limits, so the node computes. That works the same for a pod someone wrote by hand.
- **The heartbeat and the tick as a separate process or a table function.** A table function runs on a scheduler
  worker and would deadlock a `SET threads` (spec 006). A second process would need its own connection to the
  catalog and its own identity. The runner's loop is already there.
- **Restart items applied live.** Not possible: the configuration is locked after the bootstrap (spec 006 step 7), on
  purpose.
- **Hot items applied with a canary, one node first.** Not now. A hot item's failure is an error the node reports, not
  a crash, and extensions come only from the signed repository. Restart items roll one node at a time already, by the
  group's restart (spec 001).

## Follow-ups

- The operator's side: hugr-platform spec 001's code.
- The front's re-seat on `wrong_resource_group` needs duckdb-acl's machine-readable `x-acl-resource-groups` (an
  addendum to spec 096 there).
- A document from a file, for a bare-metal node with no orchestrator and no control plane.
