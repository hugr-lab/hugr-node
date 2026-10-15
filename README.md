# hugr-node

The hugr platform's part of a node, on top of [duckdb-acl](https://github.com/hugr-lab/duckdb-acl):

- the **cluster profile** - the shared part of every node's bootstrap (extensions, sources, settings), kept in the
  policy catalog as desired state (`ACL CLUSTER …`);
- **node groups** - a resource group as a set of nodes, and placement of principals onto them;
- **drift** - what a node carries against its profile;
- the **`cluster`** admin bundle; a hugr_node node is a cluster node by construction (a cluster may be one node) and
  keeps secrets only in its secrets service - tresor, built in;
- **the node process** - `duckdb -c "CALL hugr_node_run()"` bootstraps the node from its pod's environment and the
  control plane, applies the profile, serves and stops on SIGTERM; no key or secret in the image, env or profile
  (spec 006, [docs/node.md](docs/node.md));
- **OpenTelemetry** - acl_otel built in (spec 005): the audit, metrics, spans and lineage of acl and tresor exported
  over OTLP and OpenLineage, nothing until an endpoint is set; the destination is one for the whole cluster (a cluster
  profile item, never a group's);
- later: the node agent that converges a fleet with the platform's control plane.

duckdb-acl is the open node's access control (MIT), tresor its client of the secrets service (MIT), acl-otel its
OpenTelemetry export (BUSL). This repository builds one extension, `hugr_node`, from the three and its own part, which plugs into acl through the registration points
of duckdb-acl's spec 126 (design: duckdb-acl `design/080`). A platform node loads `hugr_node` instead of `acl`, `tresor`
and `acl_otel`, never beside them. Distributed for Linux amd64 and arm64 only (spec 002); it builds anywhere duckdb-acl
does, for development.

> **Status: in progress** - the code moved here from duckdb-acl with its history (spec 001; duckdb-acl spec 127).

## Build

```sh
git submodule update --init
git -C duckdb-acl submodule update --init third_party/quack duckdb-ext-common
git -C tresor submodule update --init duckdb-ext-common
git -C acl-otel submodule update --init duckdb-ext-common
make vcpkg-setup                    # once (or VCPKG_TOOLCHAIN_PATH=<an existing vcpkg>/scripts/buildsystems/vcpkg.cmake)
GEN=ninja make                      # the hugr_node extension (acl, tresor and acl_otel built in) and its test binary
HUGR_NODE_DEV=1 GEN=ninja make      # + json, autocomplete and httpfs linked in: the CLI acl-clients' dev node runs
                                    #   (ACL_CLUSTER=1 dev/node.sh - an IdP's discovery is read through httpfs; spec 004)
build/release/test/unittest 'test/sql/*'
make test-cpp
make test-e2e                       # tresor inside the cluster (docker, go), acl_otel's export (python), the bootstrap
                                    #   (docker, go; HUGR_NODE_E2E=1 GEN=ninja make first, for its postgres_scanner)
```

vcpkg is part of every build: the built-in acl's Flight door takes Arrow from it, the built-in tresor OpenSSL, the
built-in acl_otel the OpenTelemetry SDK (its gRPC is the Flight door's), the linked azure extension the Azure SDK.
httpfs and azure are linked into the node's duckdb until duckdb 2.0 (spec 006). duckdb-acl, tresor and acl-otel must carry the
same duckdb-ext-common (tresor's and acl-otel's configure refuse otherwise).

## License

Business Source License 1.1 - see [LICENSE](LICENSE). Production use is permitted except as a hosted or managed
service offered to third parties; each version becomes Apache-2.0 four years after its release. The parameters
(Additional Use Grant, Change Date, Change License) are the licensor's and may be revised before the first release.
