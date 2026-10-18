// Spec 010: the engine against the pod - every rule of hugr_node_engine.hpp, header-only: the defaults a node sets
// from its container (memory_limit 80%, threads its CPU, max_temp_directory_size 90% of the spill volume), the
// ceilings a profile item is cut to (90% of the memory, the CPU, 90% of the volume), and which settings are the
// engine's. What the bootstrap and the tick do with them is hugr_node_bootstrap.test's and the e2e's.
// Build + run via `GEN=ninja make test-cpp`.

#include "acl_test_util.hpp"

#include "hugr_node_engine.hpp"

using namespace duckdb::hugr_node;
using namespace acl_test;

int main(int argc, char *argv[]) {
	return RunMain("the engine against the pod (spec 010)", [&] {
		const uint64_t GIB = 1024ULL * MEBIBYTE;
		Scenario("the engine's settings, by name", [&] {
			Check(EngineSettingOf("memory_limit") == EngineQuantity::MEMORY, "memory_limit is memory");
			Check(EngineSettingOf("MAX_MEMORY") == EngineQuantity::MEMORY, "...max_memory too, any case");
			Check(EngineSettingOf("threads") == EngineQuantity::THREADS, "threads");
			Check(EngineSettingOf("worker_threads") == EngineQuantity::THREADS, "...worker_threads too");
			Check(EngineSettingOf("max_temp_directory_size") == EngineQuantity::TEMP, "the spill's size");
			Check(EngineSettingOf("temp_directory") == EngineQuantity::NONE,
			      "the spill's place is hardening, not sized");
			Check(EngineSettingOf("acl_max_sessions") == EngineQuantity::NONE, "any other setting is none");
		});

		Scenario("the defaults follow the pod", [&] {
			PodResources pod;
			pod.memory = 128 * GIB;
			pod.cpu = 16;
			pod.temp_size = 600 * GIB;
			pod.temp_directory = "/var/lib/hugr-node/tmp";
			auto defaults = EngineDefaults(pod);
			Check(defaults.size() == 3, "three settings for a pod that says all three");
			Check(defaults[0].name == "memory_limit" && defaults[0].value == "104857MiB",
			      "memory_limit at 80% of 128 GiB, in whole MiB: " + defaults[0].value);
			Check(defaults[1].name == "threads" && defaults[1].value == "16", "threads at the pod's CPU");
			Check(defaults[2].name == "max_temp_directory_size" && defaults[2].value == "552960MiB",
			      "the spill at 90% of 600 GiB: " + defaults[2].value);
			Check(EngineDefaults(PodResources {}).empty(), "a pod that says nothing sets nothing");
			PodResources memory_only;
			memory_only.memory = 8 * GIB;
			Check(EngineDefaults(memory_only).size() == 1, "...and one that says only its memory sets only that");
		});

		Scenario("a profile item is cut to the pod's ceiling", [&] {
			PodResources pod;
			pod.memory = 100 * GIB;
			pod.cpu = 8;
			pod.temp_size = 1000 * GIB;
			Check(EngineCeiling(EngineQuantity::MEMORY, pod) == 90 * GIB, "memory's ceiling is 90% of the pod");
			Check(EngineCeiling(EngineQuantity::THREADS, pod) == 8, "threads' is the pod's CPU");
			Check(EngineCeiling(EngineQuantity::TEMP, pod) == 900 * GIB, "the spill's is 90% of the volume");
			Check(EngineCeiling(EngineQuantity::NONE, pod) == 0, "a setting not the engine's has none");
			bool cut = false;
			Check(ClampToCeiling(500 * GIB, 90 * GIB, cut) == 90 * GIB && cut, "500 GiB asked, 90 GiB kept, cut");
			Check(ClampToCeiling(60 * GIB, 90 * GIB, cut) == 60 * GIB && !cut, "60 GiB asked, 60 GiB kept");
			Check(ClampToCeiling(90 * GIB, 90 * GIB, cut) == 90 * GIB && !cut, "the ceiling itself is not a cut");
			Check(ClampToCeiling(32, 0, cut) == 32 && !cut, "no ceiling, nothing cut");
			Check(ClampToCeiling(UINT64_MAX, 90 * GIB, cut) == 90 * GIB && cut,
			      "a value that is no amount (unlimited, a percentage) is handed in as everything: cut to the ceiling");
			Check(EngineValueOf(EngineQuantity::MEMORY, 90 * GIB) == "92160MiB", "a size is written in MiB");
			Check(EngineValueOf(EngineQuantity::THREADS, 8) == "8", "a thread count as is");
		});
	});
}
