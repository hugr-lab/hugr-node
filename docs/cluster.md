# Node groups and the cluster profile

Moved from duckdb-acl's `website/docs/management-sql.md` (tag `pre-platform-split`, duckdb-acl spec 127) with the
code it describes; the statements are the same on a `hugr_node` node.

## Resource groups as sets of nodes

**A group is also a set of nodes** (spec 096). A node belongs to the group its deployment names with
`SET GLOBAL acl_node_group = '<group>'` (in its bootstrap; never from the policy, never under a
principal). A node of a group serves the sessions of principals that hold the group, **with that
group's limits only**, and refuses the others with `wrong_resource_group`, naming the groups that
would serve them - so a front sends the client to the right node. A node without a group serves
everyone, as before: a single node needs none of this. A node whose group the policy does not have
serves nobody new until it does (fail closed). The gateway's `ACL TOKEN` / `ACL ROLE` path is not
placed; `manage` and `passthrough` are placed like anyone.

- **The default group** (`DEFAULT`, or `ALTER RESOURCE GROUP g SET DEFAULT`): a principal whose roles
  are in no group is its member - for placement and for its limits. At most one group is the
  default: marking a second is refused, naming the current one. A re-create without `DEFAULT` keeps
  the mark; `DROP DEFAULT` removes it. Without a default group, ungrouped principals are served by
  ungrouped nodes only.
- **Dropping a group** with cluster profile items (`IN GROUP g`) is refused, naming them: remove its
  part of the profile first.
- **Names compare as written**: `General` is not `general`, in `acl_node_group` as in the bindings.

## Cluster profile

```sql
CLUSTER INSTALL EXTENSION <name> VERSION '<v>' [FROM <repository>] [IN GROUP <group>] [COMMENT '<text>']
CLUSTER UPDATE  EXTENSION <name> VERSION '<v>' [FROM <repository>] [IN GROUP <group>] [COMMENT '<text>']
CLUSTER REMOVE  EXTENSION <name> [IN GROUP <group>]
CLUSTER ATTACH '<path>' AS <alias> (TYPE <type> [, SECRET <secret>] [, <option> [<value>]] …)
        [LINEAGE '<identity>'] [DEPENDS ON (<alias>, …)] [IN GROUP <group>] [COMMENT '<text>']
CLUSTER DETACH <alias> [CASCADE] [FORCE] [IN GROUP <group>]
CLUSTER SET <setting> = <value> [IN GROUP <group>]
CLUSTER RESET <setting> [IN GROUP <group>]
```

**A cluster node's** (spec 118): every hugr_node node is a cluster node, whatever the number of nodes - a cluster
may be one (hugr-node spec 002: there is no single-node mode and no setting to choose one). One node and many differ
only in where the policy catalog and the secrets service's state live - a local file or a shared database - which is
the deployment's composition, not a mode of the node. The open node - duckdb-acl's own `acl` - has no profile at all:
its bootstrap SQL and its admin's `ACL NATIVE` configure it.

