# Spec 003: re-pin duckdb to f702a1f - with duckdb-acl and tresor

- **Status**: implemented
- **Date**: 2026-10-14

hugr_node builds duckdb-acl and tresor in, so the three move the same day: duckdb bba7360 → f702a1f (duckdb-acl's
spec 128, #227), duckdb-acl → 1714d05 (that PR's merge), tresor → d5fca88 (its spec 028 re-pin and its `ACL_COMMIT`
on 1714d05). Both still carry duckdb-ext-common 21a695d, so tresor's shared-header check holds.

- Why now: hugr-node's first distribution build (on main, 59ace1a) failed - it compiles the head of
  `v2.0-cyanoptera`, where duckdb #27108 changed `StreamsByDefault` under duckdb-acl's Flight door. The fix is
  duckdb-acl's (spec 128); nothing in this repository's `src/` changes.
- hugr_node's own build carries neither ducklake nor the scanners, so duckdb-acl's ducklake patch (spec 128) does not
  apply here.

## Testing

On the new pins (macOS): the build (no warnings), `test/sql/*` (10 files, 487 assertions), `make test-cpp` (its
install case SKIPs without a postgres_scanner loadable), `make test-e2e` (tresor inside the cluster: PASS). CI on the
PR, the distribution build (linux_amd64, linux_arm64) dispatched on it.
