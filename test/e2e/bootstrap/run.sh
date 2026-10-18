#!/usr/bin/env bash
# A node bootstraps itself (spec 006): `duckdb -c "PRAGMA hugr_node_run"` with nothing but its environment. Around it:
# Keycloak (the node's identity - the realm's keynode client, private_key_jwt: the dev flow, as there is no Kubernetes
# here), tresor-server (the catalog's Postgres login and the control plane's token, static secrets - it and Keycloak
# over plain http on loopback, HUGR_TRESOR_INSECURE_HTTP: a Go service on macOS cannot be made to trust a test CA), a
# Postgres holding the policy catalog and its cluster profile, kista in its dev profile over https (a channel for the
# build's duckdb version, our postgres_scanner published into it) and a fake control plane over https serving the
# bootstrap document - every https server behind a CA made for this run. Checked:
#   1. the node fetches the bootstrap document with the control plane's token from tresor;
#   2. it trusts the repository by its discovery document and installs postgres_scanner from it;
#   3. it attaches the policy catalog with the secret from tresor;
#   4. it applies the cluster profile (a setting, a source) and reports the profile's version;
#   5. it serves Flight;
#   6. on SIGTERM it drains, stops and exits 0;
#   7. a broken step (a secret the service does not have) stops the bootstrap with the step named, and no port opens.
# Then the orchestrated entry (spec 010): a fake orchestrator over https answers a document v2 to each node's token
# and records their heartbeats, and two nodes run on the one catalog:
#   8. both enter through the orchestrator, log in to tresor with the document's identity, take the catalog's type from
#      its secret, and serve;
#   9. the engine's defaults follow HUGR_POD_*, and a profile memory_limit and threads above the pod are cut - drift,
#      not pending;
#  10. a hot ATTACH written on the catalog is in effect on both within a tick, reported applied at the new version;
#      re-pointed to the same type it is pending a drain on both, and dropped it is detached by both;
#  11. a hot item failing on one node (a file the other holds) is listed there with its error's class, the rest of
#      the write applied;
#  12. a restart-class item (SET ... IN GROUP) is reported by both as needs_restart, with the item pending; withdrawn
#      before it ran it waits no more, while a setting the nodes took at their bootstrap and the profile dropped does;
#  13. a dropped resource group drains both by themselves, and recreating it resumes them;
#  14. the heartbeat's node_id is the document's label, and the load report carries it as acl_node_label.
# Needs docker, go, python3, curl, openssl and the hugr_node build with the e2e's postgres_scanner
# (HUGR_NODE_E2E=1 GEN=ninja make). A missing tool or build is a SKIP (exit 0) on a developer's machine; CI asserts the
# run happened.
#
#   test/e2e/bootstrap/run.sh   # TRESOR_SERVER_DIR / REPOSITORY_DIR = checkouts to build; otherwise their main is cloned
#   (no KISTA_* variable here: kista reads its configuration from them)
#
# The release (spec 008) runs it on what it is about to publish: NODE_IMAGE=<image> starts the node as that image's
# container (docker run --network host, its own user, tini as PID 1 - Linux only), and ASSETS_DIR=<release assets>
# seeds the repository with every extension of the release - through scripts/publish_to_kista.sh, so kista's intake
# checks each of them - and installs the catalog's from there.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
NODE_IMAGE="${NODE_IMAGE:-}"
ASSETS_DIR="${ASSETS_DIR:-}"
if [ -n "$ASSETS_DIR" ]; then
	PGEXT="$ASSETS_DIR/postgres_scanner.duckdb_extension"
	SEED=("$ASSETS_DIR"/*.duckdb_extension)
else
	PGEXT="${PGEXT:-$ROOT/build/release/extension/postgres_scanner/postgres_scanner.duckdb_extension}"
	SEED=("$PGEXT")
fi
KC_PORT="${KEYCLOAK_PORT:-18680}"
TS_PORT="${TRESOR_SERVER_PORT:-18643}"
REPO_PORT="${REPOSITORY_PORT:-18744}"
CP_PORT="${CONTROL_PLANE_PORT:-18745}"
ORCH_PORT="${ORCHESTRATOR_PORT:-18746}"
FLIGHT_A_PORT="${FLIGHT_A_PORT:-18311}"
FLIGHT_B_PORT="${FLIGHT_B_PORT:-18312}"
PG_PORT="${POSTGRES_PORT:-18546}"
FLIGHT_PORT="${FLIGHT_PORT:-18310}"
ISSUER="http://127.0.0.1:$KC_PORT/realms/tresor"
TS_HOST="127.0.0.1:$TS_PORT"

skip() {
	echo "SKIP: $*"
	exit 0
}
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
redact() { sed -E -e 's/eyJ[A-Za-z0-9._-]*/<token>/g'; }

for tool in docker go python3 curl openssl; do command -v "$tool" >/dev/null || skip "$tool not found"; done
docker info >/dev/null 2>&1 || skip "docker is not running"
[ -x "$DUCKDB" ] || skip "no hugr_node build at $DUCKDB (GEN=ninja make)"
[ -f "$PGEXT" ] || skip "no postgres_scanner loadable at $PGEXT (HUGR_NODE_E2E=1 GEN=ninja make)"
if [ -n "$NODE_IMAGE" ]; then
	docker image inspect "$NODE_IMAGE" >/dev/null 2>&1 || fail "no image $NODE_IMAGE"
fi
[ -f "$ROOT/tresor/server/docker-compose.yml" ] || skip "no tresor submodule (git submodule update --init tresor)"
for port in "$TS_PORT" "$REPO_PORT" "$CP_PORT" "$PG_PORT" "$FLIGHT_PORT" "$ORCH_PORT" "$FLIGHT_A_PORT" "$FLIGHT_B_PORT"; do
	if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then fail "127.0.0.1:$port is taken"; fi
done

TMP="$(mktemp -d)"
COMPOSE=(docker compose -p hugr-node-bootstrap-kc -f "$ROOT/tresor/server/docker-compose.yml")
PG_CONTAINER="hugr-node-bootstrap-pg"
cleanup() {
	for pid in "${NODE_PID:-}" "${NODE_A_PID:-}" "${NODE_B_PID:-}" "${SERVER_PID:-}" "${KISTA_PID:-}" "${CP_PID:-}" \
		"${ORCH_PID:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null || true
	done
	docker rm -f "$PG_CONTAINER" >/dev/null 2>&1 || true
	for container in "${NODE_CONTAINER:-}" "${NODE_A_CONTAINER:-}" "${NODE_B_CONTAINER:-}"; do
		[ -z "$container" ] || docker rm -f "$container" >/dev/null 2>&1 || true
	done
	[ "${KEEP_KEYCLOAK:-0}" = 1 ] || "${COMPOSE[@]}" down >/dev/null 2>&1 || true
	rm -rf "$TMP"
}
trap cleanup EXIT

echo "a node bootstraps itself e2e (hugr-node spec 006)"

# --- a CA made for this run, one server certificate for every https server here --------------------------------------
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/ca.key" -out "$TMP/ca.pem" -days 1 -subj /CN=e2e-ca 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout "$TMP/tls.key" -out "$TMP/tls.csr" -subj /CN=localhost 2>/dev/null
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' >"$TMP/san.cnf"
openssl x509 -req -in "$TMP/tls.csr" -CA "$TMP/ca.pem" -CAkey "$TMP/ca.key" -CAcreateserial -out "$TMP/tls.crt" \
	-days 1 -extfile "$TMP/san.cnf" 2>/dev/null

# --- Keycloak, and the node's key registered on its keynode client -------------------------------------------------
echo "  Keycloak on 127.0.0.1:$KC_PORT"
KEYCLOAK_PORT="$KC_PORT" "${COMPOSE[@]}" up -d >/dev/null 2>&1 || fail "Keycloak did not start"
for _ in $(seq 120); do curl -sf "$ISSUER/.well-known/openid-configuration" >/dev/null && break; sleep 2; done
curl -sf "$ISSUER/.well-known/openid-configuration" >/dev/null || fail "Keycloak did not come up"
kc_admin="$(curl -sf -d grant_type=password -d client_id=admin-cli -d username=admin -d password=admin \
	"http://127.0.0.1:$KC_PORT/realms/master/protocol/openid-connect/token" |
	python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])')"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/node-key.pem" 2>/dev/null
