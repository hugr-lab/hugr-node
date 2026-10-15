# Running a platform node

A platform node is one process: the node's duckdb build with `hugr_node` linked in, started as

```sh
duckdb -c "CALL hugr_node_run()"
```

`hugr_node_run()` builds the node from its environment and the control plane's bootstrap document, applies the cluster
profile, locks the configuration, opens the doors and blocks. On SIGTERM it drains and exits 0. Nothing in the image,
the environment or the profile is a key or a secret: the node's identity is its workload identity, and every
credential comes from tresor when it is used (spec 006).

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

## Stopping

On SIGTERM the node stops seating new sessions and waits until the sessions end or `HUGR_DRAIN_DEADLINE` passes. Then
it ends the rest, stops the doors and exits 0. Keep the pod's `terminationGracePeriodSeconds` above the deadline. A
second SIGTERM skips the wait.

## What a profile may not change

The bootstrap sets the node's trust settings, and no cluster-profile item may change them. These are
`ca_cert_file`, `tresor_http_client`, the autoinstall settings and the extension repository settings, together with
the hardening list in [Node groups and the cluster profile](cluster.md).
