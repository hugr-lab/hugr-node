# Spec 004: the dev build - a hugr_node for the client tools' dev node

- **Status**: implemented
- **Date**: 2026-10-14
- **Author**: the agent, at the owner's ask ("давай чинить дальше режим в dev/node.sh")

## Problem

acl-clients' `ACL_CLUSTER=1 dev/node.sh` serves a cluster node for the DBeaver plugin's node and cluster folders
(acl-clients 008.3). It ran duckdb-acl's build with `SET GLOBAL acl_deployment = 'cluster'`; since duckdb-acl spec
127 the cluster part is hugr_node's, and since this repository's spec 002 there is no `acl_deployment` (a hugr_node
node is a cluster node by construction) - the mode no longer starts. It needs hugr_node's own CLI, and that CLI
cannot read the dev IdP's discovery: the node reads keys through duckdb's filesystem, and this build links no httpfs.

## Design

- `HUGR_NODE_DEV=1` (an environment flag at configure, like duckdb-acl's `ACL_QUACK`): json, autocomplete and httpfs
  statically linked into the build's CLI and test binary - from the submodule's own pins, as duckdb-acl does. Off by
  default, in CI and in the distribution build: the artifact stays hugr_node alone.
- acl-clients' `dev/node.sh` runs `ACL_CLUSTER=1` on this build (`HUGR_REPO`, default `../hugr-node`) and no longer
  sets `acl_deployment`.

## Testing

`HUGR_NODE_DEV=1 GEN=ninja make`, then `ACL_CLUSTER=1 dev/node.sh` (acl-clients) comes up and answers the cluster
part (`acl_cluster_version()`, `platform.console_info()` says `cluster`); the suite as before.