chmod 600 "$TMP/node-key.pem"
openssl req -x509 -new -key "$TMP/node-key.pem" -subj /CN=keynode -days 1 -out "$TMP/node-key.crt" 2>/dev/null
node_cert="$(openssl x509 -in "$TMP/node-key.crt" -outform der | base64 | tr -d '\n')"
admin_api="http://127.0.0.1:$KC_PORT/admin/realms/tresor/clients"
keynode_uuid="$(curl -sf -H "Authorization: Bearer $kc_admin" "$admin_api?clientId=keynode" |
	python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["id"])')"
curl -sf -o /dev/null -X PUT -H "Authorization: Bearer $kc_admin" -H 'Content-Type: application/json' \
	"$admin_api/$keynode_uuid" \
	-d '{"clientId":"keynode","clientAuthenticatorType":"client-jwt","attributes":{"jwt.credential.certificate":"'"$node_cert"'","use.jwks.url":"false"}}' ||
	fail "the node's key could not be registered on Keycloak"

# --- tresor-server on loopback http (see above): the operator (etl) admin, the node (keynode) an actor -----------------
if [ -n "${TRESOR_SERVER_DIR:-}" ]; then
	server_src="$(cd "$TRESOR_SERVER_DIR" && pwd)"
else
	git clone -q --depth 1 https://github.com/hugr-lab/tresor-server "$TMP/tresor-server" || fail "no tresor-server"
	server_src="$TMP/tresor-server"