The cluster profile is the shared part of every node's bootstrap (spec 093): which extensions a node
runs, which sources it attaches and which settings it carries. It lives in the policy catalog as
desired state, next to the policy, and a `config_version` counts its changes. With `IN GROUP` an item
belongs to the nodes of one resource group (spec 096: the nodes whose `acl_node_group` it is); without
it, to the whole cluster. A group's item overrides the cluster's item of the same kind and name, and
is applied live only on a node of that group - written from any other node, it answers
`applied_here = false` with the note "item of resource group g - …; the group's nodes roll it out".
A group's source named like a cluster's is a re-point on the group's nodes (drain class), and
detaching it brings the cluster's back there, never a `DETACH`; a detach (and its `CASCADE`) follows
the scope - a group's item depends on the cluster's source of a name only while the group has none of
its own.
`LINEAGE '<scheme>://<host:port>[/<database>]'` names the source in lineage as others know it (spec
112, [lineage](lineage.md#a-sources-identity)): the item's spec carries it as `lineage`, and a node
attaches the source with it (`ATTACH … LINEAGE '…'`). It is a name, never a credential.
`acl_cluster_effective()` lists what applies to *this* node (the cluster's items with its group's over
them, each with the scope it came from); `acl_cluster_applied(version)` is the node agent saying which
profile version this node has converged to, which the load report shows (`config.applied` against
`config.target`).

A statement does four things:

1. It checks the change. A refusal writes nothing.
2. It writes the item and bumps `config_version`, in one catalog transaction.
3. It applies the change **on this node** when that can be done live.
4. It answers `{version, class, applied_here, note}`.

If the live apply fails, the write is rolled back. So an item reaches the cluster only once it has
worked on the node that wrote it.

The class says how each kind of change reaches a running node:

| change | class |
| --- | --- |
| `INSTALL EXTENSION`, `ATTACH`, `DETACH` of an unused source | hot, applied here at once |
| `ATTACH` over an existing alias (re-pointing), `DETACH` of a source in use | drain |
| `UPDATE` / `REMOVE EXTENSION`, `SET` / `RESET` | restart |

duckdb cannot unload an extension from a running process. A node's configuration is locked after its
bootstrap. Each node applies hot changes on its own tick and reports drain and restart changes pending
in its heartbeat; the orchestrator rolls the nodes (hugr-node spec 010, hugr-platform spec 001).

- **No credentials, anywhere.** A path with a credential key is refused. That covers a conninfo
  `password=`, a URI's `user:password@`, a SAS `sig=` and the like, and so does an option with such a
  key. A postgres, mysql, mssql or hugr ([another platform](federation.md)) source must name a `SECRET`, and the
  node resolves it from its secrets service. The refusal names the key, never its value.
- **The node's hardening is not a profile item.** `allow_unsigned_extensions`, `lock_configuration`,
  `allow_persistent_secrets`, `allow_extension_repositories`, the extension and secret directories
  and the like are refused - and (spec 118) the node's trust: `acl_allow_anonymous_admin`, the quack door's
  `acl_quack_authentication_function` / `acl_quack_authorization_function`,
  `enable_server_cert_verification` / `enable_curl_server_cert_verification`, `disabled_filesystems` - and
  (spec 010) `temp_directory` (the pod's spill volume), `httpfs_client_implementation` and `redact_http_logs` (the
  path the node's tokens travel).
  Only a GLOBAL setting may be profiled.
- **Extensions come from a trusted repository.** `FROM` names a repository created with duckdb's
  `CREATE EXTENSION REPOSITORY`; with exactly one, an install may leave it out. An update keeps what it does not name
  (spec 019): without `FROM` the item's repository (still one the node trusts), without `COMMENT` its comment. A path, a URL, `core` and
  `community` are refused, and `VERSION` is required.
  - duckdb installs such an extension under `repositories/<repository>/` and loads it with
    `LOAD <name> FROM <repository>`.
  - Integrity is the repository's signature, which duckdb verifies against the repository's
    pinned keys at every `INSTALL` and `LOAD`, together with a version that never changes once
    released. A profile does not pin a hash: one would change with every key rotation of the
    repository. `SHA256` is refused (spec 103).
- **Start order.** A node applies the profile in this order: settings, extensions, secrets, sources,
  the policy catalog.
  - Sources are ordered by `DEPENDS ON`, and by creation order where nothing orders them.
  - A dependency on an unknown source, or one that would make a cycle, is refused.
  - A source's extension follows from its `TYPE`, and `REMOVE EXTENSION` is refused while a source
    needs it.
- **Removing a source.**
  - `DETACH` is refused while another source depends on it. `CASCADE` removes the dependents too.
  - It is also refused while the policy reads through it: a virtual table, schema or function over
    `<alias>.…`, or a virtual view or macro whose body names `<alias>.` (spec 118 - read as text, so an
    unrelated name spelled the same also blocks). `FORCE` detaches anyway, and `acl_check_catalog` then
    reports `source_missing`.
- **Authorization.** The profile is the cluster's infrastructure: every `CLUSTER` statement needs the
  **`cluster`** bundle (spec 118; `passthrough` carries it) - `manage` / `policy` and `operate` are not
  enough, and no point grant stands in. The bundle sets the node's **resources and tuning** only - an
  allowlist: `threads`, `memory_limit`, `max_temp_directory_size`, `preserve_insertion_order`, checkpoint and
  ordering defaults, the http client's timeouts and retries, `TimeZone` / `Calendar`, and acl's limits,
  windows and timeouts (`acl_max_sessions`, `acl_max_result_rows`, `acl_quack_*` sizes, `acl_*_timeout`,
  `acl_alias_types` / `acl_enum_types`, `acl_lineage_namespace` / `_level`, `acl_profile_level`, ...).
  Anything else - a data path (`http_proxy*`, an endpoint, a log or audit sink), a key
  location, the audit, a repository - is `passthrough`'s. `acl_node_group` is the deployment's, never an item.
- **The telemetry's destination is the cluster's** (spec 005): acl_otel, built into hugr_node, exports nothing until
  `acl_otel_endpoint` (OTLP) or `acl_otel_lineage` (OpenLineage) is set. Where it goes - `acl_otel_endpoint`,
  `acl_otel_protocol`, `acl_otel_insecure`, `acl_otel_certificate`, `acl_otel_lineage`, `acl_otel_lineage_endpoint`,
  `acl_otel_lineage_secret` - is one destination for the whole cluster: a `CLUSTER SET` of them without `IN GROUP`
  (an endpoint is `passthrough`'s, above), refused with `IN GROUP`. acl_otel's other settings (batching, intervals,
  series, rules) may be a group's. The lineage key is a secret of the secrets service, named by
  `acl_otel_lineage_secret` - never a value in the profile. A node's own environment (`OTEL_EXPORTER_OTLP_*`,
  `OPENLINEAGE_URL`) is its deployment's, as its bootstrap is.
- **Drift** (spec 118): `platform.drift` (the `cluster` bundle) lists what this node carries against its
  effective profile - per database, loaded extension and profiled setting a `state`: `same`, `differs`
  (`node_value` / `profile_value`), `node_only` (an `ACL NATIVE` ATTACH, say), `profile_only` (the node
  lacks it) or `bootstrap` (the default database, the policy's, a secrets service, statically linked
  extensions and acl, `acl_node_group` - listed so nothing is hidden). A source is
  compared by its type (the path is shown, not judged: duckdb shows a file path absolute), a setting by
  the value duckdb would show (`4GB` = `3.7 GiB`); a setting the profile does not name is not compared. A
  database that is not a file shows its type, never its path - an attach written natively may carry a
  credential. Showing only: the node reverts nothing; `ACL NATIVE` stays the
  unrestricted break-glass.
- **Audit.** Each statement is one `admin` event. Its object is the item, as `source:<alias>`,
  `extension:<name>` or `setting:<name>`, with capability `cluster`. The item's spec is never in the
  event, because a path is a physical name.
- **Listings.** `acl_cluster_items([group])` lists the profile: scope, kind, name, spec (JSON),
  class, version, depends_on and comment. `acl_cluster_version()` answers the counter.
- **Functions.** The admin functions behind the statements are:
  - `acl_cluster_extension(verb, group, name, version, repository, comment)`;
  - `acl_cluster_attach(group, alias, path, type, secret, options_json, depends_on_csv, comment)`;
  - `acl_cluster_detach(group, alias, cascade, force)`;
  - `acl_cluster_setting(verb, group, name, value)`.

