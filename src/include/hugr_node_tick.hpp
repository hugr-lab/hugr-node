//===----------------------------------------------------------------------===//
// hugr_node_tick.hpp - the convergence tick's decision (spec 010)
//
// Every heartbeat interval a node compares its effective profile (ClusterEffective) with what is in effect on it
// (ClusterDrift) and with what it TOOK from the profile (at its bootstrap, on a tick, or by its own write), and
// decides:
//   - a hot item missing here is applied now: a source the profile has and the node does not (ATTACH), an extension
//     the profile has and the node has not loaded (INSTALL + LOAD);
//   - a source this node took from the profile and the profile no longer has is detached now (a source someone
//     attached on this node by hand - ACL NATIVE, break-glass - is never touched: it is drift, shown);
//   - what cannot change live waits: a setting that differs, or one the profile dropped (RESET) - the configuration is
//     locked after the bootstrap; an extension at another version, written again, or removed; a source re-pointed -
//     to another type (the drift says so) or to the same type (its spec is not the one this node attached).
// What this node's own profile write applied before its commit (an ATTACH, an INSTALL) is adopted: then it is the
// node's to detach or to report, like one it applied on a tick. Nothing else is: a database attached here by hand that
// the drift reads as `same` (it compares a source by its type) stays drift, never the profile's to detach.
// The policy catalog's own extension is the bootstrap's (loaded before the profile can be read): never pending.
// Pure: rows in, a decision out; the agent runs it (hugr_node_agent.cpp) and every rule is simulated in
// test/cpp/test_hugr_node_tick.cpp.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace hugr_node {

//! An item of the effective profile: kind setting | extension | source, and its spec as stored (the JSON the agent
//! applies)
struct TickItem {
	std::string kind;
	std::string name;
	std::string spec;
};

//! A row of ClusterDrift: kind setting | extension | database; state same | differs | node_only | profile_only |
//! bootstrap, or cut - a setting the agent cut to the pod's ceiling (hugr_node_engine.hpp), which is not pending
struct TickDrift {
	std::string kind;
	std::string name;
	std::string state;
};

//! What waits: the item, and why - restart, drain, or (a hot item that failed) hot with the error's class
struct TickPending {
	std::string kind;
	std::string name;
	std::string item_class;
	std::string error;
};

//! What the node took from the profile: (kind, lowercased name) -> the spec it applied
using TickKey = std::pair<std::string, std::string>;
using TickTaken = std::map<TickKey, std::string>;

struct TickDecision {
	std::vector<size_t> apply;        //! indices into the items: hot, to apply now
	std::vector<std::string> detach;  //! sources this node took from the profile that the profile no longer has
	std::vector<size_t> adopt;        //! indices into the items: this node's own write applied them, not yet taken
	std::vector<TickKey> forget;      //! taken, dropped by the profile, and gone from the node already
	std::vector<TickPending> pending; //! what waits for a restart or a drain
	bool needs_restart = false;
};

inline std::string TickLower(const std::string &text) {
	std::string out;
	for (char c : text) {
		out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}
	return out;
}

//! `applied_here`: what this node's own profile writes applied since the last tick (kind, lowercased name)
inline TickDecision DecideTick(const std::vector<TickItem> &items, const std::vector<TickDrift> &drift,
                               const TickTaken &taken, const std::set<TickKey> &applied_here = {}) {
	// a drift row's kind as the item's: a database is a source
	std::map<TickKey, std::string> state_of;
	for (auto &row : drift) {
		auto kind = row.kind == "database" ? std::string("source") : row.kind;
		state_of[{kind, TickLower(row.name)}] = row.state;
	}
	TickDecision out;
	std::set<TickKey> in_profile;
	for (size_t i = 0; i < items.size(); i++) {
		auto &item = items[i];
		TickKey key = {item.kind, TickLower(item.name)};
		in_profile.insert(key);
		auto found = state_of.find(key);
		auto state = found == state_of.end() ? std::string("same") : found->second;
		if (state == "same" || state == "cut") {
			auto took = taken.find(key);
			if (took == taken.end()) {
				if (applied_here.count(key)) {
					out.adopt.push_back(i);
				}
			} else if (item.kind != "setting" && took->second != item.spec) {
				// held, but not as the profile has it now: a source re-pointed to the same type, an extension written
				// again (another repository) - a setting's value is the drift's to judge
				out.pending.push_back({item.kind, item.name, item.kind == "source" ? "drain" : "restart", ""});
			}
			continue;
		}
		if (state == "bootstrap") {
			continue;
		}
		if (item.kind == "source") {
			if (state == "profile_only") {
				out.apply.push_back(i);
			} else if (state == "differs") {
				out.pending.push_back({item.kind, item.name, "drain", ""});
			}
		} else if (item.kind == "extension") {
			if (state == "profile_only") {
				out.apply.push_back(i);
			} else if (state == "differs") {
				out.pending.push_back({item.kind, item.name, "restart", ""});
			}
		} else if (item.kind == "setting") {
			if (state == "differs" || state == "profile_only") {
				out.pending.push_back({item.kind, item.name, "restart", ""});
			}
		}
	}
	// what the node took and the profile dropped: a source goes now (if it is still here), the rest at a restart
	for (auto &took : taken) {
		if (in_profile.count(took.first)) {
			continue;
		}
		auto &kind = took.first.first;
		auto &name = took.first.second;
		auto found = state_of.find(took.first);
		if (found != state_of.end() && found->second == "bootstrap") {
			continue;
		}
		if (kind == "source") {
			if (found != state_of.end() && found->second == "node_only") {
				out.detach.push_back(name);
			} else {
				out.forget.push_back(took.first);
			}
		} else {
			out.pending.push_back({kind, name, "restart", ""});
		}
	}
	out.needs_restart = !out.pending.empty();
	return out;
}

//! What the tick does about the node's resource group (duckdb-acl spec 096: a node whose group the policy lacks
//! refuses new sessions already): drain the node while its group is gone, and resume only a drain it made itself -
//! never one an operator ordered
struct GroupDecision {
	bool drain = false;
	bool resume = false;
	bool self_drained = false; //! the memory for the next tick
};

inline GroupDecision DecideGroup(bool has_group, bool known, bool draining, bool self_drained) {
	GroupDecision out;
	out.self_drained = self_drained;
	if (!has_group) {
		return out;
	}
	if (!known && !draining && !self_drained) {
		out.drain = true;
		out.self_drained = true;
	} else if (known && self_drained) {
		out.resume = draining;
		out.self_drained = false;
	}
	return out;
}

} // namespace hugr_node
} // namespace duckdb
