#!/usr/bin/env bash
# A node bootstraps itself (spec 006): `duckdb -c "CALL hugr_node_run()"` with nothing but its environment. Around it:
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
# Needs docker, go, python3, curl, openssl and the hugr_node build with the e2e's postgres_scanner
# (HUGR_NODE_E2E=1 GEN=ninja make). A missing tool or build is a SKIP (exit 0) on a developer's machine; CI asserts the
# run happened.
#
#   test/e2e/bootstrap/run.sh   # TRESOR_SERVER_DIR / REPOSITORY_DIR = checkouts to build; otherwise their main is cloned
#   (no KISTA_* variable here: kista reads its configuration from them)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
PGEXT="${PGEXT:-$ROOT/build/release/extension/postgres_scanner/postgres_scanner.duckdb_extension}"
KC_PORT="${KEYCLOAK_PORT:-18680}"
TS_PORT="${TRESOR_SERVER_PORT:-18643}"
REPO_PORT="${REPOSITORY_PORT:-18744}"
CP_PORT="${CONTROL_PLANE_PORT:-18745}"
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
[ -f "$ROOT/tresor/server/docker-compose.yml" ] || skip "no tresor submodule (git submodule update --init tresor)"
for port in "$TS_PORT" "$REPO_PORT" "$CP_PORT" "$PG_PORT" "$FLIGHT_PORT"; do
	if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then fail "127.0.0.1:$port is taken"; fi
done

TMP="$(mktemp -d)"
COMPOSE=(docker compose -p hugr-node-bootstrap-kc -f "$ROOT/tresor/server/docker-compose.yml")
PG_CONTAINER="hugr-node-bootstrap-pg"
cleanup() {
	for pid in "${NODE_PID:-}" "${SERVER_PID:-}" "${KISTA_PID:-}" "${CP_PID:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null || true
	done
	docker rm -f "$PG_CONTAINER" >/dev/null 2>&1 || true
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
duckdb_version="$(kista ext inspect "$PGEXT" | sed -n 's/^duckdb version: *//p')"
[ -n "$duckdb_version" ] || fail "kista could not read the build's duckdb version from $PGEXT"
ka migrate >/dev/null
ka tenant create hugr >/dev/null
ka channel create hugr/e2e -kind signed >/dev/null
ka version add "$duckdb_version" -kind dev >/dev/null
ka channel versions hugr/e2e -add "$duckdb_version" >/dev/null
ka key add hugr/e2e -signer file:e2e.pem -active >/dev/null
ka release add hugr/e2e "$PGEXT" -name postgres_scanner >/dev/null
echo "  kista on https://127.0.0.1:$REPO_PORT (channel hugr/e2e, duckdb $duckdb_version)"
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
CALL owner.grant_secret('control_plane', 'role:default-roles-tresor', ['use']);
ATTACH 'host=127.0.0.1 port=$PG_PORT dbname=postgres user=postgres password=pg-e2e' AS pg (TYPE postgres);
SELECT acl_use_db('pg', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
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
	"${node_env[@]}" "$DUCKDB" -no-agent -c "CALL hugr_node_run()" >"$1" 2>&1 &
	NODE_PID=$!
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
echo "PASS: a node bootstraps itself"
