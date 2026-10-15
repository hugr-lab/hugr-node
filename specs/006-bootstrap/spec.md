# Spec 006: the node process and its bootstrap

- **Status**: accepted (owner, 2026-10-15: "в целом да") - implementation in progress
- **Date**: 2026-10-15
- **Author**: the agent, at the owner's ask (platform phase 1, step 1); the design and the owner's decisions are in
  the Claude Doc "Платформа, фаза 1: флот из одного профиля"
  (claude.ai/code/artifact/933639a1-34b9-4563-b466-7a223693c0c1) and design/017 §3.2, §7 items 15-18

## Problem

A platform node today is a duckdb CLI fed through a FIFO, with bootstrap SQL written by hand (test/e2e/tresor,
acl-clients' dev/node.sh). Phase 1 needs a node that builds itself in a pod: it starts with no key and no secret in
its image, env or profile, gets everything else from tresor and the control plane, applies the cluster profile,
serves, and stops cleanly on SIGTERM. Nothing does that now:

- no code applies the cluster profile at startup - only the node that writes a hot item applies it
  (`acl_cluster.cpp`), and `acl_cluster_applied` is never called;
- the duckdb CLI has no daemon mode and no SIGTERM handler (`tools/shell`: SIGINT only);
- the trust settings a bootstrap sets (`ca_cert_file`, the autoinstall settings) are not in the hardening list, so a
  profile item could change them on every node.

## Design

### The entry: `hugr_node_run()`

The image's entrypoint is `duckdb -c "CALL hugr_node_run()"`. `hugr_node_run()` is a table function that:

1. reads the local part from the environment;
2. runs the bootstrap below, in order - the first failure raises an error that names the step, nothing listens, and
   the CLI exits non-zero (the pod restarts, the reason is in its log);
3. opens the doors and blocks;
4. on SIGTERM (or SIGINT) drains, stops the doors and returns one row (`reason`, `sessions_left`). The CLI then exits 0,
   which is what gives the database its final checkpoint (spec 084 of duckdb-acl: an unstopped door gets none).

The signal handler only sets an atomic flag (async-signal-safe); the blocked call polls it every 250 ms. It runs once
per process (a second call is refused) and only outside a principal: under `ACL ROLE|TOKEN|SESSION` the function gate
refuses it like any uncategorized function, and a test proves it.

The bootstrap is split into a pure **plan** and its **execution**. `hugr_node_bootstrap_plan(document_json)` is a table
function that returns the statements the bootstrap would run for a document and the current environment, without
running them. Most tests are on the plan, and the plan is what an operator reads when a node will not start.

### The local part: environment

Only what is needed to log in to tresor and to find the control plane. No value in it is a credential.

| variable | what |
| --- | --- |
| `HUGR_TRESOR` | tresor-server's host (`tresor:<host>`) |
| `HUGR_IDENTITY_ISSUER`, `HUGR_IDENTITY_CLIENT_ID` | the node's issuer and client id at the IdP |
| `HUGR_IDENTITY_ASSERTION_FILE` | the workload identity: the projected service-account token (tresor FLOW `federated`) |
| `HUGR_IDENTITY_PRIVATE_KEY_FILE` | **dev only**, instead of the assertion: private_key_jwt (FLOW `client_credentials`) for compose and the e2e, where there is no Kubernetes. The node logs that it runs on a dev identity |
| `HUGR_CONTROL_PLANE` | the control plane's base URL (https) |
| `HUGR_CA_FILE` | optional: a CA bundle for https to non-public certificates (`ca_cert_file`); it replaces the system roots, so it holds them too when the node reaches public services |
| `HUGR_FLIGHT`, `HUGR_FLIGHT_CERT`, `HUGR_FLIGHT_KEY` | the Flight door's address (the pod's port) and its TLS files |
| `HUGR_QUACK`, `HUGR_QUACK_CERT`, `HUGR_QUACK_KEY` | optional: the quack door |
| `HUGR_DRAIN_DEADLINE` | seconds to wait for sessions on SIGTERM (default 25; below the pod's `terminationGracePeriodSeconds`) |

**The projected token.** The node's identity is Kubernetes' service-account token volume projection. kubelet writes a
short-lived JWT for the pod's service account into a file: the cluster signs it, the audience is the IdP's, it lasts
an hour, and kubelet renews it before it expires. The file lives on an in-memory volume. Nobody creates or stores
it, and it is in neither the image nor the env. tresor (FLOW `federated`) re-reads it at every mint and sends it to
the IdP as a client assertion. The IdP trusts the cluster: Keycloak 26.7+ through its Kubernetes identity provider,
Entra through a federated credential. It works in any Kubernetes, the abstract one included, and in Azure it is what
`AZURE_FEDERATED_TOKEN_FILE` names.

```yaml
spec:
  serviceAccountName: hugr-node          # the node group's identity
  containers:
  - name: node
    env:
    - name: HUGR_IDENTITY_ASSERTION_FILE
      value: /var/run/secrets/hugr/token
    volumeMounts:
    - { name: hugr-identity, mountPath: /var/run/secrets/hugr, readOnly: true }
  volumes:
  - name: hugr-identity
    projected:
      sources:
      - serviceAccountToken:
          path: token
          audience: <the IdP's audience>   # a Keycloak realm, or api://AzureADTokenExchange
          expirationSeconds: 3600
```

### The bootstrap document: the contract with the control plane

`GET <HUGR_CONTROL_PLANE>/v1/bootstrap` answers the rest for the calling node's identity. The control plane holds only
pointers - never a secret. hugr-platform spec 001 serves it; until then the e2e serves a static file.

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

- `node_group` is optional (no group = serves everyone, spec 096 of duckdb-acl).
- `catalog.path` is the ATTACH string without credentials; the secret carries the full set (host, port, dbname, user,
  password - owner, 2026-10-15), so it may be empty. `catalog.extension` is empty for a type the build links or duckdb
  has built in.
- An unknown `version` is refused (fail closed). Unknown fields are ignored.

The call is made with httpfs (`read_text`), authenticated by an `http` secret whose SCOPE is the control plane's URL.
tresor serves it - a dynamic secret minted by token exchange (tresor-server's `token_exchange` provider, spec 010
there), granted `use` to the node's role. The operator defines it in tresor-server; the node holds no token of its own.

### The bootstrap, in order

1. **Hardening.** `allow_extension_repositories` from `undecided` to `allowed`; `autoinstall_known_extensions` and
   `autoload_known_extensions` off; `ca_cert_file` from `HUGR_CA_FILE`; `allow_persistent_secrets` off. Unsigned and
   community stay off (duckdb cannot turn them on at runtime). duckdb accepts `allow_persistent_secrets` only before
   the secret manager is first used: if loading tresor has already used it, hugr_node sets it in its load, before
   `LoadTresorCore` (to verify in the implementation).
2. **tresor.** `CREATE SECRET hugr_node_identity (TYPE tresor, SCOPE 'tresor:<HUGR_TRESOR>', FLOW 'federated',
   ISSUER …, CLIENT_ID …, ASSERTION_FILE …)` (or the dev flow), then
   `ATTACH 'tresor:<HUGR_TRESOR>' AS hugr_tresor (SECRET hugr_node_identity, ACT_FOR_SESSIONS true)`. tresor is built
   in; nothing is loaded.
3. **The bootstrap document** from the control plane (above).
4. **The repository.** `CREATE EXTENSION REPOSITORY <name> WITH PREFIX '<prefix>'`: the keys by discovery (design/017
   item 15). A private channel's `http` secret comes from tresor by its SCOPE.
5. **The policy catalog.** `INSTALL <extension> FROM <repository>; LOAD <extension> FROM <repository>` when an
   extension is named; `ATTACH '<path>' AS hugr_policy (TYPE <type>, SECRET <secret>)` with the secret from tresor;
   `SET GLOBAL acl_node_group = '<group>'` when the document names one; `SELECT acl_use_db('hugr_policy', '<schema>')`.
   The schema window (spec 094) is judged here: a catalog this build may not read stops the bootstrap.
6. **The profile.** `acl_cluster_effective()` in its order:
   - settings: `SET GLOBAL <name> = <value>`;
   - extensions: `INSTALL <name> FROM <repository> VERSION '<v>'` and `LOAD <name> FROM <repository>`;
   - sources: the ATTACH the item stands for, built by the same `AttachStatement` the hot apply uses (secret from
     tresor, LINEAGE marker kept).
7. **Lock.** `SET GLOBAL lock_configuration = true`: no SET, RESET or new extension repository from here on.
8. **Serve.** `acl_cluster_applied(<version read in step 6>)`, then `acl_flight_serve(HUGR_FLIGHT, cert, key)` and, when
   configured, `acl_quack_serve(…)`. The quack door's server token is a random value made per process (CSPRNG): it is
   the outer fence of a default-configured quack, not what admits a client (their JWT is) - to confirm that no client
   needs it before relying on that. The Flight port opens last, so a TCP probe on it is the readiness probe.

### Stopping

On SIGTERM: `acl_drain()`; wait until `acl_drain()` reports no live session or `HUGR_DRAIN_DEADLINE` passes; end the
rest with `acl_session_kill` (ids from `acl_sessions()`); `acl_flight_stop` / `acl_quack_stop`; return. A second
SIGTERM during the wait skips straight to stopping the doors.

### The hardening list (acl_cluster.cpp)

`HARDENING_SETTINGS` gains `ca_cert_file`, `autoinstall_known_extensions`, `autoload_known_extensions`,
`custom_extension_repository` and `autoinstall_extension_repository`. They decide which certificates the node trusts
and where code may come from: the bootstrap sets them from the environment, and no profile item may change them on the
fleet (found 2026-10-15: `ca_cert_file` was missing, so a passthrough admin could hand every node a new CA).

### The build

httpfs and azure are linked statically into the default build (owner, 2026-10-15: until duckdb 2.0). httpfs because
the repository's discovery and the control plane are https; azure because the `azure` secret type mssql reads comes
from it. `HUGR_NODE_DEV=1` keeps adding json and autocomplete for the client tools' dev node. azure brings the Azure
SDK through vcpkg (`azure-identity-cpp`, `azure-storage-blobs-cpp`, `azure-storage-files-datalake-cpp`): the CI cache
grows, and the artifact size is measured in the PR.

### Not in this spec

- the convergence tick, `config.applied` after hot changes, the heartbeat - spec 008;
- the image, the release workflow, publishing the profile's extensions - spec 007;
- the control plane - hugr-platform spec 001;
- the kind demo - hugr-platform spec 002;
- dynamic database secrets - later, in tresor-server (owner, 2026-10-15: static secrets in phase 1).

## Enforcement / security

- **No credential is written anywhere.**
  - The environment holds addresses and file paths; the identity is the projected token.
  - The bootstrap document holds pointers. Every secret comes from tresor at use.
  - The dev private key exists only in the dev flow, and the node says so.
- **Nothing listens before the policy is loaded and the configuration is locked.** The doors open in step 8, after
  `lock_configuration`.
- **https only**: the control plane, tresor-server and the repository. A non-public CA comes through `HUGR_CA_FILE`.
  Verified 2026-10-15 at the f702a1f pin: discovery over https with a private CA works only with `ca_cert_file`, and
  `SSL_CERT_FILE` is ignored. tresor talks to tresor-server through its own transport (the OIDC core's TLS, not
  httpfs): whether it trusts the same CA from `HUGR_CA_FILE` is to verify in the implementation, and if not it gets
  the same file through its own option.
- `hugr_node_run()` and `hugr_node_bootstrap_plan()` are refused to a principal by the function gate.
- `ACL NATIVE` stays the break-glass. Nothing here vetoes what it loads (design/017 item 18).

## Testing

- **sqllogictest:**
  - `hugr_node_run()` refused under a principal and refused a second time;
  - `hugr_node_bootstrap_plan()` on documents: the full order, an extension-less catalog type, no node group, an
    unknown version refused, a missing field named;
  - the new hardening settings refused as profile items (each, case-insensitive).
- **e2e** `test/e2e/bootstrap/run.sh`, which SKIPs without docker or go:
  - **the setup:** Keycloak (dev identity via private_key_jwt), tresor-server (static secrets: the catalog's Postgres
    login and the control plane's http token), Postgres as the policy catalog, kista in its dev profile (a channel for
    the build's duckdb version, our postgres_scanner published into it), and a static https bootstrap document behind
    a test CA;
  - **checks 1-3:** the node starts with nothing but the environment, fetches the bootstrap document with the control
    plane's token from tresor, installs postgres_scanner from the repository, and attaches the catalog with the secret
    from tresor (the unverified paths from the phase-1 doc);
  - **checks 4-5:** it applies a profile (a setting, a source), and `acl_node_load()` shows `config.applied =
    config.target`;
  - **check 6:** it serves Flight;
  - **check 7:** on SIGTERM it drains, stops and exits 0;
  - **check 8:** a broken step (a wrong secret name) stops the bootstrap with the step named, and no port opens.
- **CI:** the e2e runs in the linux job, like `test/e2e/tresor`. postgres_scanner is built as a loadable for it, from
  the duckdb submodule's pin, as duckdb-acl's integration build does.

## Alternatives

- **A small main of our own on libduckdb** instead of the CLI and a blocking function: a second artifact, and it gives
  nothing the function does not.
- **The bootstrap fully in env (no control plane)**: the first draft. The owner chose tresor first and the control plane
  for the rest, so the pod's environment stays minimal and the pointers change in one place.
- **httpfs installed from the repository**: needs the keys before httpfs exists. A DuckDB-signed httpfs from a
  passthrough channel solves it after duckdb 2.0, and the owner decided to link until then.
