# Spec 040: duckdb-acl after specs 170 and 171

- **Status**: implemented
- **Date**: 2026-11-17

duckdb-acl moves from a845082 (spec 039) to 7be0b70:

- spec 170 (#306): DROP RELATION of a name a live alias shares is refused while object grants are on it, and a
  principal's view never shadows an object of its alias's source;
- spec 171 (#307): an object's list of bare names and renames is writable.

- **Schema.** Unchanged (v27, `min_reader` 27).
- **Sources.** No translation unit added or removed. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
