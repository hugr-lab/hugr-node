#!/usr/bin/env bash
# tresor inside an acl cluster (spec 002, part 2): a real tresor-server (built from hugr-lab/tresor-server, its state
# in SQLite sealed under a local KEK), the Keycloak of tresor's own test compose (the realm tresor's suites use) and
# two hugr_node nodes running side by side. What the nodes share is the secrets service and nothing else - each has
# its own policy catalog, from the same bootstrap. Checked:
#   1. a secret a principal writes through node A is used on node B, which never had it;
#   2. rotated through A, the running B reads the new material - no restart, no sync of its own;
#   3. both nodes verify tokens with an issuer whose keys come from the service (duckdb-acl spec 095, FROM SECRET),
#      and the secret taken away fails that issuer closed on both;
#   4. the service's SQLite database holds no material in the clear.
# Needs docker, go, python3, curl, openssl and the hugr_node build (build/release/duckdb, hugr_node linked in). A
# missing tool or build is a SKIP (exit 0) on a developer's machine; CI asserts the run happened.
#
#   test/e2e/tresor/run.sh      # TRESOR_SERVER_DIR=<checkout> builds that tresor-server; otherwise its main is cloned
#                               # (TRESOR_SERVER_REF). KEEP_KEYCLOAK=1 leaves Keycloak running.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
KC_PORT="${KEYCLOAK_PORT:-18580}"
SERVER_PORT="${TRESOR_SERVER_PORT:-18543}"
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
# a dump never shows a token or a session handle
redact() { sed -E -e 's/eyJ[A-Za-z0-9._-]*/<token>/g' -e 's/[0-9A-Fa-f]{32}/<handle>/g'; }

for tool in docker go python3 curl openssl; do command -v "$tool" >/dev/null || skip "$tool not found"; done
docker info >/dev/null 2>&1 || skip "docker is not running"
[ -x "$DUCKDB" ] || skip "no hugr_node build at $DUCKDB (GEN=ninja make)"
[ -f "$ROOT/tresor/server/docker-compose.yml" ] || skip "no tresor submodule (git submodule update --init tresor)"
# the service's port must be free; Keycloak's may be this script's own, left running by KEEP_KEYCLOAK=1
if curl -s -o /dev/null "http://$HOST/"; then
	fail "$HOST is taken - set TRESOR_SERVER_PORT"
fi
if curl -s -o /dev/null "http://127.0.0.1:$KC_PORT/" && ! curl -sf -o /dev/null "$ISSUER/.well-known/openid-configuration"; then
	fail "127.0.0.1:$KC_PORT is taken - set KEYCLOAK_PORT"
fi

TMP="$(mktemp -d)"
# a project of its own: a developer's standing Keycloak (tresor-kc, tresor-kc-test) is left alone
COMPOSE=(docker compose -p hugr-node-e2e-kc -f "$ROOT/tresor/server/docker-compose.yml")
cleanup() {
	exec 3>&- 4>&- || true
	for pid in "${A_PID:-}" "${B_PID:-}" "${SERVER_PID:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null || true
	done
	[ "${KEEP_KEYCLOAK:-0}" = 1 ] || "${COMPOSE[@]}" down >/dev/null 2>&1 || true
	rm -rf "$TMP"
}
trap cleanup EXIT

echo "tresor in the cluster e2e (hugr-node spec 002)"

# --- the service: tresor-server on SQLite, sealed under a KEK made for this run ------------------------------------
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
curl -sf "$ISSUER/.well-known/openid-configuration" >/dev/null || {
	"${COMPOSE[@]}" logs --tail 50 >&2
	fail "Keycloak did not come up"
}

echo "  tresor-server on $HOST (state: SQLite)"
TRESOR_TEST_KEK="$(openssl rand -base64 32)"
TRESOR_STATE__KIND=sqlite TRESOR_STATE__PATH="$TMP/tresor.db" TRESOR_STATE__MAC=true \
	TRESOR_KEYS__KIND=local TRESOR_KEYS__KEY_ENV=TRESOR_TEST_KEK TRESOR_TEST_KEK="$TRESOR_TEST_KEK" \
	TRESOR_EXCHANGE_SECRET=svc-secret "$TMP/bin/tresor-server" -config "$TMP/server.yaml" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
disown "$SERVER_PID"
for _ in $(seq 150); do curl -sf "http://$HOST/.well-known/duckdb-secrets" >/dev/null && break; sleep 0.2; done
kill -0 "$SERVER_PID" 2>/dev/null && curl -sf "http://$HOST/.well-known/duckdb-secrets" >/dev/null || {
	cat "$TMP/server.log" >&2
	fail "tresor-server did not come up"
}

