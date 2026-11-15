# Spec 032: duckdb-acl after specs 165 and 166

- **Status**: implemented
- **Date**: 2026-11-15

duckdb-acl moves from a375346 (spec 031) to 466ef91, its specs 165 (#298) and 166 (#299).

- **165**: a virtual table function called with the body's columns inside a principal's view keeps its probe at the
  body's root, so a name its template or policy lost refuses when the view is read, as it does for a direct call.
- **166**: a call's argument carrier column is named as duckdb names the argument as the client wrote it, so a result
  name or an error never shows the rewritten argument (a physical table, an RLS predicate).

No new translation unit; tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all unchanged.
