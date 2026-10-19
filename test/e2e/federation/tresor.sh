#!/usr/bin/env bash
# The `hugr` type's identity (spec 011, "The ATTACH"): A's link to B is a secret in A's tresor whose token tresor
# mints for the node at every read (`mint+exchange://`, tresor spec 029), never a credential of A's. A real
# tresor-server and the Keycloak of tresor's own test compose, B (a hugr_node with a quack door, trusting Keycloak for
# the exchanged token's audience) and A (a hugr_node attached to tresor as its node identity, acl-node). Checked:
#   1. A attaches B with the secret's name only; B verified the minted token and answers A's node role (nodes);
#   2. a user of A (alice, through an acl session) reads B through A's virtual table;
#   3. B ended A's sessions: the reconnect inside alice's statement reads the secret as the node - alice may not use
#      it - and her statement succeeds through the one retry;
#   4. no token is in A's or B's output.
# Needs docker, go, python3, curl and the hugr_node build. A missing tool or build is a SKIP (exit 0).
#
#   test/e2e/federation/tresor.sh   # TRESOR_SERVER_DIR=<checkout> builds that tresor-server; otherwise its main is
#                                   # cloned (TRESOR_SERVER_REF). KEEP_KEYCLOAK=1 leaves Keycloak running.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
KC_PORT="${KEYCLOAK_PORT:-18680}"
SERVER_PORT="${TRESOR_SERVER_PORT:-18643}"
B_PORT="${B_PORT:-31721}"
ISSUER="http://127.0.0.1:$KC_PORT/realms/tresor"
HOST="127.0.0.1:$SERVER_PORT"

skip() {
	echo "SKIP: $*"
	exit 0
}
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
redact() { sed -E -e 's/eyJ[A-Za-z0-9._-]*/<token>/g' -e 's/[0-9A-Fa-f]{32}/<handle>/g'; }

for tool in docker go python3 curl openssl; do command -v "$tool" >/dev/null || skip "$tool not found"; done
docker info >/dev/null 2>&1 || skip "docker is not running"
[ -x "$DUCKDB" ] || skip "no hugr_node build at $DUCKDB (GEN=ninja make)"
[ -f "$ROOT/tresor/server/docker-compose.yml" ] || skip "no tresor submodule (git submodule update --init tresor)"
if curl -s -o /dev/null "http://$HOST/"; then
	fail "$HOST is taken - set TRESOR_SERVER_PORT"
fi
if curl -s -o /dev/null "http://127.0.0.1:$KC_PORT/" && ! curl -sf -o /dev/null "$ISSUER/.well-known/openid-configuration"; then
	fail "127.0.0.1:$KC_PORT is taken - set KEYCLOAK_PORT"
fi

TMP="$(mktemp -d)"
COMPOSE=(docker compose -p hugr-node-e2e-federation-kc -f "$ROOT/tresor/server/docker-compose.yml")
cleanup() {
	exec 3>&- 4>&- 2>/dev/null || true
	for pid in "${A_PID:-}" "${B_PID:-}" "${SERVER_PID:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null || true
	done
	[ "${KEEP_KEYCLOAK:-0}" = 1 ] || "${COMPOSE[@]}" down >/dev/null 2>&1 || true
	rm -rf "$TMP"
}
trap cleanup EXIT

echo "the hugr type's identity e2e (hugr-node spec 011)"

# --- the service: tresor-server on SQLite, sealed under a KEK made for this run --------------------------------------
if [ -n "${TRESOR_SERVER_DIR:-}" ]; then
	server_src="$(cd "$TRESOR_SERVER_DIR" && pwd)"
else
	git clone -q --depth 1 --branch "${TRESOR_SERVER_REF:-main}" https://github.com/hugr-lab/tresor-server \
		"$TMP/tresor-server" || fail "could not clone tresor-server"
	server_src="$TMP/tresor-server"
fi
(cd "$server_src" && GOWORK=off CGO_ENABLED=0 go build -o "$TMP/bin/tresor-server" ./cmd/tresor-server) ||
	fail "tresor-server did not build"
sed -e "s/127.0.0.1:18480/127.0.0.1:$KC_PORT/g" -e "s/127.0.0.1:18443/127.0.0.1:$SERVER_PORT/g" \
	"$server_src/testdata/keycloak/server.yaml" >"$TMP/server.yaml"

echo "  Keycloak on 127.0.0.1:$KC_PORT"
KEYCLOAK_PORT="$KC_PORT" "${COMPOSE[@]}" up -d >/dev/null 2>&1 || fail "Keycloak did not start"
for _ in $(seq 120); do curl -sf "$ISSUER/.well-known/openid-configuration" >/dev/null && break; sleep 2; done
curl -sf "$ISSUER/.well-known/openid-configuration" >/dev/null || fail "Keycloak did not come up"

