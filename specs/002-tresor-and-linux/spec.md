# Spec 002: tresor built in, and the Linux distribution

- **Status**: implemented - part 1 (tresor built in, the deployment setting gone, CI, the Linux distribution) and
  part 2 (the e2e of tresor inside the cluster); scenarios 2 and 6 are follow-ups (below)
- **Date**: 2026-10-13
- **Author**: the agent, from the owner's decisions of 2026-10-13

## Owner's decisions

1. **hugr_node always includes tresor.** A platform node's secrets live in the secrets service and reach every node
   through tresor - there is no other right way to keep them in step across nodes; a node without tresor would have
   to sync secrets itself.
2. **hugr_node ships for Linux only: amd64 and arm64.** No macOS, Windows or WASM artifacts (duckdb-acl, the open
   node, keeps its full platform matrix).
3. **No single-node mode** (2026-10-13): a hugr_node node is a cluster node, whatever the number of nodes - a
   cluster may be one. What differs between one node and many is where the policy catalog and tresor's state live
   (a local file / SQLite or a shared database) - the deployment's composition, not a mode of the node.

## Design

- **tresor built in, as acl is**: tresor (MIT, a submodule) gains the embedding hooks duckdb-acl spec 127 gave acl -
  `tresor_sources.cmake` (sources by its root, duckdb's paths from the build's tree, `tresor_configure_targets`) and
  `LoadTresorCore(loader)` under `TRESOR_EMBEDDED` (its entry point, extension class and `DUCKDB_EXTENSION_MAIN`
  not compiled). hugr_node's load: `LoadAclCore`, `LoadTresorCore`, then the platform part. Each keeps the OIDC core
  under its own namespace (`acl`, `tresor` - duckdb-ext-common charter R13), so the two copies never collide.
- **The node's deployment - the setting goes**: `acl_deployment` is removed from hugr_node; the `cluster_node` trait
  is always true. The build is the deployment: duckdb-acl (the open node) registers no trait and is standalone by
  construction (local secrets when no service is attached, spec 118), hugr_node is a cluster node - the profile,
  drift, the `cluster` bundle, secrets only in the service. A switch would only re-open local secrets in a node's
  files on a platform node. `RequireClusterDeployment` and its refusal go; `console_info.deployment` stays (it says
  the build: `standalone` / `cluster`), so the plugin reads it unchanged. A one-node platform = hugr_node +
  tresor-server (SQLite) [+ the orchestrator]; without a service attached the node starts and a principal's
  `CREATE SECRET` is refused ("no secrets service").
- **Pins**: tresor's submodule follows the same duckdb pin as duckdb-acl (both at bba7360 today) and the same
  duckdb-ext-common tag.
- **Distribution**: a `distribution.yml` built on extension-ci-tools for `linux_amd64` and `linux_arm64` only
  (the duckdb-acl workflow's linux legs, with vcpkg for the Flight door's Arrow); artifacts published to the hugr
  extension repository (duckdb-extension-repository) as `hugr_node`.

## Tests

- The cluster tests stop setting `acl_deployment`; the standalone-refusal cases go; `hugr_node.test` checks that
  `console_info` says `cluster` with no setting and that `SET GLOBAL acl_deployment` is unknown.
- hugr-node CI: tresor's surface answers in hugr_node (its functions registered, `ATTACH ... (TYPE tresor)` refused
  without a server, as tresor's own tests do), the existing cluster tests unchanged.
- The distribution build on both Linux legs: build, the extension loads, the suite.
- **e2e - tresor inside an acl cluster** (owner, 2026-10-13): a real tresor-server built from its repository with the
  SQLite state store (`state: {kind: sqlite}`, a local KEK), Keycloak in docker as the IdP (as tresor-server's
  conformance run does), and two hugr_node nodes (as built: each its own policy catalog from one bootstrap - what they
  share is the service). The scenarios:
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

- ~~`acl_deployment` on hugr_node~~ - removed: no single-node mode (owner, 2026-10-13; Design above).
- ~~Which of tresor's own tests run in hugr-node~~ - the scenarios of tresor inside the cluster (above), against a
  real tresor-server on SQLite (owner, 2026-10-13); tresor's own suite stays tresor's.

## As built - part 1 (2026-10-14)

