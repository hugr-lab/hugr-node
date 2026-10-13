# Spec 002: tresor built in, and the Linux distribution

- **Status**: draft
- **Date**: 2026-10-13
- **Author**: the agent, from the owner's decisions of 2026-10-13

## Owner's decisions

1. **hugr_node always includes tresor.** A platform node's secrets live in the secrets service and reach every node
   through tresor - there is no other right way to keep them in step across nodes; a node without tresor would have
   to sync secrets itself.
2. **hugr_node ships for Linux only: amd64 and arm64.** No macOS, Windows or WASM artifacts (duckdb-acl, the open
   node, keeps its full platform matrix).

## Design

- **tresor built in, as acl is**: tresor (MIT, a submodule) gains the embedding hooks duckdb-acl spec 127 gave acl -
  `tresor_sources.cmake` (sources by its root, duckdb's paths from the build's tree, `tresor_configure_targets`) and
  `LoadTresorCore(loader)` under `TRESOR_EMBEDDED` (its entry point, extension class and `DUCKDB_EXTENSION_MAIN`
  not compiled). hugr_node's load: `LoadAclCore`, `LoadTresorCore`, then the platform part. Each keeps the OIDC core
  under its own namespace (`acl`, `tresor` - duckdb-ext-common charter R13), so the two copies never collide.
- **The node's deployment**: with tresor always present, a hugr_node node is a cluster node by construction - the
  secrets-only-in-the-service rule (spec 118's `acl_deployment = cluster`) becomes the node's default; whether the
  setting stays as an override is a question for the owner.
- **Pins**: tresor's submodule follows the same duckdb pin as duckdb-acl (both at bba7360 today) and the same
  duckdb-ext-common tag.
- **Distribution**: a `distribution.yml` built on extension-ci-tools for `linux_amd64` and `linux_arm64` only
  (the duckdb-acl workflow's linux legs, with vcpkg for the Flight door's Arrow); artifacts published to the hugr
  extension repository (duckdb-extension-repository) as `hugr_node`.

## Tests

- hugr-node CI: tresor's surface answers in hugr_node (its functions registered, `ATTACH ... (TYPE tresor)` refused
  without a server, as tresor's own tests do), the existing cluster tests unchanged.
- The distribution build on both Linux legs: build, the extension loads, the suite.
- **e2e - tresor inside an acl cluster** (owner, 2026-10-13): a real tresor-server built from its repository with the
  SQLite state store (`state: {kind: sqlite}`, a local KEK), Keycloak in docker as the IdP (as tresor-server's
  conformance run does), and two hugr_node nodes sharing one policy catalog. The scenarios:
  1. **A secret written once reaches every node**: a principal's `CREATE PERSISTENT SECRET` on node A lands in the
     service (spec 082); node B reads it - no sync of its own.
  2. **The cluster profile's sources use the service's secrets**: `ACL CLUSTER ATTACH … (TYPE postgres, SECRET s)` on
     node A; node B applies the item and attaches with the same secret.
  3. **Rotation reaches the fleet**: the secret rotated in the service; both nodes' next connects use the new value.
  4. **An issuer's keys from the service** (spec 095 `FROM SECRET`): a token verifies on both nodes; the secret
     removed fails that issuer closed on both.
  5. **No local secrets on a platform node**: a principal's `CREATE SECRET` without a service is refused (spec 118).
  6. The Flight door's node-group refusal kept from duckdb-acl (`test/e2e/flight_node_group.block.sh`), wired here.

## Open questions

- `acl_deployment` on hugr_node: fixed `cluster`, or a default that can still be set `standalone`?
- ~~Which of tresor's own tests run in hugr-node~~ - the scenarios of tresor inside the cluster (above), against a
  real tresor-server on SQLite (owner, 2026-10-13); tresor's own suite stays tresor's.
