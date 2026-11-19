# Spec 045: the orchestrator's scale-down drains a node

- **Status**: implemented
- **Date**: 2026-11-19
- **Author**: the owner (2026-11-19: a node that is to go stops taking new connections through a long drain, and is
  resumed if the load grows), hugr-platform spec 011

## Summary

hugr-platform's autoscaler (its spec 011) scales an auto group down by draining one node. It picks the node when the
group's load fits in one node fewer. The node's sessions then end by themselves, and once it has been idle the node
stops. If the load grows back, it is resumed. The orchestrator says so in its answer to the node's heartbeat:
`{"drain": "scale-down"}`. The node's tick drains the node (`acl_drain()`, duckdb-acl spec 066) and resumes it when the
answer no longer says so.

## Problem

The front spreads new connections over a group's nodes. Under steady traffic no node is ever idle, so a group that
grew for a peak never shrinks back. A node marked for the scale-down must take no new connections and keep the ones it
has.

## Design

- The heartbeat's answer: 204 as before (nothing to do), or 200 with a JSON object. `"drain": "scale-down"` is the
  orchestrator's order. A heartbeat the orchestrator did not answer keeps its last word, and an answer the node does
  not read is no order.
- `DecideGroup` gains the scale-down as a second reason to drain, beside the resource group the policy lacks. The
  tick drains the node when either holds and it is not draining. It resumes only a drain it made itself, once
  neither holds. An operator's `DRAIN NODE` is never taken over, nor resumed.
- The log says why the node drained and why it resumed.
- A drained node refuses new sessions (Flight UNAVAILABLE "draining", quack discovery 503) and says `draining` in its
  load report. The front seats elsewhere. Established sessions keep working, and the node never times them out
  itself.

## Security

The order comes only in the answer to the node's own heartbeat. That is the orchestrator's TLS endpoint, which the
node verifies (`ca_cert_file`), authenticated by the pod's projected token. It drains or resumes this node and nothing
else.

## Tests

- `test/cpp/test_hugr_node_tick.cpp`:
  - the scale-down drains, and its end resumes;
  - an operator's drain is neither taken over nor resumed;
  - the scale-down's end while the group is gone keeps the node drained, and so does the group's return while the
    node is scaled down;
  - a node in no group is scaled down all the same.
- `test/e2e/bootstrap/run.sh` (14b): the fake orchestrator answers node a's heartbeat with the scale-down. Node a
  drains and node b does not; when the answer stops, node a resumes.