fi
(cd "$server_src" && GOWORK=off CGO_ENABLED=0 go build -o "$TMP/bin/tresor-server" ./cmd/tresor-server) ||
	fail "tresor-server did not build"
cat >"$TMP/server.yaml" <<EOF
listen: $TS_HOST
public_url: http://$TS_HOST
state: {kind: memory}
issuers:
  - issuer: $ISSUER
    audience: duckdb-secrets
    client_id: duckdb
    scopes: [openid]
    human_flows: [authorization_code, device_code]
    service_flows: [client_credentials, private_key_jwt]
    roles_claim: realm_access.roles
    service: {claim: client_id}
policy:
  admins: [client:etl]
  actors:
    - {principal: client:keynode, verbs: [use]}
EOF
echo "  tresor-server on http://$TS_HOST"
"$TMP/bin/tresor-server" -config "$TMP/server.yaml" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 150); do curl -sf "http://$TS_HOST/.well-known/duckdb-secrets" >/dev/null && break; sleep 0.2; done
curl -sf "http://$TS_HOST/.well-known/duckdb-secrets" >/dev/null || {
	cat "$TMP/server.log" >&2
	fail "tresor-server did not come up"
}

# --- Postgres for the policy catalog --------------------------------------------------------------------------------
echo "  Postgres on 127.0.0.1:$PG_PORT"
docker run -d --rm --name "$PG_CONTAINER" -e POSTGRES_PASSWORD=pg-e2e -p "127.0.0.1:$PG_PORT:5432" postgres:16 \
	>/dev/null || fail "Postgres did not start"
for _ in $(seq 60); do docker exec "$PG_CONTAINER" pg_isready -U postgres >/dev/null 2>&1 && break; sleep 1; done
docker exec "$PG_CONTAINER" pg_isready -U postgres >/dev/null 2>&1 || fail "Postgres did not come up"

# --- kista: a channel for the build's duckdb version, our postgres_scanner in it ------------------------------------
if [ -n "${REPOSITORY_DIR:-}" ]; then
	kista_src="$(cd "$REPOSITORY_DIR" && pwd)"
else
	git clone -q --depth 1 https://github.com/hugr-lab/duckdb-extension-repository "$TMP/kista-src" || fail "no kista"
	kista_src="$TMP/kista-src"
fi
(cd "$kista_src" && GOWORK=off CGO_ENABLED=0 go build -o "$TMP/bin/kista" ./cmd/kista) || fail "kista did not build"
mkdir -p "$TMP/kista/keys"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/kista/keys/e2e.pem" 2>/dev/null
cat >"$TMP/kista/kista.yaml" <<EOF
profile: dev
store: {kind: sqlite, sqlite: {path: $TMP/kista/kista.db}}
signers: {file_dir: $TMP/kista/keys}
serve:
  public_url: https://127.0.0.1:$REPO_PORT
  listeners:
    - addr: 127.0.0.1:$REPO_PORT
      scheme: https
      tls: {cert_file: $TMP/tls.crt, key_file: $TMP/tls.key}
EOF
kista() { exec_kista=(env $(env | sed -n "s/^\(KISTA_[A-Za-z0-9_]*\)=.*/-u \1/p") "$TMP/bin/kista"); "${exec_kista[@]}" "$@"; }
ka() {
	kista admin -config "$TMP/kista/kista.yaml" "$@" 2>>"$TMP/kista-admin.log" || {
		cat "$TMP/kista-admin.log" >&2
		fail "kista admin $1 failed"
	}
}
ka migrate >/dev/null
ka tenant create hugr >/dev/null
ka channel create hugr/e2e -kind signed >/dev/null
ka key add hugr/e2e -signer file:e2e.pem -active >/dev/null
# the duckdb version and the releases: what a dev or test repository is seeded with (spec 008)
"$ROOT/scripts/publish_to_kista.sh" "$TMP/bin/kista" "$TMP/kista/kista.yaml" hugr/e2e "${SEED[@]}" \
	>"$TMP/publish.log" 2>&1 || {
	cat "$TMP/publish.log" >&2
	fail "publishing the build's extensions into kista failed"
}
sed -n 's/^published/  published/p' "$TMP/publish.log"
echo "  kista on https://127.0.0.1:$REPO_PORT (channel hugr/e2e)"
(exec env $(env | sed -n "s/^\(KISTA_[A-Za-z0-9_]*\)=.*/-u \1/p") "$TMP/bin/kista" serve -config "$TMP/kista/kista.yaml") \
	>"$TMP/kista.log" 2>&1 &
