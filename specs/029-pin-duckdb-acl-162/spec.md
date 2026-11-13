# Spec 029: duckdb-acl after specs 160 and 162

- **Status**: implemented
- **Date**: 2026-11-13

duckdb-acl moves from 670d50f (spec 028) to 8977ade.

- **Spec 160** (#294): a table a policy text names is stored whole, `<db>.<schema>.<table>`, where it is written; a
  form not rewritten in place is refused. `acl_check_catalog` adds `body_table_unqualified`.
- **Spec 162** (#295): a principal's own view keeps its policy reads as CTEs at the body's root.

No new translation unit; tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp`, all unchanged.
