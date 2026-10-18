# Spec 010: the node in an orchestrated cluster - the entry, the tick, the heartbeat

- **Status**: accepted (the owner, 2026-10-18)
- **Date**: 2026-10-18
- **Author**: Claude (with the owner). The node's side of hugr-platform spec 001 (the operator), settled the same day

## Summary

hugr-platform spec 001 makes the orchestrator a Kubernetes operator. It deploys a group's pods, answers a node's entry
with a bootstrap document, and records each node's heartbeat in a Lease. This spec is the node's side of that.

1. **The entry, orchestrator first.** The pod's environment holds the orchestrator's address and two projected
   ServiceAccount tokens, and nothing else of the platform. The node fetches document v2 with the first token. The
   document names the group, the node's label, tresor and the node's identity in the IdP, the repository and the
   catalog. With the second token the node then logs in to tresor. From there on spec 006's order runs unchanged.
2. **The engine's defaults from the pod.** Memory, threads and the spill volume follow from the pod's own resources.
   A profile item wins, but never above the pod.
3. **The tick.** Every interval the node:
   - applies the profile's **hot** items it does not have yet;
   - reports **restart** and **drain** items as pending;
   - drains itself when its resource group is gone;
   - sends the heartbeat.
4. **The label.** `acl_node_label` (duckdb-acl spec 134) is set from the document, so the node's sessions carry its
   name for the front.

Without an orchestrator, spec 006's entry stays as it is: tresor first, then a document v1 from `HUGR_CONTROL_PLANE`.

## Problem

- **Only the writing node applies a hot item.** Spec 006 left it this way: "only the node that writes a hot item
  applies it". The other nodes of its scope take a new source or extension at their next start only, and nothing
  reports what a node has applied.
- **The entry asks tresor first for a token to the control plane.** In an orchestrated cluster the pod's own
  ServiceAccount token is that proof, checked by TokenReview (spec 001). The tresor login and the node's IdP identity
  then come from the document, so they leave the pod's environment.
- **The engine does not know the pod.**
  - DuckDB sizes its memory limit and thread count from the machine, and spills to its temp directory up to 90% of the
    free disk.
  - In a pod, the first two can exceed the container's limits and get it OOM-killed.
  - The third fills the node's ephemeral storage, so kubelet evicts the pod and every session on it dies.
- **A node whose resource group is dropped keeps running.** It already refuses new sessions (duckdb-acl spec 096), but
  nothing says it is draining, so the front cannot tell.
- **Nothing sends the heartbeat** the operator records.

## Design

### The entry (with `HUGR_ORCHESTRATOR`)

**The environment.** No value in it is a credential.

| variable | what |
| --- | --- |
| `HUGR_ORCHESTRATOR` | the operator's in-cluster base URL (https) |
| `HUGR_ORCHESTRATOR_TOKEN_FILE` | the projected ServiceAccount token, audience `hugr-platform`: the entry and the heartbeat |
| `HUGR_IDENTITY_ASSERTION_FILE` | the projected ServiceAccount token with the IdP's federation audience: tresor's FLOW `federated` (spec 006) |
| `HUGR_FLIGHT`, `HUGR_FLIGHT_CERT`, `HUGR_FLIGHT_KEY`, `HUGR_QUACK`, … | the doors, as in spec 006 |
| `HUGR_DRAIN_DEADLINE`, `HUGR_CA_FILE` | as in spec 006 |
| `HUGR_POD_MEMORY`, `HUGR_POD_CPU` | the container's limits, through the Downward API (`resourceFieldRef: limits.memory`, `limits.cpu`) |
| `HUGR_TEMP_DIRECTORY`, `HUGR_TEMP_SIZE` | the spill volume's mount and its size (the operator sets the volume's `sizeLimit` to the same) |
| `HUGR_IDENTITY_PRIVATE_KEY_FILE`, `HUGR_TRESOR_INSECURE_HTTP` | **dev only**, as in spec 006 |

`HUGR_TRESOR`, `HUGR_IDENTITY_ISSUER`, `HUGR_IDENTITY_CLIENT_ID` and `HUGR_IDENTITY_SCOPE` move into the document. When
`HUGR_ORCHESTRATOR` is set, they are refused in the environment: one source per value.

**The order.** Only steps 2, 3 and 5 change.