KISTA_PID=$!
PREFIX="https://127.0.0.1:$REPO_PORT/hugr/e2e"
for _ in $(seq 100); do curl -sf --cacert "$TMP/ca.pem" "$PREFIX/.well-known/duckdb-extension-repo.json" >/dev/null && break; sleep 0.2; done
curl -sf --cacert "$TMP/ca.pem" "$PREFIX/.well-known/duckdb-extension-repo.json" >/dev/null || {
	cat "$TMP/kista.log" >&2
	fail "kista did not come up"
}

# --- the operator seeds the service and the policy catalog -----------------------------------------------------------
cat >"$TMP/seed.sql" <<EOF
LOAD '$PGEXT';
SET GLOBAL ca_cert_file = '$TMP/ca.pem';
CREATE SECRET etl (TYPE tresor, SCOPE 'tresor:$TS_HOST', FLOW 'client_credentials', CLIENT_ID 'etl', CLIENT_SECRET 'etl-secret', ISSUER '$ISSUER');
SET GLOBAL tresor_http_client = 'duckdb';
ATTACH 'tresor:$TS_HOST' AS owner (INSECURE_HTTP true, SECRET etl);
CREATE PERSISTENT SECRET policy_catalog IN owner (TYPE postgres, HOST '127.0.0.1', PORT '$PG_PORT', DATABASE 'postgres', USER 'postgres', PASSWORD 'pg-e2e');
CREATE PERSISTENT SECRET control_plane IN owner (TYPE http, SCOPE 'https://127.0.0.1:$CP_PORT/', BEARER_TOKEN 'cp-e2e-token');
CALL owner.grant_secret('policy_catalog', 'role:default-roles-tresor', ['use']);
-- spec 010: the reserved name an orchestrated node finds its catalog by - the secret's own type is the catalog's
CREATE PERSISTENT SECRET hugr_policy_catalog IN owner (TYPE postgres, HOST '127.0.0.1', PORT '$PG_PORT', DATABASE 'postgres', USER 'postgres', PASSWORD 'pg-e2e');
CALL owner.grant_secret('hugr_policy_catalog', 'role:default-roles-tresor', ['use']);
CALL owner.grant_secret('control_plane', 'role:default-roles-tresor', ['use']);
ATTACH 'host=127.0.0.1 port=$PG_PORT dbname=postgres user=postgres password=pg-e2e' AS pg (TYPE postgres);
SELECT acl_use_db('pg', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
-- a thread-count change joins every scheduler worker, so it proves the node does not run on one (spec 006)
ACL ADMIN CLUSTER SET threads = 3;
ACL ADMIN CLUSTER ATTACH ':memory:' AS e2e_src (TYPE duckdb);
SELECT 'version:' || acl_cluster_version();
EOF
"$DUCKDB" -no-agent -unsigned -bail -csv -noheader <"$TMP/seed.sql" >"$TMP/seed.log" 2>&1 || true
VERSION="$(sed -n 's/^version://p' "$TMP/seed.log")"
[ -n "$VERSION" ] || {
	redact <"$TMP/seed.log" >&2
	fail "the operator could not seed the service and the catalog"
}

# --- the control plane: the bootstrap document, only with the token tresor hands the node -----------------------------
document() { # <secret name>
	cat <<EOF
{"version": 1, "repository": {"name": "hugr", "prefix": "$PREFIX"},
 "catalog": {"type": "postgres", "extension": "postgres_scanner", "path": "", "secret": "$1", "schema": "acl"}}
EOF
}
document policy_catalog >"$TMP/bootstrap.json"
cat >"$TMP/cp.py" <<'PY'
import http.server, ssl, sys
doc_file, cert, key, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/v1/bootstrap" or self.headers.get("Authorization") != "Bearer cp-e2e-token":
            self.send_response(401); self.send_header("Content-Length", "0"); self.end_headers(); return
        body = open(doc_file, "rb").read()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *args): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
PY
python3 "$TMP/cp.py" "$TMP/bootstrap.json" "$TMP/tls.crt" "$TMP/tls.key" "$CP_PORT" &
CP_PID=$!
for _ in $(seq 50); do curl -s -o /dev/null --cacert "$TMP/ca.pem" "https://127.0.0.1:$CP_PORT/" && break; sleep 0.1; done

# --- the node: nothing but its environment ---------------------------------------------------------------------------
node_env=(env -i PATH="$PATH" HOME="$TMP/node-home"
	HUGR_TRESOR="$TS_HOST" HUGR_TRESOR_INSECURE_HTTP=1 HUGR_IDENTITY_ISSUER="$ISSUER" HUGR_IDENTITY_CLIENT_ID=keynode
	HUGR_IDENTITY_PRIVATE_KEY_FILE="$TMP/node-key.pem" HUGR_CONTROL_PLANE="https://127.0.0.1:$CP_PORT"
	HUGR_CA_FILE="$TMP/ca.pem" HUGR_FLIGHT="grpc://localhost:$FLIGHT_PORT" HUGR_DRAIN_DEADLINE=5)