- **tresor built in** at tresor main e7c7c65 (its spec 024, #43): `set(TRESOR_EMBEDDED ON)` and
  `include(tresor/tresor_sources.cmake)` after acl's file, `${TRESOR_SOURCES}` among the extension's sources,
  `tresor_configure_targets(...)`; the entry is `LoadAclCore`, `LoadTresorCore`, `RegisterAclCluster`. The image
  exports one symbol (`hugr_node_duckdb_cpp_init`) and carries two copies of the OIDC core, `duckdb::acl::oidc` and
  `duckdb::tresor::oidc`, and tresor's keychain. Both submodules are on duckdb-ext-common 21a695d (v0.12.0) - the
  configure refuses otherwise. acl's target-wide `DUCKDB_EXT_COMMON_OIDC_NAMESPACE=acl` meets tresor's source-level
  `=tresor` on tresor's TUs: tresor's wins, with a `-Wmacro-redefined` warning per tresor TU (tresor spec 024's
  follow-up for duckdb-acl; hugr_node does not build with `-Werror`).
- duckdb-acl at its main d77d11e (spec 127 PR B: the platform part is only here now - the `REMOVE_ITEM` is gone).
- **The deployment setting goes**: `acl_deployment`, `RequireClusterDeployment` and drift's standalone refusal are
  removed; the `cluster_node` trait answers true; drift's bootstrap rows list `acl_node_group` alone.
- **vcpkg in every build**: `openssl` beside Arrow in `vcpkg.json`; the Makefile always takes the toolchain
  (`make vcpkg-setup` added); `ACL_NO_FLIGHT` is no longer this repository's (the platform node is one configuration).
- **CI**: the linux job builds that configuration (Flight and tresor), its vcpkg packages from the family's R2 cache
  (`vars.VCPKG_BINARY_SOURCES` + the `VCPKG_CACHING_AWS_*` secrets, duckdb's public read-only cache without them).
- **Distribution** (`distribution.yml`): duckdb-acl's reusable workflow and inputs, `exclude_archs` leaving
  `linux_amd64` and `linux_arm64` (the musl legs and windows_arm64 are opt-in, so not built), the R2 cache. Publishing
  to the hugr extension repository is a follow-up for when that repository takes uploads; until then the artifacts
  are the workflow's.
- Tests: `hugr_node.test` - autoloading off, `console_info.deployment` = `cluster` with nothing set, tresor's version,
  neither `acl` nor `tresor` loaded beside, both ATTACH spellings reaching tresor's discovery, the `tresor` secret
  type, a tresor setting; `node_cluster.test` - `acl_deployment` is an unknown setting, the cluster bundle is offered
  all of the profile, a principal's `CREATE SECRET` without a service is refused; the other cluster tests no longer
  set the deployment. The loadable, copied out of the tree, loads into a duckdb CLI that carries neither acl nor tresor
  (tresor's test shell at the same pin): `acl_drain_status()` serving, 269 `acl_*` functions, the override STRICT.
- Gate (macOS): the build, `test/sql/*` (10 files, 487 assertions), `make test-cpp` (its install case SKIPs without a
  postgres_scanner loadable, as before), yamllint, clang-format.

## As built - part 2: tresor inside the cluster, end to end (2026-10-14)

`test/e2e/tresor/run.sh` (`make test-e2e`; CI runs it after the suites and asserts it passed - on a developer's
machine a missing tool is a SKIP):

- **The service**: tresor-server built from its repository (`TRESOR_SERVER_DIR`, else its main is cloned; CI checks
  out its main), state in SQLite with every row's MAC checked, sealed under a local KEK made for the run; Keycloak from
  tresor's own test compose (`tresor/server/docker-compose.yml`, the realm tresor's suites use, a compose project of
  its own); tresor-server's `testdata/keycloak/server.yaml` (etl and `role:secrets_admin` are service admins,
  `client:acl-node` an actor) on this run's ports.
- **The operator** seeds the service once, as etl: an `oidc_issuer` secret `kc_conn` (Keycloak's URL and its JWKS
  pasted as `KEYS`), granted `use` to `role:nodes` - the nodes' service account.
- **Two nodes side by side**: two hugr_node CLI processes, each driven through its stdin, each logged in to the service
  as `acl-node` (`ATTACH 'tresor:…' AS node`), each with its own in-memory policy catalog from the same bootstrap -
  `CREATE ISSUER kc FROM SECRET kc_conn IN node`, `node.act_for_sessions()`, a catalog with a `secrets` grant. What
  they share is the service and nothing else.
- Checked:
  1. **the issuer's keys from the service** - a person's token (alice, through acl-door) opens a session on both
     nodes;
  2. **a secret written once reaches every node** - under bob's session (a service admin; node A acts for him) node A
     writes `CREATE PERSISTENT SECRET shared_s (TYPE http, …)` and `ACL GRANT SECRET shared_s TO ROLE nodes`; node B,
     which never had it, finds it (`which_secret`) within tresor's 30 s list refresh and reads its material
     (`tresor_secret_param`);
  3. **rotation reaches the running fleet** - `CREATE OR REPLACE` through A; the running B reads the new material
     within 45 s, no restart;
  4. **at rest** - the SQLite database (and its WAL) holds no copy of the secret's bearer token;
  5. **fail closed** - the operator drops `kc_conn` from the service; on both nodes, after `acl_jwks_refresh('kc')`,
     a fresh token opens no session.
- **Not here yet**: scenario 2 of the design (a cluster profile source with the service's secret) needs a scanner that
  takes a `SECRET` (postgres / mysql / mssql), which hugr-node's build does not carry - with the integration databases,
  later; scenario 6 (the Flight door's node-group refusal, `test/e2e/flight_node_group.block.sh`) needs a Flight e2e
  harness here (duckdb-acl's `test/e2e/flight/run.sh` drives its own build). A principal's secret refused without a
  service (scenario 5) is `node_cluster.test`'s.
