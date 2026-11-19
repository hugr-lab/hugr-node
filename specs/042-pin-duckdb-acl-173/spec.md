# Spec 042: duckdb-acl after spec 173

- **Status**: implemented
- **Date**: 2026-11-19

duckdb-acl moves from fc11ff4 (spec 041) to 95fd2a3, its spec 173 (#309): a Flight bulk load by the object's declared
key (`CommandStatementIngest.options["acl.mode"]` = `merge` | `remove`) and the explicit-only capability `ingest`,
judged on top of the verb wherever a door streams rows - Flight's ingest and a quack client's drain (on quack every
INSERT into an attached table is a drain). The platform's caps STRUCT gains `ingest`.

- **Schema.** No step: v28 / `min_reader` 28 as in spec 041.
- **Grants.** A role that loads through either door needs `ingest` named beside `insert` / `merge` / `delete` /
  `create` - nothing in this repository's tests streams rows, so none changes.
- **Sources.** No translation unit added or removed. tresor and ext-common do not move.

## Tests

`HUGR_NODE_E2E=1 GEN=ninja make`, `test/sql/*` (1043 assertions in 14 files) and `make test-cpp` (4), all pass.
