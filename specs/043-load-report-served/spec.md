# Spec 043: the node serves its load report

- **Status**: implemented
- **Date**: 2026-11-19
- **Author**: the owner (2026-11-19: the console should read a node's report without opening a session)

## Summary

The bootstrap turns `acl_metrics_endpoint` on. The load report (duckdb-acl spec 079) is then served over the Flight
Handshake `node-load`, and `/metrics` is served on the quack listener. Both answer only a bearer whose roles hold
`observe` (spec 097). hugr-platform's console reads a node's report there (its spec 012). The Handshake opens no
session.

## Problem

The console read `platform.node_load` with a statement. A statement runs in a per-call session, and that session's end
counts as a session's end (duckdb-acl spec 175). So each look at a node moved its `sessions.idle_since`: the
autoscaler (hugr-platform spec 011) would never see a node idle while someone had its page open. The Handshake does
not open a session. A node that does not serve the report, though, answers it `NotImplemented`, and no hugr-node
bootstrap turned the endpoint on. The cluster bundle cannot set it either: it is passthrough's
(`acl_cluster_profile.test`).

## Design

- Step 1 (hardening) adds `SET GLOBAL acl_metrics_endpoint = true`. It sits beside the node's other trust settings,
  before the lock (step 7), so nothing sets it afterwards.
- `acl_observe_unauthenticated` stays off. The lock keeps it that way, so the report is never open without a token.
- Nothing else changes. Seeing the report already needs `observe` (a bundle, or passthrough, or an unrestricted
  manage). The front does not expose the Flight Handshake or `/metrics` (hugr-platform spec 002); the console and the
  operator reach the nodes directly.

## Security

The report holds counts, states and sizes. It never names a principal or carries statement text (duckdb-acl spec
079 / 175). Every read is a `door` event `observe_<surface>`, and refusals are floored. A bearer without `observe` is
refused with `Unauthorized`, and one that does not verify with `Unauthenticated`.

## Tests

`test/sql/agent/hugr_node_bootstrap.test`: the plan's hardening step carries the setting in both of its plans.
duckdb-acl's `test/e2e/flight/auth.sh` (its spec 175): with the endpoint on, a Handshake read leaves `idle_since`
where it was.

## Alternatives considered

- **A cluster-profile item.** The cluster bundle may not set it. Passthrough could, but then every installation would
  need a break-glass write to get the console working.
- **`acl_observe_unauthenticated`.** That would let anyone who reaches the port read the report.
