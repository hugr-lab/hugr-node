# Spec 048: the lock leaves a client its session's settings

- **Status**: implemented
- **Date**: 2026-11-28
- **Author**: found by hugr-platform's demo check 21 (spec 016); the owner (2026-11-28): the time zone is a session
  parameter, and it comes from the client.

## Summary

Step 7 of the bootstrap locks the node's configuration. The lock left out what duckdb-acl's spec 068 lets a client SET
on its own session, so a platform node refused a client's `SET TimeZone`, a Flight `SetSessionOptions` of it, and the
trace context (`acl_correlation_id`) with "the configuration has been locked". Step 7 now first sets duckdb's
`allowed_configs` to acl's one list of those settings, then locks.

## Problem

- duckdb's `lock_configuration` refuses every `SET` but `schema` and `search_path`, at any scope, unless the setting is
  in `allowed_configs`.
- hugr-node locked without `allowed_configs`. So acl admitted a principal's `SET TimeZone = 'Asia/Tokyo'` on its
  session, and duckdb then refused it.
- The same held for every setting on spec 068's list: `TimeZone`, `Calendar`, `acl_metadata_max_rows` (a lower bound
  only), `acl_correlation_id`, `acl_traceparent`, `acl_lineage_parent`, `acl_lineage_root_parent`, `acl_lineage_job`.
  A moved session's restore (duckdb-acl spec 135) sets them through the same path, so it would refuse them too.
- Found when hugr-platform's demo used a session script that set the zone (duckdb-acl spec 183's trial reported the
  refusal). A script is the wrong place for a zone: the zone is the client's. The bug is that the client could not set
  it.

## Design

- Step 7 is two statements:
  1. `SET GLOBAL allowed_configs = ['TimeZone', 'Calendar', …]`, the list being acl's `PORTABLE_SETTINGS`
     (`acl_session_state.hpp`). It is the one list acl's `ClientSettingAllowed` reads and a moved session carries.
     hugr-node keeps no list of its own.
  2. `SET GLOBAL lock_configuration = true`.
- **Nothing more is opened.** Every other setting stays locked, the node's own included. A session script (duckdb-acl
  spec 181) runs on the locked connection like the client does, so it may set the same settings and no others.
- `allowed_configs` is the node's hardening. It is not a profile item (`HARDENING_SETTINGS`): a profile item could
  otherwise open any setting after the lock.

## Security

- duckdb's lock has no scope: a setting in `allowed_configs` may also be `SET GLOBAL`. A principal cannot do that,
  because acl refuses any scope but the session's (spec 068). Only SQL that runs as the node can: a boot or session
  script, or `ACL NATIVE`. Each of those already holds the node's power. The settings on the list decide how a value is
  rendered, what a statement's trace and lineage events name, and a lower metadata bound. None decides what a
  statement reads or what it costs.
- The list grows only by a duckdb-acl spec (068's rule), and this node takes it at the re-pin.

## Tests

- `test/sql/agent/hugr_node_bootstrap.test`: the plan's step 7 is the two statements, the list as acl writes it.
- `test/sql/cluster/acl_cluster_profile.test`: `CLUSTER SET allowed_configs` is refused as hardening.
- `test/e2e/bootstrap/run.sh`: on a node that bootstrapped and locked, a client (ADBC, its session kept by the cookie)
  sets `TimeZone` and `acl_correlation_id`, and a `TIMESTAMPTZ` renders in its zone.
- Without the fix, the first two fail. The e2e's SET was refused before this spec (hugr-platform demo check 21's
  finding).
- hugr-platform's demo check 21 sets the client's zone through Flight's `SetSessionOptions` on a platform node.

## Alternatives considered

- **Leave the zone to a session script.** The zone is the client's. A script would make every client of a role render
  in one zone.
- **Lock nothing.** The lock keeps the node's trust settings fixed after the bootstrap. Keep it.
- **A list in hugr-node.** Two lists drift. acl's is the one its doors and a moved session read.
