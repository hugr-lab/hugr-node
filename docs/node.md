# Running a platform node

A platform node is one process: the node's duckdb build with `hugr_node` linked in, started as

```sh
duckdb -c "PRAGMA hugr_node_run"
```

`PRAGMA hugr_node_run` builds the node from its environment and the control plane's bootstrap document, applies the cluster
profile, locks the configuration, opens the doors and blocks. On SIGTERM it drains and exits 0. Nothing in the image,
the environment or the profile is a key or a secret: the node's identity is its workload identity, and every
credential comes from tresor when it is used (spec 006).

## The image

A release (a tag `v*`, spec 008) publishes `ghcr.io/hugr-lab/hugr-node:<tag>` for linux amd64 and arm64.

- **What is inside**: the node's duckdb CLI, with hugr_node (acl, tresor and acl_otel built in), httpfs, azure and icu
  linked in. There is no other extension, no key and no secret: the profile's extensions come from the repository at
  start.
- **The base** is `ubuntu:24.04`, the glibc the CLI is built against.
- **The process**: it runs as uid 10001. `tini` is PID 1, so a SIGTERM reaches the node: one still bootstrapping ends
  at once, and one serving drains.
- **The emptyDir**: `HOME=/var/lib/hugr-node` is where duckdb installs extensions, so mount an emptyDir there.
- **The release's other assets**: one archive per platform holds the same CLI and the extensions a profile installs
  (`hugr_node`, `postgres_scanner`, `mssql`, `ducklake`), unsigned, with `manifest.json` and `SHA256SUMS`.
  - The extensions are built at the pins duckdb-acl tests and at the image's duckdb version.
  - A repository signs them when it takes them in.
  - `scripts/publish_to_kista.sh <kista> <config> <tenant/channel> <files>` seeds a dev or test kista with them.

## The environment

| variable | what |
| --- | --- |
| `HUGR_TRESOR` | tresor-server's host |
| `HUGR_IDENTITY_ISSUER`, `HUGR_IDENTITY_CLIENT_ID` | the node's issuer and client id at the IdP |
| `HUGR_IDENTITY_SCOPE` | optional: the OAuth scope of the login |
| `HUGR_IDENTITY_ASSERTION_FILE` | the projected service-account token (below) |
| `HUGR_CONTROL_PLANE` | the control plane's https base URL |
| `HUGR_CA_FILE` | optional: a CA bundle for certificates no public root signs. It replaces the system roots, so include them too when the node reaches public services |
| `HUGR_FLIGHT`, `HUGR_FLIGHT_CERT`, `HUGR_FLIGHT_KEY` | the Flight door's address and TLS files (cleartext only on localhost) |
| `HUGR_QUACK`, `HUGR_QUACK_CERT`, `HUGR_QUACK_KEY` | optional: the quack door |
| `HUGR_DRAIN_DEADLINE` | seconds to wait for sessions on SIGTERM (default 25) |

Development only - the node warns in its log when it runs on either:

| variable | what |
| --- | --- |
| `HUGR_IDENTITY_PRIVATE_KEY_FILE`, `HUGR_IDENTITY_KEY_ID` | a private_key_jwt identity instead of the projected token, where there is no Kubernetes |
| `HUGR_TRESOR_INSECURE_HTTP=1` | tresor-server and its IdP over plain http, on loopback only |

### The node's identity

The node's identity comes from Kubernetes' service-account token volume projection. kubelet writes a short-lived JWT
for the pod's service account into a file on an in-memory volume, with the IdP's audience, and renews it before it
expires. tresor sends it to the IdP as a client assertion. The IdP trusts the cluster: Keycloak 26.7+ through its
Kubernetes identity provider, Entra through a federated credential.

```yaml
spec:
  serviceAccountName: hugr-node          # the node group's identity
  containers:
  - name: node
    image: ghcr.io/hugr-lab/hugr-node:<tag>
    env:
    - name: HUGR_IDENTITY_ASSERTION_FILE
      value: /var/run/secrets/hugr/token
    volumeMounts:
    - { name: hugr-identity, mountPath: /var/run/secrets/hugr, readOnly: true }
    - { name: home, mountPath: /var/lib/hugr-node }   # what duckdb installs from the repository
  volumes:
  - name: home
    emptyDir: {}
  - name: hugr-identity
    projected:
      sources:
      - serviceAccountToken:
          path: token
          audience: <the IdP's audience>
          expirationSeconds: 3600
```

## The bootstrap document

The node asks the control plane at `GET <HUGR_CONTROL_PLANE>/v1/bootstrap`. It authenticates with an `http` secret in
tresor whose SCOPE is the control plane's URL. The control plane answers with pointers, never a secret:

```json
{
  "version": 1,
  "node_group": "analytics",
  "repository": { "name": "hugr", "prefix": "https://ext.example/<tenant>/<channel>" },
  "catalog": {
    "type": "postgres", "extension": "postgres_scanner",
    "path": "", "secret": "policy_catalog", "schema": "acl"
  }
}
```

- `node_group` is optional.
- `catalog.secret` names a secret in tresor that carries the whole login: host, port, dbname, user and password.
- `catalog.extension` is empty for a type the build already has.

## What the node does, in order

1. **Hardening.** Extension repositories allowed; autoinstall, autoload and persistent secrets off. tresor's requests
   go through duckdb's HTTP client, so they trust `HUGR_CA_FILE`.
2. **tresor.** It logs in with the node's identity and attaches the service, acting for sessions.
3. **The bootstrap document** from the control plane.
4. **The repository.** `CREATE OR REPLACE EXTENSION REPOSITORY`: the signing keys come from the repository's
   `.well-known` at every start, so a key rotation needs only a restart.
