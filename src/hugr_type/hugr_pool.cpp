// The `hugr` type's pool (spec 011, "The pool"): the lock, the wait, the connect and the counters around
// hugr_pool_core.hpp's decisions.

#include "hugr_pool.hpp"

#include "hugr_client.hpp"

#include "acl_audit.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <chrono>

namespace duckdb {
namespace hugr {

namespace {

//! quack's own default lease: a heartbeat about every 20 s keeps it
constexpr idx_t HEARTBEAT_TIMEOUT_S = 60;
//! how often idle connections are looked at, so a link nobody reads gives B its seats back
constexpr int64_t SWEEP_INTERVAL_MS = 30000;
//! how often a waiting loan looks at its statement's interrupt
constexpr int64_t WAIT_SLICE_MS = 100;

shared_ptr<acl::AuditHooks> HooksOf(DatabaseInstance &db) {
	string why;
	return acl::AuditHooks::Reach(db.GetObjectCache(), why);
}

} // namespace

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

bool PooledConnection::Usable() const {
	return !client->ServerInvalidated() && !client->HeartbeatFailed();
}

// ---- Loan ---------------------------------------------------------------------------------------------------------

Loan::Loan(shared_ptr<Pool> pool_p, shared_ptr<PooledConnection> conn_p)
    : pool(std::move(pool_p)), conn(std::move(conn_p)) {
}

Loan::~Loan() {
	Release();
}

Loan::Loan(Loan &&other) noexcept : pool(std::move(other.pool)), conn(std::move(other.conn)), keep(other.keep.load()) {
	other.conn = nullptr;
}

Loan &Loan::operator=(Loan &&other) noexcept {
	if (this != &other) {
		Release();
		pool = std::move(other.pool);
		conn = std::move(other.conn);
		keep = other.keep.load();
		other.conn = nullptr;
	}
	return *this;
}

HugrClientConnection &Loan::Client() const {
	D_ASSERT(conn);
	return *conn->client;
}

void Loan::Answered(Answer answer) {
	if (!conn) {
		return;
	}
	if (EndsConnection(answer)) {
		keep = false;
	}
	if (answer == Answer::NODE_GONE) {
		pool->NodeGone(LabelOf(conn->client->ConnectionId()));
	}
}

void Loan::Release() {
	if (!conn) {
		return;
	}
	auto returned = std::move(conn);
	conn = nullptr;
	try {
		pool->Return(returned, keep);
	} catch (...) {
		// a return never fails the statement that is ending
	}
}

// ---- Pool ---------------------------------------------------------------------------------------------------------

Pool::Pool(DatabaseInstance &db_p, string catalog_p, QuackUri uri_p, string secret_p, LinkOptions options_p)
    : db(db_p), catalog(std::move(catalog_p)), uri(std::move(uri_p)), secret(std::move(secret_p)),
      options(options_p), core(options_p) {
	sweeper = make_uniq<QuackPeriodicWorker>();
	sweeper->Start([] { return milliseconds(SWEEP_INTERVAL_MS); },
	               [this] {
		               vector<shared_ptr<PooledConnection>> retired;
		               {
			               lock_guard<mutex> guard(lock);
			               core.Sweep(NowMs(), retired);
		               }
		               Retire(retired);
	               });
}

Pool::~Pool() {
	sweeper->Stop();
	Close();
}

void Pool::Count(const string &name, const string &reason) {
	auto hooks = HooksOf(db);
	if (!hooks) {
		return;
	}
	vector<std::pair<string, string>> attributes {{"catalog", catalog}};
	if (!reason.empty()) {
		attributes.emplace_back("reason", reason);
	}
	hooks->Counters().Add(name, attributes);
}

void Pool::Retire(vector<shared_ptr<PooledConnection>> &retired) {
	for (auto &conn : retired) {
		try {
			conn.reset();
		} catch (...) {
		}
	}
	retired.clear();
}

string Pool::ReadToken() {
	// the node's own read: a system transaction carries no statement, so tresor serves the node, never the user of
	// the statement that made the connect (tresor spec 008); the secret is read at every connect - a reconnect is
	// the token's refresh (tresor's mint+exchange caches it until shortly before its exp)
	auto &manager = SecretManager::Get(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);
	auto entry = manager.GetSecretByName(transaction, secret);
	if (!entry) {
		throw InvalidInputException("hugr: %s: no secret \"%s\" the node may use", catalog, secret);
	}
	auto &base = *entry->secret;
	if (!StringUtil::CIEquals(base.GetType().GetIdentifierName(), "hugr")) {
		throw InvalidInputException("hugr: %s: secret \"%s\" is of type %s, not hugr", catalog, secret,
		                            base.GetType().GetIdentifierName());
	}
	auto kv = dynamic_cast<const KeyValueSecret *>(&base);
	Value token;
	if (!kv || !kv->TryGetValue("token", token) || token.IsNull() || token.ToString().empty()) {
		throw InvalidInputException("hugr: %s: secret \"%s\" carries no token", catalog, secret);
	}
	return token.ToString();
}

shared_ptr<HugrClientConnection> Pool::Connect(ClientContext &context) {
	auto token = ReadToken();
	// no client id: every connection of A is A's one role, and the door's seat accounting is per connection
	return HugrClient::ConnectToServer(context, uri, std::move(token), string(), HEARTBEAT_TIMEOUT_S,
	                                   options.read_ahead);
}

Loan Pool::Borrow(ClientContext &context, bool wait, bool fresh) {
	vector<shared_ptr<PooledConnection>> retired;
	unique_lock<mutex> guard(lock);
	if (closed) {
		throw InvalidInputException("hugr: the catalog %s is detached", catalog);
	}
	auto deadline = NowMs() + static_cast<int64_t>(options.pool_wait_s) * 1000;
	auto ticket = core.Join();
	auto leave = [&] {
		core.Leave(ticket);
		changed.notify_all();
	};
	while (true) {
		if (core.First(ticket)) {
			shared_ptr<PooledConnection> lent;
			auto step = core.Next(NowMs(), lent, retired, fresh);
			if (step == PoolCore<PooledConnection>::Step::LEND) {
				leave();
				guard.unlock();
				Retire(retired);
				return Loan(shared_from_this(), std::move(lent));
			}
			if (step == PoolCore<PooledConnection>::Step::CONNECT) {
				// out of the queue while connecting: the next borrower may take a returned connection meanwhile
				leave();
				guard.unlock();
				Retire(retired);
				shared_ptr<HugrClientConnection> client;
				ErrorData failure;
				try {
					client = Connect(context);
				} catch (std::exception &ex) {
					failure = ErrorData(ex);
				}
				guard.lock();
				if (client) {
					auto conn = make_shared_ptr<PooledConnection>();
					conn->client = std::move(client);
					core.Connected(conn, LabelOf(conn->client->ConnectionId()), NowMs());
					if (closed) {
						// detached while connecting: the new connection retires at once
						core.Return(conn, false, NowMs(), retired);
						changed.notify_all();
						guard.unlock();
						Retire(retired);
						throw InvalidInputException("hugr: the catalog %s is detached", catalog);
					}
					changed.notify_all();
					guard.unlock();
					return Loan(shared_from_this(), std::move(conn));
				}
				core.ConnectFailed(NowMs());
				changed.notify_all();
				auto answer = ClassifyAnswer(failure.RawMessage());
				auto waitable = answer == Answer::REFUSED || answer == Answer::RECONNECT;
				if (waitable) {
					last_refusal = failure.RawMessage();
				}
				guard.unlock();
				// a refusal of B, or no answer: wait for a connection in use, or for the backoff. Anything else (no
				// secret, a protocol B does not speak) - and anything at all for a borrower that may not wait - is the
				// answer now
				if (waitable) {
					Count("hugr.pool.refused", AnswerName(answer));
				}
				if (!wait || !waitable || NowMs() >= deadline) {
					if (waitable) {
						throw IOException("hugr: no connection to %s (%s) - B answered: %s", catalog,
						                  uri.CanonicalUri(), failure.RawMessage());
					}
					failure.Throw();
				}
				guard.lock();
				ticket = core.JoinFront();
				continue;
			}
		}
		auto now = NowMs();
		if (now >= deadline) {
			auto message = WaitTimeoutMessage(catalog, options.pool_wait_s, core.Busy(), options.pool_size,
			                                  core.Waiting() - 1, last_refusal);
			leave();
			guard.unlock();
			Retire(retired);
			Count("hugr.pool.wait_timeouts");
			throw IOException(message);
		}
		if (context.IsInterrupted()) {
			leave();
			guard.unlock();
			Retire(retired);
			throw InterruptException();
		}
		auto wake = MinValue<int64_t>(deadline, now + WAIT_SLICE_MS);
		changed.wait_for(guard, std::chrono::milliseconds(wake - now));
	}
}

void Pool::Return(const shared_ptr<PooledConnection> &conn, bool keep) {
	vector<shared_ptr<PooledConnection>> retired;
	{
		lock_guard<mutex> guard(lock);
		core.Return(conn, keep && !closed, NowMs(), retired);
	}
	changed.notify_all();
	Retire(retired);
}

void Pool::NodeGone(const string &label) {
	vector<shared_ptr<PooledConnection>> retired;
	{
		lock_guard<mutex> guard(lock);
		core.NodeGone(label, retired);
	}
	Retire(retired);
}

void Pool::Close() {
	vector<shared_ptr<PooledConnection>> retired;
	{
		lock_guard<mutex> guard(lock);
		closed = true;
		core.Clear(retired);
	}
	changed.notify_all();
	Retire(retired);
}

PoolView Pool::View() {
	lock_guard<mutex> guard(lock);
	PoolView view;
	view.catalog = catalog;
	view.server = uri.CanonicalUri();
	view.size = options.pool_size;
	view.busy = core.Busy();
	view.idle = core.Idle();
	view.connecting = core.Connecting();
	view.waiting = core.Waiting();
	view.last_refusal = last_refusal;
	auto now = NowMs();
	for (auto &entry : core.Entries()) {
		PoolView::Connection connection;
		connection.label = entry.label;
		connection.age_s = (now - entry.created) / 1000;
		connection.idle_s = entry.busy ? 0 : (now - entry.since) / 1000;
		connection.busy = entry.busy;
		view.connections.push_back(std::move(connection));
	}
	return view;
}

// ---- the registry -------------------------------------------------------------------------------------------------

shared_ptr<PoolRegistry> PoolRegistry::Get(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<PoolRegistry>(ObjectType());
}

void PoolRegistry::Add(const shared_ptr<Pool> &pool) {
	lock_guard<mutex> guard(lock);
	for (auto it = pools.begin(); it != pools.end();) {
		it = it->expired() ? pools.erase(it) : it + 1;
	}
	pools.push_back(pool);
}

vector<shared_ptr<Pool>> PoolRegistry::Pools() {
	lock_guard<mutex> guard(lock);
	vector<shared_ptr<Pool>> out;
	for (auto &weak : pools) {
		if (auto pool = weak.lock()) {
			out.push_back(std::move(pool));
		}
	}
	return out;
}

} // namespace hugr
} // namespace duckdb
