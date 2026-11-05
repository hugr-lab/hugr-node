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

- duckdb-acl re-pinned to f2f520f (its spec 150 3g-3a - the seam - and 3g-3c - every management statement a rule,
  the old seam gone; 27 commits from 8b938d1, the same duckdb, ext-common and quack pins).
- `RegisterClusterRules` (src/acl_cluster.cpp) registers five forms under the keyword `cluster`, each a rule headed by
  `'CLUSTER'` with its transform and its usage: `AclClusterExtension` (install / update / remove),
  `AclClusterAttach`, `AclClusterDetach`, `AclClusterSet`, `AclClusterReset`; helper rules `AclCluster…` (the option
  list and `DEPENDS ON` read item by item, `IN GROUP`, VERSION / FROM / SHA256). Values through `MgmtCall`'s readers
  (a quoted value one plain literal; a value acl's `AclValue` - a word, a quoted value or a signed number). The
  compiled calls are the same `acl_cluster_*` calls as before.
- Kept: the SHA256 refusal in spec 103's words (whatever follows the word - its tokens to the statement's end are
  the clause's), a key alone is `true`, TYPE lower-cased, SECRET split out, other option keys upper-cased into the
  JSON.
- Changed, each now refused or read as duckdb's tokens read it: a comment anywhere is read; a trailing comma in a
  list is read; an empty list item, a value of more than one token (`(TYPE=duckdb)`, `(X 'a' 'b')`, `DEPENDS ON (a
  b)`), a bare path, `E'…'` and joined strings are refused; VERSION / FROM / an option written twice is refused (the
  last used to win), and so are a DETACH flag and a `DEPENDS ON` source (an empty one too); `REMOVE … VERSION |
  FROM | COMMENT` is refused in its own words; an empty list (`()`, `DEPENDS ON ()`) is refused; an option key in
  quotes or with a dot (`("Q" 1)`, `(x.y 1)`) and `+1` are refused; `""` is the tokenizer's refusal; `SET x = -1`,
  a value with a dot (`0.5`, `v1.0.0`, `a.b`) and the line breaks inside an option item (`(TYPE⏎duckdb)`, once read
  as one key) are read. A statement the rules do not read is told the usage of its form (by its verb), else "ACL
  CLUSTER expects …".
- Found by the review and fixed with it (both older than the rules): `ENCRYPTION_KEY` was no credential key - an
  option holding a database's key was stored in the profile; a source whose path names its type (`'postgres:…'`, no
  `TYPE`) skipped the SECRET requirement and did not keep its extension in the profile (`SourceTypeOf`: the TYPE, else
  the path's prefix as duckdb reads it - a URL's `scheme://` names a filesystem, no type); `TYPE` / `SECRET` among
  the options JSON of `acl_cluster_attach` (the function and `platform.cluster_attach` route) skipped the requirement
  too - refused, they are the call's own arguments; a `depends_on` CSV with an empty or a repeated name is refused
  there as in the grammar.
- Messages of acl's own forms moved with the re-pin (resource groups are rules since acl 3f): the tests'
  expectations follow, and the schema window test's versions are acl's v23.

## Tests

`test/sql/cluster/cluster_grammar.test`: a comment, a value-less option, trailing commas, IN GROUP, `SET … TO`, a
negative value, the refusals and each form's usage; `acl_cluster_profile.test`: `ENCRYPTION_KEY`, a `postgres:` /
`mysql:` path without a secret; `test_acl_cluster_install.cpp`: a prefix-typed source keeps its extension. The suite
(`test/sql/*`, 14 files) and `make test-cpp` pass. A differential against the old `ParseCluster` over 561 statements
(160 from the tests) found no call compiled differently but the four the old reader misread.

## Alternatives considered

- Keeping the scanner until stage 4: the old seam is what 3g-3c removes from acl; hugr-node moving first keeps both
  repositories building at every step.

## Follow-ups

- The profile's "never a credential" rests on keys: option VALUES are not linted (`(METADATA_PATH 'dbname=x
  password=p')`, `(DATA_PATH 's3://b/p?s3_secret_access_key=…')`), DuckLake's `META_<option>` forwards a key one
  prefix away (`META_ENCRYPTION_KEY`), settings carrying one are not refused (`azure_storage_connection_string`,
  `azure_proxy_password`), `motherduck_token` is no key, and `ExtensionOfType` ignores duckdb's extension aliases
  (`https://…` / `s3://…` vs httpfs). Found by this spec's reviews, all older than it - spec 021.