echo "  tresor-server on $HOST"
TRESOR_TEST_KEK="$(openssl rand -base64 32)"
TRESOR_STATE__KIND=sqlite TRESOR_STATE__PATH="$TMP/tresor.db" TRESOR_STATE__MAC=true \
	TRESOR_KEYS__KIND=local TRESOR_KEYS__KEY_ENV=TRESOR_TEST_KEK TRESOR_TEST_KEK="$TRESOR_TEST_KEK" \
	TRESOR_EXCHANGE_SECRET=svc-secret "$TMP/bin/tresor-server" -config "$TMP/server.yaml" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
disown "$SERVER_PID"
for _ in $(seq 150); do curl -sf "http://$HOST/.well-known/duckdb-secrets" >/dev/null && break; sleep 0.2; done
curl -sf "http://$HOST/.well-known/duckdb-secrets" >/dev/null || fail "tresor-server did not come up"

token() { # <user>: a token of acl-door's (meant for acl-node), as a door receives it
	curl -sf -d grant_type=password -d client_id=acl-door -d "username=$1" -d "password=$1-pass" -d scope=openid \
		"$ISSUER/protocol/openid-connect/token" | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])'
}

# --- the operator seeds the service: the issuer's keys for A, and A's link to B - a token minted for the caller -------
jwks="$(curl -sf "$ISSUER/protocol/openid-connect/certs")" || fail "no JWKS from Keycloak"
cat >"$TMP/seed.sql" <<EOF
CREATE SECRET etl (TYPE tresor, SCOPE 'tresor:$HOST', FLOW 'client_credentials', CLIENT_ID 'etl', CLIENT_SECRET 'etl-secret', ISSUER '$ISSUER');
ATTACH 'tresor:$HOST' AS owner (INSECURE_HTTP true, SECRET etl);
CREATE PERSISTENT SECRET kc_conn IN owner (TYPE oidc_issuer, URL '$ISSUER', KEYS '$jwks');
CALL owner.grant_secret('kc_conn', 'role:nodes', ['use']);
CREATE PERSISTENT SECRET b_link IN owner (TYPE hugr, SCOPE 'hugr:localhost:$B_PORT', TOKEN 'mint+exchange://echo-api');
CALL owner.grant_secret('b_link', 'role:nodes', ['use']);
SELECT 'seeded:' || count(*) FROM owner.secrets() WHERE name IN ('kc_conn', 'b_link');
EOF
cd "$ROOT"
"$DUCKDB" -no-agent -unsigned -csv -noheader <"$TMP/seed.sql" >"$TMP/seed.log" 2>&1 || true
grep -q '^seeded:2$' "$TMP/seed.log" || {
	redact <"$TMP/seed.log" >&2
	fail "the operator could not seed the service"
}

# --- B and A: hugr_node processes whose stdin is ours (fd 3: A, fd 4: B) ------------------------------------------------
mkfifo "$TMP/A.in" "$TMP/B.in"
"$DUCKDB" -no-agent -unsigned -csv -noheader <"$TMP/A.in" >"$TMP/A.log" 2>&1 &
A_PID=$!
disown "$A_PID"
exec 3>"$TMP/A.in"
"$DUCKDB" -no-agent -unsigned -csv -noheader <"$TMP/B.in" >"$TMP/B.log" 2>&1 &
B_PID=$!
disown "$B_PID"
exec 4>"$TMP/B.in"
fd_of() { [ "$1" = A ] && echo 3 || echo 4; }
pid_of() { [ "$1" = A ] && echo "$A_PID" || echo "$B_PID"; }
says() {
	local node=$1 mark="m$$_$RANDOM$RANDOM"
	echo "SELECT '$mark' AS m, * FROM ($2);" >&"$(fd_of "$node")"
	for _ in $(seq 600); do
		if grep -q "^$mark," "$TMP/$node.log"; then
			sleep 0.2
			grep "^$mark," "$TMP/$node.log" | cut -d, -f2- | tr -d '\r'
			return 0
		fi
		kill -0 "$(pid_of "$node")" 2>/dev/null || break
		sleep 0.1
	done
	redact <"$TMP/$node.log" | tail -30 >&2
	fail "node $node did not answer: $(echo "$2" | redact)"
}
run() {
	local node=$1 before
	shift
	before="$(wc -l <"$TMP/$node.log")"
	printf '%s\n' "$@" >&"$(fd_of "$node")"
	says "$node" "SELECT 1" >/dev/null
	if tail -n +"$((before + 1))" "$TMP/$node.log" | grep -q "Error"; then
		tail -n +"$((before + 1))" "$TMP/$node.log" | redact >&2
		fail "node $node: a statement failed"
	fi
}

