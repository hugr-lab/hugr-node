# Spec 016: re-pin duckdb to the v2.0.0 tag - the node is v2.0.0

- **Status**: implemented
- **Date**: 2026-10-21
- **Author**: the agent, at the owner's ask ("да можно сделать перепин всю семью")

DuckDB v2.0.0 was released on 2026-10-21. hugr_node builds duckdb-acl, tresor and acl-otel in, so the four move the same
day:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb | f8f3f4d | d54cbbe | the v2.0.0 tag (duckdb-acl spec 141, #250) |
| duckdb-acl | 8d0c1d9 | 41ec046 | spec 141: the tag; quack f96420e, ducklake 417a911 - the release's own pins |
| tresor | d7f2a1e | 036180c | its spec 038 (#67), `ACL_COMMIT` 41ec046 |
| acl-otel | 8542cad | 3916552 | its spec 027 (#56), `ACL_BASE` 41ec046 |
| extension-ci-tools | 39ffc46 | fe955ed | main's head, as duckdb-acl's |

- **The node is v2.0.0, not a commit.** duckdb 2.0 no longer reads `git describe`. A build is `v2.0.0-dev<commits>`
  and names its extensions' folder by the commit, unless the version is given (duckdb-acl spec 141). The Makefile
  gives it: `DUCKDB_VERSION ?= v2.0.0`, bumped with the pin.
  - The node's duckdb, its release's extensions and the image label `dev.hugr.duckdb.version` are then v2.0.0.
  - A node installs from a repository that serves v2.0.0. kista can mirror DuckDB's own repositories for it, which
    publish nothing for a development build.
  - Locally, CI and in the release build alike: the release used `make` without it and shipped `f8f3f4dcd9`.
- **`scripts/publish_to_kista.sh` takes a release.** A `vX.Y.Z` version is kista's `release` kind, a commit's its
  `dev` kind. It used to refuse a release ("register it with its C API version by hand"). A CPP build is served under
  its exact version, so it needs no C API maximum.
- **The distribution builds the tag**: `duckdb_version: v2.0.0`.
- **The hugr type's copy of quack's client** (spec 011) follows duckdb-acl's quack, f96420e. duckdb applies no patches
  to quack since v2.0.0, so `src/hugr_type/sync.py` copies it as it is (with ours), as duckdb-acl's sync does. It
  refused a pin without duckdb patches before - found by this PR's CI. The copied files are unchanged but their
  banner.
- **The release's extensions** follow duckdb-acl's pins (spec 008): postgres_scanner c0f5a3c, ducklake 417a911, mssql
  7e1bb1c (until its own re-pin, #511, merges).
- **Nothing in `src/` changes.**

## Testing

On the new pins (macOS), `HUGR_NODE_E2E=1 HUGR_NODE_RELEASE=1`:

- the build, and the version it names;
- `test/sql/*`, `make test-cpp`;
- `make test-e2e`: tresor in the cluster, acl_otel in hugr_node, a node bootstraps itself, the federation.

Run on macOS, all green:

- the build: `library_version` v2.0.0 at d54cbbe3df; the release's extensions' footers name v2.0.0 (postgres_scanner
  c0f5a3c, CPP);
- `test/sql/*`: 982 assertions in 13 files; test-cpp: 4;
- test-e2e, all PASS: tresor in the cluster, acl_otel in hugr_node, a node bootstraps itself (its postgres_scanner
  published to kista as `v2.0.0`, the release kind), the hugr type across processes and its identity through tresor.

On the PR: CI, the release's dry run (the image's e2e, now with a v2.0.0 channel) and the distribution build.

## Addendum (2026-10-21): tresor at its first release

tresor tagged v0.1.0 at 550791c: 036180c and docs only (protocol version 1, the CHANGELOG, the release's wording), its
pins unchanged (duckdb v2.0.0, ext-common v0.13.0, `ACL_COMMIT` 41ec046). hugr_node pins the tag: 036180c → 550791c.
