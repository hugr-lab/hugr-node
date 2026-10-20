# Spec 014: re-pin duckdb-acl - the doors take a renewed certificate

- **Status**: implemented
- **Date**: 2026-10-20

hugr-platform spec 005 (the owner's decision 6) has cert-manager's csi-driver issue each node pod its own door
certificate, with the key made in the pod and never in etcd, renewed in place. duckdb-acl spec 140 makes a door take
the renewed pair. hugr_node builds duckdb-acl in, so the node does from this pin on:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb-acl | d3f261e | b6f15d4 | spec 066's addendum (#248): a statement a cookie-less client's DoGet runs names the DoGet's own session - the live one - to its `AclConnection`, so `acl_drain()`, the load report and `platform.sessions` leave the call's own session out in either order, and a secrets service acting for the session sees a live one |
| duckdb-acl | b6f15d4 | c5c87b7 | spec 140 (#247): a door served from local files re-reads them every `acl_door_certificate_refresh` seconds (60); a renewed pair that checks serves the new connections - Flight through gRPC's certificate-config fetcher, quack through httplib's in-place update; `acl_node_load()` gains `certificates` |
| duckdb-acl | c5c87b7 | 8d0c1d9 | spec 140 (#249): a Flight door that cannot watch its files serves as before, reading its pair once, and says `anchor_unavailable` |

duckdb (f8f3f4d), tresor and acl-otel stay: no duckdb-ext-common contract changes, and duckdb-acl's pin of duckdb is
the same.

- **The node is watched by default.** The agent serves both doors from the files the operator mounts
  (`HUGR_FLIGHT_CERT` / `HUGR_QUACK_CERT` = `/etc/hugr/tls/tls.crt`, the key beside it): local paths, so they are
  watched. The csi-driver's renewal swaps `..data` in one rename, and the door takes the new pair a minute later at
  most. Open connections carry on.
- **The heartbeat carries `certificates`** as it is: the agent sends `acl_node_load()` whole as its `load`
  (`HeartbeatBody`).
- **The anchor** is a private directory under `/tmp`, writable in the node's pod (no read-only root). A pod that had
  none would serve its doors as before, and the load report would say so.
- **Nothing in `src/` changes.**

## Testing

- duckdb-acl's own: `test/e2e/flight/certificate_reload.sh` (both doors, a swap, a broken pair, the fallback) and
  `drain.sh` (the call's own session, 20 of 20 runs).
- The csi-driver on hugr_node: hugr-platform's demo through its chart (spec 005), whose groups get `tls.csi` from the
  internal issuer. Check 2 finds no Secret on a node.
- Locally: the build, `test/sql/*`, `make test-cpp`; on the PR: CI and the release's dry run.
