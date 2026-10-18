//===----------------------------------------------------------------------===//
// hugr_node_engine.hpp - the engine against the pod (spec 010)
//
// A node sizes DuckDB from its container, not from the machine: the defaults it sets from the container's limits
// (memory_limit 80% of the pod's memory - DuckDB's own ratio of a machine -, threads its CPU, max_temp_directory_size
// 90% of the spill volume), and the ceilings a profile item for one of those settings is cut to (90% of the memory,
// the CPU, 90% of the volume) - past them the pod is OOM-killed or evicted with every session on it. Pure: the pod's
// numbers in, settings out; a memory string is parsed by the caller (duckdb's own parser) and handed in as bytes - a
// value that is no finite amount (unlimited, a percentage) is handed in as the most there is, so the ceiling cuts it.
// Every rule is simulated in test/cpp/test_hugr_node_engine.cpp.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace hugr_node {

//! What the pod's environment says about the container (0 = not said: no default, no ceiling)
struct PodResources {
	uint64_t memory = 0;        //! HUGR_POD_MEMORY: the container's memory limit, bytes (Downward API limits.memory)
	uint64_t cpu = 0;           //! HUGR_POD_CPU: the container's CPU, whole cores (Downward API requests.cpu)
	uint64_t temp_size = 0;     //! HUGR_TEMP_SIZE: the spill volume's size, bytes
	std::string temp_directory; //! HUGR_TEMP_DIRECTORY: the spill volume's mount
};

//! Which of the engine's quantities a setting sizes
enum class EngineQuantity : uint8_t { NONE, MEMORY, THREADS, TEMP };

inline EngineQuantity EngineSettingOf(const std::string &name) {
	std::string lower;
	for (char c : name) {
		lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}
	if (lower == "memory_limit" || lower == "max_memory") {
		return EngineQuantity::MEMORY;
	}
	if (lower == "threads" || lower == "worker_threads") {
		return EngineQuantity::THREADS;
	}
	if (lower == "max_temp_directory_size") {
		return EngineQuantity::TEMP;
	}
	return EngineQuantity::NONE;
}

static constexpr uint64_t MEBIBYTE = 1024ULL * 1024ULL;

//! A byte count as the setting is written: whole MiB, rounded down
inline std::string MebibytesOf(uint64_t bytes) {
	return std::to_string(bytes / MEBIBYTE) + "MiB";
}

//! The value a quantity is written as: a byte count in MiB, a thread count as is
inline std::string EngineValueOf(EngineQuantity quantity, uint64_t value) {
	return quantity == EngineQuantity::THREADS ? std::to_string(value) : MebibytesOf(value);
}

struct EngineSetting {
	std::string name;
	std::string value;
};

//! The settings the bootstrap writes before the profile, from the pod: only those the pod says something about
inline std::vector<EngineSetting> EngineDefaults(const PodResources &pod) {
	std::vector<EngineSetting> out;
	if (pod.memory > 0) {
		out.push_back({"memory_limit", MebibytesOf(pod.memory / 10 * 8)});
	}
	if (pod.cpu > 0) {
		out.push_back({"threads", std::to_string(pod.cpu)});
	}
	if (pod.temp_size > 0) {
		out.push_back({"max_temp_directory_size", MebibytesOf(pod.temp_size / 10 * 9)});
	}
	return out;
}

//! The most a profile item may set a quantity to on this pod, in the quantity's own unit (bytes, or a thread count);
//! 0 = no ceiling (the pod says nothing about it)
inline uint64_t EngineCeiling(EngineQuantity quantity, const PodResources &pod) {
	switch (quantity) {
	case EngineQuantity::MEMORY:
		return pod.memory / 10 * 9;
	case EngineQuantity::THREADS:
		return pod.cpu;
	case EngineQuantity::TEMP:
		return pod.temp_size / 10 * 9;
	default:
		return 0;
	}
}

//! A profile value against its ceiling: what to set, and whether it was cut
inline uint64_t ClampToCeiling(uint64_t value, uint64_t ceiling, bool &cut) {
	cut = ceiling > 0 && value > ceiling;
	return cut ? ceiling : value;
}

} // namespace hugr_node
} // namespace duckdb
