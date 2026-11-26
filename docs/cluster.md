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
        [LAZY [IDLE '<interval>']] [LINEAGE '<identity>'] [DEPENDS ON (<alias>, …)] [IN GROUP <group>]
        [COMMENT '<text>']
CLUSTER DETACH <alias> [CASCADE] [FORCE] [IN GROUP <group>]
CLUSTER SET <setting> = | TO <value> [IN GROUP <group>]
CLUSTER RESET <setting> [IN GROUP <group>]
CLUSTER SET SOURCES (<alias>, …) | ALL [LAZY [IDLE '<interval>']] IN GROUP <group>
CLUSTER RESET SOURCES IN GROUP <group>
CLUSTER ADD SCRIPT <name> BEFORE SOURCES | AFTER SOURCE <alias> | AFTER SOURCES AS '<sql>'
        [ON ERROR FAIL NODE | CONTINUE | DETACH SOURCE | RETRY] [IN GROUP <group>] [COMMENT '<text>']
CLUSTER ALTER SCRIPT <name> [<place>] [AS '<sql>'] [ON ERROR …] [IN GROUP <group>]
CLUSTER DROP SCRIPT <name> [IN GROUP <group>]
```

The statements are read on duckdb's tokens (spec 020), like acl's own management statements: a comment may stand
anywhere; a quoted value is one plain literal (`'…'` or `"…"`, never `E'…'` or a string continued on the next line);
a list is read item by item - a trailing comma is allowed, an empty item or an empty list is not; each clause,
each option, each flag and each source a source depends on is written once; a value is a word (`4GB`, `v1.0`), a quoted value or a number (`-1`). A statement that does not read is
told how its form is written.

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

### A group's source set

By default a group's nodes attach every source: the cluster's and the group's own. `CLUSTER SET SOURCES (sales,
crm) IN GROUP etl` declares the sources its nodes attach (spec 035), and `CLUSTER RESET SOURCES IN GROUP etl` gives
them every source again.

- **The names** are aliases of `ATTACH` items, the cluster's or that group's own. A name that is neither is refused
  where it is written. A cluster-wide set is refused: the cluster attaches every source.
- **Dependencies stay listed.** A listed source that `DEPENDS ON` another needs that one listed too. The rule holds
  when the set is written, when an `ATTACH` re-points a listed source, and when a `DETACH` of a group's own source
  hands its nodes the cluster's source of that name. Where it is a group's own item, the group's own dependencies
  decide.
- **An `ATTACH` the set leaves out** is written, and its answer names the groups whose nodes do not attach it.
- **The nodes converge at their next tick.** A source left out is detached (a source in use waits for a drain),
  and a listed one that is missing is attached. `acl_cluster_effective()` shows only the listed sources, and the set
  itself is listed in `acl_cluster_items()` as an item of kind `sources`. A source attached to the group later is
  attached only once the set names it.
- **Such a node writes no policy** (duckdb-acl spec 176). Every management statement that writes the policy or the
  profile, `platform.check_catalog` and `platform.check_grant` are refused there with `acl: this node attaches the
  sources of group "etl" only - run it on a node of a group that attaches every source`. That is because each of
  them would be judged against sources the node lacks. The node's operation (sessions, `DRAIN` / `RESUME NODE`),
  `console_info`, `SET` / `RESET SESSION VIEW AS`, the policy's reads, the group's queries and a principal's own DDL
  (its record names an object in a source the node holds) all run there. `ACL NATIVE` and the operator's own SQL
  are unchanged.
- A listing names a source outside the set as `not attached` (duckdb-acl spec 136).

- **Lazily** (spec 035 stage 2b). `CLUSTER SET SOURCES (sales, crm) LAZY IDLE '30 min' IN GROUP etl` attaches only
  those sources, each when a statement first reads it. `CLUSTER SET SOURCES ALL LAZY … IN GROUP etl` does the same for
  every source, so the group's nodes start with none attached and have no list to keep up to date. A new grant of a
  role of the group works at its first statement. On the group's nodes the set's marks replace a source's own
  (`acl_cluster_effective()` shows them), and everything below about a lazy source applies.
  - A source in a `DEPENDS ON` chain (one that loads through another, or one another loads through) is attached at
    once all the same, and so is the secrets service.
  - A set's `IDLE` replaces a source's own; a set that states none keeps it.
  - Write `ALL LAZY` with an `IDLE`: a policy write on such a node attaches every lazy source for its probes, and
    without an idle time they then stay attached.
  - `ALL` is written only with `LAZY`: every source attached at once is what `RESET SOURCES` gives.
  - A node of an `ALL` group is not partial, so it writes the policy: its probes attach the lazy sources first.

- **What a group's roles reach.** `platform.group_sources(['<group>'])` (the cluster bundle's, in FROM; unprefixed
  `acl_cluster_group_sources`) answers one row per source that the group's roles reach or that its set lists. Its
  columns are `group`, `source`, `roles`, `listed` and `repair`.
  - `roles` are the group's roles that reach the source: a grant, a relation, a live alias, or a view, an RLS, a
    mask, a computed column or a template that names it, as `platform.exposures` reads them. The default group's
    roles are every role bound to no group, a token's role without a `CREATE ROLE` included.
  - `listed` says whether the set lists the source; it is NULL when the group has no list.
  - `repair` is the statement that lists a source the set leaves out: the set as written, the missing source, and
    whatever it `DEPENDS ON`, with the set's lazy marks. Paste it after your prefix.
  - Another group's own source that the roles reach is shown with `listed = false`. Its repair names the
    `CLUSTER ATTACH … IN GROUP <group>` to write first.
  - Without an argument it answers every group whose set is a list. That is the check: a row with `listed = false`
    is a source the group's roles reach but its nodes do not attach, and every read of it by them is refused.
  - Nothing is applied by itself: a new grant leaves the set as it is. Its answer says so instead: the `message` of
    a `GRANT CATALOG | SCHEMA | TABLE | VIEW | OBJECT` or a `GRANT RESOURCE GROUP` to a role names each group whose
    list leaves out a source the role now reaches, with the repair (`group etl does not list sources crm, archive,
    which role loader reads - CLUSTER SET SOURCES (…) IN GROUP etl`).
  - **Its limits.**
    - A principal that holds a role bound to the group and a role bound to none reads with both on the group's nodes.
      The unbound role's reach is shown only for the default group.
    - The reach of a physical function the gate admits, of a `GRANT SOURCE` and of a physical view reading another
      source is not counted.
    - Run it on a node that attaches every source: a two-part name in an old body is read against the attached
      databases.

### A lazy source

`CLUSTER ATTACH … LAZY` marks a source that a node attaches only when a statement reads it (spec 035 stage 2), and
`LAZY IDLE '30 min'` also detaches it after that long without a read. It suits a big source that few statements read
(an archive): nodes start without it, and its memory goes back once nobody reads it. It is meant for the nodes of a
group that runs known statements (an application's queries, an ETL job), not one that serves analysts or other
interactive users: their tools browse the catalog first, and a lazy source shows them its objects without columns
until a statement reads it. For those, give their roles a resource group whose source set lists only the sources
those roles can see (`CLUSTER SET SOURCES … IN GROUP`, above).

- **Never attached by the profile.** Neither the bootstrap nor the tick attach it. The drift shows it `lazy` while it
  is not attached (and `same` once it is), and the node reports the profile applied all the same.
- **Attached on demand** (duckdb-acl spec 177). A principal's statement that reads the source attaches it before the
  statement binds, under a lock per source, so concurrent statements make one attach. The source can be read
  through a relation's physical name, a live alias, an operator's view, an RLS's subquery or a template.
  - A statement wakes only what the policy names, because a principal's own names are virtual.
  - A source that does not attach refuses the statement with `acl: a source this statement reads could not be
    attached (<class>)`. Only the error's class is shown, never its text, and the principal is not told the source's
    name. The node's log names it: `hugr_node: lazy source <alias> could not be attached (<class>)`.
  - A statement decided earlier (a prepared one, a later statement of its batch) attaches it again before it runs.
  - A policy write and `platform.check_catalog` / `check_grant` attach every lazy source first: their probes read
    the sources.
  - A listing never wakes a source: it is listed `not attached`. Until a statement reads the source, an object read
    live from it (no declared column list) is listed without columns. A client that loads the catalog whole (a
    quack client) sees it that way, so declare the columns of the objects such a source holds.
- **Detached when idle.** With `IDLE`, the tick detaches the source once no statement has read it for that long and
  no transaction uses it. A running statement or an open transaction keeps it attached, and the next tick tries
  again. Without `IDLE` it stays attached until the node stops. Only what the node took is detached: a source it
  attached on demand or from the profile, never one attached by hand under the alias (`ACL NATIVE`: drift).
- **A lazy source attaches alone.** `LAZY` with `DEPENDS ON` is refused, so is a source that depends on a lazy one,
  and so is marking lazy a source others depend on. The secrets service (`TYPE tresor`) is never lazy.
- **The interval is duckdb's** (`CAST('<interval>' AS INTERVAL)`), at least a minute. `IDLE` without `LAZY` does not
  read, nor does `IDLE ''`. The item's spec carries `lazy` and `idle_seconds`. Writing the source again with other marks (or none)
  re-points nothing: an attached source stays, and one no longer lazy is attached as any source.
- `ACL NATIVE` and the operator's own SQL are not rewritten, so they never attach a lazy source themselves; they may
  `ATTACH` it by hand.

### Scripts

`CLUSTER ADD SCRIPT` puts the administrator's own SQL into a node's boot (spec 047): what a setting cannot say, such
as a macro or a table in an attached database, a source's own parameters once it is connected, or a command in the
source before anything reads it.

```sql
ACL CLUSTER ADD SCRIPT 10_defaults BEFORE SOURCES AS 'SET GLOBAL preserve_insertion_order = false';
ACL CLUSTER ADD SCRIPT 20_warehouse AFTER SOURCE warehouse
    AS 'CALL postgres_execute(''warehouse'', ''ANALYZE'')' ON ERROR DETACH SOURCE;
