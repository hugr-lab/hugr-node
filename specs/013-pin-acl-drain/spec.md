# Spec 013: re-pin duckdb-acl - a draining node seats its operators' calls

- **Status**: implemented
- **Date**: 2026-10-20

hugr-platform's console (spec 003) calls a node without keeping a session on it: each call is a session of its own,
ended with the call, so a closed page leaves nothing on the node's seats. A draining node refused every new session,
so the console could not watch the drain, end its stragglers or resume a node somebody else drained. duckdb-acl
seats such a call now; hugr_node builds duckdb-acl in, so the node does from this pin on:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb-acl | ad784b9 | d3f261e | spec 066's addendum (#246): a draining node seats a cookie-less Flight call (a session for one call, spec 050) of a principal who holds the node's own rights - a node view to read (the `observe` bundle, a point grant) or a node operation to call (`operate`, a point grant; a deny wins); a connection-long session (a cookie, quack, `acl_session_open`) stays refused, a draining node hands out no session cookie, and `acl_drain()`, `platform.sessions` and the load report leave the calling call's own session out |

duckdb (f8f3f4d), tresor and acl-otel stay: the change touches no duckdb-ext-common contract, and duckdb-acl's pin of
duckdb is the same.

- **The `cluster` bundle is seated**: it is this repository's (`RegisterAdminScope(CLUSTER)`), and duckdb-acl reads it
  as operate + observe, so the platform's operators (hugr-platform's `platform_admin`: `cluster` + `policy`) reach a
  draining node from the console. A `policy` bundle alone, a catalog's `manage` and a principal who administers
  nothing are refused as before.
- **The agent's own drain is unchanged**: on SIGTERM it runs `acl_drain()` on its own connection, which is no session;
  a console call seated meanwhile counts for that call only.
- **Federation is unchanged**: the `hugr` type reaches another platform's node through quack, whose sessions are
  connection-long - a draining node still refuses them, and the pool moves to another node (spec 011).
- **Nothing in `src/` changes.**

## Testing

- duckdb-acl's own: `test/sql/acl_drain.test` (every token refused through `acl_session_open` while draining) and
  `test/e2e/flight/drain.sh` (the seated rights, the deny, the refused, no cookie handed out, the own call left out).
- The `cluster` bundle on hugr_node: hugr-platform's demo (spec 004, check 4c) drains a node through the console,
  reads its page and resumes it, each call with a new token, on this pin's image, then finds no console session left
  on any node.
- Locally: the build, `test/sql/*`, `make test-cpp`; on the PR: CI, the release's dry run and the distribution build.
