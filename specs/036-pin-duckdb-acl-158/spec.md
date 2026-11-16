# Spec 036: duckdb-acl after spec 158 stage 1

- **Status**: implemented
- **Date**: 2026-11-16

duckdb-acl moves from f208911 (spec 034) to f6cb901, its spec 158 stage 1 (#302): an object defines its columns (a
SELECT list over its source); an object grant's `COLUMNS` is a test on the object's names (`a, b`, `*`, `* EXCLUDE
(…)`) and `MASK (<column> AS <expr>)` its masks; a catalog grant takes no column list; the `acl_has_role('r')` marker.

- **Schema.** v25, `min_reader` 25 (`role_object_caps.masks`): `acl_schema_window_cluster.test` stamps a catalog one
  step ahead (26 / 25).
- **Sources.** No new translation unit (one header, `acl_grant_columns.hpp`). tresor and ext-common do not move.
- **This repository's policies** name plain column lists only, which read the same.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
