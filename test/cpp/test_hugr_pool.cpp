// Spec 011: the `hugr` type's pool, its decisions (hugr_pool_core.hpp, header-only, clock-free) - B's answers and
// what each does, the one retry before a row, the node's label, loans served in arrival order, a refused connect
// that suspends growth instead of storming B, and when a connection retires: idle, past its lifetime, unusable, its
// node gone. The live pool (the lock, the wait, the connect) is the e2e's. Build + run via `GEN=ninja make test-cpp`.

#include "acl_test_util.hpp"

#include "hugr_pool_core.hpp"

using namespace duckdb;
using namespace duckdb::hugr;
using namespace acl_test;

namespace {

struct FakeConn {
	std::string id;
	bool usable = true;
	bool Usable() const {
		return usable;
	}
};

using Pool = PoolCore<FakeConn>;

shared_ptr<FakeConn> Conn(const std::string &id) {
	auto conn = make_shared_ptr<FakeConn>();
	conn->id = id;
	return conn;
}

LinkOptions Options(idx_t size) {
	LinkOptions options;
	options.pool_size = size;
	options.idle_timeout_s = 600;
	options.max_lifetime_s = 1800;
	return options;
}

//! the front borrower connects the reserved place: Next said CONNECT
shared_ptr<FakeConn> Grow(Pool &pool, const std::string &id, int64_t now) {
	shared_ptr<FakeConn> lent;
	vector<shared_ptr<FakeConn>> retired;
	Check(pool.Next(now, lent, retired) == Pool::Step::CONNECT, "a place to connect (" + id + ")");
	auto conn = Conn(id);
	pool.Connected(conn, LabelOf(id), now);
	return conn;
}

} // namespace

