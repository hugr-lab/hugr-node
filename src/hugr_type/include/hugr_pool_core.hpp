//===----------------------------------------------------------------------===//
// hugr_pool_core.hpp - the `hugr` type's pool, its decisions only (spec 011)
//
// Header-only and clock-free: every call takes `now` (milliseconds), so test_hugr_pool.cpp drives it without a B, a
// thread or a sleep. hugr_pool.cpp wraps it with the lock, the wait and the connect.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string_util.hpp"

#include <deque>

namespace duckdb {
namespace hugr {

//! An ATTACH's options (spec 011, "The ATTACH"): what one link to B may hold and for how long.
struct LinkOptions {
	//! connections at most, per node of A
	idx_t pool_size = 4;
	//! how long a loan may wait, in seconds (spec 080's queue bound)
	idx_t pool_wait_s = 25;
	//! a connection idle this long retires: below B's acl_session_idle_timeout (900 s)
	idx_t idle_timeout_s = 600;
	//! a connection retires at its first return after this
	idx_t max_lifetime_s = 1800;
	//! FETCHes in flight per scan, and the HTTP clients a connection caches
	idx_t read_ahead = 8;
	//! when the catalog is read again, in seconds
	idx_t catalog_ttl_s = 300;
};

//! What one of B's answers means to the pool (spec 011, "Failures and the reconnect").
enum class Answer : uint8_t {
	//! the statement's own error (binder, access, function): it fails, the connection returns
	STATEMENT,
	//! the connection is done - the lease ran out, the session ended, no answer: reconnect, retry once before a row
	RECONNECT,
	//! "not known on this node": RECONNECT, and every connection of that node's label goes (spec 134)
	NODE_GONE,
	//! at connect, no new connection now: at capacity, draining, max_sessions, another group, a refused token
	REFUSED,
	//! B's database is invalidated: the connection goes and the statement fails
	INVALIDATED
};

//! B's answer, from the raw message of the error it raised. The door's own texts (duckdb-acl's acl_embed_server.cpp
//! and acl_quack_door.cpp) and the client's transport failure.
inline Answer ClassifyAnswer(const string &message) {
	auto has = [&](const char *text) {
		return message.find(text) != string::npos;
	};
	if (has("this connection is not known on this node - connect again")) {
		return Answer::NODE_GONE;
	}
	// "Authorization failed": the door's authorize answered NULL - the connection's session has ended (Q9)
	if (has("lease ran out on this node - connect again") || message == "Authorization failed" ||
	    StringUtil::StartsWith(message, "Failed to send message") ||
	    has("Connection does not exist / already disconnected")) {
		return Answer::RECONNECT;
	}
	// the door's seats (spec 079), its placement (spec 096), and SessionOpen's refusals - a drain, max_sessions, a
	// token it does not accept - which the door does not tell apart
	if (StringUtil::StartsWith(message, "acl: node at capacity") || message == "Authentication failed" ||
	    StringUtil::StartsWith(message, "Authentication callback failed")) {
		return Answer::REFUSED;
	}
	if (has("invalidated its database") || has("The server database has been invalidated") ||
	    message == "Database was closed") {
		return Answer::INVALIDATED;
	}
	return Answer::STATEMENT;
}

//! The bounded name of an answer, for a counter's attribute and hugr_pools()
inline const char *AnswerName(Answer answer) {
	switch (answer) {
	case Answer::RECONNECT:
		return "reconnect";
	case Answer::NODE_GONE:
		return "node_gone";
	case Answer::REFUSED:
		return "refused";
	case Answer::INVALIDATED:
		return "invalidated";
	default:
		return "statement";
	}
}

//! The connection is done with: it never goes back to the pool.
inline bool EndsConnection(Answer answer) {
	return answer == Answer::RECONNECT || answer == Answer::NODE_GONE || answer == Answer::INVALIDATED;
}

//! The one transparent retry: once, on a new connection, and only before any row reached A's pipeline - the
//! PREPARE. Stage 1 opens no transaction on B, so no connection holds anything pinned.
inline bool MayRetry(Answer answer, bool rows_seen, bool retried) {
	return !retried && !rows_seen && (answer == Answer::RECONNECT || answer == Answer::NODE_GONE);
}

//! The node's label a connection id starts with (`<label>.<random>`, duckdb-acl spec 134); '' for an unlabelled id
inline string LabelOf(const string &connection_id) {
	auto dot = connection_id.find('.');
	return dot == string::npos ? string() : connection_id.substr(0, dot);
}

//! The pool of one link: who is lent what, in arrival order, and when a connection retires. CONN has
//! `bool Usable() const` - false once its heartbeat failed or B invalidated its database.
template <class CONN>
class PoolCore {
public:
	//! a refused connect suspends growth for this long, doubling per refusal up to BACKOFF_MAX_MS
	static constexpr int64_t BACKOFF_MIN_MS = 500;
	static constexpr int64_t BACKOFF_MAX_MS = 8000;

