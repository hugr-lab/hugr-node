#!/usr/bin/env bash
# The `hugr` type across processes (spec 011): what one process cannot show. Two B nodes (b1, b2: hugr_node
# processes, each with a node label and a quack door, the same policy), a stand-in for B's front (front.py: it routes
# by the label in X-Quack-Connection-Id and places new connections where it is told), and A attaching B through the
# front with a `hugr` secret (a fixture token of test/idp/s - the nodes read test/idp/ from the repository root).
# Checked:
#   1. the front loses b1: b2 answers "not known", A continues on b2 within the statement, b1's connection goes;
#   2. A stopped (SIGSTOP) longer than its lease: its next statement succeeds;
#   3. b1 drained, a short max_lifetime: A's reads continue on b2 and b1 holds none of A's sessions after a lifetime;
#   4. B at capacity: a loan waits pool_wait and fails, B's answer in hugr_pools(), and B is not stormed (the backoff);
#   5. b1 killed during a long scan: that statement fails, the next one works;
#   6. TLS to B, its certificate verified against the node's ca_cert_file (no TLS bypass: an unknown CA is refused);
#   7. our copies of quack's client are not exported by the loadable (a stock quack loaded beside binds its own).
# Needs python3, openssl and the hugr_node build (build/release/duckdb). A missing tool or build is a SKIP (exit 0).
#
#   test/e2e/federation/run.sh      # FRONT_PORT / B1_PORT / B2_PORT / B2_TLS_PORT to move the ports
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
LOADABLE="$ROOT/build/release/extension/hugr_node/hugr_node.duckdb_extension"
FRONT_PORT="${FRONT_PORT:-31710}"
B1_PORT="${B1_PORT:-31711}"
B2_PORT="${B2_PORT:-31712}"
B2_TLS_PORT="${B2_TLS_PORT:-31713}"

skip() {
	echo "SKIP: $*"
	exit 0
}
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
# a dump never shows a token
redact() { sed -E 's/eyJ[A-Za-z0-9._-]*/<token>/g'; }

for tool in python3 openssl curl; do command -v "$tool" >/dev/null || skip "$tool not found"; done
[ -x "$DUCKDB" ] || skip "no hugr_node build at $DUCKDB (GEN=ninja make)"
for port in "$FRONT_PORT" "$B1_PORT" "$B2_PORT" "$B2_TLS_PORT"; do
	if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
		fail "127.0.0.1:$port is taken - set FRONT_PORT / B1_PORT / B2_PORT"
	fi
done
# A's token for B: the fixture the sqllogictest uses (iss test/idp/s, role analyst)
TOKEN="$(grep -o "TOKEN 'eyJ[^']*'" "$ROOT/test/sql/hugr_type/hugr_type.test" | head -1 | cut -d"'" -f2)"
[ -n "$TOKEN" ] || fail "no fixture token in test/sql/hugr_type/hugr_type.test"

TMP="$(mktemp -d)"
cleanup() {
	exec 3>&- 4>&- 5>&- 2>/dev/null || true
	for pid in "${A_PID:-}" "${B1_PID:-}" "${B2_PID:-}" "${FRONT_PID:-}"; do
		[ -n "$pid" ] && kill -CONT "$pid" 2>/dev/null && kill "$pid" 2>/dev/null || true
	done
	rm -rf "$TMP"
}
trap cleanup EXIT

echo "the hugr type e2e (hugr-node spec 011)"

# --- the processes: each node a hugr_node CLI whose stdin is ours (fd 3: A, fd 4: b1, fd 5: b2) ---------------------
cd "$ROOT" # the nodes read the fixture issuer from test/idp/, relative to here
start() { # <name> <fd>
	mkfifo "$TMP/$1.in"
	"$DUCKDB" -no-agent -unsigned -csv -noheader <"$TMP/$1.in" >"$TMP/$1.log" 2>&1 &
	eval "${1}_PID=$!"
	disown "$!"
	eval "exec $2>\"$TMP/$1.in\""
}
start A 3
start B1 4
start B2 5
fd_of() { case "$1" in A) echo 3 ;; B1) echo 4 ;; B2) echo 5 ;; esac }
pid_of() { eval "echo \$${1}_PID"; }

