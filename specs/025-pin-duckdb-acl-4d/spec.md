# Spec 025: duckdb-acl after spec 150 stage 4d

- **Status**: implemented
- **Date**: 2026-11-12

duckdb-acl moves from a4b4617 (spec 024) to 7711f26, its spec 150 stage 4d (#287).

- **A role's claims list is a rule now.** `CLAIMS (name = value, …)`: a name is a word or quoted (`"custom:tenant"`), a
  value a word, words joined by `.`, a signed number or quoted. What touches in a bare name or value is quoted.
- **One reader of a claims csv.** `ParseClaims` (now in `acl_policy.cpp`) reads the quoted form and the function forms.
  It refuses an item naming no claim (`'{}'` was skipped), a claim named twice in any case, and a value the split
  would misread. This repository writes `CREATE ROLE r` and `acl_define_role(r, '')` only.

Nothing stored changes. The source list, ext-common and quack do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` and `make test-cpp`, all unchanged.
