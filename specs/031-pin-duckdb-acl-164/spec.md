# Spec 031: duckdb-acl after spec 164

- **Status**: implemented
- **Date**: 2026-11-14

duckdb-acl moves from 718b5bb (spec 030) to a375346, its spec 164 (#297). A virtual table function called with the
caller's columns is bound alone before the statement is optimized, with those arguments NULL. A name that its template
or its grant's policy lost then refuses. It no longer binds to the caller's columns.

No new translation unit; tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp`, all unchanged.
