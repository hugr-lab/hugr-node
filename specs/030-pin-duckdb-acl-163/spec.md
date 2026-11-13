# Spec 030: duckdb-acl after spec 163

- **Status**: implemented
- **Date**: 2026-11-13

duckdb-acl moves from 8977ade (spec 029) to 718b5bb, its spec 163 (#296). A virtual table function's or scalar's
argument that names the caller's query is now evaluated beside the call (`VALUES`), and the template reads only its
value. Aliases of macros are covered. A builtin that an alias or a template calls is written `system.main.<name>`.

No new translation unit; tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp`, all unchanged.
