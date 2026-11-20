# Spec 035: a resource group's sources, and sources attached on demand

- **Status**: accepted (the owner, 2026-11-16); stage 1 (the source set and the partial node) and stage 2 (the lazy
  sources) implemented 2026-11-20
- **Date**: 2026-11-16
- **Author**: Claude, from the owner's decisions of 2026-11-16
- **Follows**: duckdb-acl spec 093 (the cluster profile: `ACL CLUSTER ATTACH … [IN GROUP g]`), spec 096 (a resource
  group is a set of nodes), spec 136 (a listing marks a source `not attached`), this repository's spec 010 (the tick)
  and hugr-platform spec 011 (a group scales down to 0)

## Summary

Two ways a node keeps fewer sources attached, so that its memory goes to the queries it serves:

1. **A group's source set.** A resource group declares the sources its nodes attach (`ACL CLUSTER SET SOURCES (…) IN
   GROUP g`); its nodes attach only those. Such a node serves its group and nothing more: policy writes, the catalog
   check, ANALYZE and the write-time probes run on nodes that attach every source.
2. **A lazy source.** A source marked `LAZY` is not attached at bootstrap; a node attaches it on the first query that
   reads it, and detaches it after an idle time: `ACL CLUSTER ATTACH … LAZY IDLE '30 min'`.

## Problem

- Every node attaches every cluster source (spec 093, except `IN GROUP` items). Each attached source costs memory (a
  scanner's connection pool, its catalog cache) on nodes whose roles never read it - an ETL group attaches the BI
  sources, and the other way round.
- `IN GROUP g` adds a group's own sources; nothing takes the cluster's sources away from a group.
- With nodes that start and stop on demand (hugr-platform spec 011), a big source attached at every start delays every
  start.

## Design

### A group's source set (the owner: it is declared, never guessed)

```sql
ACL CLUSTER SET SOURCES (sales, crm) IN GROUP etl;      -- etl's nodes attach only these (cluster or group items)
ACL CLUSTER RESET SOURCES IN GROUP etl;                 -- back to every source of the cluster and of the group
```

- A cluster profile item of a new kind, `sources`, scoped to one group (a cluster-wide one is refused: the cluster
  attaches every source). The names are the aliases of `ATTACH` items, the cluster's or the group's; one that names no
  such item is refused where written.
- **What a group's node attaches**: with a `sources` item, only the listed aliases; without one, as today. The tick's
  convergence (spec 010) reads it like any item: a source left out is never attached on the group's nodes; a listed one
  is attached hot; a change of the list detaches what left (a drain item when in use, as a dropped source is).
- **A suggestion from the policy, never applied by itself**: `hugr_node_group_sources(g)` answers the physical
  databases the roles holding group g can reach (their grants' objects' physical names, live schema aliases, the
  databases a view's, a template's or an RLS's tables name - spec 138's `ResolvedReference`, as the source gate reads
  them), for the operator to write the list from; `acl_check_catalog` gains `group_source_missing`: a role of a group
  with a `sources` item reaches a source the list leaves out (every read of it by the group's principals refuses).

### A node with a partial set only serves

A node whose group has a `sources` item attaches only part of the cluster's sources. What judges a stored fact
against the sources would answer wrongly there (a `source_missing` for a source the group never attaches, a grant's
predicate refused because its table is not attached), so such a node does not run it:

- **Refused there**, with where to run it: every policy write (`ACL ADMIN …` that writes the policy, the `acl_*`
  writers), `acl_check_catalog`, `ANALYZE VIRTUAL …`, `acl_refresh_schema*`, the write-time probes they run - `acl:
  this node attaches the sources of group "etl" only - run it on a node of a group that attaches every source`.
  Reads of the policy (listings, `platform.*` views) and the group's queries run as usual.
- **The seam** (duckdb-acl): `PolicyStore::NodeTraits` gains `partial_sources` (set by this node part from the
  profile), and the authorizer refuses the calls above on it (`PlatformRight` POLICY / CATALOG writes and the check's
  read functions). The front (hugr-platform spec 002) may route such a statement to the default group itself - a
  follow-up, not this spec.
- **Listings** mark a source outside the group `not attached` (duckdb-acl spec 136: absence, never counted nor audited).

### A lazy source

```sql
ACL CLUSTER ATTACH 'postgres:…' AS archive (TYPE postgres, SECRET pg_archive) LAZY IDLE '30 min';
ACL CLUSTER ATTACH 's3://…/lake.ducklake' AS lake (TYPE ducklake) LAZY;    -- no IDLE: attached until the node stops
```

- `LAZY` and `IDLE '<interval>'` are options of the `ATTACH` item (the grammar's, after the options list). The
  interval is read by duckdb's own `INTERVAL` parsing (`CAST('30 min' AS INTERVAL)`), never by a reader of ours; it
  is at least 1 minute.
- **At bootstrap and on a tick** a lazy item is not attached; the drift shows it `lazy` (not `profile_only`), and it
  counts as applied.
- **On demand**: when the rewriter of a statement resolves a physical name in a database that is not attached
  (`NotePhysical`, duckdb-acl), it asks the seam `PolicyStore::NodeTraits::attach_on_demand(db)`; the node part
  attaches the item's ATTACH on its own connection (the same statement `ClusterItemApplyStatements` builds) before the
  statement binds, under a lock per source (one attach for many concurrent statements). A failure refuses the statement
  with the source's error class (the source gate's `source_error`), never its text. A principal's statement can only
  wake a source its policy already names: the rewriter asks for a database only after the policy resolved the object.
- **Idle detach**: the node part notes a use per lazy source at each statement that resolved into it (`QueryBegin`)
  and keeps the set of lazy sources each connection's open transaction touched (cleared at commit / rollback). The
  tick detaches a lazy source whose last use is older than its `IDLE` and that no open transaction and no running
  statement holds; a source in use is left for the next tick. Its catalog caches go with the DETACH; the next statement
  attaches it again.
- **Listings** of a principal (duckdb-acl spec 119) read the physical facts only of the sources a listing names; a
  lazy source not attached is listed `not attached` (spec 136) rather than attached for a listing - a listing never
  wakes a source.

### The profile

`cluster_items` (duckdb-acl schema) already keeps an item's spec as JSON; `lazy` and `idle_seconds` are fields of an
ATTACH item's spec, `sources` a new kind with its list. No schema step: the items are text.

## Enforcement & security

- **Nothing is attached a principal could not read anyway**: an on-demand attach follows the policy's resolution of a
  granted object; the source gate (spec 118.3, 138) is unchanged.
- **A partial node writes nothing the policy keeps**, so no write judged against missing sources lands in the
  catalog.
- **A source is never detached under a reader**: an open transaction or a running statement holds it.
- **The interval is duckdb's**: no hand-written duration reader.
- `ACL NATIVE` (the break-glass) is unchanged: it may attach and detach by hand; such a database is drift, shown.

## Testing

- sqllogictest (`test/sql/cluster/`): `SET SOURCES` / `RESET SOURCES` written and refused (cluster-wide, an unknown
  alias), `LAZY` and `IDLE '30 min'` (an interval duckdb reads, one under a minute and `'soon'` refused), the drift's
  `lazy`, `hugr_node_group_sources(g)`, the check's `group_source_missing`;
- the partial node: a policy write, `acl_check_catalog`, ANALYZE refused with the message; reads served;
- C++ (`test_hugr_node_tick.cpp`): the convergence with a `sources` item (what attaches, what detaches), the idle
  detach decision (last use, open transactions, running statements);
- e2e (`test/e2e/bootstrap`): a lazy source attached by the first query that reads it, detached after its idle time,
  attached again by the next; a group with `SET SOURCES` attaching only its list.

## Open questions

1. Whether the front should send an administrator's session to a node with every source by itself (hugr-platform spec
   002). The front does not read statements, so it could only place a whole session; proposed: no - an
   administrator's role belongs to a group whose nodes attach every source, and the refusal says so.
2. Whether `hugr_node_group_sources` should also suggest the lazy marks (sources few roles read).

## Decisions (owner, 2026-11-16)

- The source set is declared, never derived by itself; the checks against the sources never run on a partial node.
- Lazy only for sources marked so; `LAZY` without `IDLE` is never detached.

## Alternatives considered

- **Derive the set from the policy at every bootstrap.** The owner's point: what a group's nodes need is declared
  clearly; a policy change would otherwise attach and detach sources by itself. The derivation stays a suggestion.
- **Every source lazy.** The owner: only specific ones - a first query paying an attach is acceptable for an archive,
  not for the sources every query reads.

## Stage 1 as built (2026-11-20)

- `CLUSTER SET SOURCES (…) IN GROUP g` / `CLUSTER RESET SOURCES IN GROUP g` compile to `acl_cluster_sources(verb,
  scope, names_json)` (`platform.cluster_sources`, INFRASTRUCTURE). The item is kind `sources`, name `sources`, spec
  `{"names": [...]}`, with the aliases as their items write them. A listed source that `DEPENDS ON` another needs
  that one listed too. The rule is checked when the set is written, when an `ATTACH` re-points a listed source, and
  when a `DETACH` of a group's own listed source hands its nodes the cluster's. Each check reads the item the group
  reads: its own, else the cluster's. An `ATTACH` a set leaves out is written, its answer names those groups, and a
  node of such a group does not attach it.
- `ClusterEffective` drops the `sources` item and every source the set leaves out, so the drift, the tick and the
  agent's bootstrap read the narrowed profile as they read any other.
- The partial node is duckdb-acl spec 176's seam (`NodeTraits::partial_sources`: the node's group when it declares a
  set). It refuses every compiled management call except OPERATE / OPEN and view-as, plus `platform.check_catalog` /
  `check_grant`. The trait is read only for a batch that writes.
  The operator's own SQL on the node (its bootstrap's direct `acl_*` calls) and `ACL NATIVE` are unchanged. A
  principal's own DDL record (duckdb-acl spec 113) is not refused: it records an object in a source the node holds.
- Tests: `test/sql/cluster/cluster_group_sources.test`, and the bootstrap e2e's step 11b (a source left out is
  detached by both nodes, and attached again on RESET).

## Stage 2 as built (2026-11-20)

- `CLUSTER ATTACH … LAZY [IDLE '<interval>']`. The rule is `AclClusterLazy <- 'LAZY' AclClusterIdle?`, after the
  options list, and it compiles to `acl_cluster_attach`'s two last arguments, `lazy` ('true') and `idle`.
  - The interval is read by duckdb (`epoch(CAST(… AS INTERVAL))`). One under a minute, or one that does not read, is
    refused. `IDLE` without `LAZY` is refused too.
  - The spec carries `"lazy": "true"` and `"idle_seconds": "<n>"`.
  - A re-write that changes only the marks is no re-point (`ClusterSourceIdentity`: the spec without them). The tick
    compares a source by that identity as well.
- **Not attached by the profile.** The bootstrap (`ProfileItemStatements`) and the writing node's hot apply skip a
  lazy source. The drift says `lazy` for one not attached, and the tick treats `lazy` as applied.
- **The registry** (`LazySources`, per instance in its object cache) keeps, per lazy alias, its item, its idle time,
  its last use and a lock. It is refreshed from the effective profile at the bootstrap, at every tick and after this
  node's own ATTACH / DETACH writes; the other nodes take a change at their next tick.
- **Attached on demand.** duckdb-acl spec 177's traits read the registry. `attach_on_demand` takes the source's lock
  (never with the registry's: a slow ATTACH must not stall every parse), notes the use, and runs the item's ATTACH on
  a connection of its own when the database is not there.
  - A failure throws `acl: a source this statement reads could not be attached (<class>)`; the node's log names the
    source, the principal is not told it.
  - An ATTACH that fails because another path attached it meanwhile is success.
  - What it attached is an applied-here note, so the tick takes it as the node's (the bootstrap takes no lazy item):
    a profile that drops it detaches it, one that re-points it leaves it pending a drain.
- **The idle detach** runs in the tick (`DetachIdleLazySources`, the decision `DecideIdleDetach` in
  hugr_node_tick.hpp). It detaches an attached source with an `IDLE` whose last use is older than it, unless a
  transaction holds the database. "Holds" is duckdb's own rule: the manager's reference and ours are all there is
  (`use_count() > 2` means a running statement or an open transaction still uses it). It runs under the source's
  lock, so an attach and the detach never cross; the tick only tries that lock, so a source being attached (in use)
  is left for the next tick and never holds up the heartbeat. Only a source the tick took is detached - a database
  attached by hand under the alias is drift - and the detach drops it from what the tick took. The tick takes the
  applied-here notes before it reads the drift, so a note left after that read is the next tick's, never lost.
- `CLUSTER DETACH` of a lazy source takes it out of the writing node's registry before its DETACH, so no statement
  attaches it again in between.
- **Refused where written** (the review): `LAZY` with `DEPENDS ON`, a source depending on a lazy one, marking lazy a
  source others depend on (a lazy source attaches alone), a lazy `tresor`, `IDLE ''`, a `lazy` argument other than
  `'true'` / `'false'`.
- **Who it is for** (the owner, 2026-11-20): groups whose nodes run known statements (an application, an ETL job),
  not analysts or interactive users, whose tools browse the catalog first.
- **Known limit.** A listing never wakes a source, so an object read live from a lazy source that is not attached is
  listed without columns. A client that loads the catalog whole (quack) sees it so; such a source's objects should
  declare their columns.
- `hugr_node_group_sources(g)` and the check's `group_source_missing` are stage 3.
- Tests:
  - `test/sql/cluster/cluster_lazy_sources.test`: the refusals, the spec, the drift's `lazy` / `same`, the policy
    write that attaches the source, a principal's read that attaches it again, a listing that does not, the marks
    changed without a re-point, and no longer lazy = attached;
  - `test_hugr_node_tick.cpp`: the tick's `lazy` state and the idle decision;
  - the bootstrap e2e's step 17b, over Flight with an administrator's token: node b's bootstrap skips the source,
    node a attaches it for a policy write and for a read, detaches it after its idle minute, and a read attaches it
    again.