token() { # <user> -> a token of acl-door's (meant for acl-node), as a door receives it
	curl -sf -d grant_type=password -d client_id=acl-door -d "username=$1" -d "password=$1-pass" -d scope=openid \
		"$ISSUER/protocol/openid-connect/token" | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])'
}

# --- the operator seeds the service: the issuer's keys as an oidc_issuer secret, usable by the nodes' role --------
# the etl service is a service admin (tresor-server's testdata); the keys are Keycloak's own JWKS, pasted (KEYS)
jwks="$(curl -sf "$ISSUER/protocol/openid-connect/certs")" || fail "no JWKS from Keycloak"
cat >"$TMP/seed.sql" <<EOF
CREATE SECRET etl (TYPE tresor, SCOPE 'tresor:$HOST', FLOW 'client_credentials', CLIENT_ID 'etl', CLIENT_SECRET 'etl-secret', ISSUER '$ISSUER');
ATTACH 'tresor:$HOST' AS owner (INSECURE_HTTP true, SECRET etl);
CREATE PERSISTENT SECRET kc_conn IN owner (TYPE oidc_issuer, URL '$ISSUER', KEYS '$jwks');
CALL owner.grant_secret('kc_conn', 'role:nodes', ['use']);
SELECT 'seeded:' || count(*) FROM owner.secrets() WHERE name = 'kc_conn';
EOF
"$DUCKDB" -no-agent -unsigned -csv -noheader <"$TMP/seed.sql" >"$TMP/seed.log" 2>&1 || true
grep -q '^seeded:1$' "$TMP/seed.log" || {
	redact <"$TMP/seed.log" >&2
	fail "the operator could not seed the service"
}

