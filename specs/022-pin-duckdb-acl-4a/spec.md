# Spec 022: duckdb-acl after spec 150 stage 4a

- **Status**: implemented
- **Date**: 2026-11-06

duckdb-acl moves from f2f520f (its spec 150 stage 3, which spec 020 followed) to 4acccd7:

| duckdb-acl | what reaches hugr_node |
| --- | --- |
| spec 151 (#279, #280) | the record after a principal's DDL is a `PRAGMA` of its own that answers no row; the embedded quack server's collector skips it (a `sync.py` hunk, built in with acl's sources) |
| spec 153 (#281) | a client's DDL over Flight runs as text on the session's connection |
| spec 154 (#282) | a principal's catalog record follows the client's ROLLBACK |
| spec 155 (#283) | a principal's error names nothing it was not shown |
| spec 150 4a (#284) | a name in a management statement is read exactly as duckdb reads an identifier; every revoke of nothing is refused |

For this repository's rules (spec 020) 4a means: a NAME the CLUSTER forms read (`AS <alias>`, `IN GROUP g`, `SECRET
s`, `FROM <repo>`, an extension, `DEPENDS ON`) follows duckdb's identifier rule - bare unless it starts with a digit
or is a keyword duckdb takes as no name (a reserved word, `full`, `left`, `join`), quoted otherwise, backticks too; a
setting's name and every value stay words. A refusal at such a name says so (`… - full is an SQL keyword: as a name
it is written in double quotes ("full")`). Nothing stored changes, the source list (`acl_sources.cmake`) and the
submodules duckdb-acl pins (ext-common, quack) do not move, and acl-otel stays on its pin.

## Tests

The build (`HUGR_NODE_E2E=1`), `test/sql/*` and `make test-cpp`, unchanged: the names this repository's tests and
docs write all read under 4a (checked by duckdb-acl 4a's compatibility review), and its one revoke
(`acl_revoke_admin('ops_cluster', 'operate')`) revokes what the role holds.
