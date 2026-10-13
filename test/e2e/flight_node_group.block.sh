# Extracted from duckdb-acl test/e2e/flight/run.sh (tag pre-platform-split) by duckdb-acl spec 127: the Flight door's
# node-group refusal (spec 096) needs the platform part, which is hugr_node's now. It runs inside that script's
# harness (a served node on $URI, `ask`, `fail`, the server's stdin on fd 3, $TMP) - to be wired into hugr-node's
# own e2e once it has one (hugr-node spec 001, follow-ups). Kept verbatim so nothing of it is lost.

# --- a node of another resource group says so, and the client goes elsewhere (spec 096) ------------
# The node joins a group the token's roles are not in: a new session is refused with UNAVAILABLE - what
# a load balancer and a driver read as "try another node" - and a sentence naming the groups only.
# A session established before the node joined keeps working (placement is judged at open only); it
# is primed here, on a cookie connection, for the swap check below.
SJ="$TMP/swapjar"
ACL_COOKIE_JAR="$SJ" ask "SELECT 1" >/dev/null
ACL_COOKIE_JAR="$SJ" ask "SELECT 1" >/dev/null
[ -s "$SJ" ] || fail "the door set no session cookie for the swap check"
# one statement per line: the ACL prefix is the start of a query, and the CLI sends a line whole
{
	echo "SET GLOBAL acl_allow_anonymous_admin=true;"
	echo "ACL ADMIN CREATE RESOURCE GROUP far_away (max_sessions 5);"
	echo "SET GLOBAL acl_allow_anonymous_admin=false;"
	echo "SET GLOBAL acl_node_group = 'far_away';"
	echo "SELECT 'grouped' AS s;"
} >&3
grouped=""
for _ in $(seq 1 40); do grep -q "grouped" "$TMP/server.log" && { grouped=1; break; }; sleep 0.25; done
[ -n "$grouped" ] || { tail -20 "$TMP/server.log" >&2; fail "the server did not join the group"; }
got="$(ask "SELECT 1")"
echo "$got" | grep -q "FlightUnavailableError" || fail "a node of another group did not answer UNAVAILABLE: $got"
echo "$got" | grep -q 'this node serves resource group "far_away"' || fail "the refusal does not name the node's group: $got"
# A swap - somebody else's token on that cookie - opens the new session FIRST: refused here, it leaves
# the connection's session alone, so its principal keeps working (it used to be closed before the open)
OTHER_TOKEN='eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6InRlc3Qta2V5In0.eyJpc3MiOiJ0ZXN0L2lkcC9zIiwiYXVkIjoiYXBpOi8vYWNsLXRlc3QiLCJleHAiOjQxMDI0NDQ4MDAsInN1YiI6InUtb3RoZXIiLCJyb2xlcyI6WyJhbmFseXN0Il0sInRpZCI6ImFjbWUifQ.Ifsm-Q43Lg4wm0CEpmOAyeL_UXV06u4Hl4IiHRxO0BY_dKASjm63-E0p-nG7T5SMl7xDsYaYChTaQXoCG2HfYjq-SiXskmZjx3F56fgfdHHRR7rbNE_h43DytVjuEr4AZidkWk9cquJ-IMRqg_8EtWxRA52Vzm4dy7NEALL1_j2QmdJ3EHntkipTo-I-c9zzR0v_kGGe_-cL8aJqy5LblQfSo_g9CG-JTYuuBNiTODIGg9EPc6IJiMtr0oLQ9v_TRVE9aYbymiHq3RlHD45KtV1o0vaStuFRCKpNvK0oi7WaxS9zMi5lXbvT5tTU-J6Wfhrk0abHXTXpccuXrKNJUg'
got="$(ACL_COOKIE_JAR="$SJ" ask "SELECT 1" "$OTHER_TOKEN")"
echo "$got" | grep -q "FlightUnavailableError" || fail "a swap to a principal of another group was not refused: $got"
got="$(ACL_COOKIE_JAR="$SJ" ask "SELECT 1 AS kept")"
echo "$got" | grep -q "kept" || fail "a refused swap ended the connection's own session: $got"
echo "SET GLOBAL acl_node_group = ''; SELECT 'ungrouped' AS s;" >&3
ungrouped=""
for _ in $(seq 1 40); do grep -q "ungrouped" "$TMP/server.log" && { ungrouped=1; break; }; sleep 0.25; done
[ -n "$ungrouped" ] || fail "the server did not leave the group"
got="$(ask "SELECT 1 AS one")"
echo "$got" | grep -q "one" || fail "an ungrouped node does not serve again: $got"

