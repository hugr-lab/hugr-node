# Spec 015: the first policy, written by the first node

- **Status**: draft
- **Date**: 2026-10-20

## Summary

A platform installed from nothing has an empty policy catalog: no issuer, no administrator, no resource group. No
token verifies, and a node whose group the policy lacks refuses everyone and drains (duckdb-acl spec 096, this
repository's tick). hugr-platform spec 005 (the owner, 2026-10-20) has the first node write the first policy. The
operator sends structured fields, never SQL, in the node's bootstrap document. A node that attaches a catalog with no
issuer writes them once, in its own process, before it serves. After that, the administrator signs into the console
and does the rest there.

## Problem

- **No install Job can do it.** A Job run as a group's ServiceAccount is refused by the platform's admission
  policies: only a group's ReplicaSet may run as the group (hugr-platform spec 001). A Job of its own would need an
  identity of its own in the IdP, plus a grant in tresor.
- **The node already has everything:** its identity, the catalog's login through tresor, and the catalog attached, in
  a process nobody can reach before its doors open.
- **Today the demo writes the catalog by a seed Job** that carries a principal's client secret (hugr-platform spec
  004). An organisation's install must not.

## Design

- **The document** (version 2, a new optional field; a node of an older pin ignores it):

  ```json
  "first_policy": {
    "issuer": {"name": "corp", "url": "https://login.example/t/v2.0", "audiences": ["api://hugr"],
               "roles_claim": "roles"},
    "clients": [{"name": "console", "client_id": "<the console's application id>"}],
    "administrators": "platform_admin",
    "resource_groups": [{"name": "a", "default": true}, {"name": "b"}]
  }
  ```

  - **The issuer** is the organisation's IdP: its URL, the audiences the platform's tokens carry, and the claim that
    names roles.
  - **The clients** are the applications whose tokens count. The console's is always there, because the console is
    how the administrator gets in. A tool's client (a DBeaver or driver registration) can be listed too, or added
    later from the console.
  - **The administrators** are the value of the roles claim that makes a person the platform's administrator.
  - **The resource groups** are the operator's: one per `HugrNodeGroup` the namespace holds when the document is made
    (its resource group's name), and `default` on the group the platform names. They are the deployment's groups,
    chosen in a marketplace's form or in the chart's values.
- **The write** comes after step 5 (the catalog attached) and before the engine, the profile and the doors.
  1. Is there a first policy to write? `acl_use_db` opens the catalog, creating its schema when the database is empty
     (its `init`). A catalog that lists any issuer (`acl_issuers()`) is left alone: the first policy never
     overwrites, and is written once in the platform's life.
  2. In the node's own process, with `acl_allow_anonymous_admin` on only for these statements, it writes:
     - `CREATE ISSUER IF NOT EXISTS <name> URL '<url>'`;
     - for each client, `CREATE CLIENT IF NOT EXISTS <name> ISSUER <issuer> AUDIENCES ('<audience>', …) AZP
       '<client_id>' ROLES FROM ('<roles_claim>')`: a token counts only when that application asked for it;
     - `CREATE ROLE IF NOT EXISTS platform_admin`, then `GRANT ADMIN cluster` and `GRANT ADMIN policy` to it;
     - for each client, `MAP CLAIM '<administrators>' FROM CLIENT <name> TO ROLE platform_admin`. A role holding an
       admin scope is reached only through a client's own mapping (duckdb-acl spec 095);
     - for each group, `CREATE RESOURCE GROUP <g> … [DEFAULT]` and `GRANT RESOURCE GROUP <g> TO ROLE platform_admin`.
  3. **Nodes that start together** race. Each statement is `IF NOT EXISTS` or idempotent, and a node that finds an
     issuer when it is its turn writes nothing. The outcome is one first policy, whichever node wrote which line.
- **It is logged as a step** of the plan (`5 first policy`), its statements shown in `hugr_node_plan()`, values in
  them blanked as the other steps' are. It carries no secret: the fields name an issuer and roles.
- **After it, the node goes on as any node:** its group is known, it serves, and the administrator's console sign-in
  verifies.

## Enforcement & security

- **No credential travels.** The document's new field names an IdP, applications and a role value. The node logs in
  to tresor by its own projected token, as at every start.
- **Who can set it** is whoever may edit the `HugrPlatform`, that is, the platform's namespace administrators, who can
  already run a pod's process (spec 001's boundary). The field writes only into an empty catalog, so a later edit
  changes nothing.
- **The node writes the policy unprefixed** (anonymous admin) only before its doors open, and only these statements.
  The setting is off again before the engine step.

## Testing

- **A C++ test of the plan**: a document with a first policy yields its statements after the catalog's, and one
  without it yields none.
- **The bootstrap e2e** (`test/e2e/bootstrap/run.sh`): a node started on an empty catalog with a first policy writes
  it. A second node and a restart write nothing more. A token of the administrators' claim, from the console's
  client, reads `platform.console_info()`.
- **hugr-platform's demo** (its spec 004) keeps its seed for now. The seed writes an issuer first, so the first policy
  is skipped there. Moving the demo's policy to the administrator's path (the console, the front) is its own step.

## Alternatives considered

- **An install Job with an identity of its own**: one more IdP registration and tresor grant per installation, to
  do once what a node can do.
- **SQL in a ConfigMap**: anything an editor writes would run unprefixed as the node.
- **`kubectl exec` by hand**: no automation for a marketplace install.

## Follow-ups

- hugr-platform: `HugrPlatform.spec.firstPolicy` and the document's field (spec 005).