# says <node> <sql>: the answer's csv lines (one query, its marker cut off)
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
	fail "node $node did not answer: $2"
}
# run <node> <statement...>: statements that must not fail
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
# fails <node> <statement> <pattern>: the statement fails, with the pattern in its error
fails() {
	local node=$1 before
	before="$(wc -l <"$TMP/$node.log")"
	echo "$2" >&"$(fd_of "$node")"
	says "$node" "SELECT 1" >/dev/null
	tail -n +"$((before + 1))" "$TMP/$node.log" | grep -q "$3" || {
		tail -n +"$((before + 1))" "$TMP/$node.log" | redact >&2
		fail "node $node: '$2' did not fail with '$3'"
	}
}
until_says() { # <seconds> <node> <sql> <expected>
	local seconds=$1 node=$2 sql=$3 want=$4 got=""
	for _ in $(seq "$((seconds * 2))"); do
		got="$(says "$node" "$sql")"
		[ "$got" = "$want" ] && return 0
		sleep 0.5
	done
	fail "node $node answered '$got' for $sql, not '$want', within ${seconds}s"
}
front() { curl -sf "http://127.0.0.1:$FRONT_PORT/control/$1?node=$2" >/dev/null || fail "the front refused $1 $2"; }
metric() { says A "SELECT coalesce(sum(value), 0) FROM acl_metrics() WHERE name = '$1'"; }
labels() { says A "SELECT coalesce(string_agg(DISTINCT c.label, ',' ORDER BY c.label), '') FROM (SELECT unnest(connections) AS c FROM hugr_pools())"; }
quack_sessions() { says "$1" "SELECT len(regexp_extract_all(acl_sessions(), '\"door\":\"quack\"'))"; }