1. **Hardening**, as in spec 006, plus `temp_directory` from `HUGR_TEMP_DIRECTORY`.
2. **The document.** `GET <HUGR_ORCHESTRATOR>/v1/bootstrap` with `Authorization: Bearer <the token file's content>`.
   - It goes through duckdb's HTTP client, so `ca_cert_file` applies.
   - The token is read from its file at each call, because kubelet renews it.
   - It is never written into a secret, a setting or a log.
3. **tresor**, as in spec 006, with the document's `tresor.host` and `tresor.identity` (issuer, client id, scope) and
   `HUGR_IDENTITY_ASSERTION_FILE`.
4. **The repository**, as in spec 006.
5. **The policy catalog**, as in spec 006, plus two settings: `SET GLOBAL acl_node_group` from `node_group`, and
   `SET GLOBAL acl_node_label` from `node_label`.
6. **The engine, then the profile.**
   - First the defaults:
     - `memory_limit` at 80% of `HUGR_POD_MEMORY`, DuckDB's own ratio of the machine;
     - `threads` at `HUGR_POD_CPU`;
     - `max_temp_directory_size` at 90% of `HUGR_TEMP_SIZE`.
   - Then the profile, as in spec 006. An item for one of those three settings is applied cut to its ceiling, and the
     cut is reported as drift. The ceilings:
     - `memory_limit` at 90% of the pod's memory;
     - `threads` at the pod's CPU;
     - `max_temp_directory_size` at 90% of the volume.
7. **Lock**, as in spec 006.
8. **Serve**, as in spec 006, then the tick (below).

**Document v2** (hugr-platform spec 001). An unknown `version` is refused, and unknown fields are ignored.

```json
{"version": 2,
 "node_group": "etl", "node_label": "hugr-node-etl-7f9c-x2k8p",
 "tresor": {"host": "tresor.example:443",
            "identity": {"issuer": "https://login.microsoftonline.com/<tenant>/v2.0",
                         "client_id": "<the group's workload identity>", "scope": "api://tresor/.default"}},
 "repository": {"name": "hugr", "prefix": "https://kista.example/<tenant>/<channel>"},
 "catalog": {"secret": "hugr_policy_catalog", "schema": "acl"},
 "heartbeat": {"interval_seconds": 15}}
```

**The catalog's type comes from its secret.**
- The document names only the reserved secret (the owner's decision of 2026-10-17), with no type. The secret's own
  TYPE gives the catalog's: tresor's `duckdb_secrets()` lists a descriptor with its type before any ATTACH.
- The node reads the secret's type and attaches the catalog as that type, installing the type's extension from the
  repository when the build does not link it.
- A secret of a type that is not a database is refused.

### The tick

The runner's wait loop (spec 006: a poll every 250 ms on the PRAGMA's own thread, never a worker) runs one tick every
`heartbeat.interval_seconds`. A tick that fails is logged and the next one tries again; the node never stops serving
because of one.

1. **Convergence.**
   - The node reads the profile's version and its effective items for its scope (`ClusterEffective`), and what is in
     effect on it (`ClusterDrift`).
   - Every **hot** item not yet in effect is applied with the statements its write already uses
     (`ClusterItemApplyStatements`, spec 093): an `INSTALL`/`LOAD` from the repository, an `ATTACH` of a new source, a
     `DETACH` of an unused one.
   - Every **restart** or **drain** item the node cannot apply live becomes `needs_restart`, and is listed in
     `pending` with its kind, name and class.
   - Once every hot item of the version is in effect, `acl_cluster_applied(version)` records it.
   - A hot item that fails is listed in `pending` with its error class (never its text, which may carry a path). The
     rest still apply.
2. **The resource group.**
   - A node of a group the policy lacks already refuses new sessions (spec 096, fail closed).
   - The tick makes it explicit: it calls `acl_drain()`, so the load report and the heartbeat say `draining`, and logs
     why.
   - When the group is back, the tick calls `acl_resume()`, but only for a drain it made itself. A drain an operator
     ordered (`DRAIN NODE`) is left alone.
3. **The heartbeat.** `POST <HUGR_ORCHESTRATOR>/v1/heartbeat` with the same bearer and through the same client as the
   document. The body is spec 001's contract:
   - `node_id`: the document's `node_label`, the pod's name;
   - `image`, `duckdb`: the build's version and duckdb's;
   - `config`: `target`, `applied`, `needs_restart`, `pending`;
   - `drift`: the counts of `platform.drift` by state;
   - `load`: `acl_node_load()`.

   A heartbeat that fails is logged and the next tick sends again. The operator counts a node lost after three
   intervals; the node keeps serving.

