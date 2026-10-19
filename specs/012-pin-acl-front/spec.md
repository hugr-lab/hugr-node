# Spec 012: re-pin duckdb-acl for the platform's front

- **Status**: in progress
- **Date**: 2026-10-19

hugr-platform spec 002 (the front, accepted 2026-10-19) reads two things from a node that duckdb-acl added for it.
hugr_node builds duckdb-acl in, so the node carries them from this pin on:

| submodule | from | to | why |
| --- | --- | --- | --- |
| duckdb-acl | 0ea1ef6 | b100dad | spec 096's addendum (#243): `x-acl-resource-groups` on a `wrong_resource_group` refusal (a Flight trailer, a header of quack's CONNECT answer) and `group_default` in the load report |
| duckdb-acl | b100dad | (#245) | spec 134's addendum: a Flight ticket and a prepared statement's handle start with the node's label |

duckdb (f8f3f4d), tresor and acl-otel stay: neither change touches duckdb-ext-common's contracts, and duckdb-acl's
pin of duckdb is the same.

- **The heartbeat carries `group_default`** as it is: the agent sends `acl_node_load()` whole as its `load`
  (`HeartbeatBody`), and the operator writes it to the Lease (hugr-platform `platform.hugr.io/group-default`, with
  the group the operator gave the node).
- **Nothing in `src/` changes.**

## Testing

- `test/sql/cluster/acl_node_group.test`: the report's `group_default` - false without a group, true on a node of the
  default group, false at once after `DROP DEFAULT` (the write is the node's, the cached read is dropped).
- The trailer, the header and the labelled ids are read by the front: hugr-platform's demo through it (spec 004's
  checks with spec 002) seats a principal of another group by the trailer, on this pin's image.
- Locally: the build, `test/sql/*`, `make test-cpp`; on the PR: CI, the release's dry run and the distribution build.