b_bootstrap() { # <label> <port>
	cat <<EOF
SET GLOBAL acl_jwks_locations = 'test/idp/';
SET GLOBAL acl_node_label = '$1';
ATTACH ':memory:' AS store;
ATTACH ':memory:' AS phys;
CREATE TABLE phys.main.orders(id INTEGER, tenant VARCHAR, ssn VARCHAR);
INSERT INTO phys.main.orders VALUES (1, 'acme', 's1'), (2, 'globex', 's2');
CREATE TABLE phys.main.huge AS SELECT i AS id FROM range(2000000) t(i);
SELECT acl_use_db('store', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
SELECT acl_define_issuer('test/idp/s', '{"url": "test/idp/s", "client": {"audiences": ["api://acl-test"], "roles_from": ["roles"], "attributes": {"tid": "tenant"}}}');
ACL ADMIN CREATE VIRTUAL CATALOG c;
ACL ADMIN CREATE VIRTUAL TABLE c.orders AS phys.main.orders COLUMNS (id, tenant);
ACL ADMIN CREATE VIRTUAL TABLE c.huge AS phys.main.huge;
ACL ADMIN CREATE ROLE analyst;
ACL ADMIN GRANT CATALOG c TO ROLE analyst WITH (select) MAIN;
SET GLOBAL acl_allow_anonymous_admin = false;
SELECT acl_quack_serve('quack:localhost:$2', 'e2e-server-token');
EOF
}
lines=()
while IFS= read -r line; do lines+=("$line"); done < <(b_bootstrap b1 "$B1_PORT")
run B1 "${lines[@]}"
lines=()
while IFS= read -r line; do lines+=("$line"); done < <(b_bootstrap b2 "$B2_PORT")
run B2 "${lines[@]}"

python3 "$ROOT/test/e2e/federation/front.py" "$FRONT_PORT" "b1=localhost:$B1_PORT" "b2=localhost:$B2_PORT" \
	>"$TMP/front.log" 2>&1 &
FRONT_PID=$!
disown "$FRONT_PID"
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$FRONT_PORT/control/none" && break; sleep 0.1; done

attach() { # <alias> <options>: A's link to B, through the front
	run A "DETACH DATABASE IF EXISTS $1;" "ATTACH 'localhost:$FRONT_PORT' AS $1 (TYPE hugr, SECRET bsec, DISABLE_SSL true$2);"
}
# the secret names the front's door - the address A's token goes to
run A "CREATE SECRET bsec (TYPE hugr, SCOPE 'hugr://localhost:$FRONT_PORT', TOKEN '$TOKEN');"
front place b1
# a lease of an hour: no heartbeat lands between a check's cause and its read (step 2 asks for a short one)
attach b ", POOL_SIZE 2, HEARTBEAT_TIMEOUT 3600"
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A does not read B through the front"
[ "$(labels)" = b1 ] || fail "A's connections are not on b1: $(labels)"
echo "  two B nodes behind the front, A attached through it (on b1)"

# --- 1. the front loses b1 ---------------------------------------------------------------------------------------------
retries="$(metric hugr.pool.retries)"
front forget b1
front place b2
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A did not continue on b2"
[ "$(metric hugr.pool.retries)" = "$((retries + 1))" ] || fail "the statement was not retried once"
[ "$(labels)" = b2 ] || fail "b1's connection did not go: $(labels)"
front know b1
echo "ok: the front lost b1 - b2 said 'not known', A continued on b2 within the statement, b1's connection went"

# --- 2. A stopped longer than its lease ----------------------------------------------------------------------------------
attach b ", HEARTBEAT_TIMEOUT 2"
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A does not read B"
kill -STOP "$A_PID"
sleep 6
kill -CONT "$A_PID"
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A did not read B after its lease ran out"
echo "ok: A stopped longer than its lease reads B at its next statement"

# --- 3. b1 drained, a short max_lifetime ----------------------------------------------------------------------------------
front place b1
# b1 may still hold a session of step 1's: its DISCONNECT went through a front that had lost b1, so b1 ends it when its
# lease lapses. What is A's here is what this link adds
base="$(quack_sessions B1)"
attach b ", POOL_SIZE 2, MAX_LIFETIME 2, HEARTBEAT_TIMEOUT 3600"
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A does not read b1"
[ "$(labels)" = b1 ] || fail "A's connections are not on b1: $(labels)"
[ "$(says B1 "SELECT acl_drain() IS NOT NULL")" = true ] || fail "b1 did not drain"
front place b2 # a front takes a draining node out of placement (its discovery answers 503)
for _ in $(seq 10); do
	[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "A's reads did not continue while b1 drained"
	sleep 0.5
done
for _ in $(seq 20); do
	[ "$(quack_sessions B1)" -le "$base" ] && break
	sleep 0.5
done
[ "$(quack_sessions B1)" -le "$base" ] || fail "b1 still holds A's sessions a lifetime after its drain"
[ "$(labels)" = b2 ] || fail "A's connections did not move to b2: $(labels)"
run B1 "SELECT acl_resume();"
echo "ok: b1 drained - A's reads went on, its connections moved to b2 within a lifetime, b1 holds none of A's sessions"

# --- 4. B at capacity -------------------------------------------------------------------------------------------------------
# two links to B2, one connection each; then B2 seats one quack client only, and both sessions end: bb's read must
# make a new connection, and B2 refuses it while b's connection holds the seat
front place b2
attach b ", POOL_SIZE 4, HEARTBEAT_TIMEOUT 3600"
attach bb ", POOL_SIZE 4, POOL_WAIT 3, HEARTBEAT_TIMEOUT 3600"
[ "$(says A "SELECT count(*) FROM bb.main.orders")" = 2 ] || fail "A does not read B2"
refused="$(metric hugr.pool.refused)"
run B2 "SET GLOBAL acl_quack_client_depth = 1024;" # 1024 slots / 1024 per client: one quack client seated
[ "$(says B2 "SELECT count(*) FROM (SELECT unnest(regexp_extract_all(acl_sessions(), '\"id\":\"([^\"]+)\"[^}]*\"door\":\"quack\"', 1)) AS id) WHERE acl_session_kill(id)")" -ge 1 ] ||
	fail "B2 ended none of A's sessions"
fails A "SELECT count(*) FROM bb.main.orders;" "no connection to bb within 3 s"
[ "$(says A "SELECT last_refusal LIKE 'acl: node at capacity%' FROM hugr_pools() WHERE catalog = 'bb'")" = true ] ||
	fail "hugr_pools() does not show B's refusal"
got="$(metric hugr.pool.refused)"
[ "$((got - refused))" -ge 1 ] && [ "$((got - refused))" -le 5 ] ||
	fail "B was asked $((got - refused)) times in 3 s - the backoff does not hold"
run B2 "RESET GLOBAL acl_quack_client_depth;"
[ "$(says A "SELECT count(*) FROM bb.main.orders")" = 2 ] || fail "the pool is stuck after the wait"
run A "DETACH bb;"
echo "ok: at B's capacity a loan waited pool_wait (B's answer in hugr_pools()); B was asked $((got - refused)) times"

# --- 5. b1 killed during a long scan ------------------------------------------------------------------------------------------
run B1 "SET GLOBAL acl_quack_target_batch_bytes = 65536;" # many small batches
front place b1
attach b ", POOL_SIZE 2, HEARTBEAT_TIMEOUT 3600"
[ "$(labels)" = b1 ] || fail "A's connections are not on b1: $(labels)"
front place b2
front slow "b1&ms=300"
before="$(wc -l <"$TMP/A.log")"
echo "SELECT sum(id) FROM b.main.huge;" >&3
sleep 2
kill -9 "$B1_PID"
says A "SELECT 1" >/dev/null
tail -n +"$((before + 1))" "$TMP/A.log" | grep -q "Error" || {
	tail -n +"$((before + 1))" "$TMP/A.log" | redact >&2
	fail "the scan b1 died under did not fail"
}
[ "$(says A "SELECT count(*) FROM b.main.orders")" = 2 ] || fail "the next statement did not work on b2"
echo "ok: b1 killed during a scan - that statement failed, the next one read b2"

# --- 6. TLS to B: the certificate is verified, against the node's ca_cert_file ----------------------------------------------
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" \
	-keyout "$TMP/b2.key" -out "$TMP/b2.crt" >/dev/null 2>&1 || fail "no certificate for B2"
run B2 "SELECT acl_quack_serve('quack:localhost:$B2_TLS_PORT', 'e2e-server-token', '$TMP/b2.crt', '$TMP/b2.key');"
run A "CREATE SECRET btls (TYPE hugr, SCOPE 'hugr://localhost:$B2_TLS_PORT', TOKEN '$TOKEN');"
# a certificate of a CA the node does not trust: no connection, never a downgrade
fails A "ATTACH 'localhost:$B2_TLS_PORT' AS bt (TYPE hugr, SECRET btls);" "no connection to bt"
run A "SET GLOBAL ca_cert_file = '$TMP/b2.crt';" "ATTACH 'localhost:$B2_TLS_PORT' AS bt (TYPE hugr, SECRET btls);"
[ "$(says A "SELECT count(*) FROM bt.main.orders")" = 2 ] || fail "A does not read B2 over TLS"
echo "ok: TLS to B - an unknown CA refused, the node's ca_cert_file trusted"

# --- 7. our copies are not exported -------------------------------------------------------------------------------------------
if [ -f "$LOADABLE" ] && command -v nm >/dev/null; then
	if [ "$(uname)" = Darwin ]; then exported="$(nm -gU "$LOADABLE")"; else exported="$(nm -D --defined-only "$LOADABLE")"; fi
	count="$(echo "$exported" | c++filt | grep -c 'duckdb::Hugr\|duckdb::hugr::' || true)"
	[ "$count" = 0 ] || fail "the loadable exports $count of the type's symbols"
	echo "ok: the loadable exports none of the type's symbols"
else
	echo "skip: no loadable or nm - the symbol check"
fi

echo "PASS: the hugr type across processes"