port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$FLIGHT_PORT") 2>/dev/null; }
start_node() { # <log>
	mkdir -p "$TMP/node-home"
	if [ -z "$NODE_IMAGE" ]; then
		"${node_env[@]}" "$DUCKDB" -no-agent -c "PRAGMA hugr_node_run" >"$1" 2>&1 &
		NODE_PID=$!
		return
	fi
	# the image as a pod runs it: its own user, HOME in the image, only the HUGR_* variables. The files they name are
	# this run's, mounted read-only and readable the way a pod's fsGroup makes a secret volume readable: the group may
	# read (0640), others nothing - tresor refuses a key others may read
	local docker_env=() kv
	for kv in "${node_env[@]}"; do
		case "$kv" in HUGR_*) docker_env+=(-e "$kv") ;; esac
	done
	chmod 750 "$TMP" && chmod 640 "$TMP/ca.pem" "$TMP/node-key.pem"
	NODE_CONTAINER="hugr-node-e2e-$$-$RANDOM"
	docker run --rm --name "$NODE_CONTAINER" --network host --group-add "$(id -g)" -v "$TMP:$TMP:ro" \
		"${docker_env[@]}" "$NODE_IMAGE" >"$1" 2>&1 &
	NODE_PID=$! # the docker client: it hands SIGTERM to the container (tini, then the node) and returns its exit code
}
wait_ready_or_exit() { # <log>: 0 ready, 1 exited
	for _ in $(seq 120); do
		grep -q 'hugr_node: ready' "$1" && return 0
		kill -0 "$NODE_PID" 2>/dev/null || return 1
		sleep 0.5
	done
	return 1
}

start_node "$TMP/node.log"
wait_ready_or_exit "$TMP/node.log" || {
	redact <"$TMP/node.log" >&2
	fail "the node did not become ready"
}
grep -q 'step 3 bootstrap document: ok' "$TMP/node.log" || fail "no bootstrap document"
echo "ok: the node fetched the bootstrap document with the control plane's token from tresor"
grep -q 'step 4 repository: ok' "$TMP/node.log" && grep -q 'step 5 policy catalog: ok' "$TMP/node.log" ||
	fail "the repository or the catalog's extension did not take"
echo "ok: the node trusted the repository by its discovery document and installed postgres_scanner from it"
grep -c 'step 5 policy catalog: ok' "$TMP/node.log" | grep -q '^[3-9]' || fail "the policy catalog did not attach"
echo "ok: the node attached the policy catalog with the secret from tresor"
grep -q 'step 6 profile: setting threads: ok' "$TMP/node.log" && grep -q 'step 6 profile: source e2e_src: ok' "$TMP/node.log" &&
	grep -q "ready - profile version $VERSION," "$TMP/node.log" || fail "the profile was not applied (version $VERSION)"
echo "ok: the node applied the cluster profile (a setting, a source) at version $VERSION"
port_open || fail "the Flight port is not open"
echo "ok: the node serves Flight on localhost:$FLIGHT_PORT"
kill -TERM "$NODE_PID"
status=0
wait "$NODE_PID" || status=$?
NODE_PID=""
[ "$status" = 0 ] && grep -q 'hugr_node: stopped' "$TMP/node.log" || {
	redact <"$TMP/node.log" >&2
	fail "the node did not stop cleanly on SIGTERM (exit $status)"
}
echo "ok: on SIGTERM the node drained, stopped its doors and exited 0"

# --- a broken step: a secret the service does not have -----------------------------------------------------------------
document no_such_secret >"$TMP/bootstrap.json"
start_node "$TMP/broken.log"
if wait_ready_or_exit "$TMP/broken.log"; then fail "a node with a missing catalog secret became ready"; fi
status=0
wait "$NODE_PID" || status=$?
NODE_PID=""
[ "$status" != 0 ] && grep -q 'bootstrap step "5 policy catalog" failed' "$TMP/broken.log" && ! port_open || {
	redact <"$TMP/broken.log" >&2
	fail "a broken step did not stop the bootstrap with the step named (exit $status)"
}
echo "ok: a broken step stopped the bootstrap with the step named, and no port opened"

# --- spec 010: the orchestrated entry - two nodes, one catalog, a fake orchestrator ---------------------------------------
# the operator's SQL on the catalog, from outside any node (what an administrator's statement does through a node)
operator_sql() { # <sql>...
	{
		echo "LOAD '$PGEXT';"
		echo "ATTACH 'host=127.0.0.1 port=$PG_PORT dbname=postgres user=postgres password=pg-e2e' AS pg (TYPE postgres);"
		echo "SELECT acl_use_db('pg', 'acl');"
		echo "SET GLOBAL acl_allow_anonymous_admin = true;"
		printf '%s\n' "$@"
		echo "SELECT 'version:' || acl_cluster_version();"
	} >"$TMP/operator.sql"
	"$DUCKDB" -no-agent -unsigned -bail -csv -noheader <"$TMP/operator.sql" >"$TMP/operator.log" 2>&1 || {
		redact <"$TMP/operator.log" >&2
		fail "the operator's statements failed"
	}
	sed -n 's/^version://p' "$TMP/operator.log"
}
operator_sql "ACL ADMIN CREATE RESOURCE GROUP e2e;" "ACL ADMIN CLUSTER SET memory_limit = '500GB';" >/dev/null

