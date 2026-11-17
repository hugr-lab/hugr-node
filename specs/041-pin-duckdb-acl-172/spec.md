# Spec 041: duckdb-acl after spec 172

- **Status**: implemented
- **Date**: 2026-11-17

duckdb-acl moves from 7be0b70 (spec 040) to fc11ff4, its spec 172 (#308): a virtual table's `ON INSERT SET` / `ON
UPDATE SET` - values the node computes on every write it decides (INSERT, UPDATE, MERGE, Flight ingest, a quack
stream), over the client's own; Flight ingest reads its stream through `arrow_scan_dumb`.

- **Schema.** v28, `min_reader` 28 (`relation_write_values`): `acl_schema_window_cluster.test` stamps a catalog one
  step ahead (29 / 28).
- **Sources.** No translation unit added or removed. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
