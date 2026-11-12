# Spec 026: duckdb-acl after spec 150 stage 4e

- **Status**: implemented
- **Date**: 2026-11-12

duckdb-acl moves from 7711f26 (spec 025) to 705dc3c, its spec 150 stage 4e (#288).

- **A grant's column list is a rule now.** In `COLUMNS (…)`, each item is a column or a path whose every part is an
  identifier as duckdb reads one (quote a keyword), plus an optional mask, which is duckdb's own expression. List and
  struct literals, `MAP` and lambdas are accepted, and the mask is stored as written.
- **A stored list is split on duckdb's tokens.** The split is `SplitListItems`, a new translation unit
  `src/acl_list_items.cpp`; it reaches this build through `acl_sources.cmake`. A column the list would not read back
  bare is stored quoted.
- **Rows stored before 4e read as before.** A text holding a comment, a `$…$` / `E'…'` string or a backtick name is
  read the old way.

This repository writes relation column lists only (`CREATE VIRTUAL TABLE … COLUMNS (id, tenant)`). Those are
unchanged until 4f. ext-common, quack and tresor do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` and `make test-cpp`, all unchanged.
