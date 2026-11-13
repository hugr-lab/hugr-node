# Spec 028: duckdb-acl after specs 157, 159 and 161; tresor with them

- **Status**: implemented
- **Date**: 2026-11-13

duckdb-acl moves from 4d5685d (spec 027) to 670d50f, and tresor to 8cdd8c3 (its #90). Both now carry
duckdb-ext-common v0.16.0, so their OIDC headers match.

- **Spec 157** (#291): a principal's `DELETE`, `UPDATE` or `MERGE` under a column policy first binds the role's read
  of its target, and refuses when that read does not bind. `acl_check_catalog` names an object grant left with no
  column (`grant_exposes_nothing`).
- **Spec 159** (#292): every policy read is bound alone before the statement is optimized. A name its source lost
  refuses instead of binding to a column of the principal's query. The checks are keyed by the read's content.
  Plans and a scanner's pushdown are unchanged.
- **Spec 161** (#293): duckdb-ext-common v0.14.0 → v0.16.0. `ClientAuth::assertion_type` is new, and the contracts
  are unchanged.

No new translation unit; the cluster rules and this repository's sources are unchanged.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp`, all unchanged.
