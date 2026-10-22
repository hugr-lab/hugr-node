# Spec 017: the canary migrates the catalog, and the heartbeat says what each node is

- **Status**: implemented
- **Date**: 2026-10-21

## Summary

hugr-platform spec 008 (accepted 2026-10-21) rolls the nodes onto a new release in phases. The node's side is two
pieces:
- **`catalog.migrate`**, a new flag of the bootstrap document. A node told so migrates the policy catalog at step 5,
  before it opens it. Without the flag a node never migrates.
- **The heartbeat says what the node is**: its DuckDB release and platform, and the catalog's state as its build
  judges it. The operator decides the rollout's phases from these.

## Problem

- **A new build refuses an older catalog.** duckdb-acl spec 094's rule is "migrate it first", and an old build cannot
  migrate forward. Today a release with a schema step has no path but a manual duckdb of the new build.
- **Nodes must not migrate by themselves.** The first new node would then change the shared catalog with no
  decision (duckdb-acl spec 094, design/017 §3.2a).
- **The operator cannot see the catalog.** It holds no login to it (only tresor does), so the stamp has to come from
  the nodes. It also does not know which DuckDB a node runs: the heartbeat's `duckdb` is the source id (a commit), not
  the release the repository's channel is named by.

## Design

- **The document** (version 2, an optional field; a node of an older pin ignores it):

  ```json
  "catalog": {"secret": "hugr_policy_catalog", "schema": "acl", "migrate": true}
  ```

  A value that is not a boolean is refused, like every other field of the document.
- **Step 5** runs `SELECT acl_migrate_catalog('hugr_policy', '<schema>')` after the catalog is attached and before
  `acl_use_db`, when the flag is set and the catalog has its schema.
  - The function is acl's own: the steps the build carries, in one transaction. A catalog already at the build's
    version answers "already" and nothing changes, so a pod restarted after it migrated runs the step again harmlessly.
  - A catalog with no schema yet (the first policy's `init`, spec 015) is created at the build's version, so there is
    nothing to migrate.
  - The answer is logged; a failure stops the bootstrap at step 5 with acl's message, as any step's does.
  - Without the flag nothing changes: a new build on an older catalog stops at step 5 with acl's "migrate it first".
- **The heartbeat** adds three fields, all read from the node itself:
  - `duckdb_version`: `DuckDB::LibraryVersion()`, the release (`v2.0.0`), which names the repository's channel.
  - `platform`: `DuckDB::Platform()` (`linux_amd64`), the other part of an extension's path in the repository.
  - `catalog`: the catalog's state as this build judges it, from the store's `acl_catalog_schema()` - `build`,
    `build_min_reader`, `schema`, `min_reader`, `mode` - plus `type` and `extension`. The `extension` is the one the
    node installed from the repository for the catalog's type (`postgres_scanner`), or empty when its build links it.
  - The existing fields stay as they are, and an operator of an older pin ignores the new ones.
- **The plan** (`hugr_node_bootstrap_plan`) shows the migration step when the document sets the flag.

## Enforcement and security

- **The flag is the operator's.** It arrives only in the document the operator answers to the pod's own token
  (spec 010), and hugr-platform spec 008 serves it to the canary alone.
- **The migration runs as the node.** It uses the catalog's login the node already holds (its tresor grant). No new
  credential exists.
- **The heartbeat adds no secret**: versions, a platform, schema numbers and an extension's name.

## Testing

- **sqllogictest** (`test/sql/agent/hugr_node_bootstrap.test`): the flag puts the migration step before
  `acl_use_db`, without it there is none, and a flag that is not a boolean is refused.
- **e2e** (`test/e2e/bootstrap/run.sh`): the catalog is stamped one schema version back, and two node starts follow.
  - Without the flag, the node stops at step 5 with "migrate it first", and no port opens.
  - With the flag, it migrates the catalog, serves, and its heartbeat carries `duckdb_version`, `platform` and the
    catalog's state at the build's version.

## Alternatives considered

- **A migration Job.** It needs an identity that may read the catalog's secret from tresor, and an image of the new
  build anyway. The canary already has both (hugr-platform spec 008).
- **The operator reads the catalog's stamp itself.** It would need the catalog's login, which only tresor holds.