### Stopping

As in spec 006. The tick stops first, so that no statement of its own runs while the doors drain.

### Changes to acl_cluster.cpp

- `HARDENING_SETTINGS` gains `temp_directory`. Where a node spills is the bootstrap's, from the pod, and a profile item
  could otherwise point every node's temp files somewhere else.
- The clamp rule (a setting's ceiling) is one function, shared by the bootstrap's step 6 and the tick's report.

### Pins

duckdb-acl moves to main (`0c3415c`, spec 134: `acl_node_label`), which also brings mssql 986fd19. tresor moves to
`d7f2a1e` (its spec 036: `enable_external_access = false` stops its own HTTP too; a node does not disable it, so
nothing changes here).

## Enforcement / security

- **No credential is written anywhere.**
  - The two tokens stay in kubelet's files and are read per call.
  - The document holds pointers and names.
  - The catalog's secret comes from tresor, as before.
- **The document reveals a group's pointers only to its pods.** The operator binds the token to a pod of the group by
  TokenReview (spec 001).
- **The node speaks only for itself.** Its heartbeat's `node_id` is its pod's name, and the operator refuses another.
- **The tick applies only what a profile write already applies**, from the same statements, under the same hardening
  and the same trusted repository. A restart item never runs live: the configuration stays locked after the
  bootstrap.
- **Nothing listens before the policy is loaded and the configuration is locked**, as in spec 006.
- **A dropped resource group closes the node to new sessions** (spec 096). The tick only says so, and reopens only what
  it closed.

## Testing

- **sqllogictest:**
  - `hugr_node_bootstrap_plan()` with a v2 document and an orchestrated environment: the new order, `acl_node_group`
    and `acl_node_label` from the document, the engine's defaults, `temp_directory`, and the catalog type taken from
    the secret's;
  - the legacy tresor variables refused when `HUGR_ORCHESTRATOR` is set;
  - an unknown version refused;
  - `temp_directory` refused as a profile item.
- **C++ (pure functions):**
  - the clamp: memory strings parsed, a value cut to its ceiling, threads, temp size;
  - the tick's decision from (effective items, what is in effect):
    - the hot items to apply;
    - the pending list with classes;
    - `needs_restart`;
    - the version to record;
    - a failed hot item listed and the rest kept;
  - the self-drain's memory: resumed only when it drained itself;
  - the heartbeat's body.
- **e2e** `test/e2e/bootstrap/run.sh`, extended with an orchestrated mode. A fake orchestrator over https serves the
  v2 document to the token file it knows and records the heartbeats. Two nodes run on one catalog, and the checks
  are:
  1. both enter through the orchestrator, log in to tresor with the document's identity, and serve;
  2. the engine's defaults follow `HUGR_POD_*`, and a profile `memory_limit` above the pod is cut and reported;
  3. a hot `ACL CLUSTER ATTACH … IN GROUP g` written on node A is in effect on node B within one tick, and B reports
     `applied` at the new version;
  4. a `SET … IN GROUP g` is reported by both as `needs_restart` with the item in `pending`;
  5. a `DROP RESOURCE GROUP g` drains both by themselves (the heartbeat says `draining`), and recreating it resumes
     them;
  6. the heartbeat's `node_id` is the document's label, and a session's cookie is `acl_node=<label>`.
- **CI**: as spec 006's e2e, in the linux job.

## Alternatives

- **The operator computing the engine's values and passing them in env.** The Downward API gives the node the
  container's own limits, so the node computes. That works the same for a pod someone wrote by hand.
- **The heartbeat and the tick as a separate process or a table function.** A table function runs on a scheduler
  worker and would deadlock a `SET threads` (spec 006). A second process would need its own connection to the
  catalog and its own identity. The runner's loop is already there.
- **Restart items applied live.** Not possible: the configuration is locked after the bootstrap (spec 006 step 7), on
  purpose.
- **Hot items applied with a canary, one node first.** Not now. A hot item's failure is an error the node reports, not
  a crash, and extensions come only from the signed repository. Restart items roll one node at a time already, by the
  group's restart (spec 001).

## Follow-ups

- The operator's side: hugr-platform spec 001's code.
- The front's re-seat on `wrong_resource_group` needs duckdb-acl's machine-readable `x-acl-resource-groups` (an
  addendum to spec 096 there).
- A document from a file, for a bare-metal node with no orchestrator and no control plane.