# the projected tokens' stand-ins: a token per node, and the label the orchestrator answers it with
printf 'orch-token-a' >"$TMP/orch-token-a"
printf 'orch-token-b' >"$TMP/orch-token-b"
cat >"$TMP/orch.py" <<'PY'
import http.server, json, ssl, sys
prefix, ts_host, issuer, cert, key, port, beats = sys.argv[1:8]
labels = {"Bearer orch-token-a": "hugr-node-e2e-a", "Bearer orch-token-b": "hugr-node-e2e-b"}
class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self, status, body=b""):
        self.send_response(status); self.send_header("Content-Length", str(len(body)))
        if body: self.send_header("Content-Type", "application/json")
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        label = labels.get(self.headers.get("Authorization"))
        if self.path != "/v1/bootstrap" or not label: return self.answer(401)
        doc = {"version": 2, "node_group": "e2e", "node_label": label,
               "tresor": {"host": ts_host, "identity": {"issuer": issuer, "client_id": "keynode", "scope": ""}},
               "repository": {"name": "hugr", "prefix": prefix},
               "catalog": {"secret": "hugr_policy_catalog", "schema": "acl"}, "heartbeat": {"interval_seconds": 2}}
        self.answer(200, json.dumps(doc).encode())
    def do_POST(self):
        label = labels.get(self.headers.get("Authorization"))
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path != "/v1/heartbeat" or not label: return self.answer(401)
        beat = json.loads(body)
        if beat.get("node_id") != label: return self.answer(409)   # a node speaks only for itself
        with open(beats, "a") as f: f.write(json.dumps(beat) + "\n")
        self.answer(204)
    def log_message(self, *args): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
PY
python3 "$TMP/orch.py" "$PREFIX" "$TS_HOST" "$ISSUER" "$TMP/tls.crt" "$TMP/tls.key" "$ORCH_PORT" "$TMP/beats.jsonl" &
ORCH_PID=$!
for _ in $(seq 50); do curl -s -o /dev/null --cacert "$TMP/ca.pem" "https://127.0.0.1:$ORCH_PORT/" && break; sleep 0.1; done

start_orchestrated_node() { # <a|b> <flight port> <log>
	mkdir -p "$TMP/node-home-$1" "$TMP/spill-$1"
	local env_vars=(HUGR_ORCHESTRATOR="https://127.0.0.1:$ORCH_PORT" HUGR_ORCHESTRATOR_TOKEN_FILE="$TMP/orch-token-$1"
		HUGR_TRESOR_INSECURE_HTTP=1 HUGR_IDENTITY_PRIVATE_KEY_FILE="$TMP/node-key.pem" HUGR_CA_FILE="$TMP/ca.pem"
		HUGR_FLIGHT="grpc://localhost:$2" HUGR_DRAIN_DEADLINE=5 HUGR_POD_MEMORY=4294967296 HUGR_POD_CPU=2
		HUGR_TEMP_DIRECTORY="$TMP/spill-$1" HUGR_TEMP_SIZE=1GiB)
	if [ -z "$NODE_IMAGE" ]; then
		env -i PATH="$PATH" HOME="$TMP/node-home-$1" "${env_vars[@]}" "$DUCKDB" -no-agent -c "PRAGMA hugr_node_run" \
			>"$3" 2>&1 &
		eval "NODE_$(echo "$1" | tr a-z A-Z)_PID=$!"
		return
	fi
	local docker_env=() kv
	for kv in "${env_vars[@]}"; do docker_env+=(-e "$kv"); done
	chmod 640 "$TMP/orch-token-$1" && chmod 770 "$TMP/spill-$1"
	local name="hugr-node-e2e-$1-$$-$RANDOM"
	eval "NODE_$(echo "$1" | tr a-z A-Z)_CONTAINER=$name"
	docker run --rm --name "$name" --network host --group-add "$(id -g)" -v "$TMP:$TMP" "${docker_env[@]}" \
		"$NODE_IMAGE" >"$3" 2>&1 &
	eval "NODE_$(echo "$1" | tr a-z A-Z)_PID=$!"
}
wait_for() { # <seconds> <command...>: 0 when the command succeeds within the time
	local deadline=$((SECONDS + $1))
	shift
	while [ "$SECONDS" -lt "$deadline" ]; do
		"$@" && return 0
		sleep 0.5
	done
	return 1
}
# the latest heartbeat of a node, as one field: <label> <python expression over the beat `b`>
beat() {
	python3 - "$TMP/beats.jsonl" "$1" "$2" <<'PY'
import json, sys
beats, label, expr = sys.argv[1], sys.argv[2], sys.argv[3]
last = None
try:
    for line in open(beats):
        b = json.loads(line)
        if b.get("node_id") == label: last = b
except FileNotFoundError:
    pass
print("" if last is None else eval(expr, {"b": last}))
PY
}