	struct Entry {
		shared_ptr<CONN> conn;
		string label;
		int64_t created = 0;
		//! lent since, or idle since
		int64_t since = 0;
		bool busy = false;
		//! retires at its return (its node went, or the pool ends)
		bool retire = false;
	};

	enum class Step : uint8_t { LEND, CONNECT, WAIT };

	explicit PoolCore(LinkOptions options_p) : options(options_p) {
	}

	//! A borrower takes a ticket and is served in arrival order.
	uint64_t Join() {
		auto ticket = next_ticket++;
		queue.push_back(ticket);
		return ticket;
	}
	//! Back at the front: a borrower whose connect failed keeps its turn.
	uint64_t JoinFront() {
		auto ticket = next_ticket++;
		queue.push_front(ticket);
		return ticket;
	}
	//! Served, or gave up: either way out of the queue.
	void Leave(uint64_t ticket) {
		for (auto it = queue.begin(); it != queue.end(); ++it) {
			if (*it == ticket) {
				queue.erase(it);
				return;
			}
		}
	}
	bool First(uint64_t ticket) const {
		return !queue.empty() && queue.front() == ticket;
	}

	//! The front borrower's next step: LEND an idle connection (`lent`, now busy); CONNECT - a place is reserved,
	//! the caller connects outside the lock and reports Connected or ConnectFailed; or WAIT. Idle connections that
	//! may not be lent (unusable, idle too long, past their lifetime) retire into `retired` on the way. `fresh`: the
	//! retry after B ended a connection - a new connection, never an idle one (it may have ended the same way); at
	//! the pool's size the least recently used idle one makes room.
	Step Next(int64_t now, shared_ptr<CONN> &lent, vector<shared_ptr<CONN>> &retired, bool fresh = false) {
		RetireIdle(now, retired);
		optional_idx newest, oldest;
		for (idx_t i = 0; i < entries.size(); i++) {
			if (entries[i].busy) {
				continue;
			}
			// the most recently returned is lent: the others stay idle long enough to retire, so the pool shrinks
			if (!newest.IsValid() || entries[i].since >= entries[newest.GetIndex()].since) {
				newest = i;
			}
			if (!oldest.IsValid() || entries[i].since < entries[oldest.GetIndex()].since) {
				oldest = i;
			}
		}
		if (!fresh && newest.IsValid()) {
			auto &entry = entries[newest.GetIndex()];
			entry.busy = true;
			entry.since = now;
			lent = entry.conn;
			return Step::LEND;
		}
		if (now < refused_until) {
			return Step::WAIT;
		}
		if (entries.size() + connecting >= MaxValue<idx_t>(options.pool_size, 1)) {
			if (!fresh || !oldest.IsValid()) {
				return Step::WAIT;
			}
			retired.push_back(std::move(entries[oldest.GetIndex()].conn));
			entries.erase(entries.begin() + oldest.GetIndex());
		}
		connecting++;
		return Step::CONNECT;
	}

	//! The reserved place's connection, lent to whoever connected it.
	void Connected(shared_ptr<CONN> conn, const string &label, int64_t now) {
		D_ASSERT(connecting > 0);
		connecting--;
		backoff = 0;
		refused_until = 0;
		Entry entry;
		entry.conn = std::move(conn);
		entry.label = label;
		entry.created = now;
		entry.since = now;
		entry.busy = true;
		entries.push_back(std::move(entry));
	}

