// Spec 010: the convergence tick's decision - every rule of hugr_node_tick.hpp, header-only: what a node applies live
// (a source or an extension the profile has and the node has not), what it detaches (a source it took from the
// profile and the profile dropped - never one attached by hand), what waits for a restart (a setting that differs or
// was dropped, an extension at another version, written again or removed) or a drain (a source re-pointed - to
// another type or to the same one), what it adopts (what its own write applied - never a database attached by hand),
// and what is neither (same, cut to the pod, the bootstrap's - the policy catalog's extension among them). And the
// resource group's: the tick drains the node while its group is gone and resumes only a drain it made itself. The agent
// running both is the e2e's. Build + run via `GEN=ninja make test-cpp`.

#include "acl_test_util.hpp"

#include "hugr_node_tick.hpp"

using namespace duckdb::hugr_node;
using namespace acl_test;

namespace {

bool Has(const std::vector<TickPending> &pending, const std::string &kind, const std::string &name,
         const std::string &item_class) {
	for (auto &p : pending) {
		if (p.kind == kind && p.name == name && p.item_class == item_class) {
			return true;
		}
	}
	return false;
}

const std::string SALES = R"({"type":"postgres","path":"","secret":"sales"})";
const std::string SALES_V2 = R"({"type":"postgres","path":"","secret":"sales_v2"})";
const std::string SPATIAL = R"({"version":"v1.4.0","repository":"hugr"})";

} // namespace

