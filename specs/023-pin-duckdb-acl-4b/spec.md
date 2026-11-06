# Spec 023: duckdb-acl after spec 150 stage 4b

- **Status**: implemented
- **Date**: 2026-11-06

duckdb-acl moves from 4acccd7 (spec 022) to 7eede0b, its spec 150 stage 4b (#285).

- **The limits are a rule now.** A resource group's limits `(window_max 64, batch_bytes '32MiB', …)` are read by grammar rules, not a text reader. Each item is the limit's name (a word), an optional `=`, and one value. The value is one of:
  - a number and its unit written together (`32MiB`, `.5GB`);
  - a signed number;
  - a quoted string. A value with a space is quoted.
- **Function fixes.** The limits function refuses with the limit's name what used to escape it:
  - a size past int64, which raised an internal error that invalidated the database;
  - a JSON integer past int64, which wrapped;
  - `std::stoll`'s exceptions.
- **`MgmtCall::Text`.** It keeps its behaviour for this repository's two calls (`AclValue`), and still refuses a part that is no value.
- **The usage lines are unchanged.** This repository's tests match their text.

Nothing stored changes. The source list, ext-common and quack do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` and `make test-cpp`, all unchanged.
