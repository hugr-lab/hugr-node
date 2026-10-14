# Spec 005: acl_otel built in - one telemetry destination for the cluster

- **Status**: implemented
- **Date**: 2026-10-14
- **Author**: the agent, at the owner's ask ("А давай подумаем может имеет смысл включить в hugr-node
  acl-otel?" - "Согласен, думаю 1, и эндпоинт у всего кластера должен быть тоже один")

## Problem

A platform node's audit, metrics, spans and lineage leave the node through acl_otel (hugr-lab/acl-otel). Shipped as a
loadable beside `hugr_node`, it is a second artifact (57 MB, with its own copy of gRPC and protobuf) that every node's
bootstrap must install and load, and its endpoint is set per node - nothing keeps two nodes of one cluster from
sending to two places.

## Design

- **Built in** (option 1 of the owner's three): acl-otel is a submodule (bf9da0f, its #50 and #51), built into `hugr_node`
  through its `acl_otel_sources.cmake` (acl-otel spec 024: `ACL_OTEL_EMBEDDED`, `LoadAclOtelCore`), as duckdb-acl and
  tresor are.
  `LoadInternal` loads it after acl's and tresor's cores; it attaches to both hook registries as the loadable does.
  The OpenTelemetry SDK comes from vcpkg (`opentelemetry-cpp[otlp-http,otlp-grpc]`); its gRPC and protobuf are the
  ports the Flight door already links, so the artifact is 79.6 MB on macOS arm64 with acl_otel in, where acl_otel's
  loadable alone is 57 MB.
- **Inert until configured**: acl_otel's own rule - with `acl_otel_endpoint` empty (and no `OTEL_EXPORTER_OTLP_*` in the
  environment) the sink is the stand-in that drops and counts, and nothing leaves the node; with `acl_otel_lineage`
  empty (and no `OPENLINEAGE_URL`) no lineage lane. Nothing new to turn on or off.
- **One destination for the cluster**: `acl_otel_endpoint`, `acl_otel_protocol`, `acl_otel_insecure`,
  `acl_otel_certificate`, `acl_otel_lineage`, `acl_otel_lineage_endpoint`, `acl_otel_lineage_secret` are profile items
  of the cluster's scope only - `CLUSTER SET … IN GROUP g` of one is refused (`ClusterSetting`,
  `CLUSTER_WIDE_SETTINGS`). A `RESET … IN GROUP` stays a plain write (nothing to remove: "not in the profile"). The
  rest of acl_otel's settings (batching, intervals, series, rules) may be a group's.
- **Who sets it**: unchanged - an endpoint is outside the `cluster` bundle's allowlist (`ClusterBundleMaySet`: a data
  path, a sink), so it is `passthrough`'s. The class is `restart`, as every setting's: the node agent rolls it out.

## Enforcement / security

- acl_otel observes and never decides; built in, its surface (`acl_otel_*`) is under the function gate's never set
  (`acl_*`), refused to a principal - tested.
- No credential in the profile: the lineage key is a secret of the secrets service, named by
  `acl_otel_lineage_secret`; OTLP headers come from the environment only (acl_otel has no headers setting).
- One duckdb-ext-common: acl-otel's configure compares its contracts and hooks with duckdb-acl's and tresor's copies
  and refuses a difference (acl-otel spec 024).
- Not enforced: a node's environment (`OTEL_EXPORTER_OTLP_ENDPOINT`, `OPENLINEAGE_URL`) and its bootstrap's own
  `SET GLOBAL` are the deployment's, as `acl_node_group` is; the profile holds the cluster's one destination, and
  `ACL NATIVE` stays the break-glass.

## Testing

- `test/sql/hugr_node.test`: acl_otel's version is acl-otel's own, no acl_otel loaded beside, attached to the built-in
  acl's registry with the stand-in exporter (no endpoint), its status refused to a principal.
- `test/sql/cluster/acl_otel_cluster.test`: each destination setting refused `IN GROUP` (the name's case changes
  nothing), written for the whole cluster (restart class, nothing applied here), refused to the `cluster` bundle and
  written by `passthrough`, a non-destination setting written for a group. Fails without the rule (checked: the first
  refusal is not raised).
- `test/e2e/otel/run.sh` (in `make test-e2e`, CI): a fake OTLP/HTTP receiver and one node - with no endpoint nothing is
  sent; with one, the decision records arrive at `/v1/logs` naming the principal's role and the metrics at
  `/v1/metrics`; the endpoint emptied on the same node, another role's statements send nothing.

## Alternatives

- **A loadable beside `hugr_node`** (option 2): the second artifact and gRPC copy, the bootstrap that must load it,
  and a `LOAD` that a locked configuration has to allow - refused by the owner.
- **No telemetry in the platform node** (option 3): the cluster's audit then lives only in each node's ring.
- **The destination per group**: two groups of one cluster could send its audit to two places; the owner's rule is one.