# --- two nodes, side by side: each a hugr_node process whose stdin is ours (fd 3: A, fd 4: B) ----------------------
# one statement per line: the ACL prefix is the start of a query, and the CLI reads a line whole
bootstrap() {
	cat <<EOF
CREATE SECRET node_login (TYPE tresor, SCOPE 'tresor:elsewhere', FLOW 'client_credentials', CLIENT_ID 'acl-node', CLIENT_SECRET 'node-secret', ISSUER '$ISSUER');
ATTACH 'tresor:$HOST' AS node (INSECURE_HTTP true, SECRET node_login);
ATTACH ':memory:' AS store;
SELECT acl_use_db('store', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
ACL ADMIN CREATE ISSUER kc FROM SECRET kc_conn IN node AUDIENCES ('acl-node') ROLE CLAIM 'realm_access.roles';
SELECT changed FROM node.act_for_sessions();
ACL ADMIN CREATE VIRTUAL CATALOG c;
ACL ADMIN CREATE ROLE secrets_admin;
ACL ADMIN GRANT CATALOG c TO ROLE secrets_admin WITH (select, secrets) MAIN;
ACL ADMIN CREATE ROLE analysts;
ACL ADMIN GRANT CATALOG c TO ROLE analysts WITH (select) MAIN;
EOF
}
mkfifo "$TMP/A.in" "$TMP/B.in"
# -unredacted: the checks read a secret's (unredacted) parameter as the node holds it (tresor_secret_param)
# (disowned: their end at cleanup is ours, not a job report)
"$DUCKDB" -no-agent -unsigned -unredacted -csv -noheader <"$TMP/A.in" >"$TMP/A.log" 2>&1 &
A_PID=$!
disown "$A_PID"
exec 3>"$TMP/A.in"
"$DUCKDB" -no-agent -unsigned -unredacted -csv -noheader <"$TMP/B.in" >"$TMP/B.log" 2>&1 &
B_PID=$!
disown "$B_PID"
exec 4>"$TMP/B.in"

fd_of() { [ "$1" = A ] && echo 3 || echo 4; }
pid_of() { [ "$1" = A ] && echo "$A_PID" || echo "$B_PID"; }
# says <node> <sql>: the answer's csv lines (one query, its marker cut off)
says() {
	local node=$1 mark="m$$_$RANDOM$RANDOM"
	echo "SELECT '$mark' AS m, * FROM ($2);" >&"$(fd_of "$node")"
	for _ in $(seq 300); do
		if grep -q "^$mark," "$TMP/$node.log"; then
			sleep 0.2 # the rest of the answer, if it is more than one line
			grep "^$mark," "$TMP/$node.log" | cut -d, -f2- | tr -d '\r'
			return 0
		fi
		kill -0 "$(pid_of "$node")" 2>/dev/null || break
		sleep 0.1
	done
	redact <"$TMP/$node.log" | tail -30 >&2
	fail "node $node did not answer"
}
# run <node> <statement...>: statements that must not fail - nothing the node printed between them and the marker
# is an error
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
# until <seconds> <node> <sql> <expected>: the answer becomes the expected one within the time
until_says() {
	local seconds=$1 node=$2 sql=$3 want=$4 got=""
	for _ in $(seq "$((seconds * 2))"); do
		got="$(says "$node" "$sql")"
		[ "$got" = "$want" ] && return 0
		sleep 0.5
	done
	fail "$(echo "node $node answered '$got' for $sql, not '$want', within ${seconds}s" | redact)"
}

lines=()
while IFS= read -r line; do lines+=("$line"); done < <(bootstrap)
for node in A B; do
	run "$node" "${lines[@]}"
done
echo "  two nodes up, each attached to the service and trusting Keycloak through it"

# --- 3a. the issuer's keys come from the service: a person's token verifies on both nodes --------------------------
alice="$(token alice)" && [ -n "$alice" ] || fail "no token for alice"
bob="$(token bob)" && [ -n "$bob" ] || fail "no token for bob"
for node in A B; do
	[ "$(says "$node" "SELECT acl_session_open('$alice') IS NOT NULL")" = true ] ||
		fail "node $node did not verify alice's token with the service's keys"
done
echo "ok: both nodes verify tokens with the issuer's keys from the service"

# --- 1. a secret a principal writes through A is used on B ----------------------------------------------------------
# bob is a service admin: under his session node A acts for him (tresor), so the secret is his to make and grant
handle="$(says A "SELECT acl_session_open('$bob')")"
[ ${#handle} -eq 32 ] || fail "node A opened no session for bob"
run A "ACL SESSION '$handle' CREATE PERSISTENT SECRET shared_s (TYPE http, SCOPE 'https://shared.example', HTTP_PROXY 'proxy-v1', BEARER_TOKEN 'hugr-e2e-s3cr3t-v1');" \
	"ACL SESSION '$handle' ACL GRANT SECRET shared_s TO ROLE nodes;"
# B listed the service's secrets when it attached, before this one existed: tresor refreshes the list within 30s
until_says 45 B "SELECT coalesce(max(name), 'none') FROM which_secret('https://shared.example/x', 'http')" shared_s
got="$(says B "SELECT tresor_secret_param('shared_s', 'http_proxy')")"
[ "$got" = "VARCHAR proxy-v1" ] || fail "node B does not read the material written through node A: '$got'"
echo "ok: a secret written through node A is used on node B"

# --- 2. rotated through A, the running B reads the new material (tresor's descriptor list lives 30s) --------------
run A "ACL SESSION '$handle' CREATE OR REPLACE PERSISTENT SECRET shared_s (TYPE http, SCOPE 'https://shared.example', HTTP_PROXY 'proxy-v2', BEARER_TOKEN 'hugr-e2e-s3cr3t-v2');"
until_says 45 B "SELECT tresor_secret_param('shared_s', 'http_proxy')" "VARCHAR proxy-v2"
echo "ok: rotated through node A, the running node B reads the new material"
says A "SELECT acl_session_close('$handle')" >/dev/null

# --- 4. at rest: the service's database holds no material in the clear --------------------------------------------
[ -s "$TMP/tresor.db" ] || fail "the service wrote no SQLite database"
for f in "$TMP/tresor.db" "$TMP/tresor.db-wal"; do
	if [ -f "$f" ] && grep -aq 'hugr-e2e-s3cr3t' "$f"; then
		fail "$(basename "$f") holds a secret's material in the clear"
	fi
done
echo "ok: the service's SQLite database holds no material in the clear"

# --- 3b. the issuer's secret taken away: the issuer fails closed on both nodes --------------------------------------
"$DUCKDB" -no-agent -unsigned -csv -noheader >"$TMP/unseed.log" 2>&1 <<EOF || true
CREATE SECRET etl (TYPE tresor, SCOPE 'tresor:$HOST', FLOW 'client_credentials', CLIENT_ID 'etl', CLIENT_SECRET 'etl-secret', ISSUER '$ISSUER');
ATTACH 'tresor:$HOST' AS owner (INSECURE_HTTP true, SECRET etl);
DROP PERSISTENT SECRET kc_conn FROM owner;
SELECT 'dropped:' || count(*) FROM owner.secrets() WHERE name = 'kc_conn';
EOF
grep -q '^dropped:0$' "$TMP/unseed.log" || {
	redact <"$TMP/unseed.log" >&2
	fail "the operator could not take the issuer's secret away"
}
alice="$(token alice)"
for node in A B; do
	until_says 45 "$node" "SELECT acl_jwks_refresh('kc') IS NOT NULL AND acl_session_open('$alice') IS NULL" true
done
echo "ok: the issuer's secret taken away from the service fails the issuer closed on both nodes"

echo "PASS: tresor in the cluster"
