# Spec 038: duckdb-acl after spec 158 stage 3

- **Status**: implemented
- **Date**: 2026-11-16

duckdb-acl moves from 0215027 (spec 037) to eabdc28, its spec 158 stage 3 (#304): spec 102's field paths are removed -
the `grant_columns` table, `platform.grant_columns`, `acl_grant_columns()` and `acl_listed_type` with them.

- **Schema.** v27, `min_reader` 27 (`grant_columns` dropped): `acl_schema_window_cluster.test` stamps a catalog one
  step ahead (28 / 27).
- **Sources.** No translation unit added or removed. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
