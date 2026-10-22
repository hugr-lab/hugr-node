# Spec 018: duckdb-acl and tresor on ext-common v0.14.0

- **Status**: implemented
- **Date**: 2026-10-22

duckdb-ext-common v0.14.0 (its spec 017; tag f09b0a8, commit ac39cbe) has the OIDC core send PKCE on the device flow
when the issuer advertises `S256`, as Keycloak does. A device login at a Keycloak client that enforces PKCE failed
before it. hugr_node compiles the OIDC core of both duckdb-acl and tresor, which must be the same, so the two move
together:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb-acl | 41ec046 | 8b938d1 | its spec 142 (#252): ext-common v0.14.0; also spec 141's addendum (#251, mssql 6df11bc) |
| tresor | 550791c (v0.1.0) | 029913c | its specs 040 (device PKCE) and 041 (an optional `name` in the whoami response, a `name` column appended last to `corp.whoami()`), `ACL_COMMIT` 8b938d1 (#71) |

Both name ext-common ac39cbe. No contract changes (acl-otel stays on its pin). The pins and the tag were checked here
before moving.

## Tests

The build (`HUGR_NODE_E2E=1`), `test/sql/*`, `make test-cpp` and the bootstrap e2e (Keycloak, tresor-server, kista,
the orchestrated nodes, the first policy and spec 017's migration).
