//===----------------------------------------------------------------------===//
// hugr_pool.hpp - the `hugr` type's pool of connections to B (spec 011, "The pool")
//
// One pool per attached catalog per node of A. A connection is one quack connection to B's door: one acl session on
// B as A's node role, one connection id, one query at a time. It is lent for one remote read (a scan, the catalog's
// listing) and comes back when the read is over. The decisions are hugr_pool_core.hpp's; this is the lock, the wait,
// the connect and the counters.
//===----------------------------------------------------------------------===//

#pragma once

#include "hugr_pool_core.hpp"
#include "quack_uri.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <condition_variable>

namespace duckdb {
class ClientContext;
class DatabaseInstance;
class HugrClientConnection;
class QuackPeriodicWorker;

namespace hugr {

//! One of the pool's connections, as the core sees it.
struct PooledConnection {
	shared_ptr<HugrClientConnection> client;
	//! false once its heartbeat failed or B invalidated its database: replaced before it is lent
	bool Usable() const;
};

class Pool;

//! A connection lent for one remote read. Back to the pool at its end, unless B's answer ended it.
class Loan {
public:
	Loan() = default;
	Loan(shared_ptr<Pool> pool, shared_ptr<PooledConnection> conn);
	~Loan();
	Loan(Loan &&other) noexcept;
	Loan &operator=(Loan &&other) noexcept;
	Loan(const Loan &) = delete;
	Loan &operator=(const Loan &) = delete;

	explicit operator bool() const {
		return conn != nullptr;
	}
	HugrClientConnection &Client() const;
	//! B answered `answer`: a connection it ended never goes back, and a gone node takes its label's others.
	void Answered(Answer answer);
	//! Back to the pool now.
	void Release();

private:
	shared_ptr<Pool> pool;
	shared_ptr<PooledConnection> conn;
	//! a scan's threads may each see B end the connection
	atomic<bool> keep {true};
};

//! What hugr_pools() shows of a pool. Never a connection id: its random part is the connection's secret (spec 134).
struct PoolView {
	struct Connection {
		string label;
		int64_t age_s = 0;
		int64_t idle_s = 0;
		bool busy = false;
	};
	string catalog;
	string server;
	idx_t size = 0;
	idx_t busy = 0;
	idx_t idle = 0;
	idx_t connecting = 0;
	idx_t waiting = 0;
	string last_refusal;
	vector<Connection> connections;
};

class Pool : public enable_shared_from_this<Pool> {
public:
	//! `secret` names the `hugr` secret the token is read from at every connect, on the node's own behalf.
	Pool(DatabaseInstance &db, string catalog, QuackUri uri, string secret, LinkOptions options);
	~Pool();

	//! A connection for one remote read, in arrival order. A refused or failed connect waits (backoff) for up to
	//! pool_wait when `wait`; without it (the ATTACH) the first failure is the answer. `fresh`: a new connection,
	//! never an idle one - the retry after B ended one.
	Loan Borrow(ClientContext &context, bool wait = true, bool fresh = false);
	//! A loan's end (Loan calls it). `keep` false: B's answer ended the connection.
	void Return(const shared_ptr<PooledConnection> &conn, bool keep);
	//! B said a connection of this node's label is not known: the label's connections go (spec 134).
	void NodeGone(const string &label);
	//! The catalog is detached: idle connections now, lent ones at their return.
	void Close();

	PoolView View();
	const LinkOptions &Options() const {
		return options;
	}
	const string &Catalog() const {
		return catalog;
	}

	//! counters, to the registry acl-otel reads (hugr.pool.*, attributes from bounded sets)
	void Count(const string &name, const string &reason = string());

private:
	shared_ptr<HugrClientConnection> Connect(ClientContext &context);
	string ReadToken();
	//! retired connections are destroyed here, outside the lock: each sends its DISCONNECT (seats freed on B at once)
	static void Retire(vector<shared_ptr<PooledConnection>> &retired);

	DatabaseInstance &db;
	const string catalog;
	const QuackUri uri;
	const string secret;
	const LinkOptions options;

	mutex lock;
	std::condition_variable changed;
	PoolCore<PooledConnection> core;
	string last_refusal;
	bool closed = false;
	unique_ptr<QuackPeriodicWorker> sweeper;
};

//! The pools of an instance, for hugr_pools() and the gauges. Pools register themselves; a gone pool drops out.
class PoolRegistry : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "hugr_type_pools";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
	static shared_ptr<PoolRegistry> Get(DatabaseInstance &db);

	void Add(const shared_ptr<Pool> &pool);
	vector<shared_ptr<Pool>> Pools();

private:
	mutex lock;
	vector<weak_ptr<Pool>> pools;
};

//! Monotonic milliseconds: the pool's clock
int64_t NowMs();

} // namespace hugr
} // namespace duckdb