int main(int argc, char *argv[]) {
	return RunMain("the hugr type's pool (spec 011)", [&] {
		Scenario("B's answers", [&] {
			Check(ClassifyAnswer("acl: this connection is not known on this node - connect again") == Answer::NODE_GONE,
			      "an unknown connection is a gone node");
			Check(ClassifyAnswer("acl: this connection's lease ran out on this node - connect again") ==
			          Answer::RECONNECT,
			      "a lapsed lease is a reconnect");
			Check(ClassifyAnswer("Authorization failed") == Answer::RECONNECT, "an ended session is a reconnect (Q9)");
			Check(ClassifyAnswer("Failed to send message: Connection refused") == Answer::RECONNECT,
			      "no answer is a reconnect");
			Check(ClassifyAnswer("acl: node at capacity - 16 of 16 quack clients seated (...); try another node") ==
			          Answer::REFUSED,
			      "a node at capacity refuses a new connection");
			Check(ClassifyAnswer("Authentication failed") == Answer::REFUSED, "a refused session too");
			Check(ClassifyAnswer("Authentication callback failed: acl: resource group g is served by ...") ==
			          Answer::REFUSED,
			      "and another group's node");
			Check(ClassifyAnswer("The server database has been invalidated and the server must be restarted") ==
			          Answer::INVALIDATED,
			      "an invalidated B");
			Check(ClassifyAnswer("Catalog Error: Table with name t does not exist!") == Answer::STATEMENT,
			      "a statement's own error stays the statement's");
			Check(ClassifyAnswer("x Authorization failed") == Answer::STATEMENT,
			      "the session's end is the door's exact answer, not a statement's text that contains it");
			Check(EndsConnection(Answer::RECONNECT) && EndsConnection(Answer::NODE_GONE) &&
			          EndsConnection(Answer::INVALIDATED),
			      "a connection those answers ended never goes back");
			Check(!EndsConnection(Answer::STATEMENT) && !EndsConnection(Answer::REFUSED),
			      "a statement's error returns its connection");
		});

		Scenario("one retry, before any row", [&] {
			Check(MayRetry(Answer::RECONNECT, false, false), "a reconnect before a row retries");
			Check(MayRetry(Answer::NODE_GONE, false, false), "a gone node too");
			Check(!MayRetry(Answer::RECONNECT, false, true), "once");
			Check(!MayRetry(Answer::RECONNECT, true, false), "never once rows have flowed");
			Check(!MayRetry(Answer::STATEMENT, false, false), "a statement's error never");
			Check(!MayRetry(Answer::INVALIDATED, false, false), "an invalidated B never");
			Check(!MayRetry(Answer::REFUSED, false, false), "a refusal is the pool's wait, not a retry");
		});

		Scenario("the secret's scope names the door", [&] {
			Check(ScopeNamesDoor({"hugr://b.example:443"}, "b.example", 443), "the door as written");
			Check(ScopeNamesDoor({"hugr:B.Example:443/"}, "b.example", 443), "either form, any case, a path cut");
			Check(ScopeNamesDoor({"https://x", "hugr://b.example"}, "b.example", 9494), "quack's port when left out");
			Check(ScopeNamesDoor({"hugr://[::1]:31591"}, "::1", 31591), "an IPv6 door");
			Check(!ScopeNamesDoor({"hugr://b.example:443"}, "c.example", 443), "another host is not the door");
			Check(!ScopeNamesDoor({"hugr://b.example:443"}, "b.example", 8443), "another port is not the door");
			Check(!ScopeNamesDoor({"hugr:", "hugr://", ""}, "b.example", 443), "a catch-all scope names no door");
			Check(!ScopeNamesDoor({"https://b.example:443"}, "b.example", 443), "another scheme names no door");
			Check(!ScopeNamesDoor({"hugr://b.example:443x", "hugr://b.example:99999"}, "b.example", 443),
			      "a port that is no port names no door");
		});

		Scenario("the node's label", [&] {
			Check(LabelOf("node-b1.3f9a7c") == "node-b1", "the label before the first dot");
			Check(LabelOf("3f9a7c").empty(), "an unlabelled id names no node");
		});

		Scenario("loans in arrival order, no more than the pool's size", [&] {
			Pool pool(Options(2));
			auto first = pool.Join(), second = pool.Join(), third = pool.Join();
			Check(pool.First(first) && !pool.First(second), "the first to come is the first served");
			auto a = Grow(pool, "b1.a", 0);
			pool.Leave(first);
			Check(pool.First(second), "then the next");
			auto b = Grow(pool, "b1.b", 0);
			pool.Leave(second);
			shared_ptr<FakeConn> lent;
			vector<shared_ptr<FakeConn>> retired;
			Check(pool.Next(0, lent, retired) == Pool::Step::WAIT, "at its size the pool waits");
			Check(pool.Busy() == 2 && pool.Waiting() == 1, "two lent, one waiting");
			pool.Return(a, true, 10, retired);
			Check(pool.Next(10, lent, retired) == Pool::Step::LEND && lent == a, "a returned connection is lent next");
			pool.Leave(third);
			Check(retired.empty() && pool.Waiting() == 0, "nothing retired");
		});

		Scenario("a refused connect suspends growth, it does not storm", [&] {
			Pool pool(Options(4));
			auto a = Grow(pool, "b1.a", 0);
			shared_ptr<FakeConn> lent;
			vector<shared_ptr<FakeConn>> retired;
			Check(pool.Next(0, lent, retired) == Pool::Step::CONNECT, "room for a second");
			pool.ConnectFailed(0);
			Check(pool.Next(1, lent, retired) == Pool::Step::WAIT, "after a refusal the loan waits for one in use");
			Check(pool.Next(Pool::BACKOFF_MIN_MS, lent, retired) == Pool::Step::CONNECT, "growth is tried again later");
			pool.ConnectFailed(Pool::BACKOFF_MIN_MS);
			Check(pool.RefusedUntil() == 3 * Pool::BACKOFF_MIN_MS, "the backoff doubles");
			Check(pool.Next(2 * Pool::BACKOFF_MIN_MS, lent, retired) == Pool::Step::WAIT, "and is kept");
			pool.Return(a, true, 2 * Pool::BACKOFF_MIN_MS, retired);
			Check(pool.Next(2 * Pool::BACKOFF_MIN_MS, lent, retired) == Pool::Step::LEND && lent == a,
			      "a connection in use serves meanwhile");
			auto now = pool.RefusedUntil();
			for (int i = 0; i < 10; i++) {
				Check(pool.Next(now, lent, retired) == Pool::Step::CONNECT, "growth tried at the backoff's end");
				pool.ConnectFailed(now);
				Check(pool.RefusedUntil() - now <= Pool::BACKOFF_MAX_MS, "never past the ceiling");
				now = pool.RefusedUntil();
			}
			Check(pool.Connecting() == 0, "no place stays reserved");
			Check(pool.Next(now, lent, retired) == Pool::Step::CONNECT, "the next try");
			pool.Connected(Conn("b1.b"), "b1", now);
			Check(pool.RefusedUntil() == 0, "a connect that succeeds ends the backoff");
			Check(retired.empty(), "nothing retired");
		});

		Scenario("an empty pool retries after the backoff", [&] {
			Pool pool(Options(4));
			shared_ptr<FakeConn> lent;
			vector<shared_ptr<FakeConn>> retired;
			Check(pool.Next(0, lent, retired) == Pool::Step::CONNECT, "the first connect");
			pool.ConnectFailed(0);
			Check(pool.Next(100, lent, retired) == Pool::Step::WAIT, "waits out the backoff");
			Check(pool.Next(Pool::BACKOFF_MIN_MS, lent, retired) == Pool::Step::CONNECT, "then tries again");
			pool.ConnectFailed(Pool::BACKOFF_MIN_MS);
		});

		Scenario("when a connection retires", [&] {
			Pool pool(Options(4));
			const int64_t S = 1000;
			auto a = Grow(pool, "b1.a", 0);
			auto b = Grow(pool, "b1.b", 0);
			vector<shared_ptr<FakeConn>> retired;
			pool.Return(a, true, 100 * S, retired);
			pool.Return(b, true, 500 * S, retired);
			pool.Sweep(750 * S, retired);
			Check(retired.size() == 1 && retired[0] == a, "idle past idle_timeout: the one idle since 100 s");
			retired.clear();
			shared_ptr<FakeConn> lent;
			Check(pool.Next(800 * S, lent, retired) == Pool::Step::LEND && lent == b, "the other is lent");
			pool.Return(b, true, 1800 * S, retired);
			Check(retired.size() == 1 && retired[0] == b, "past max_lifetime it retires at its return");
			retired.clear();

			auto c = Grow(pool, "b1.c", 2000 * S);
			pool.Return(c, false, 2001 * S, retired);
			Check(retired.size() == 1 && retired[0] == c, "a connection B's answer ended never goes back");
			retired.clear();

			auto d = Grow(pool, "b1.d", 2100 * S);
			pool.Return(d, true, 2101 * S, retired);
			d->usable = false;
			Check(pool.Next(2102 * S, lent, retired) == Pool::Step::CONNECT,
			      "a connection whose heartbeat failed is replaced before it is lent");
			Check(retired.size() == 1 && retired[0] == d, "and retires");
			pool.ConnectFailed(2102 * S);
		});

		Scenario("the retry takes a new connection, never an idle one", [&] {
			Pool pool(Options(2));
			auto a = Grow(pool, "b1.a", 0);
			auto b = Grow(pool, "b1.b", 0);
			vector<shared_ptr<FakeConn>> retired;
			pool.Return(a, true, 10, retired);
			pool.Return(b, false, 20, retired);
			Check(retired.size() == 1 && retired[0] == b, "the connection B ended retires");
			retired.clear();
			shared_ptr<FakeConn> lent;
			Check(pool.Next(30, lent, retired, true) == Pool::Step::CONNECT && !lent,
			      "the retry connects anew while there is room, though one is idle");
			pool.Connected(Conn("b1.c"), "b1", 30);
			Check(pool.Entries().size() == 2 && retired.empty(), "the idle one stays");
			auto c = pool.Entries()[1].conn;
			pool.Return(c, true, 40, retired);
			Check(pool.Next(50, lent, retired, true) == Pool::Step::CONNECT,
			      "at the pool's size a retry still connects anew");
			Check(retired.size() == 1 && retired[0] == a, "the least recently used idle one made room");
			pool.ConnectFailed(50);
			Check(pool.Next(60, lent, retired) == Pool::Step::LEND && lent == c, "a plain loan takes the idle one");
		});

		Scenario("a gone node takes its connections", [&] {
			Pool pool(Options(4));
			auto a = Grow(pool, "b1.a", 0);
			auto b = Grow(pool, "b1.b", 0);
			auto c = Grow(pool, "b2.c", 0);
			vector<shared_ptr<FakeConn>> retired;
			pool.Return(a, true, 10, retired);
			pool.Return(c, true, 10, retired);
			pool.NodeGone("b1", retired);
			Check(retired.size() == 1 && retired[0] == a, "the idle one of b1 at once");
			Check(pool.Entries().size() == 2, "b1's busy one stays until it returns, b2's stays");
			pool.Return(b, true, 20, retired);
			Check(retired.size() == 2 && retired[1] == b, "b1's busy one at its return");
			pool.NodeGone("", retired);
			Check(pool.Entries().size() == 1 && pool.Entries()[0].conn == c, "an unlabelled id names no node");
		});

		Scenario("the pool's end", [&] {
			Pool pool(Options(4));
			auto a = Grow(pool, "b1.a", 0);
			auto b = Grow(pool, "b1.b", 0);
			vector<shared_ptr<FakeConn>> retired;
			pool.Return(a, true, 1, retired);
			pool.Clear(retired);
			Check(retired.size() == 1 && retired[0] == a, "the idle ones now");
			pool.Return(b, true, 2, retired);
			Check(retired.size() == 2 && pool.Entries().empty(), "the lent one when it returns");
		});

		Scenario("the wait's message", [&] {
			Check(WaitTimeoutMessage("b", 25, 4, 4, 3, "") == "hugr: no connection to b within 25 s (4 of 4 in use, 3 "
			                                                  "waiting)",
			      "the spec's text");
			Check(WaitTimeoutMessage("b", 25, 0, 4, 1, "Authentication failed")
			              .find("B answered: Authentication "
			                    "failed") != std::string::npos,
			      "with B's last refusal");
		});
	});
}