	//! B refused a new connection (or none could be made): growth waits until the backoff passes. Waiters wait for
	//! a connection in use; an empty pool tries again after it (design/019 §5.1: wait, do not storm).
	void ConnectFailed(int64_t now) {
		D_ASSERT(connecting > 0);
		connecting--;
		backoff = backoff == 0 ? BACKOFF_MIN_MS : MinValue<int64_t>(backoff * 2, BACKOFF_MAX_MS);
		refused_until = now + backoff;
	}

	//! A loan comes back. `keep` false: B's answer ended the connection. Past its lifetime, or marked, it retires.
	void Return(const shared_ptr<CONN> &conn, bool keep, int64_t now, vector<shared_ptr<CONN>> &retired) {
		for (idx_t i = 0; i < entries.size(); i++) {
			if (entries[i].conn != conn) {
				continue;
			}
			auto &entry = entries[i];
			auto lapsed = now - entry.created >= Ms(options.max_lifetime_s);
			if (!keep || entry.retire || lapsed || !entry.conn->Usable()) {
				retired.push_back(std::move(entry.conn));
				entries.erase(entries.begin() + i);
			} else {
				entry.busy = false;
				entry.since = now;
			}
			return;
		}
	}

	//! A node of B is gone (its label answered "not known"): idle connections there retire now, busy ones at their
	//! return. An unlabelled id names no node: nothing else goes.
	void NodeGone(const string &label, vector<shared_ptr<CONN>> &retired) {
		if (label.empty()) {
			return;
		}
		for (idx_t i = entries.size(); i-- > 0;) {
			if (entries[i].label != label) {
				continue;
			}
			if (entries[i].busy) {
				entries[i].retire = true;
			} else {
				retired.push_back(std::move(entries[i].conn));
				entries.erase(entries.begin() + i);
			}
		}
	}

	//! The idle connections past idle_timeout (or unusable, or past their lifetime) retire.
	void Sweep(int64_t now, vector<shared_ptr<CONN>> &retired) {
		RetireIdle(now, retired);
	}

	//! The pool's end: idle ones now, busy ones at their return.
	void Clear(vector<shared_ptr<CONN>> &retired) {
		for (idx_t i = entries.size(); i-- > 0;) {
			if (entries[i].busy) {
				entries[i].retire = true;
			} else {
				retired.push_back(std::move(entries[i].conn));
				entries.erase(entries.begin() + i);
			}
		}
	}

	idx_t Busy() const {
		idx_t n = 0;
		for (auto &entry : entries) {
			n += entry.busy;
		}
		return n;
	}
	idx_t Idle() const {
		return entries.size() - Busy();
	}
	idx_t Connecting() const {
		return connecting;
	}
	idx_t Waiting() const {
		return queue.size();
	}
	const vector<Entry> &Entries() const {
		return entries;
	}
	//! when growth may be tried again (0: now)
	int64_t RefusedUntil() const {
		return refused_until;
	}
	const LinkOptions &Options() const {
		return options;
	}

private:
	static int64_t Ms(idx_t seconds) {
		return static_cast<int64_t>(seconds) * 1000;
	}
	void RetireIdle(int64_t now, vector<shared_ptr<CONN>> &retired) {
		for (idx_t i = entries.size(); i-- > 0;) {
			auto &entry = entries[i];
			if (entry.busy) {
				continue;
			}
			auto lapsed = now - entry.created >= Ms(options.max_lifetime_s);
			auto idle = now - entry.since >= Ms(options.idle_timeout_s);
			if (lapsed || idle || !entry.conn->Usable()) {
				retired.push_back(std::move(entry.conn));
				entries.erase(entries.begin() + i);
			}
		}
	}

	LinkOptions options;
	vector<Entry> entries;
	idx_t connecting = 0;
	std::deque<uint64_t> queue;
	uint64_t next_ticket = 1;
	int64_t backoff = 0;
	int64_t refused_until = 0;
};

//! The message a loan that waited out pool_wait fails with
inline string WaitTimeoutMessage(const string &catalog, idx_t wait_s, idx_t busy, idx_t size, idx_t waiting,
                                 const string &last_refusal) {
	auto message = StringUtil::Format("hugr: no connection to %s within %llu s (%llu of %llu in use, %llu waiting)",
	                                  catalog, wait_s, busy, size, waiting);
	if (!last_refusal.empty()) {
		message += " - B answered: " + last_refusal;
	}
	return message;
}

} // namespace hugr
} // namespace duckdb
