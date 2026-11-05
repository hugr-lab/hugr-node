# Spec 020: `ACL CLUSTER` read by rules on duckdb's tokens

- **Status**: implemented
- **Date**: 2026-11-05
- **Author**: hugr lab (duckdb-acl spec 150 stage 3g-3: no statement text parsed by hand; the owner's order of
  2026-11-04 - 3g-3 moves the platform part's forms onto the rules, then the old seam goes)

## Problem

The CLUSTER forms were the last management statements read by a hand scanner (`ParseCluster` on acl's
`AdminScanner`, registered through `RegisterMgmtGrammar`): option lists and `DEPENDS ON` split as text at commas, a
comment refused, a quoted value taken however it was written. duckdb-acl's 3g-3a seam lets a module register PEG
rules for its statements instead (`RegisterMgmtRules` / `MgmtCall`, `acl_mgmt_rules.hpp`).

## Design

- duckdb-acl re-pinned to 8d8bb59 (its spec 150 3g-3a; 23 commits from 8b938d1, the same duckdb, ext-common and
  quack pins).
- `RegisterClusterRules` (src/acl_cluster.cpp) registers five forms under the keyword `cluster`, each a rule headed by
  `'CLUSTER'` with its transform and its usage: `AclClusterExtension` (install / update / remove),
  `AclClusterAttach`, `AclClusterDetach`, `AclClusterSet`, `AclClusterReset`; helper rules `AclCluster…` (the option
  list and `DEPENDS ON` read item by item, `IN GROUP`, VERSION / FROM / SHA256). Values through `MgmtCall`'s readers
  (a quoted value one plain literal; a value acl's `AclValue` - a word, a quoted value or a signed number). The
  compiled calls are the same `acl_cluster_*` calls as before.
- Kept: the SHA256 refusal in spec 103's words, a key alone is `true`, TYPE lower-cased, SECRET split out, other
  option keys upper-cased into the JSON.
- Changed, each now refused or read as duckdb's tokens read it: a comment anywhere is read; a trailing comma in a
  list is read; an empty list item, a value of more than one token (`(TYPE=duckdb)`, `(X 'a' 'b')`, `DEPENDS ON (a
  b)`), a bare path, `E'…'` and joined strings are refused; VERSION / FROM / an option written twice is refused (the
  last used to win); `REMOVE … VERSION` is refused in its own words; `SET x = -1` is read. A statement the rules do
  not read is told the usage of its form (by its verb), else "ACL CLUSTER expects …".
- Messages of acl's own forms moved with the re-pin (resource groups are rules since acl 3f): the tests'
  expectations follow, and the schema window test's versions are acl's v23.

## Tests

`test/sql/cluster/cluster_grammar.test`: a comment, a value-less option, trailing commas, IN GROUP, the refusals
and each form's usage. The suite (`test/sql/*`, 13 files) and `make test-cpp` pass.

## Alternatives considered

- Keeping the scanner until stage 4: the old seam is what 3g-3c removes from acl; hugr-node moving first keeps both
  repositories building at every step.
