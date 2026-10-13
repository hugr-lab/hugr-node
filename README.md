# hugr-node

The hugr platform's part of a node, on top of [duckdb-acl](https://github.com/hugr-lab/duckdb-acl):

- the **cluster profile** - the shared part of every node's bootstrap (extensions, sources, settings), kept in the
  policy catalog as desired state (`ACL CLUSTER …`);
- **node groups** - a resource group as a set of nodes, and placement of principals onto them;
- **drift** - what a node carries against its profile;
- the node's **deployment** (a platform node keeps secrets only in its secrets service) and the **`cluster`**
  admin bundle;
- later: the node agent that converges a fleet with the platform's control plane.

duckdb-acl is the open node's access control (MIT). This repository plugs into it through the registration
points of duckdb-acl's spec 126 and builds one extension with it (design: duckdb-acl `design/080`).

> **Status: in progress** - the code moves here from duckdb-acl (its spec 127) before the duckdb-acl release.

## License

Business Source License 1.1 - see [LICENSE](LICENSE). Production use is permitted except as a hosted or managed
service offered to third parties; each version becomes Apache-2.0 four years after its release. The parameters
(Additional Use Grant, Change Date, Change License) are the licensor's and may be revised before the first release.
