# Spec 024: duckdb-acl after spec 150 stage 4c

- **Status**: implemented
- **Date**: 2026-11-12

duckdb-acl moves from 7eede0b (spec 023) to a4b4617, its spec 150 stage 4c (#286).

- **A client's and an issuer's lists are rules now.** AUDIENCES, AZP, FLOWS, ROLES FROM / CONSTANT, SUBJECT,
  REQUIRE, ATTRIBUTES and CLAIM MAP are read by grammar rules, not text readers. The last hand-written statement
  scanner, `AdminScanner` (`acl_admin_scanner.hpp`), is gone; this repository never included it.
- **What reads differently.** A bare list value is a word, words joined by `.`, or a signed number; anything else is
  quoted (`'acme-prod'`, `'custom:tenant'`, a URL). Everything this repository composes is already quoted.
- **Refused now.** `'v' IN (paths)` (it read the other way round), `ATTRIBUTES (a = CONSTANT)` without a value, and
  an attribute name that is not one name.

Nothing stored changes. The source list, ext-common and quack do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` and `make test-cpp`, all unchanged.