int main(int argc, char *argv[]) {
	return RunMain("the convergence tick's decision (spec 010)", [&] {
		Scenario("a hot item the node lacks is applied", [&] {
			std::vector<TickItem> items = {
			    {"source", "sales", SALES}, {"extension", "spatial", SPATIAL}, {"setting", "threads", "{}"}};
			std::vector<TickDrift> drift = {{"database", "sales", "profile_only"},
			                                {"extension", "spatial", "profile_only"},
			                                {"setting", "threads", "same"}};
			TickTaken taken = {{{"setting", "threads"}, "{}"}};
			auto decision = DecideTick(items, drift, taken);
			Check(decision.apply.size() == 2 && decision.apply[0] == 0 && decision.apply[1] == 1,
			      "the new source and the new extension, in the profile's order");
			Check(decision.pending.empty() && !decision.needs_restart, "nothing waits");
			Check(decision.adopt.empty(), "nothing to adopt: the setting is taken already");
		});

		Scenario("what cannot change live waits", [&] {
			std::vector<TickItem> items = {
			    {"setting", "memory_limit", "{}"}, {"extension", "spatial", SPATIAL}, {"source", "sales", SALES}};
			std::vector<TickDrift> drift = {{"setting", "memory_limit", "differs"},
			                                {"extension", "spatial", "differs"},
			                                {"database", "sales", "differs"}};
			auto decision = DecideTick(items, drift, {});
			Check(decision.apply.empty(), "nothing applied live");
			Check(Has(decision.pending, "setting", "memory_limit", "restart"), "a setting waits for a restart");
			Check(Has(decision.pending, "extension", "spatial", "restart"), "an extension at another version too");
			Check(Has(decision.pending, "source", "sales", "drain"), "a source of another type waits for a drain");
			Check(decision.needs_restart, "and the node says it needs a restart");
		});

		Scenario("a source re-pointed to the same type is seen by its spec", [&] {
			std::vector<TickItem> items = {{"source", "sales", SALES_V2}};
			std::vector<TickDrift> drift = {{"database", "sales", "same"}};
			auto decision = DecideTick(items, drift, {{{"source", "sales"}, SALES}});
			Check(Has(decision.pending, "source", "sales", "drain") && decision.needs_restart,
			      "the drift says same (one type), the node attached another spec: a drain");
			Check(decision.adopt.empty(), "...and it is not adopted over what the node attached");
			auto held = DecideTick(items, drift, {{{"source", "sales"}, SALES_V2}});
			Check(held.pending.empty() && !held.needs_restart, "the spec the node attached: nothing waits");
		});

		Scenario("what the node took and the profile dropped", [&] {
			TickTaken taken = {{{"source", "old_sales"}, SALES},
			                   {{"extension", "spatial"}, SPATIAL},
			                   {{"setting", "threads"}, "{}"},
			                   {{"source", "gone"}, SALES}};
			std::vector<TickDrift> drift = {{"database", "old_sales", "node_only"},
			                                {"database", "hand_made", "node_only"},
			                                {"database", "hugr_policy", "bootstrap"},
			                                {"extension", "spatial", "node_only"}};
			auto decision = DecideTick({}, drift, taken);
			Check(decision.detach.size() == 1 && decision.detach[0] == "old_sales",
			      "only the source this node took is detached - the hand-made one (ACL NATIVE) is drift, not work");
			Check(Has(decision.pending, "extension", "spatial", "restart"), "a removed extension waits for a restart");
			Check(Has(decision.pending, "setting", "threads", "restart"), "a RESET setting too");
			Check(decision.pending.size() == 2 && decision.needs_restart, "and nothing else waits");
			Check(decision.forget.size() == 1 && decision.forget[0] == TickKey("source", "gone"),
			      "a taken source no longer here is forgotten");
		});

		Scenario("what holds is left alone; what the node's own write applied is adopted, nothing else", [&] {
			std::vector<TickItem> items = {
			    {"setting", "memory_limit", "{}"}, {"source", "Sales", SALES}, {"setting", "threads", "{}"}};
			std::vector<TickDrift> drift = {{"setting", "memory_limit", "cut"}, {"database", "sales", "same"}};
			auto decision = DecideTick(items, drift, {}, {{"source", "sales"}});
			Check(decision.apply.empty() && decision.pending.empty() && !decision.needs_restart,
			      "a setting cut to the pod, a source in place (names fold), a setting with no row: nothing to do");
			Check(decision.adopt.size() == 1 && decision.adopt[0] == 1,
			      "the source the node's own write attached is the node's now");
			TickTaken taken = {{{"source", "sales"}, SALES}};
			auto dropped = DecideTick({}, {{"database", "sales", "node_only"}}, taken);
			Check(dropped.detach.size() == 1 && dropped.detach[0] == "sales",
			      "...so a DETACH written elsewhere detaches it here too");
			auto by_hand = DecideTick(items, drift, {});
			Check(by_hand.adopt.empty(),
			      "a database attached here by hand that reads `same` (one type) is not adopted");
			auto by_hand_dropped = DecideTick({}, {{"database", "sales", "node_only"}}, {});
			Check(by_hand_dropped.detach.empty(), "...so a DETACH of the profile's item never takes it: it is drift");
		});

		Scenario("the policy catalog's own extension is the bootstrap's", [&] {
			std::vector<TickItem> items = {{"extension", "postgres_scanner", SPATIAL}};
			std::vector<TickDrift> drift = {{"extension", "postgres_scanner", "bootstrap"}};
			TickTaken taken = {{{"extension", "postgres_scanner"}, "{\"version\":\"old\"}"}};
			auto decision = DecideTick(items, drift, taken);
			Check(decision.pending.empty() && !decision.needs_restart,
			      "loaded at step 5, before the profile: another version in the profile is never pending");
			auto removed = DecideTick({}, drift, taken);
			Check(removed.pending.empty(), "...nor its removal: a restart loads it the same way");
		});

		Scenario("the resource group: a drain the node made, never one an operator ordered", [&] {
			auto none = DecideGroup(false, false, false, false);
			Check(!none.drain && !none.resume, "a node in no group serves everyone - nothing to watch");
			auto gone = DecideGroup(true, false, false, false);
			Check(gone.drain && gone.self_drained, "its group gone: the node drains itself and remembers it");
			auto still = DecideGroup(true, false, true, true);
			Check(!still.drain && !still.resume && still.self_drained, "still gone: nothing more");
			auto back = DecideGroup(true, true, true, true);
			Check(back.resume && !back.self_drained, "its group back: the node resumes its own drain");
			auto ordered = DecideGroup(true, false, true, false);
			Check(!ordered.drain && !ordered.self_drained, "an operator's drain while the group is gone is not taken");
			auto ordered_back = DecideGroup(true, true, true, false);
			Check(!ordered_back.resume, "...and is never resumed when the group comes back");
			auto resumed_by_hand = DecideGroup(true, true, false, true);
			Check(!resumed_by_hand.resume && !resumed_by_hand.self_drained,
			      "an operator resumed it first: nothing to resume, the memory cleared");
		});

		Scenario("spec 045: the orchestrator's scale-down drains the node, its end resumes it", [&] {
			auto down = DecideGroup(true, true, false, false, true);
			Check(down.drain && down.self_drained, "a scale-down: the node drains itself and remembers it");
			auto still = DecideGroup(true, true, true, true, true);
			Check(!still.drain && !still.resume, "still scaled down: nothing more");
			auto up = DecideGroup(true, true, true, true, false);
			Check(up.resume && !up.self_drained, "the load came back: the node resumes its own drain");
			auto ungrouped = DecideGroup(false, false, false, false, true);
			Check(ungrouped.drain, "a node in no group is scaled down all the same");
			auto ordered = DecideGroup(true, true, true, false, true);
			Check(!ordered.drain && !ordered.self_drained, "an operator's drain is not taken over by a scale-down");
			auto ordered_end = DecideGroup(true, true, true, false, false);
			Check(!ordered_end.resume, "...and is never resumed when the scale-down ends");
			auto both = DecideGroup(true, false, true, true, false);
			Check(!both.resume && both.self_drained, "the scale-down ends while the group is gone: still drained");
			auto group_back = DecideGroup(true, true, true, true, true);
			Check(!group_back.resume, "the group back while scaled down: still drained");
		});

		Scenario("spec 035 stage 2: a lazy source is as the profile wants it, attached or not", [&] {
			std::vector<TickItem> items = {{"source", "archive", SALES}};
			TickTaken taken = {{{"source", "archive"}, SALES}};
			auto asleep = DecideTick(items, {{"database", "archive", "lazy"}}, taken);
			Check(asleep.apply.empty() && asleep.pending.empty() && asleep.detach.empty(),
			      "not attached: nothing to apply, nothing pending - it counts as applied");
			auto awake = DecideTick(items, {{"database", "archive", "same"}}, taken);
			Check(awake.apply.empty() && awake.pending.empty(), "attached on demand: same");
			auto dropped = DecideTick({}, {{"database", "archive", "node_only"}}, taken);
			Check(dropped.detach.size() == 1 && dropped.detach[0] == "archive",
			      "dropped by the profile while attached: detached, as any source it took");
			auto late = DecideTick(items, {{"database", "archive", "same"}}, {}, {{"source", "archive"}});
			Check(late.adopt.size() == 1,
			      "written after the bootstrap, attached on demand (its applied-here note): the node takes it");
			auto by_hand = DecideTick(items, {{"database", "archive", "same"}}, {});
			Check(by_hand.adopt.empty(), "attached by hand under its alias (no note): never taken");
			auto repointed = DecideTick({{"source", "archive", SALES_V2}}, {{"database", "archive", "same"}}, taken);
			Check(Has(repointed.pending, "source", "archive", "drain"),
			      "re-pointed while attached on demand: pending a drain, as any source");
			auto switched = DecideTick({}, {{"database", "archive", "node_only"}}, {}, {{"source", "archive"}});
			Check(switched.detach.size() == 1 && switched.detach[0] == "archive",
			      "attached on demand, dropped (a set switched) before the node took it: its note makes it the "
			      "node's - detached");
			auto hand = DecideTick({}, {{"database", "archive", "node_only"}}, {});
			Check(hand.detach.empty(), "the same without a note (attached by hand): left alone");
			auto dropped_asleep = DecideTick({}, {}, taken);
			Check(dropped_asleep.detach.empty() && dropped_asleep.forget.size() == 1,
			      "dropped while not attached: forgotten");
		});

		Scenario("spec 035 stage 2: the idle detach", [&] {
			LazyIdle idle;
			idle.idle_seconds = 1800;
			idle.seconds_since_use = 1800;
			idle.attached = true;
			Check(DecideIdleDetach(idle), "idle for its time, used by nobody: detached");
			idle.seconds_since_use = 1799;
			Check(!DecideIdleDetach(idle), "read within its time: kept");
			idle.seconds_since_use = 7200;
			idle.held = true;
			Check(!DecideIdleDetach(idle), "a transaction still uses it (a running statement, an open one): kept");
			idle.held = false;
			idle.attached = false;
			Check(!DecideIdleDetach(idle), "not attached: nothing to detach");
			idle.attached = true;
			idle.idle_seconds = 0;
			Check(!DecideIdleDetach(idle), "LAZY without IDLE: never detached by itself");
		});
	});
}