5. **The policy catalog.** The node installs its extension from the repository, attaches the catalog with the secret
   from tresor, and opens the policy.
6. **The cluster profile**: settings, extensions, sources, in the profile's order.
7. **Lock.** No SET, RESET or new repository from here on.
8. **Serve.** The doors open last, so a TCP probe on the Flight port is the readiness probe.

The first failing step stops the bootstrap. The error names the step, no port opens, and the process exits non-zero.
`hugr_node_bootstrap_plan('<document>')` shows the statements a document would run, and `hugr_node_profile_statements()`
shows the ones that apply this node's profile - both run nothing.

## In an orchestrated cluster

With `HUGR_ORCHESTRATOR` set, the node asks the orchestrator first (spec 010; the orchestrator is
hugr-platform's operator).

| variable | what |
| --- | --- |
| `HUGR_ORCHESTRATOR` | the operator's https base URL (no userinfo) |
| `HUGR_ORCHESTRATOR_TOKEN_FILE` | a projected service-account token with the audience `hugr-platform`: the entry and the heartbeat |
| `HUGR_IDENTITY_ASSERTION_FILE` | a second projected token, with the IdP's federation audience: the login to tresor |
| `HUGR_POD_MEMORY`, `HUGR_POD_CPU` | the container's memory limit and CPU request, through the Downward API (`limits.memory`, `requests.cpu`, divisor 1) |
| `HUGR_TEMP_DIRECTORY`, `HUGR_TEMP_SIZE` | the spill volume's mount and its size (bytes, `600GiB` or `600Gi`) |

`HUGR_TRESOR`, `HUGR_IDENTITY_ISSUER`, `HUGR_IDENTITY_CLIENT_ID`, `HUGR_IDENTITY_SCOPE` and `HUGR_CONTROL_PLANE` are
refused then: the orchestrator's document names them. `HUGR_POD_*` and `HUGR_TEMP_*` work without an orchestrator too.

- **The document** (version 2) comes from `GET <HUGR_ORCHESTRATOR>/v1/bootstrap` with the first token as the bearer. It
  names the group, the node's label, tresor and the node's identity in the IdP, the repository, the catalog's secret
  and the heartbeat's interval. The catalog's type is the secret's own type, read from tresor before anything is
  attached.
- **The node's name.** The node sets `acl_node_group` and `acl_node_label` from the document. Its sessions carry the
  label, so a front routes by it.
- **The first policy** (spec 015). A platform installed from nothing has an empty catalog: no token verifies, and a
  node whose group the policy lacks refuses everyone. The document may carry the platform's first policy, as names,
  never SQL:

  ```json
  "first_policy": {
    "issuer": {"name": "corp", "url": "https://login.example/t/v2.0", "audiences": ["api://hugr"],
               "roles_claim": "roles"},
    "clients": [{"name": "console", "client_id": "<the console's application id>"}],
    "administrators": "hugr-admins",
    "resource_groups": [{"name": "etl", "default": true}, {"name": "bi"}]
  }
  ```

  - A database that holds no schema yet is opened with `acl_use_db`'s `init`, which creates it (and never migrates).
  - It writes the policy only while the catalog lists no role mapping and no issuer but this one. The policy: the
    issuer; a client per application, accepting only that application's tokens (`AZP`); the role `platform_admin`
    with `cluster` and `policy`, mapped from the `administrators` value of the roles claim for each client; each
    resource group (one default), bound to `platform_admin`.
  - The mappings are written last, so a start that stopped part-way is completed by the next one, and a catalog that
    has its policy is never written again.
  - Every field is required, nothing is named twice, and the node's own group must be among the resource groups.
  - `platform_admin` holds no `passthrough`, so it cannot hand out administration: grant a bundle, map a claim to an
    administering role, or change the issuer or a client that carries such a mapping.
- **The engine follows the pod.**
  - The defaults: `memory_limit` at 80% of the pod's memory, `threads` at its CPU, `max_temp_directory_size` at 90%
    of the spill volume.
  - A profile item for one of them wins, but is cut to a ceiling: 90% of the memory, the CPU, 90% of the volume. A
    value that is no amount (`-1`, `none`, `95%`) is cut to the ceiling too.
  - This keeps a node from being OOM-killed, or evicted because its spill filled the node's disk.
- **The tick.** Every heartbeat interval, and every 15 s without an orchestrator, the node:
  - applies the profile's hot items it does not have yet: a new source, a new extension;
  - detaches a source it took from the profile that the profile no longer has;
  - reports what needs a restart or a drain: a setting that differs or was reset, an extension at another version or
    removed, a re-pointed source (to the same type too);
  - drains itself when its resource group is gone from the policy, and resumes when it is back;
  - sends the heartbeat: its versions, the profile version it has applied, what is pending, its drift and its load.

  A source attached by hand (`ACL NATIVE`) is never detached: it shows as drift.

## Stopping

On SIGTERM the node stops seating new sessions and waits until the sessions end or `HUGR_DRAIN_DEADLINE` passes. Then
it ends the rest, stops the doors and exits 0. Keep the pod's `terminationGracePeriodSeconds` above the deadline. A
second SIGTERM skips the wait.

## What a profile may not change

The bootstrap sets the node's trust settings, and no cluster-profile item may change them. These are
`ca_cert_file`, `tresor_http_client`, `temp_directory`, the autoinstall settings and the extension repository settings,
together with the hardening list in [Node groups and the cluster profile](cluster.md).
