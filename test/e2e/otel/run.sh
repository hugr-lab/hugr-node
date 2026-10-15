#!/usr/bin/env bash
# acl_otel inside hugr_node (spec 005): the built-in export reaches a collector, and nothing leaves the node until an
# endpoint is set. A fake OTLP/HTTP receiver (python, on a free port) records what arrives; one hugr_node CLI runs a
# principal's statements with the audit on. Checked:
#   1. inert: with no endpoint, the decided statements and a flush send nothing;
#   2. the endpoint set: the decision records arrive at /v1/logs as OTLP protobuf, naming the principal's role;
#   3. the metrics arrive at /v1/metrics;
#   4. the endpoint emptied again (the same node): another role's statements send nothing.
# Needs python3 and the hugr_node build (build/release/duckdb, hugr_node linked in); a missing one is a SKIP (exit 0).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"

skip() {
	echo "SKIP: $*"
	exit 0
}
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
command -v python3 >/dev/null || skip "python3 not found"
[ -x "$DUCKDB" ] || skip "no hugr_node build at $DUCKDB (GEN=ninja make)"
# an OTLP endpoint of the environment would make the "inert" check meaningless
for var in OTEL_EXPORTER_OTLP_ENDPOINT OTEL_EXPORTER_OTLP_LOGS_ENDPOINT OTEL_EXPORTER_OTLP_METRICS_ENDPOINT \
	OTEL_EXPORTER_OTLP_TRACES_ENDPOINT OPENLINEAGE_URL; do
	unset "$var"
done

TMP="$(mktemp -d)"
cleanup() {
	if [ -n "${RECEIVER_PID:-}" ]; then
		kill "$RECEIVER_PID" 2>/dev/null || true
		wait "$RECEIVER_PID" 2>/dev/null || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT

echo "acl_otel in hugr_node e2e (hugr-node spec 005)"

# the receiver: one line per request - path, content type, body length, whether the body names each role
cat >"$TMP/receiver.py" <<'PY'
import http.server, sys

log = open(sys.argv[1], "a", buffering=1)

class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        kind = self.headers.get("Content-Type")
        log.write(f"{self.path} {kind} {len(body)} {b'analyst' in body} {b'clerk' in body}\n")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-protobuf")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass

server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
open(sys.argv[2], "w").write(str(server.server_address[1]))
server.serve_forever()
PY
: >"$TMP/requests"
python3 "$TMP/receiver.py" "$TMP/requests" "$TMP/port" &
RECEIVER_PID=$!
for _ in $(seq 1 50); do
	[ -s "$TMP/port" ] && break
	sleep 0.1
done
[ -s "$TMP/port" ] || fail "the receiver did not start"
PORT="$(cat "$TMP/port")"

run() { "$DUCKDB" -no-agent -batch -bail -noheader -list :memory: <"$1" 2>&1; }

# the same bootstrap and statements both times; only the endpoint differs
cat >"$TMP/policy.sql" <<'SQL'
SET autoload_known_extensions = false;
ATTACH ':memory:' AS store;
CREATE TABLE orders(id INTEGER, tenant VARCHAR);
INSERT INTO orders VALUES (1, 'acme'), (2, 'globex');
SELECT acl_use_db('store', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
SET GLOBAL acl_audit_level = 'all';
ACL ADMIN CREATE VIRTUAL CATALOG sales;
ACL ADMIN CREATE VIRTUAL TABLE sales.orders AS memory.main.orders;
ACL ADMIN CREATE ROLE analyst;
ACL ADMIN GRANT CATALOG sales TO ROLE analyst WITH (select) MAIN;
ACL ADMIN CREATE ROLE clerk;
ACL ADMIN GRANT CATALOG sales TO ROLE clerk WITH (select) MAIN;
-- the bootstrap's own events drained into the stand-in: none of them waits for the endpoint below
SELECT acl_audit_flush();
SELECT acl_otel_flush();
SQL
statements() {
	cat <<SQL
ACL ROLE "$1" SELECT count(*) FROM orders;
ACL ROLE "$1" SELECT id FROM orders ORDER BY id;
SELECT acl_audit_flush();
SELECT acl_otel_flush();
SELECT acl_otel_metrics_flush();
SQL
}

# --- 1. inert: no endpoint, nothing sent -------------------------------------------------------------------------
{
	cat "$TMP/policy.sql"
	echo "SELECT acl_otel_status() LIKE '%\"exporter\":\"none (acl_otel_endpoint is empty)\"%';"
	statements analyst
} >"$TMP/inert.sql"
out="$(run "$TMP/inert.sql")" || fail "the inert run failed: $out"
echo "$out" | grep -qx true || fail "the status did not say the stand-in: $out"
sleep 1
[ ! -s "$TMP/requests" ] || fail "a node with no endpoint sent: $(cat "$TMP/requests")"
echo "ok 1 - no endpoint: nothing sent"

# --- 2-4. the endpoint set, then emptied --------------------------------------------------------------------------
{
	cat "$TMP/policy.sql"
	echo "SET GLOBAL acl_otel_endpoint = 'http://127.0.0.1:$PORT';"
	statements analyst
	echo "SET GLOBAL acl_otel_endpoint = '';"
	echo "SELECT acl_otel_status() LIKE '%\"exporter\":\"none (acl_otel_endpoint is empty)\"%';"
	statements clerk
} >"$TMP/export.sql"
out="$(run "$TMP/export.sql")" || fail "the export run failed: $out"
echo "$out" | grep -qx true || fail "the endpoint emptied did not bring the stand-in back: $out"

grep -q '^/v1/logs application/x-protobuf [1-9][0-9]* True ' "$TMP/requests" ||
	fail "no decision record naming the role arrived at /v1/logs: $(cat "$TMP/requests")"
echo "ok 2 - the decision records arrived at /v1/logs (OTLP protobuf, the role named)"
grep -q '^/v1/metrics application/x-protobuf [1-9]' "$TMP/requests" ||
	fail "no metrics arrived at /v1/metrics: $(cat "$TMP/requests")"
echo "ok 3 - the metrics arrived at /v1/metrics"

# the same node, the endpoint emptied: the other role's statements and flushes sent nothing
! grep -q ' True$' "$TMP/requests" || fail "a record of the statements after the endpoint was emptied arrived"
echo "ok 4 - the endpoint emptied: nothing more"
echo "PASS: acl_otel in hugr_node"