start_orchestrated_node a "$FLIGHT_A_PORT" "$TMP/node-a.log"
start_orchestrated_node b "$FLIGHT_B_PORT" "$TMP/node-b.log"
for node in a b; do
	grep_ready() { grep -q 'hugr_node: ready' "$TMP/node-$node.log"; }
	wait_for 90 grep_ready || {
		redact <"$TMP/node-$node.log" >&2
		fail "orchestrated node $node did not become ready"
	}
	grep -q 'step 2 bootstrap document: ok' "$TMP/node-$node.log" && grep -q 'step 3 tresor: ok' "$TMP/node-$node.log" ||
		fail "node $node did not enter through the orchestrator"
	[ "$(grep -c 'step 6 engine: ok' "$TMP/node-$node.log")" = 3 ] || fail "node $node did not size its engine from the pod"
	grep -q "step 6 profile: memory_limit cut to the pod's ceiling" "$TMP/node-$node.log" &&
		grep -q "step 6 profile: threads cut to the pod's ceiling" "$TMP/node-$node.log" ||
		fail "node $node did not cut the profile's memory_limit and threads to the pod"
done
echo "ok: both nodes entered through the orchestrator, took tresor's identity and the catalog's type from it, and serve"
echo "ok: the engine follows the pod - three defaults set, a memory_limit and threads above the pod cut"

has_beat() { [ -n "$(beat "$1" 'b["config"]["applied"]')" ]; }
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	wait_for 20 has_beat "$label" || fail "no heartbeat from $label"
	[ "$(beat "$label" 'b["load"]["label"]')" = "$label" ] || fail "$label's load report does not carry its label"
	[ "$(beat "$label" '[p["name"] for p in b["config"]["pending"]]')" = "[]" ] ||
		fail "$label reports pending items at start: $(beat "$label" 'b["config"]["pending"]')"
	# the profile's memory_limit (500GB) and threads (3) are both above this pod (4 GiB, 2 cores)
	[ "$(beat "$label" 'b["drift"].get("cut", 0)')" = 2 ] ||
		fail "$label does not report its memory_limit and threads cut to the pod: $(beat "$label" 'b["drift"]')"
done
echo "ok: each heartbeat names its node by the document's label, the load report carries it, nothing is pending, the cut is drift"

HOT="$(operator_sql "ACL ADMIN CLUSTER ATTACH ':memory:' AS e2e_hot (TYPE duckdb);")"
for node in a b; do
	label="hugr-node-e2e-$node"
	took() { grep -q "tick: source e2e_hot applied (profile version $HOT)" "$TMP/node-$node.log"; }
	wait_for 20 took || fail "node $node did not take the hot ATTACH on its tick"
	applied_at() { [ "$(beat "$label" 'b["config"]["applied"]')" = "$HOT" ]; }
	wait_for 10 applied_at || fail "$label does not report version $HOT applied"
done
echo "ok: a hot ATTACH reached both nodes within a tick, reported applied at version $HOT"

# re-pointed to the same type: the drift cannot tell (one type), the node can - it is not what the node attached
operator_sql "ACL ADMIN CLUSTER ATTACH '$TMP/e2e_hot2.duckdb' AS e2e_hot (TYPE duckdb);" >/dev/null
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	repointed() { [ "$(beat "$label" 'any(p["name"] == "e2e_hot" and p["class"] == "drain" for p in b["config"]["pending"])')" = True ]; }
	wait_for 20 repointed || fail "$label does not report the re-pointed source pending a drain: $(beat "$label" 'b["config"]')"
done
operator_sql "ACL ADMIN CLUSTER DETACH e2e_hot;" >/dev/null
for node in a b; do
	label="hugr-node-e2e-$node"
	detached() { grep -q "tick: source e2e_hot detached" "$TMP/node-$node.log" &&
		[ "$(beat "$label" '[p["name"] for p in b["config"]["pending"]]')" = "[]" ]; }
	wait_for 20 detached || fail "node $node did not detach the source the profile dropped: $(beat "$label" 'b["config"]')"
done
echo "ok: a source re-pointed to the same type is pending a drain on both; dropped, it is detached by both"

