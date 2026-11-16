# Spec 039: duckdb-acl after spec 158 stage 2

- **Status**: implemented
- **Date**: 2026-11-16

duckdb-acl moves from eabdc28 (spec 038) to a845082, its spec 158 stage 2 (#305): an object's own column list may hold
one star (`*`, `* EXCLUDE / REPLACE / RENAME`, `COLUMNS(…)`), read live; a name read twice fails closed at write, at
read (a guard bound at the version read) and in `acl_check_catalog` (`column_duplicate`).

- **Schema.** Unchanged (v27, `min_reader` 27).
- **Sources.** No translation unit added or removed. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