# B trusts Keycloak for the audience tresor mints A's token for; the token's realm role (nodes) is A's role on B
run B "SET GLOBAL acl_jwks_locations = 'http://127.0.0.1:$KC_PORT/';" \
	"ATTACH ':memory:' AS store;" "ATTACH ':memory:' AS phys;" \
	"CREATE TABLE phys.main.orders(id INTEGER, tenant VARCHAR, ssn VARCHAR);" \
	"INSERT INTO phys.main.orders VALUES (1, 'acme', 's1'), (2, 'globex', 's2');" \
	"SELECT acl_use_db('store', 'acl', true);" \
	"SET GLOBAL acl_allow_anonymous_admin = true;" \
	"ACL ADMIN CREATE ISSUER '$ISSUER' AUDIENCES ('echo-api') ROLE CLAIM 'realm_access.roles';" \
	"ACL ADMIN CREATE VIRTUAL CATALOG c;" \
	"ACL ADMIN CREATE VIRTUAL TABLE c.orders AS phys.main.orders COLUMNS (id, tenant);" \
	"ACL ADMIN CREATE ROLE nodes;" \
	"ACL ADMIN GRANT CATALOG c TO ROLE nodes WITH (select) MAIN;" \
	"SET GLOBAL acl_allow_anonymous_admin = false;" \
	"SELECT acl_quack_serve('quack:localhost:$B_PORT', 'e2e-server-token');"

# A: tresor attached as the node's identity (acl-node), acting for its users' sessions
run A "CREATE SECRET node_login (TYPE tresor, SCOPE 'tresor:elsewhere', FLOW 'client_credentials', CLIENT_ID 'acl-node', CLIENT_SECRET 'node-secret', ISSUER '$ISSUER');" \
	"ATTACH 'tresor:$HOST' AS node (INSECURE_HTTP true, SECRET node_login);" \
	"ATTACH ':memory:' AS store;" \
	"SELECT acl_use_db('store', 'acl', true);" \
	"SET GLOBAL acl_allow_anonymous_admin = true;" \
	"ACL ADMIN CREATE ISSUER kc FROM SECRET kc_conn IN node AUDIENCES ('acl-node') ROLE CLAIM 'realm_access.roles';" \
	"SELECT changed FROM node.act_for_sessions();"

# --- 1. A attaches B with the secret's name: tresor mints the token for the node, B verifies it ------------------------
run A "ATTACH 'localhost:$B_PORT' AS b (TYPE hugr, SECRET b_link, DISABLE_SSL true, POOL_SIZE 2);"
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A does not read B"
[ "$(says B "SELECT len(regexp_extract_all(acl_sessions(), '\"roles\":\\[[^]]*\"nodes\"'))")" -ge 1 ] ||
	fail "B does not seat A as its node role"
echo "ok: A attached B by the secret's name; tresor minted the node's token, B seated it as nodes"

# --- 2. a user of A reads B through A's virtual table ------------------------------------------------------------------
run A "ACL ADMIN CREATE VIRTUAL CATALOG a;" \
	"ACL ADMIN CREATE VIRTUAL TABLE a.orders AS b.main.orders;" \
	"ACL ADMIN CREATE ROLE analysts;" \
	"ACL ADMIN GRANT CATALOG a TO ROLE analysts WITH (select) MAIN;"
alice="$(token alice)" && [ -n "$alice" ] || fail "no token for alice"
handle="$(says A "SELECT acl_session_open('$alice')")"
[ ${#handle} -eq 32 ] || fail "A opened no session for alice"
# alice_reads: her statement's answer (a statement under an ACL SESSION prefix is not one `says` can wrap)
alice_reads() {
	local before
	before="$(wc -l <"$TMP/A.log")"
	echo "ACL SESSION '$handle' SELECT 'read:' || count(*) FROM orders;" >&3
	says A "SELECT 1" >/dev/null
	tail -n +"$((before + 1))" "$TMP/A.log" | grep -q '^read:2' || {
		tail -n +"$((before + 1))" "$TMP/A.log" | redact >&2
		return 1
	}
}
alice_reads || fail "alice does not read B through A's virtual table"
echo "ok: alice reads B through A's virtual table"

# --- 3. B ends A's sessions: the reconnect inside alice's statement reads the secret as the node ------------------------
retries="$(says A "SELECT coalesce(sum(value), 0) FROM acl_metrics() WHERE name = 'hugr.pool.retries'")"
[ "$(says B "SELECT count(*) FROM (SELECT unnest(regexp_extract_all(acl_sessions(), '\"id\":\"([^\"]+)\"[^}]*\"door\":\"quack\"', 1)) AS id) WHERE acl_session_kill(id)")" -ge 1 ] ||
	fail "B ended none of A's sessions"
alice_reads || fail "alice's statement did not read B after B ended A's sessions"
[ "$(says A "SELECT coalesce(sum(value), 0) FROM acl_metrics() WHERE name = 'hugr.pool.retries'")" = "$((retries + 1))" ] ||
	fail "the statement was not retried once"
echo "ok: B ended A's sessions - alice's statement reconnected, reading the secret as the node (only nodes may use it)"

# --- 4. no token in the output -------------------------------------------------------------------------------------------
if grep -q 'eyJ' "$TMP/A.log" "$TMP/B.log"; then
	fail "a token is in a node's output"
fi
echo "ok: no token in A's or B's output"

echo "PASS: the hugr type's identity through tresor"
