# Spec 009: re-pin duckdb to f8f3f4d - with duckdb-acl, tresor and acl-otel

- **Status**: implemented
- **Date**: 2026-10-17

hugr_node builds duckdb-acl, tresor and acl-otel in, so the four move the same day:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb | 7304f7b | f8f3f4d | the head of `v2.0-cyanoptera` on 2026-10-16 (duckdb-acl spec 133, #233) |
| duckdb-acl | ce1d417 | 23a8b18 | spec 133: quack e7a9be3 (the embedded door regenerated), the scanners at their mains |
| tresor | 059190a | 62c3d97 | its spec 034 (the re-pin), `ACL_COMMIT` 23a8b18 |
| acl-otel | 15b0eb2 | 8542cad | its spec 026 (#53), `ACL_BASE` 23a8b18 |

- **The release's extensions** (spec 008) follow duckdb-acl's pins, read from its `extension_config.cmake`:
  postgres_scanner c0f5a3c, mssql 75e0fb6, ducklake ca86db0. ducklake is now DuckLake v1.1, which refuses a
  1.1-dev1 catalog.
- **The shared-header check** compares duckdb-acl's and tresor's copies of duckdb-ext-common. Both are v0.13.0.
- **Nothing in `src/` changes.**
- **The distribution build compiles duckdb from the pin.** extension-ci-tools a90d6d4 hands it a prebuilt duckdb
  whose version (the release's alpha) is not the one the loadable names (the branch). duckdb-acl's test-cpp lost its
  libduckdb, and acl-otel's tests could not LOAD the extension. The Makefile overrides `DUCKDB_PREBUILT_LIBRARY` /
  `_EXTENSIONS`, as theirs do.

## Testing

On the new pins (macOS), `HUGR_NODE_E2E=1 HUGR_NODE_RELEASE=1`:

- the build;
- `test/sql/*`: 616 assertions;
- `make test-cpp`;
- `make test-e2e`: tresor in the cluster, acl_otel in hugr_node, a node bootstraps itself - all PASS.

On the PR: CI, the release's dry run (the image's e2e) and the distribution build.
