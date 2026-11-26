# Spec 047: the administrator's SQL at a node's boot and at a session's start

- **Status**: accepted (2026-11-26, the owner's answers below)
- **Date**: 2026-11-26
- **Author**: the owner (2026-11-26): arbitrary SQL the administrator writes and the node runs without acl's checks,
  ordered against the sources; the failure policy is the administrator's. The answers of the same day are folded in.

## Summary

The cluster profile holds extensions, sources, settings and a group's source set. It gains **scripts**: SQL text,
written by the `cluster` bundle, that the node runs as itself, outside acl. Each script has a place:
- at a node's **boot**: before the sources, right after one source attached (and only if it did), or after all of
  them;
- at every **session's start**, on the session's own connection, before its first statement.

Each script has its own policy for a failure: fail the node, go on, detach its source, or retry.

## Problem

- **Real deployments need setup that is not a setting.** A script might:
  - define a macro or a table in an attached database;
  - set a source's own parameters once it is connected, or run a command in the source (`postgres_execute`, a
    `CALL` of the source's) before anything reads it;
  - put a session's variables or default schema in place.
- **The profile cannot say it.** Its items are typed, and a source's ATTACH is the last thing it runs for a source.
- **Boot and session state are different things in DuckDB.** `USE`, `SET VARIABLE` and a plain `SET` live on one
  connection. A script run once at boot on the node's own connection changes nothing that a client's session sees. A
  session's state needs a script run on that session's connection.

## Design

### The items

```
CLUSTER ADD SCRIPT <name> <place> AS '<sql>' [ON ERROR <policy>] [IN GROUP <g>] [COMMENT '…']
CLUSTER ALTER SCRIPT <name> [<place>] [AS '<sql>'] [ON ERROR <policy>] [IN GROUP <g>]
CLUSTER DROP SCRIPT <name> [IN GROUP <g>]

<place>  := BEFORE SOURCES | AFTER SOURCE <alias> | AFTER SOURCES | ON SESSION
<policy> := FAIL NODE | CONTINUE | DETACH SOURCE | RETRY        -- ON SESSION: FAIL SESSION | CONTINUE;
                                                                -- BEFORE SOURCES: no RETRY
```

- **The SQL is one literal**, as acl's expression texts are (duckdb-acl spec 180). It may hold several statements, and
  it is stored and run as written. Nothing of ours reads it: no lint, no acl check (the owner).
- **A script is a profile item** of kind `script`, in `cluster_items`, like any other. It applies to the cluster or to
  one group (`IN GROUP`), and a group's script of the same name overrides the cluster's.
- **`AFTER SOURCE <alias>` depends on that source**, as `DEPENDS ON` does: the source must be in the profile, and
  dropping it is refused while a script follows it (or `CASCADE` drops both).
- **Who writes**: the `cluster` bundle (the owner). The texts are read in `platform.cluster_items` by those who read the
  profile.
- **Order inside a place**: by name. The administrator names scripts to order them (`10_vars`, `20_macros`).

### At boot

The bootstrap's step 6 applies the profile. Scripts join it in their places:
1. extensions, settings;
2. `BEFORE SOURCES` scripts;
3. each eager source's ATTACH, then its `AFTER SOURCE` scripts, in the profile's order;
4. `AFTER SOURCES` scripts.

- **It knows which node it prepares** (the owner, 2026-11-26), for its conditions. Before the script runs, the node sets
  `getvariable('acl_node_group')` (the node's resource group, `acl_node_group`) and `getvariable('acl_node_label')`
  (its label) on that connection. One cluster-wide script can then do a group's part:
  `… WHERE getvariable('acl_node_group') = 'etl'`. A script `IN GROUP g` is the other way.
- **Each script runs on its own connection of the node's**, unprefixed: as the node, outside acl. That connection's
  own state (a `USE`, a variable) goes with it, which is why session state is `ON SESSION`'s.
- **A lazy source** (spec 035) attaches on a statement's demand. Its `AFTER SOURCE` scripts run right after that
  attach and before the statement reads it, and again at every later attach (after an idle detach, after a re-point).
- **A source re-pointed by a drain** (a new item of the same name) runs its scripts again on the new attach.

### On failure (the script's `ON ERROR`)

| policy | at boot | after a source |
| --- | --- | --- |
| `FAIL NODE` (default for `BEFORE` / `AFTER SOURCES`) | the bootstrap fails and the node never serves: the pod is not ready, and its log names the script | the same |
| `CONTINUE` | a log line and an event, then boot goes on | the source stays attached, uninitialised: the administrator's choice |
| `DETACH SOURCE` (default for `AFTER SOURCE`) | not allowed | the source is detached and the node serves without it. Its readers get the "could not be attached" refusal of a lazy source; the next tick tries again |
| `RETRY` | `AFTER SOURCES` only (refused for `BEFORE SOURCES`, the owner: nothing serves on an initialisation still pending): the node serves, and the script is retried at each tick with a backoff up to 5 min until it passes | the source is detached until its script passes, and each try attaches it and runs the script again |

- **Each run is a log line**: `hugr_node: script <name> (<place>) ran | failed (<class>)`. It never carries the text,
  which may name things, nor the error's message, which a source's may fill with its connection string.
- **The heartbeat carries what fails now** as pending items of class `error`: a source a script holds detached, an
  `AFTER SOURCES` script awaiting its retry. A restart does not mend them, so they do not count toward
  `needs_restart`. The group's status showing them is hugr-platform's part (stage 4).
- **Once the node serves**, `FAIL NODE` after a source cannot stop it. At a later attach (the tick's, a lazy one) it
  holds the source detached, as `DETACH SOURCE` does.
- **`RETRY`'s backoff** starts at 10 s and doubles, up to 5 min.

### When a changed script runs

- **At restart** (the owner): a new, changed or removed script is a restart item, like a setting. Running nodes keep
  what they ran. The group's status says `needsRestart`, and the console's restart rolls it.
- **An `AFTER SOURCE` script also runs at each attach of its source** (lazy, re-point), with its current text. That is
  the source's lifecycle, not the script's change.

### At a session's start (`ON SESSION`)

- It runs on the session's own connection, right after the session opens and before its first statement. This holds
  for every door: a Flight session's connection and a quack connection's. It runs once per session, and again on a
  session's takeover on another node (duckdb-acl spec 135).
- **It runs unprefixed, as the node, with full rights** (the owner, 2026-11-26: "полные права - это решения админа").
  It may set whatever the connection holds, acl's per-session state included:
  - a virtual `USE` through `acl_session_use` (duckdb-acl spec 114);
  - a setting outside spec 068's list;
  - variables, a physical `USE`.

  A principal's own statements stay judged by acl as always: what the script set is the session's starting state,
  not a right.
- **It knows whose session it prepares** (the owner, 2026-11-26). DuckDB has no `IF`, so a script branches in its
  conditions. Before the script runs, the node sets these variables on the session's connection:
  - `getvariable('acl_session_roles')` (`VARCHAR[]`): the principal's roles;
  - `getvariable('acl_session_groups')` (`VARCHAR[]`): the resource groups those roles are in;
  - `getvariable('acl_session_group')` (`VARCHAR`): the group the session is charged to, NULL if none;
  - `getvariable('acl_session_subject')` (`VARCHAR`): the token's subject, or the role for a gateway's principal.

  A script then sets what fits:

  ```sql
  SET VARIABLE region = (SELECT 'north' WHERE list_contains(getvariable('acl_session_roles'), 'analyst_a'));
  ```

  The duckdb-acl seam hands the hook these with the connection: what `SessionOpenBody` resolved, never a token.
- **On failure**: `FAIL SESSION` refuses the session, and the client gets "the session could not be prepared",
  naming the script. `CONTINUE` logs it and serves.
- **It needs a seam in duckdb-acl**: a session-opened callback with the session's connection, in `NodeTraits`, as
  `attach_on_demand` is (duckdb-acl spec 177). That is a duckdb-acl spec of its own, before this part.

## Security

- **A script is the node's own power** (the owner), outside acl. Whoever writes the profile holds it: the `cluster`
  bundle. That bundle already decides what the nodes attach and which extensions they load, but a script can do more:
  - read any attached source as the node;
  - change the node's global state, though `lock_configuration` is set after step 6, so a boot script's `SET GLOBAL`
    lands before the lock and a later one is refused.
- **A session script runs on a principal's connection** with the node's rights. It can change what that connection
  holds before the principal's first statement, and it is written by the `cluster` bundle, not by the principal.
- **No secret in the text.** The profile's lint does not apply to a script (the owner: unchecked), so the docs say
  it: credentials go into the secrets service, and a script names them (`SECRET s`).
- **Every run is an event**; a failing script is in the status. Nothing a script does is hidden from the audit.

## Testing

- **Unit:** the grammar's forms and refusals:
  - a place naming a source the profile lacks;
  - `DETACH SOURCE` on a boot place;
  - `FAIL SESSION` on a boot place;
  - a script after a source dropped without `CASCADE`.
- **SQL tests:**
  - scripts in their places and their order at a bootstrap;
  - each `ON ERROR` policy (a script that fails on purpose);
  - an `AFTER SOURCE` script at a lazy attach and again after its idle detach.
- **e2e:** a session script's variable read by the session's statement, through both doors.
- **hugr-platform's demo:** a script after the source `sales` sets something the check reads, and a restart applies a
  changed script.

## Decisions (the owner, 2026-11-26)

1. **A session script has full rights.** It runs as the node, and what it sets is the administrator's decision.
2. **No `RETRY` for `BEFORE SOURCES`.**
3. **The defaults stand:**
   - `FAIL NODE` at boot;
   - `DETACH SOURCE` after a source;
   - `FAIL SESSION` for a session.
4. **Written by the `cluster` bundle.** A changed script applies at restart. The four policies are offered.

## As built (stage 1, 2026-11-26)

- **The verbs are `ADD` / `ALTER` / `DROP SCRIPT`.** `UPDATE` is `CLUSTER UPDATE EXTENSION`'s, and a form's verb is
  its own.
- **A script's name may start with a digit** and is written bare (`10_vars`), so names can order scripts.
- **`ON SESSION` is refused** until stage 3.
- **The code:**
  - `acl_cluster_script(verb, scope, name, place, source, sql, on_error, comment)` and `platform.cluster_script`,
    the `cluster` bundle's;
  - the scripts' registry, refreshed with the lazy sources;
  - `RunClusterScripts` in the bootstrap's step 6, the tick's hot apply, a lazy source's attach and the write's hot
    apply;
  - `DecideTick` lists a script the node did not run as pending a restart;
  - `DETACH` is refused while a script follows the source, and `CASCADE` drops the script with it;
  - the drift skips scripts.
- **Tests:**
  - `test/sql/cluster/cluster_scripts.test`: the forms and refusals; `DETACH` and `CASCADE`; no drift; a lazy
    source's script at each attach, with `acl_node_group`; `DETACH SOURCE` refusing the read; `CONTINUE`;
  - the bootstrap e2e: the three boot places in order, the last reading what the one after the source wrote; a
    `FAIL NODE` script failing stops the boot.
- **The bootstrap, one listing** (the owner, 2026-11-26: the administrator sees the scripts and the boot's order in
  one place):
  - `platform.bootstrap_order([document])`, the `cluster` bundle's (`acl_cluster_bootstrap_order`), lists
    `(pos, step, sql)`;
  - step 6 is expanded by `WalkProfileOrder`, the one function the bootstrap walks too;
  - the other steps come from the document the node booted with (`BootRecord`).

  `hugr_node_profile_statements()` follows the same order.
- **Docs**: `docs/cluster.md`, "Scripts" and "The bootstrap, one listing".

## Stages

1. Boot scripts (this repository alone): the items, the grammar, the bootstrap's places, the lazy attach, the
   policies, the events and the status.
2. duckdb-acl: the session-opened seam (`NodeTraits`), a spec there.
3. Session scripts, on that seam.
4. hugr-platform: the console's profile form for scripts, and a demo check.
