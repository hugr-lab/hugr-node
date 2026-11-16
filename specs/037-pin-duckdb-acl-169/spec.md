# Spec 037: duckdb-acl after spec 169

- **Status**: implemented
- **Date**: 2026-11-16

duckdb-acl moves from f6cb901 (spec 036) to 0215027, its spec 169 (#303): a view a principal creates is stored as its
query (relations qualified to canonical virtual keys) and read under its READER's grants; a CTE name is in scope where
duckdb's binder sees it (a self or forward reference is a catalog name the gate resolves), and a recursive CTE's
`USING KEY` targets are gated.

- **Schema.** v26, `min_reader` 26 (`relations.query`): `acl_schema_window_cluster.test` stamps a catalog one step
  ahead (27 / 26).
- **Sources.** No new translation unit. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
