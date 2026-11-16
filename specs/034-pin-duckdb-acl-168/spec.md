# Spec 034: duckdb-acl after spec 168

- **Status**: implemented
- **Date**: 2026-11-16

duckdb-acl moves from 016b510 (spec 033) to f208911, its spec 168 (#301). A grant that states no `VERSIONS` now reads
a time-travel table (DuckLake, Iceberg) at its current version only; a role reads history only where a grant states
`ANY`, `AT (…)` or `BETWEEN …`.

- **Schema.** Unchanged: v24, `min_reader` 24 (the meaning of a NULL `versions` changed, not the table).
- **Sources.** No new translation unit. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
