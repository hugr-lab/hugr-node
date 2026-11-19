# Spec 044: duckdb-acl after spec 175, tresor after its spec 047

- **Status**: implemented
- **Date**: 2026-11-19

duckdb-acl moves from 95fd2a3 (spec 042) to e72cc67:
- spec 173's owner decisions (#310);
- spec 174 (#311): a Flight ingest whose client goes silent lets its session go. `acl_flight_ingest_idle` is 300 s,
  and a killed session ends the waiting load.
- spec 175 (#312, #314): the load report's `memory`. That is `used`, `working` (used less the cache the engine
  drops), `limit`, `temp_used` / `temp_limit` and `critical`. Past `acl_node_admit_memory` (0.95) of either limit,
  the admit flags are false. The report also gains `sessions.idle_since`, and `platform.node_load` has the new
  columns. hugr-platform's autoscaler (its spec 011) decides by them. The heartbeat carries the report as it is.
- the boost-locale overlay port (#313). It concerns duckdb-acl's macOS and Windows distribution only; this
  repository builds Linux.

tresor moves from 8cdd8c3 to 9958f06, its spec 047 (#91, #92): `TOKEN_FILE` on `FLOW 'token'`, a bearer token read
from a rotated file, with a JWT-SVID named by `SPIFFE_ID`. That gives SPIFFE path B, where the node's SVID goes to
tresor-server directly without an IdP. Nothing in this repository uses it yet.

- **Cluster profile.** The two new acl settings are tuning, so the `cluster` bundle may set them:
  `acl_flight_ingest_idle` beside `acl_flight_stream_idle`, and `acl_node_admit_memory`. Before this spec they were
  passthrough's, as any setting outside the allowlist is.
- **Schema.** No step: v28 / `min_reader` 28, as in spec 042.
- **Sources.** No translation unit is added or removed in either. ext-common stays v0.16.0 in both, and duckdb stays
  v2.0.0.

## Tests

- `test/sql/cluster/acl_cluster_profile.test`: the cluster bundle sets both new settings.
- `test/sql/cluster/acl_cluster_profile.test` fails without the allowlist change.
- `HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1049 assertions in 14 files) and `make test-cpp` (4) all pass.
- The bootstrap e2e passes.