# a hot item that fails on one node (both attach one file: the second finds it locked) is listed there, and the rest
# of the write still applies
operator_sql "ACL ADMIN CLUSTER ATTACH '$TMP/e2e_lock.duckdb' AS e2e_lock (TYPE duckdb);" \
	"ACL ADMIN CLUSTER ATTACH ':memory:' AS e2e_mem (TYPE duckdb);" >/dev/null
both_seen() {
	for label in hugr-node-e2e-a hugr-node-e2e-b; do
		grep -q "tick: source e2e_mem applied" "$TMP/node-${label##*-}.log" || return 1
	done
	[ "$(python3 - "$TMP/beats.jsonl" <<'PY'
import json, sys
last = {}
for line in open(sys.argv[1]):
    b = json.loads(line); last[b["node_id"]] = b
hot = [l for l, b in last.items() if any(p["name"] == "e2e_lock" and p["class"] == "hot" and p["error"] for p in b["config"]["pending"])]
print(len(hot))
PY
)" = 1 ]
}
wait_for 20 both_seen || fail "the locked file is not listed on exactly one node, or e2e_mem did not apply on both: $(beat hugr-node-e2e-a 'b["config"]') $(beat hugr-node-e2e-b 'b["config"]')"
operator_sql "ACL ADMIN CLUSTER DETACH e2e_lock;" "ACL ADMIN CLUSTER DETACH e2e_mem;" >/dev/null
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	settled() { [ "$(beat "$label" '[p["name"] for p in b["config"]["pending"]]')" = "[]" ]; }
	wait_for 20 settled || fail "$label still reports pending items: $(beat "$label" 'b["config"]')"
done
echo "ok: a hot item failing on one node is listed there with its error's class, the rest applied; dropped, nothing waits"

operator_sql "ACL ADMIN CLUSTER SET acl_max_sessions = 77 IN GROUP e2e;" >/dev/null
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	waits() { [ "$(beat "$label" 'b["config"]["needs_restart"] and any(p["name"] == "acl_max_sessions" and p["class"] == "restart" for p in b["config"]["pending"])')" = True ]; }
	wait_for 20 waits || fail "$label does not report the restart item: $(beat "$label" 'b["config"]')"
done
echo "ok: a restart-class item is reported by both as needs_restart, the item pending"
operator_sql "ACL ADMIN CLUSTER RESET acl_max_sessions IN GROUP e2e;" >/dev/null
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	withdrawn() { [ "$(beat "$label" 'b["config"]["needs_restart"]')" = False ]; }
	wait_for 20 withdrawn || fail "$label still waits for a restart item withdrawn before it ran: $(beat "$label" 'b["config"]')"
done
# a setting the node took at its bootstrap and the profile dropped (RESET) needs the restart to go
operator_sql "ACL ADMIN CLUSTER RESET memory_limit;" >/dev/null
for label in hugr-node-e2e-a hugr-node-e2e-b; do
	reset_waits() { [ "$(beat "$label" 'b["config"]["needs_restart"] and any(p["name"] == "memory_limit" and p["class"] == "restart" for p in b["config"]["pending"])')" = True ]; }
	wait_for 20 reset_waits || fail "$label does not report the RESET memory_limit pending a restart: $(beat "$label" 'b["config"]')"
done
echo "ok: a restart item withdrawn before it ran waits no more; a setting the node took and the profile dropped waits for the restart"

operator_sql "ACL ADMIN DROP RESOURCE GROUP e2e;" >/dev/null
for node in a b; do
	label="hugr-node-e2e-$node"
	drained() { grep -q 'tick: the node.s resource group is not in the policy - draining' "$TMP/node-$node.log" &&
		[ "$(beat "$label" 'b["load"]["draining"]')" = True ]; }
	wait_for 20 drained || fail "node $node did not drain when its resource group went"
done
operator_sql "ACL ADMIN CREATE RESOURCE GROUP e2e;" >/dev/null
for node in a b; do
	label="hugr-node-e2e-$node"
	resumed() { grep -q 'tick: the node.s resource group is back - resumed' "$TMP/node-$node.log" &&
		[ "$(beat "$label" 'b["load"]["draining"]')" = False ]; }
	wait_for 20 resumed || fail "node $node did not resume when its resource group came back"
done
echo "ok: a dropped resource group drained both nodes by themselves, and recreating it resumed them"

for node in a b; do
	pid_var="NODE_$(echo "$node" | tr a-z A-Z)_PID"
	kill -TERM "${!pid_var}"
	status=0
	wait "${!pid_var}" || status=$?
	eval "$pid_var="
	[ "$status" = 0 ] && grep -q 'hugr_node: stopped' "$TMP/node-$node.log" || {
		redact <"$TMP/node-$node.log" >&2
		fail "orchestrated node $node did not stop cleanly (exit $status)"
	}
done
echo "ok: both orchestrated nodes stopped cleanly on SIGTERM"
echo "PASS: a node bootstraps itself"
