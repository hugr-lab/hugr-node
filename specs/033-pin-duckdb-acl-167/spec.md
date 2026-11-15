# Spec 033: duckdb-acl after spec 167

- **Status**: implemented
- **Date**: 2026-11-15

duckdb-acl moves from 466ef91 (spec 032) to 016b510, its spec 167 (#300). A catalog or object grant states the
versions of a time-travel table (DuckLake, Iceberg) a role reads: `VERSIONS CURRENT | ANY | AT (…) | BETWEEN …`.

- **Schema.** The policy schema moves to v24, with `min_reader` 24. A node of this build serves a v24 catalog only, and
  a v23 catalog is migrated with `acl_migrate_catalog` (the bootstrap's migrate step).
  `test/sql/cluster/acl_schema_window_cluster.test` stamps one version past the build: 25 / 24.
- **Sources.** One new translation unit, `src/acl_versions.cpp`, comes in through duckdb-acl's `acl_sources.cmake`.
  tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
