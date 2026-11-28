# Spec 049: duckdb-acl, tresor and acl-otel on ext-common v0.17.0

- **Status**: implemented
- **Date**: 2026-11-28
- **Author**: the owner (2026-11-28): re-pin to ext-common v0.17.0, then release.

## Summary

The three halves this node builds in move to duckdb-ext-common v0.17.0 (37d542e; its spec 020, the neutral contract
`contracts/secret_compute.hpp`) together. `tresor_sources.cmake` and `acl_otel_sources.cmake` refuse to configure
unless the three copies of `contracts/*` (and, for tresor, `oidc/include/*`) are the same files, and v0.17.0 changes
`tresor_audit.hpp`.

- **duckdb-acl** d2388a3 → 3bcb862, its spec 184 (#325): the submodule only, no code.
- **tresor** 9958f06 → f6cbfd0, its spec 048 (#93, #94): computed parameters. A secret declares an operation, and
  `unwrap` (a SQL Server backup's content key) runs in the secrets service with the key in a KMS; a consumer such as
  mssql reaches it through `secret_compute` without naming tresor. A new translation unit, `tresor_mint.cpp`, comes in
  through tresor's own source list. `corp.mint(…)` is a function of the tresor catalog: under a principal it is the
  function gate's, as `corp.whoami()` is (duckdb-acl spec 082). The operator grants it by a category or by name.
- **acl-otel** 3916552 → 650651d, its spec 028 (#59): the submodule only, and its Marquez e2e in acl's spec 158
  column model. A tresor event's new kind, `mint`, is passed through as an attribute.

No setting is added to the node, and nothing changes in the cluster profile, the hardening list or the never set.
Schema stays v29, `min_reader` 28. duckdb stays v2.0.0.

## Tests

- `HUGR_NODE_E2E=1 GEN=ninja make`: the configure-time header checks pass.
- `test/sql/*` (1421 assertions in 19 files) and `make test-cpp` pass, and so does the bootstrap e2e.
- CI and the release's dry run run the bootstrap e2e on the image.
