# Spec 007: re-pin duckdb to 7304f7b - with duckdb-acl, tresor and acl-otel

- **Status**: implemented
- **Date**: 2026-10-15

hugr_node builds duckdb-acl, tresor and acl-otel in, so the four move the same day:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb | f702a1f | 7304f7b | the head of `v2.0-cyanoptera` on 2026-10-15 (duckdb-acl spec 131, #231) |
| duckdb-acl | 1714d05 | ce1d417 | spec 131 (#231) and spec 130 (#230): duckdb-ext-common v0.13.0, ca_cert_file for the OIDC core's own requests |
| tresor | d5fca88 | 059190a | specs 030 (ca_cert_file for its built-in client), 031 (the re-pin) and 032 (a served secret read through its type's deserializer); `ACL_COMMIT` ce1d417 |
| acl-otel | bf9da0f | 15b0eb2 | its spec 025 (#52): the re-pin, `ACL_BASE` ce1d417 |

- **The shared-header check** compares duckdb-acl's and tresor's copies of duckdb-ext-common. Both are v0.13.0, so the
  check holds.
- **The trust setting stays `tresor_http_client = 'duckdb'`** (owner, 2026-10-15). tresor's built-in client now trusts
  ca_cert_file beside the system's roots, but the node keeps one semantics for all its traffic: `HUGR_CA_FILE`
  replaces the roots, and public roots go into the bundle.
- **Nothing in `src/` changes.**
- **Found at the bump**: `make test-cpp` kept a test binary compiled against the old duckdb's headers, because the rule
  depended only on the `.cpp`, and it aborted on the new libduckdb. The built library is now a prerequisite of
  every test binary.

## Testing

On the new pins (macOS), `HUGR_NODE_E2E=1`:

- the build;
- `test/sql/*`: 12 files, 616 assertions;
- `make test-cpp`;
- `make test-e2e`: tresor in the cluster, acl_otel in hugr_node, a node bootstraps itself - all PASS.

CI on the PR; the distribution build (linux_amd64, linux_arm64) dispatched on it.
