# Spec 027: duckdb-acl after spec 150 stage 4f and spec 156

- **Status**: implemented
- **Date**: 2026-11-12

duckdb-acl moves from 705dc3c (spec 026) to 4d5685d: its spec 150 stage 4f (#289) and spec 156 (#290).

- **An object's own column list is a rule** (4f). Each item of `CREATE VIRTUAL TABLE … COLUMNS (…)` (and of `REMAP`)
  is `name`, `name = <expression>` or `<expression> AS name`, with an optional `NULL` / `NOT NULL` mark; a name is
  read as duckdb reads an identifier. One reader splits a stored list on duckdb's tokens, used by the function forms,
  `REMAP` and the body gate. The `AS` form used to escape its author's function gate, so a catalog admin could reach
  passthrough through it; that gap is closed here.
- **A grant that leaves no column reads and writes no column** (156). When a role's grants narrow an object's columns
  to none, every read, write and upsert of it is refused; before, such a role read, wrote and returned every physical
  column. `platform.effective_rights` says why, and `REPAIR … DROP MISSING COLUMNS` no longer empties a list.

Both gaps were open in this build until this pin. The relation lists this repository writes
(`COLUMNS (id, tenant)`) read as before. No new translation unit; ext-common, quack and tresor do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` and `make test-cpp`, all unchanged.