ACL CLUSTER ADD SCRIPT 30_views AFTER SOURCES
    AS 'CREATE OR REPLACE MACRO local.main.today() AS current_date' ON ERROR RETRY;
```

- **The SQL is the node's own, unchecked.** It is one literal and may hold several statements. It is stored as
  written and run as the node on a connection of its own, unprefixed, outside acl: nothing reads it, no lint, no
  rule. So write no credential in it - name a secret of the secrets service instead (`SECRET s`). Its text is in
  `platform.cluster_items` for whoever reads the profile.
- **Who writes it**: the `cluster` bundle, as every profile item.
- **Its place** in the boot. The profile's extensions and settings come first, then:
  1. `BEFORE SOURCES`;
  2. each source's ATTACH, followed at once by its `AFTER SOURCE <alias>` scripts, which run only if it attached;
  3. `AFTER SOURCES`.

  Inside a place, scripts run in their names' order: name them to order them (`10_…`, `20_…`; a name that starts
  with a digit is written bare).
- **A script after a source runs at every attach of it**: a lazy source's attach on demand (before the statement
  reads it), its attach again after an idle detach, a re-pointed source's new attach.
- **What it knows**: the node sets `getvariable('acl_node_group')` (its resource group) and
  `getvariable('acl_node_label')` on the script's connection first. One cluster-wide script can then do a group's
  part in its conditions (DuckDB has no `IF`): `… WHERE getvariable('acl_node_group') = 'etl'`. `IN GROUP` is the
  other way.
- **What it leaves**: what a connection holds (a `USE`, a variable, a plain `SET`) goes with the script's
  connection. A boot script changes the node's global state (`SET GLOBAL`, before step 7 locks the configuration),
  the attached databases, and the sources.
- **`ON ERROR`**, the script's own policy:

  | policy | before / after the sources | after a source |
  | --- | --- | --- |
  | `FAIL NODE` (default before and after the sources) | the bootstrap stops, naming the script; the node never serves | the same at boot; once the node serves, it holds the source as `DETACH SOURCE` does |
  | `CONTINUE` | logged, and the boot goes on | the source stays attached, as the script left it |
  | `DETACH SOURCE` (default after a source) | refused | the source is detached and the node serves without it until it or its scripts change in the profile (then the next tick attaches it again) or the node restarts; a lazy source's read is refused (`could not be attached (its script failed)`) and the next read tries again; a source that `DEPENDS ON` it is held with it |
  | `RETRY` | after the sources only (refused before them: nothing serves on an initialisation still pending); the place's other scripts run on, the node serves, and the tick retries it from 10 s, doubling, up to 5 min | the source stays detached, and each try attaches it and runs the script again |

  The log names every run, `hugr_node: script <name> (<place>) ran | failed (<class>)`, never its text or its error's
  message. A script failing now is listed by the heartbeat as pending, class `error`, which never holds back the
  profile's version as applied. On a serving node an eager source the tick attaches is visible from its ATTACH until
  its scripts end; a lazy source's attach and scripts run under the lock its readers wait on.
- **It is the node's power**: whoever writes scripts (the `cluster` bundle) can, through one, do anything the node
  can - grant themselves `passthrough`, write the policy. Give that bundle to whom you would give passthrough.
- **A changed script applies at a restart.** A new, changed or dropped script is a restart item, like a setting:
  running nodes keep what they ran, and the node reports it pending until it restarts (the orchestrator rolls it).
- **A source holds the scripts that follow it**: `CLUSTER DETACH` of it is refused while one does, and `CASCADE` drops
  them with it.
- **A session's script** is the policy's, not the profile's: a resource group's, a role's or a role's grant of a
  group, written by the `policy` bundle (`ALTER RESOURCE GROUP | ROLE … SET SESSION SCRIPT`, duckdb-acl spec 181).
  `CLUSTER ADD SCRIPT … ON SESSION` is refused and says so.

### The bootstrap, one listing

`platform.bootstrap_order()` (the `cluster` bundle's; `acl_cluster_bootstrap_order()` for the node's operator) lists
a node's whole bootstrap in its order, as `(pos, step, sql)`. Step 6 is expanded:
- the profile's extensions and settings (an engine setting cut to the pod, marked);
- the scripts before the sources;
- each source, followed by its scripts (a lazy source marked as attached on demand);
- the scripts after the sources.

Each script row names its place and its `ON ERROR`, and carries its SQL; a lazy source's scripts are marked "at its
attach". The listing shows the profile as it is now - a node runs what it held at its boot, until it restarts. The other steps are the node's own, from the
document it booted with (names and paths only, never a credential). A document given as the argument is listed
instead, and a node that did not boot (a test) lists step 6 alone. The bootstrap walks the same order, so the listing
is what a node runs.

```sql
SELECT pos, step, sql FROM platform.bootstrap_order();
```

### What a statement does

A statement does four things:

1. It checks the change. A refusal writes nothing.
2. It writes the item and bumps `config_version`, in one catalog transaction.
3. It applies the change **on this node** when that can be done live.
4. It answers `(ok, message, version, class, applied_here)` - every management statement's `(ok, message)`
   (duckdb-acl spec 179), then the item's own fields; `message` says how the change reached this node.

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
  key (`ENCRYPTION_KEY` too). A postgres, mysql, mssql or hugr ([another platform](federation.md)) source - by its
  `TYPE`, or by its path's prefix without one (`'postgres:dbname=…'`) - must name a `SECRET`, and the node resolves
  it from its secrets service. The refusal names the key, never its value.
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
  `acl_flight_stream_idle` / `acl_flight_ingest_idle`, the load report's critical level `acl_node_admit_memory`,
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

