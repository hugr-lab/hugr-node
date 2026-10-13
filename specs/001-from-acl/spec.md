# Spec 001: the platform part, from duckdb-acl

- **Status**: implemented
- **Date**: 2026-10-13
- **Author**: the agent, at the owner's ask (duckdb-acl specs 125-127, design/080 variant 2)

## What

The platform part of duckdb-acl - `acl_cluster.cpp` (the cluster profile, node groups' traits, drift, the deployment,
the `cluster` bundle) and its tests - lives here, with its history (extracted from duckdb-acl's tag
`pre-platform-split` by `git filter-repo`: `git log --follow src/acl_cluster.cpp` reaches duckdb-acl's spec 093).

## How it is built

- **One extension, `hugr_node`**: duckdb-acl (a submodule) is built in through its `acl_sources.cmake` with
  `ACL_EMBEDDED` (its entry point is ours); `hugr_node`'s load is `LoadAclCore` (acl's whole load, answering the
  store) then `RegisterAclCluster` - the platform part plugs into acl's registration points (duckdb-acl spec 126). A
  platform node loads `hugr_node` instead of `acl`, never both.
- **Pins** ride duckdb-acl's: the same duckdb (bba7360), extension-ci-tools, and duckdb-acl itself as a submodule.
  duckdb-acl's own `duckdb` submodule is not used - its sources take duckdb from the build's tree.
- **Tests**: `test/sql/cluster/` (moved, `require hugr_node`), `test/sql/hugr_node.test` (the embedded acl answers,
  the platform part is registered), `test/cpp/test_acl_cluster_install.cpp` (`make test-cpp`). The IdP fixtures are
  duckdb-acl's (`test/idp` links to them).
- **CI**: linux build and the tests, without the Flight door (`ACL_NO_FLIGHT=1` - the platform part's tests need
  none); the distribution matrix is a follow-up.
