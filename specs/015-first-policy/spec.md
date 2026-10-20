# Spec 015: the first policy, written by the first node

- **Status**: implemented
- **Date**: 2026-10-20

## Summary

A platform installed from nothing has an empty policy catalog: no issuer, no administrator, no resource group. No
token verifies, and a node whose group the policy lacks refuses everyone and drains (duckdb-acl spec 096, this
repository's tick). hugr-platform spec 005 (the owner, 2026-10-20) has the first node write the first policy. The
operator sends structured fields, never SQL, in the node's bootstrap document. A node that attaches a catalog nobody
administers yet writes them, in its own process, before it serves. After that, the administrator signs into the console
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
    chosen in a marketplace's form or in the chart's values. The node's own group must be among them, and at most one
    is the default.
  - **Every field is required**, and a name (issuer, client, group) is a plain name (`[A-Za-z0-9_-]`, at most 63): it
    goes into a management statement as an identifier. Names compare exactly, as the policy compares them, and none is
    named twice; neither is a client_id (two clients for one application would accept the same tokens). The issuer's URL
    is https without userinfo, since its keys are read from it by discovery. The document is refused before anything is
    written, naming what is wrong (never the URL).
- **The write** comes after step 5 (the catalog attached) and before the engine, the profile and the doors.
  1. **The catalog.** With a first policy in the document, a database that holds no schema yet (its `meta` table is not
     in `duckdb_tables()`) is opened with `acl_use_db`'s `init`, which creates the schema. Any other is opened as every
     node opens it, without `init`: once the schema is there, a node needs no right to change it, and replaying its DDL
     at every start is not a node's work. (`init` never migrates a catalog of another version - duckdb-acl spec 094.)
  2. **Is it to be written?** Only while the catalog lists no role mapping (`acl_role_mappings()`) and no issuer but
     the first policy's own (`acl_issuers()`). A mapping is what lets a token's holder in, and the first policy writes
     its mappings last. So a catalog without one has never been administered through the platform's IdP, and a
     catalog with one is left alone: a later edit is never undone.
  3. **The statements.** In the node's own process, with `acl_allow_anonymous_admin` on only for them (off again
     after them, whatever failed), in this order:
     - `CREATE ROLE IF NOT EXISTS platform_admin`, then `GRANT ADMIN cluster`, `policy` and `passthrough` to it;
     - for each group, `CREATE RESOURCE GROUP "<g>" [DEFAULT]`, unless the catalog holds that group already (a
       re-create would replace its limits), and `GRANT RESOURCE GROUP "<g>" TO ROLE platform_admin`;
     - `CREATE ISSUER IF NOT EXISTS "<name>" URL '<url>'`;
     - for each client, `CREATE CLIENT IF NOT EXISTS "<name>" ISSUER "<issuer>" AUDIENCES ('<audience>', …) AZP
       ('<client_id>') ROLES FROM ('<roles_claim>')`. A token counts only when that application asked for it, and its
       roles are the mapped values of the claim (an explicit client ignores the unmapped ones);
     - last, for each client, `MAP CLAIM '<administrators>' FROM CLIENT "<name>" TO ROLE platform_admin`. A role
       holding an admin scope is reached only through a client's own mapping (duckdb-acl spec 095).
  4. **A write that stopped part-way** (the catalog's database went away, the pod was killed) is completed by the next
     start: no mapping is in yet, and every statement is `IF NOT EXISTS` or an upsert of the same value.
  5. **Nodes that start together** race, and the outcome is the same: one first policy, whichever node wrote which
     line. Two nodes creating the schema of an empty database at once can make one of them fail its step; its restart
     finds the schema.
- **It is logged as a step** of the plan (`5 first policy`): `hugr_node_bootstrap_plan()` shows the gate and the
  statements, and the run logs one line per statement and what it decided (`written - issuer <name>, N client(s), M
  resource group(s)` or `the catalog holds its policy - nothing written`), never a value. It carries no secret: the
  fields name an issuer, applications and roles.
- **After it, the node goes on as any node:** its group is known, it serves, and the administrator's console sign-in
  verifies.

## Enforcement & security

- **No credential travels.** The document's new field names an IdP, applications and a role value. The node logs in
  to tresor by its own projected token, as at every start.
- **Who can set it** is whoever may edit the `HugrPlatform`, that is, the platform's namespace administrators, who can
  already run a pod's process (spec 001's boundary). The field writes only into a catalog nobody administers yet, so
  a later edit changes nothing.
- **The node writes the policy unprefixed** (anonymous admin) only before its doors open, and only these statements.
  The setting is off again before the engine step, and the doors refuse to serve while it is on.
- **`platform_admin` holds `passthrough` too** (the owner, 2026-10-20). In duckdb-acl, handing out administration is
  passthrough's alone (spec 117): granting a bundle, mapping a claim to a role that administers, giving a mapped role
  a catalog's `manage`, and changing the issuer or a client that carries such a mapping. Without it the platform's
  administrator could do none of that, and no one in a deployed platform holds it otherwise. It is also `ACL NATIVE`,
  the break-glass door. So the administrators' claim value is the platform's most powerful one: give it to few.

## Testing

- **The plan** (`test/sql/agent/hugr_node_bootstrap.test`): a document with a first policy yields `init` (with its
  condition) and its statements after the catalog's, and one without it yields neither. Each statement is run on a
  catalog twice (a write that stopped part-way, completed), and the listings show the policy once. Each refusal names
  what is wrong.
- **The bootstrap e2e** (`test/e2e/bootstrap/run.sh`, checks 15-17): a node started on an empty catalog schema with a
  first policy writes it and serves, and a second node writes nothing. A token of the administrators' claim from the
  console's client holds `cluster`, `policy` and `passthrough` (`platform.console_info()`), against an IdP served over
  https in the run. The same token from another application is refused.
- **hugr-platform's demo** (its spec 004) keeps its seed for now. The seed writes an issuer and mappings first, so
  the first policy is skipped there. Moving the demo's policy to the administrator's path (the console, the front) is
  its own step.

## Alternatives considered

- **An install Job with an identity of its own**: one more IdP registration and tresor grant per installation, to
  do once what a node can do.
- **SQL in a ConfigMap**: anything an editor writes would run unprefixed as the node.
- **`kubectl exec` by hand**: no automation for a marketplace install.

## Follow-ups

- hugr-platform: `HugrPlatform.spec.firstPolicy` and the document's field (spec 005).
