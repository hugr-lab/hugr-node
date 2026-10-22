// Spec 006: the node process and its bootstrap.
//
// `duckdb -c "PRAGMA hugr_node_run"` is a platform node's entrypoint. The function reads the local part from the
// pod's environment (the way into tresor and the control plane - no credential), logs in to tresor with the node's
// workload identity, takes the bootstrap document from the control plane, trusts the extension repository by its
// discovery document, attaches the policy catalog with a secret from tresor, applies the cluster profile, locks the
// configuration, opens the doors and blocks. On SIGTERM it drains, ends the stragglers, stops the doors and returns:
// the CLI then exits 0, which is what gives the database its final checkpoint (duckdb-acl spec 084).
//
// Every statement comes from the plan (BootstrapPlan + hugr_node_profile_statements), which is pure and is what the
// tests and an operator read. The first failing step stops the bootstrap with the step named; nothing listens before
// the policy is loaded and the configuration is locked.

#include "hugr_node_agent.hpp"

#include "acl_cluster.hpp"
#include "acl_door_common.hpp"
#include "acl_node_label.hpp"
#include "acl_result_rows.hpp"
#include "hugr_node_extension.hpp"
#include "hugr_node_tick.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <thread>

namespace duckdb {
namespace hugr_node {

namespace {

//! The one run of this process and the signal that ends it. Process-wide by nature: a signal reaches the process, and
//! the node is one per process (a second PRAGMA hugr_node_run is refused). The handler only stores (async-signal-safe).
std::atomic<bool> g_running {false};
std::atomic<int> g_stop_signal {0};

extern "C" void HugrNodeOnSignal(int signal_number) {
	g_stop_signal.store(signal_number);
}

string Quoted(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

string Ident(const string &value) {
	return "\"" + StringUtil::Replace(value, "\"", "\"\"") + "\"";
}

//! a type or an extension name goes into a statement as written: a plain identifier, nothing else
void RequirePlainName(const string &value, const char *what) {
	if (value.empty()) {
		return;
	}
	for (auto c : value) {
		if (!(StringUtil::CharacterIsAlphaNumeric(c) || c == '_')) {
			throw InvalidInputException("hugr_node: the bootstrap document's %s \"%s\" is not a plain name", what,
			                            value);
		}
	}
}

string EnvOr(const char *name, const string &fallback = string()) {
	auto value = std::getenv(name);
	return value && *value ? string(value) : fallback;
}

int64_t ParseSeconds(const string &value, const char *name) {
	if (value.empty()) {
		return 25;
	}
	char *end = nullptr;
	auto parsed = std::strtoll(value.c_str(), &end, 10);
	if (!end || *end != '\0' || parsed < 0) {
		throw InvalidInputException("hugr_node: %s is a number of seconds, not \"%s\"", name, value);
	}
	return parsed;
}

bool AllDigits(const string &value) {
	return !value.empty() && value.size() < 20 &&
	       std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}

//! a finite size: a byte count (the Downward API's), a duckdb size ('600GiB') or a Kubernetes quantity's binary form
//! ('600Gi' - a volume's sizeLimit as written); invalid for anything else - unlimited, a percentage, a typo
optional_idx FiniteBytes(const string &value) {
	if (AllDigits(value)) {
		return std::strtoull(value.c_str(), nullptr, 10);
	}
	auto text = value;
	if (text.size() > 2 && text.back() == 'i' && string("KMGT").find(text[text.size() - 2]) != string::npos) {
		text += "B";
	}
	try {
		return DBConfig::ParseMemoryLimit(text);
	} catch (std::exception &) {
		return optional_idx();
	}
}

//! spec 010: a byte count from the pod; '' = 0. Below 1 MiB is no pod's size - a Downward API divisor other than 1
uint64_t ParseBytes(const string &value, const char *name) {
	if (value.empty()) {
		return 0;
	}
	auto parsed = FiniteBytes(value);
	if (!parsed.IsValid() || parsed.GetIndex() < MEBIBYTE) {
		throw InvalidInputException("hugr_node: %s is a size of at least 1 MiB - a byte count (the Downward API's, "
		                            "divisor 1) or a size like '600GiB' / '600Gi'",
		                            name);
	}
	return parsed.GetIndex();
}

//! spec 010: whole cores from the pod (the Downward API's requests.cpu with divisor 1 rounds up); '' = 0
uint64_t ParseCores(const string &value, const char *name) {
	if (value.empty()) {
		return 0;
	}
	auto parsed = AllDigits(value) && value.size() < 10 ? std::strtoull(value.c_str(), nullptr, 10) : 0;
	if (parsed == 0) {
		throw InvalidInputException("hugr_node: %s is a whole number of cores (the Downward API's, divisor 1)", name);
	}
	return parsed;
}

//! spec 010: what a profile item asks of an engine quantity - anything that is not a finite amount (unlimited, a
//! percentage, '1e3' threads, a typo) asks for everything, so the pod's ceiling cuts it: the ceiling fails closed
uint64_t RequestedAmount(EngineQuantity quantity, const string &value) {
	if (quantity == EngineQuantity::THREADS) {
		return AllDigits(value) && value.size() < 10 ? std::strtoull(value.c_str(), nullptr, 10)
		                                             : std::numeric_limits<uint64_t>::max();
	}
	auto parsed = FiniteBytes(value);
	return parsed.IsValid() ? parsed.GetIndex() : std::numeric_limits<uint64_t>::max();
}

string CutTresorScheme(const string &host) {
	return StringUtil::StartsWith(host, "tresor:") ? host.substr(7) : host;
}

string TrimSlash(string url) {
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	return url;
}

//! spec 006: plain http to tresor is for a tresor-server on loopback only - a host and a port, nothing that could
//! send the request elsewhere (userinfo, a path); the value never reaches the error
void RequireLoopbackTresor(const string &tresor) {
	bool plain = tresor.find_first_of("@/?#\\ ") == string::npos;
	auto colon = tresor.rfind(':');
	auto host = colon == string::npos ? tresor : tresor.substr(0, colon);
	if (!plain || (host != "127.0.0.1" && host != "localhost" && host != "[::1]")) {
		throw InvalidInputException("hugr_node: HUGR_TRESOR_INSECURE_HTTP is for a tresor-server on loopback only - "
		                            "a host:port of 127.0.0.1, localhost or [::1]");
	}
}

//! JSON helpers over duckdb's bundled yyjson
string StrField(duckdb_yyjson::yyjson_val *object, const char *field) {
	using namespace duckdb_yyjson; // NOLINT
	auto value = object ? yyjson_obj_get(object, field) : nullptr;
	return value && yyjson_is_str(value) ? string(yyjson_get_str(value)) : string();
}

struct JsonDoc {
	explicit JsonDoc(const string &text, const char *what) {
		doc = duckdb_yyjson::yyjson_read(text.c_str(), text.size(), 0);
		if (!doc || !duckdb_yyjson::yyjson_is_obj(duckdb_yyjson::yyjson_doc_get_root(doc))) {
			if (doc) {
				duckdb_yyjson::yyjson_doc_free(doc);
			}
			throw InvalidInputException("hugr_node: %s is not a JSON object", what);
		}
	}
	~JsonDoc() {
		duckdb_yyjson::yyjson_doc_free(doc);
	}
	duckdb_yyjson::yyjson_val *Root() const {
		return duckdb_yyjson::yyjson_doc_get_root(doc);
	}
	duckdb_yyjson::yyjson_doc *doc;
};

//! spec 015: an issuer's, a client's or a group's name goes into a management statement as an identifier - a plain
//! name of at most 63 (a resource group is named after a Kubernetes object: letters, digits, '-', '_')
void RequireFirstPolicyName(const string &value, const char *what) {
	bool plain = !value.empty() && value.size() <= 63 && std::all_of(value.begin(), value.end(), [](char c) {
		return StringUtil::CharacterIsAlphaNumeric(c) || c == '_' || c == '-';
	});
	if (!plain) {
		throw InvalidInputException("hugr_node: the bootstrap document's first_policy names %s \"%s\" that is not a "
		                            "plain name ([A-Za-z0-9_-], at most 63)",
		                            what, value);
	}
}

//! spec 015: the first policy, every field required - a partial one would write an IdP nobody can sign in through
FirstPolicy ParseFirstPolicy(duckdb_yyjson::yyjson_val *object, const string &node_group) {
	using namespace duckdb_yyjson; // NOLINT
	auto refuse = [](const char *what) {
		throw InvalidInputException("hugr_node: the bootstrap document's first_policy %s", what);
	};
	if (!yyjson_is_obj(object)) {
		refuse("is not an object");
	}
	FirstPolicy out;
	auto issuer = yyjson_obj_get(object, "issuer");
	if (!issuer || !yyjson_is_obj(issuer)) {
		refuse("has no issuer");
	}
	out.issuer_name = StrField(issuer, "name");
	RequireFirstPolicyName(out.issuer_name, "an issuer");
	out.issuer_url = StrField(issuer, "url");
	// its keys are read by discovery from it, over https only (acl_jwks_locations); the value never reaches an error
	if (!StringUtil::StartsWith(out.issuer_url, "https://") || StringUtil::Contains(out.issuer_url, "@")) {
		refuse("names an issuer url that is not an https URL without userinfo");
	}
	out.roles_claim = StrField(issuer, "roles_claim");
	if (out.roles_claim.empty()) {
		refuse("names no issuer roles_claim - the claim that carries the administrators' value");
	}
	auto audiences = yyjson_obj_get(issuer, "audiences");
	size_t idx, max;
	yyjson_val *item;
	if (audiences && yyjson_is_arr(audiences)) {
		yyjson_arr_foreach(audiences, idx, max, item) {
			if (!yyjson_is_str(item) || !*yyjson_get_str(item)) {
				refuse("has an issuer audience that is not a non-empty string");
			}
			out.audiences.emplace_back(yyjson_get_str(item));
		}
	}
	if (out.audiences.empty()) {
		refuse("names no issuer audiences - the platform's tokens carry them");
	}
	// names compare as the policy compares them: exactly
	std::set<string> client_names, client_ids, group_names;
	auto clients = yyjson_obj_get(object, "clients");
	if (clients && yyjson_is_arr(clients)) {
		yyjson_arr_foreach(clients, idx, max, item) {
			FirstPolicy::Client client {StrField(item, "name"), StrField(item, "client_id")};
			RequireFirstPolicyName(client.name, "a client");
			if (client.client_id.empty()) {
				refuse("names a client without its client_id");
			}
			// two clients for one application would accept the same tokens - the policy refuses them
			if (!client_names.insert(client.name).second || !client_ids.insert(client.client_id).second) {
				refuse("names a client or a client_id twice");
			}
			out.clients.push_back(std::move(client));
		}
	}
	if (out.clients.empty()) {
		refuse("names no client - the console's is how the administrator gets in");
	}
	out.administrators = StrField(object, "administrators");
	if (out.administrators.empty()) {
		refuse("names no administrators - the roles claim's value that makes the platform's administrator");
	}
	auto groups = yyjson_obj_get(object, "resource_groups");
	bool has_default = false, has_own = false;
	if (groups && yyjson_is_arr(groups)) {
		yyjson_arr_foreach(groups, idx, max, item) {
			FirstPolicy::Group group;
			group.name = StrField(item, "name");
			RequireFirstPolicyName(group.name, "a resource group");
			auto is_default = yyjson_is_obj(item) ? yyjson_obj_get(item, "default") : nullptr;
			if (is_default && !yyjson_is_bool(is_default)) {
				refuse("has a resource group whose default is not true or false");
			}
			group.is_default = is_default && yyjson_get_bool(is_default);
			if (group.is_default && has_default) {
				refuse("marks more than one resource group default");
			}
			has_default = has_default || group.is_default;
			if (!group_names.insert(group.name).second) {
				refuse("names a resource group twice");
			}
			has_own = has_own || group.name == node_group;
			out.resource_groups.push_back(std::move(group));
		}
	}
	// a node whose group the policy lacks refuses everyone and drains (spec 010)
	if (!has_own) {
		refuse("names no resource group for this node's group");
	}
	return out;
}

} // namespace

//===--------------------------------------------------------------------===//
// The local part
//===--------------------------------------------------------------------===//

AgentEnvironment AgentEnvironment::FromProcess() {
	AgentEnvironment env;
	env.tresor = CutTresorScheme(EnvOr("HUGR_TRESOR"));
	env.issuer = EnvOr("HUGR_IDENTITY_ISSUER");
	env.client_id = EnvOr("HUGR_IDENTITY_CLIENT_ID");
	env.scope = EnvOr("HUGR_IDENTITY_SCOPE");
	env.assertion_file = EnvOr("HUGR_IDENTITY_ASSERTION_FILE");
	env.private_key_file = EnvOr("HUGR_IDENTITY_PRIVATE_KEY_FILE");
	env.key_id = EnvOr("HUGR_IDENTITY_KEY_ID");
	env.control_plane = TrimSlash(EnvOr("HUGR_CONTROL_PLANE"));
	env.ca_file = EnvOr("HUGR_CA_FILE");
	env.flight = EnvOr("HUGR_FLIGHT");
	env.flight_cert = EnvOr("HUGR_FLIGHT_CERT");
	env.flight_key = EnvOr("HUGR_FLIGHT_KEY");
	env.quack = EnvOr("HUGR_QUACK");
	env.quack_cert = EnvOr("HUGR_QUACK_CERT");
	env.quack_key = EnvOr("HUGR_QUACK_KEY");
	env.drain_deadline = ParseSeconds(EnvOr("HUGR_DRAIN_DEADLINE"), "HUGR_DRAIN_DEADLINE");
	env.tresor_insecure_http = EnvOr("HUGR_TRESOR_INSECURE_HTTP") == "1";
	env.orchestrator = TrimSlash(EnvOr("HUGR_ORCHESTRATOR"));
	env.orchestrator_token_file = EnvOr("HUGR_ORCHESTRATOR_TOKEN_FILE");
	env.pod.memory = ParseBytes(EnvOr("HUGR_POD_MEMORY"), "HUGR_POD_MEMORY");
	env.pod.cpu = ParseCores(EnvOr("HUGR_POD_CPU"), "HUGR_POD_CPU");
	env.pod.temp_size = ParseBytes(EnvOr("HUGR_TEMP_SIZE"), "HUGR_TEMP_SIZE");
	env.pod.temp_directory = EnvOr("HUGR_TEMP_DIRECTORY");
	return env;
}

AgentEnvironment AgentEnvironment::FromJson(const string &json) {
	JsonDoc doc(json, "the environment");
	auto root = doc.Root();
	AgentEnvironment env;
	env.tresor = CutTresorScheme(StrField(root, "HUGR_TRESOR"));
	env.issuer = StrField(root, "HUGR_IDENTITY_ISSUER");
	env.client_id = StrField(root, "HUGR_IDENTITY_CLIENT_ID");
	env.scope = StrField(root, "HUGR_IDENTITY_SCOPE");
	env.assertion_file = StrField(root, "HUGR_IDENTITY_ASSERTION_FILE");
	env.private_key_file = StrField(root, "HUGR_IDENTITY_PRIVATE_KEY_FILE");
	env.key_id = StrField(root, "HUGR_IDENTITY_KEY_ID");
	env.control_plane = TrimSlash(StrField(root, "HUGR_CONTROL_PLANE"));
	env.ca_file = StrField(root, "HUGR_CA_FILE");
	env.flight = StrField(root, "HUGR_FLIGHT");
	env.flight_cert = StrField(root, "HUGR_FLIGHT_CERT");
	env.flight_key = StrField(root, "HUGR_FLIGHT_KEY");
	env.quack = StrField(root, "HUGR_QUACK");
	env.quack_cert = StrField(root, "HUGR_QUACK_CERT");
	env.quack_key = StrField(root, "HUGR_QUACK_KEY");
	env.drain_deadline = ParseSeconds(StrField(root, "HUGR_DRAIN_DEADLINE"), "HUGR_DRAIN_DEADLINE");
	env.tresor_insecure_http = StrField(root, "HUGR_TRESOR_INSECURE_HTTP") == "1";
	env.orchestrator = TrimSlash(StrField(root, "HUGR_ORCHESTRATOR"));
	env.orchestrator_token_file = StrField(root, "HUGR_ORCHESTRATOR_TOKEN_FILE");
	env.pod.memory = ParseBytes(StrField(root, "HUGR_POD_MEMORY"), "HUGR_POD_MEMORY");
	env.pod.cpu = ParseCores(StrField(root, "HUGR_POD_CPU"), "HUGR_POD_CPU");
	env.pod.temp_size = ParseBytes(StrField(root, "HUGR_TEMP_SIZE"), "HUGR_TEMP_SIZE");
	env.pod.temp_directory = StrField(root, "HUGR_TEMP_DIRECTORY");
	return env;
}

void AgentEnvironment::Validate() const {
	auto require = [](const string &value, const char *name) {
		if (value.empty()) {
			throw InvalidInputException("hugr_node: %s is not set", name);
		}
	};
	if (pod.temp_directory.empty() != (pod.temp_size == 0)) {
		throw InvalidInputException("hugr_node: HUGR_TEMP_DIRECTORY and HUGR_TEMP_SIZE go together - the spill "
		                            "volume's mount and its size");
	}
	if (Orchestrated()) {
		// spec 010: the orchestrator's document names tresor and the node's identity - one source per value
		auto refuse = [](const string &value, const char *name) {
			if (!value.empty()) {
				throw InvalidInputException("hugr_node: %s comes in the orchestrator's document when HUGR_ORCHESTRATOR "
				                            "is set - remove it from the environment",
				                            name);
			}
		};
		refuse(tresor, "HUGR_TRESOR");
		refuse(issuer, "HUGR_IDENTITY_ISSUER");
		refuse(client_id, "HUGR_IDENTITY_CLIENT_ID");
		refuse(scope, "HUGR_IDENTITY_SCOPE");
		refuse(control_plane, "HUGR_CONTROL_PLANE");
		// the bearer goes there: https, and no userinfo (a URL's value never reaches an error or a log)
		if (!StringUtil::StartsWith(orchestrator, "https://") || StringUtil::Contains(orchestrator, "@")) {
			throw InvalidInputException("hugr_node: HUGR_ORCHESTRATOR is an https URL without userinfo");
		}
		require(orchestrator_token_file, "HUGR_ORCHESTRATOR_TOKEN_FILE");
		if (assertion_file.empty() == private_key_file.empty()) {
			throw InvalidInputException("hugr_node: the node's identity is HUGR_IDENTITY_ASSERTION_FILE (the projected "
			                            "service-account token) or, for development only, "
			                            "HUGR_IDENTITY_PRIVATE_KEY_FILE - exactly one of them");
		}
		require(flight, "HUGR_FLIGHT");
		if (flight_cert.empty() != flight_key.empty()) {
			throw InvalidInputException("hugr_node: HUGR_FLIGHT_CERT and HUGR_FLIGHT_KEY go together");
		}
		if (quack_cert.empty() != quack_key.empty()) {
			throw InvalidInputException("hugr_node: HUGR_QUACK_CERT and HUGR_QUACK_KEY go together");
		}
		return;
	}
	require(tresor, "HUGR_TRESOR");
	require(issuer, "HUGR_IDENTITY_ISSUER");
	require(client_id, "HUGR_IDENTITY_CLIENT_ID");
	if (assertion_file.empty() == private_key_file.empty()) {
		throw InvalidInputException("hugr_node: the node's identity is HUGR_IDENTITY_ASSERTION_FILE (the projected "
		                            "service-account token) or, for development only, HUGR_IDENTITY_PRIVATE_KEY_FILE - "
		                            "exactly one of them");
	}
	if (tresor_insecure_http) {
		RequireLoopbackTresor(tresor);
	}
	require(control_plane, "HUGR_CONTROL_PLANE");
	if (!StringUtil::StartsWith(control_plane, "https://") || StringUtil::Contains(control_plane, "@")) {
		throw InvalidInputException("hugr_node: HUGR_CONTROL_PLANE is an https URL without userinfo");
	}
	require(flight, "HUGR_FLIGHT");
	if (flight_cert.empty() != flight_key.empty()) {
		throw InvalidInputException("hugr_node: HUGR_FLIGHT_CERT and HUGR_FLIGHT_KEY go together");
	}
	if (quack_cert.empty() != quack_key.empty()) {
		throw InvalidInputException("hugr_node: HUGR_QUACK_CERT and HUGR_QUACK_KEY go together");
	}
}

//===--------------------------------------------------------------------===//
// The bootstrap document
//===--------------------------------------------------------------------===//

BootstrapDocument BootstrapDocument::Parse(const string &json) {
	using namespace duckdb_yyjson; // NOLINT
	JsonDoc doc(json, "the bootstrap document");
	auto root = doc.Root();
	BootstrapDocument out;
	auto version = yyjson_obj_get(root, "version");
	if (!version || !yyjson_is_int(version)) {
		throw InvalidInputException("hugr_node: the bootstrap document has no version");
	}
	out.version = yyjson_get_sint(version);
	if (out.version != 1 && out.version != 2) {
		throw InvalidInputException(
		    "hugr_node: the bootstrap document's version %lld is not one this node reads (1, 2)",
		    static_cast<long long>(out.version));
	}
	out.node_group = StrField(root, "node_group");
	if (out.version == 2) {
		// spec 010: the orchestrator's - the node's label (its pod), tresor and the node's identity in the IdP
		out.node_label = StrField(root, "node_label");
		if (!acl::ValidNodeLabel(out.node_label)) {
			throw InvalidInputException(
			    "hugr_node: the bootstrap document (version 2) names no node_label that is a DNS "
			    "label ([a-z0-9-], at most 63) - the node's sessions carry it (duckdb-acl spec "
			    "134)");
		}
		if (out.node_group.empty()) {
			throw InvalidInputException("hugr_node: the bootstrap document (version 2) names no node_group - an "
			                            "orchestrated node always belongs to its group");
		}
		auto tresor = yyjson_obj_get(root, "tresor");
		auto identity = tresor && yyjson_is_obj(tresor) ? yyjson_obj_get(tresor, "identity") : nullptr;
		out.tresor_host = CutTresorScheme(StrField(tresor, "host"));
		out.tresor_issuer = StrField(identity, "issuer");
		out.tresor_client_id = StrField(identity, "client_id");
		out.tresor_scope = StrField(identity, "scope");
		if (out.tresor_host.empty() || out.tresor_issuer.empty() || out.tresor_client_id.empty()) {
			throw InvalidInputException("hugr_node: the bootstrap document (version 2) names no tresor host, issuer "
			                            "and client_id - the node logs in to tresor with them");
		}
		auto heartbeat = yyjson_obj_get(root, "heartbeat");
		auto interval = heartbeat && yyjson_is_obj(heartbeat) ? yyjson_obj_get(heartbeat, "interval_seconds") : nullptr;
		if (interval && !yyjson_is_int(interval)) {
			throw InvalidInputException("hugr_node: the bootstrap document's heartbeat interval_seconds is a whole "
			                            "number of seconds");
		}
		if (interval) {
			out.heartbeat_interval = yyjson_get_sint(interval);
		}
		if (out.heartbeat_interval < 1 || out.heartbeat_interval > 3600) {
			throw InvalidInputException(
			    "hugr_node: the bootstrap document's heartbeat interval_seconds is 1..3600, not "
			    "%lld",
			    static_cast<long long>(out.heartbeat_interval));
		}
		// spec 015: the platform's first policy, when the operator's platform names one
		auto first_policy = yyjson_obj_get(root, "first_policy");
		if (first_policy && !yyjson_is_null(first_policy)) {
			out.first_policy = ParseFirstPolicy(first_policy, out.node_group);
			out.has_first_policy = true;
		}
	}
	auto repository = yyjson_obj_get(root, "repository");
	if (!repository || !yyjson_is_obj(repository)) {
		throw InvalidInputException("hugr_node: the bootstrap document has no repository");
	}
	auto name = StrField(repository, "name");
	if (!name.empty()) {
		out.repository_name = name;
	}
	out.repository_prefix = TrimSlash(StrField(repository, "prefix"));
	if (out.repository_prefix.empty()) {
		throw InvalidInputException("hugr_node: the bootstrap document's repository has no prefix");
	}
	if (!StringUtil::StartsWith(out.repository_prefix, "https://") ||
	    StringUtil::Contains(out.repository_prefix, "@")) {
		throw InvalidInputException("hugr_node: the repository's prefix is an https URL without userinfo - its "
		                            "signing keys are read from it");
	}
	auto catalog = yyjson_obj_get(root, "catalog");
	if (!catalog || !yyjson_is_obj(catalog)) {
		throw InvalidInputException("hugr_node: the bootstrap document has no catalog");
	}
	if (out.version == 1) {
		out.catalog_type = StrField(catalog, "type");
		out.catalog_extension = StrField(catalog, "extension");
		out.catalog_path = StrField(catalog, "path");
	}
	// version 2: the reserved secret's own type is the catalog's (the owner, 2026-10-17), read at the run
	out.catalog_secret = StrField(catalog, "secret");
	auto schema = StrField(catalog, "schema");
	if (!schema.empty()) {
		out.catalog_schema = schema;
	}
	// spec 017: the canary's flag - the operator's alone, never a node's own decision
	auto migrate = yyjson_obj_get(catalog, "migrate");
	if (migrate && !yyjson_is_null(migrate)) {
		if (!yyjson_is_bool(migrate)) {
			throw InvalidInputException("hugr_node: the bootstrap document's catalog migrate is true or false");
		}
		out.catalog_migrate = yyjson_get_bool(migrate);
	}
	if (out.version == 1 && out.catalog_type.empty()) {
		throw InvalidInputException("hugr_node: the bootstrap document's catalog has no type");
	}
	if (out.catalog_secret.empty()) {
		throw InvalidInputException("hugr_node: the bootstrap document's catalog names no secret - the node reaches "
		                            "its policy catalog with a secret from tresor");
	}
	RequirePlainName(out.catalog_type, "catalog type");
	RequirePlainName(out.catalog_extension, "catalog extension");
	return out;
}

//===--------------------------------------------------------------------===//
// The plan
//===--------------------------------------------------------------------===//

namespace {

const char *POLICY_ALIAS = "hugr_policy";
const char *TRESOR_ALIAS = "hugr_tresor";
const char *IDENTITY_SECRET = "hugr_node_identity";

vector<PlanStep> HardeningSteps(const AgentEnvironment &env) {
	vector<PlanStep> steps;
	auto add = [&](const string &sql) {
		steps.push_back({"1 hardening", sql});
	};
	add("SET GLOBAL allow_extension_repositories = 'allowed'");
	add("SET GLOBAL autoinstall_known_extensions = false");
	add("SET GLOBAL autoload_known_extensions = false");
	add("SET GLOBAL allow_persistent_secrets = false");
	// every request of tresor's - discovery, the IdP, the service - through duckdb's HTTP client, which verifies
	// against ca_cert_file; tresor's builtin client trusts only the system roots (reported to tresor 2026-10-15)
	add("SET GLOBAL tresor_http_client = 'duckdb'");
	// spec 010: whichever HTTP client serves a request, it verifies the server (httplib's own default does not)
	add("SET GLOBAL enable_server_cert_verification = true");
	if (!env.ca_file.empty()) {
		add("SET GLOBAL ca_cert_file = " + Quoted(env.ca_file));
	}
	// spec 010: where the node spills is the pod's spill volume (temp_directory is a hardening setting)
	if (!env.pod.temp_directory.empty()) {
		add("SET GLOBAL temp_directory = " + Quoted(env.pod.temp_directory));
	}
	return steps;
}

vector<PlanStep> TresorSteps(const AgentEnvironment &env) {
	vector<string> params = {"TYPE tresor", "SCOPE " + Quoted("tresor:" + env.tresor)};
	if (env.DevIdentity()) {
		params.push_back("FLOW 'client_credentials'");
	} else {
		params.push_back("FLOW 'federated'");
	}
	params.push_back("ISSUER " + Quoted(env.issuer));
	params.push_back("CLIENT_ID " + Quoted(env.client_id));
	if (env.DevIdentity()) {
		params.push_back("PRIVATE_KEY_FILE " + Quoted(env.private_key_file));
		if (!env.key_id.empty()) {
			params.push_back("KEY_ID " + Quoted(env.key_id));
		}
	} else {
		params.push_back("ASSERTION_FILE " + Quoted(env.assertion_file));
	}
	if (!env.scope.empty()) {
		params.push_back("OAUTH_SCOPE " + Quoted(env.scope));
	}
	return {{"2 tresor", "CREATE SECRET " + string(IDENTITY_SECRET) + " (" + StringUtil::Join(params, ", ") + ")"},
	        {"2 tresor", "ATTACH " + Quoted("tresor:" + env.tresor) + " AS " + TRESOR_ALIAS + " (SECRET " +
	                         IDENTITY_SECRET + ", ACT_FOR_SESSIONS true" +
	                         (env.tresor_insecure_http ? ", INSECURE_HTTP true" : "") + ")"}};
}

string DocumentFetchSql(const AgentEnvironment &env) {
	return "SELECT content FROM read_text(" + Quoted(env.control_plane + "/v1/bootstrap") + ")";
}

//! spec 010: the orchestrated entry's document is fetched with the pod's own token, not by a statement - the plan
//! shows what is asked
string DocumentFetchNote(const AgentEnvironment &env) {
	return "-- GET " + env.orchestrator + "/v1/bootstrap with Authorization: Bearer <the content of " +
	       env.orchestrator_token_file + ">";
}

//! spec 010: what to set where an extension a secret type needs is the catalog's
const char *CATALOG_TYPE_NOTE = "<the secret's type>";

vector<PlanStep> RepositoryAndCatalogSteps(const BootstrapDocument &doc) {
	vector<PlanStep> steps;
	// OR REPLACE: a restarted container keeps its emptyDir, and every start reads the keys again (rotation is the
	// discovery document's, design/017 item 15)
	steps.push_back({"4 repository", "CREATE OR REPLACE EXTENSION REPOSITORY " + Ident(doc.repository_name) +
	                                     " WITH PREFIX " + Quoted(doc.repository_prefix)});
	if (doc.catalog_type.empty()) {
		// spec 010 (version 2): the reserved secret's own type is the catalog's - read from tresor's descriptor at the
		// run, and its extension installed from the repository when the build does not link it
		steps.push_back({"5 policy catalog", "-- the catalog's type: the type of secret " + Quoted(doc.catalog_secret) +
		                                         " in duckdb_secrets(), its extension from " +
		                                         Ident(doc.repository_name)});
	} else if (!doc.catalog_extension.empty()) {
		steps.push_back(
		    {"5 policy catalog", "INSTALL " + Ident(doc.catalog_extension) + " FROM " + Ident(doc.repository_name)});
		steps.push_back(
		    {"5 policy catalog", "LOAD " + Ident(doc.catalog_extension) + " FROM " + Ident(doc.repository_name)});
	}
	steps.push_back({"5 policy catalog", "ATTACH " + Quoted(doc.catalog_path) + " AS " + POLICY_ALIAS + " (TYPE " +
	                                         (doc.catalog_type.empty() ? string(CATALOG_TYPE_NOTE) : doc.catalog_type) +
	                                         ", SECRET " + Ident(doc.catalog_secret) + ")"});
	if (!doc.node_group.empty()) {
		steps.push_back({"5 policy catalog", "SET GLOBAL acl_node_group = " + Quoted(doc.node_group)});
	}
	if (!doc.node_label.empty()) {
		// duckdb-acl spec 134: the name the node's sessions carry, for the front
		steps.push_back({"5 policy catalog", "SET GLOBAL acl_node_label = " + Quoted(doc.node_label)});
	}
	return steps;
}

//! the policy opened from the attached catalog; `init` (spec 015) creates its schema in an empty database - only
//! there: init never migrates, and a catalog that has its schema is opened as any node opens it
PlanStep UseCatalogStep(const BootstrapDocument &doc, bool init) {
	return {"5 policy catalog", "SELECT acl_use_db(" + Quoted(POLICY_ALIAS) + ", " + Quoted(doc.catalog_schema) +
	                                (init ? ", true" : "") + ")"};
}

//! spec 015: what decides whether the catalog's schema is created - shown by the plan, read by the run
string CatalogSchemaSql(const BootstrapDocument &doc) {
	return "SELECT count(*) FROM duckdb_tables() WHERE database_name = " + Quoted(POLICY_ALIAS) +
	       " AND schema_name = " + Quoted(doc.catalog_schema) + " AND table_name = 'meta'";
}

//! spec 017: the canary migrates the catalog to its build's schema before it opens it (acl's own steps, one
//! transaction; a catalog at the build's version answers "already")
PlanStep MigrateCatalogStep(const BootstrapDocument &doc) {
	return {"5 policy catalog",
	        "SELECT acl_migrate_catalog(" + Quoted(POLICY_ALIAS) + ", " + Quoted(doc.catalog_schema) + ")"};
}

const char *FIRST_ADMIN_ROLE = "platform_admin";
const char *FIRST_POLICY_STEP = "5 first policy";

//! spec 015: what decides whether the first policy is written - shown by the plan, read by the run
string FirstPolicyGateNote(const BootstrapDocument &doc) {
	return "-- only while the catalog lists no role mapping (acl_role_mappings()) and no issuer but " +
	       Ident(doc.first_policy.issuer_name) + " (acl_issuers()); a resource group it holds is kept as it is";
}

//! spec 015: the first policy's statements, the anonymous hatch open for them alone. Each one is IF NOT EXISTS or an
//! upsert of the same value, so a write that stopped part-way is completed by the next start; the mappings are last:
//! the gate closes only when the whole policy is in. A group the catalog already holds is not re-created (a re-create
//! replaces its limits)
vector<PlanStep> FirstPolicySteps(const BootstrapDocument &doc, const std::set<string> &held_groups) {
	auto &policy = doc.first_policy;
	vector<PlanStep> steps;
	auto add = [&](const string &sql) {
		steps.push_back({FIRST_POLICY_STEP, sql});
	};
	auto quoted_list = [](const vector<string> &values) {
		vector<string> out;
		for (auto &value : values) {
			out.push_back(Quoted(value));
		}
		return "(" + StringUtil::Join(out, ", ") + ")";
	};
	add("SET GLOBAL acl_allow_anonymous_admin = true");
	add("ACL ADMIN CREATE ROLE IF NOT EXISTS " + string(FIRST_ADMIN_ROLE));
	add("ACL ADMIN GRANT ADMIN cluster TO ROLE " + string(FIRST_ADMIN_ROLE));
	add("ACL ADMIN GRANT ADMIN policy TO ROLE " + string(FIRST_ADMIN_ROLE));
	// the owner (2026-10-20): the platform's administrator hands out administration and holds the break-glass door
	add("ACL ADMIN GRANT ADMIN passthrough TO ROLE " + string(FIRST_ADMIN_ROLE));
	for (auto &group : policy.resource_groups) {
		if (!held_groups.count(group.name)) {
			add("ACL ADMIN CREATE RESOURCE GROUP " + Ident(group.name) + (group.is_default ? " DEFAULT" : ""));
		}
		add("ACL ADMIN GRANT RESOURCE GROUP " + Ident(group.name) + " TO ROLE " + FIRST_ADMIN_ROLE);
	}
	add("ACL ADMIN CREATE ISSUER IF NOT EXISTS " + Ident(policy.issuer_name) + " URL " + Quoted(policy.issuer_url));
	// a token counts only when the application asked for it (AZP); its roles are the mapped values of the claim
	for (auto &client : policy.clients) {
		add("ACL ADMIN CREATE CLIENT IF NOT EXISTS " + Ident(client.name) + " ISSUER " + Ident(policy.issuer_name) +
		    " AUDIENCES " + quoted_list(policy.audiences) + " AZP (" + Quoted(client.client_id) + ") ROLES FROM (" +
		    Quoted(policy.roles_claim) + ")");
	}
	// a role holding an admin scope is reached only through a client's own mapping (duckdb-acl spec 095)
	for (auto &client : policy.clients) {
		add("ACL ADMIN MAP CLAIM " + Quoted(policy.administrators) + " FROM CLIENT " + Ident(client.name) +
		    " TO ROLE " + FIRST_ADMIN_ROLE);
	}
	add("SET GLOBAL acl_allow_anonymous_admin = false");
	return steps;
}

//! spec 010: the engine's defaults from the pod, before the profile
vector<PlanStep> EngineSteps(const AgentEnvironment &env) {
	vector<PlanStep> steps;
	for (auto &setting : EngineDefaults(env.pod)) {
		steps.push_back({"6 engine", "SET GLOBAL " + setting.name + " = " + Quoted(setting.value)});
	}
	return steps;
}

vector<PlanStep> ServeSteps(const AgentEnvironment &env, const string &quack_token) {
	vector<PlanStep> steps;
	auto flight = env.flight_cert.empty() ? "SELECT acl_flight_serve(" + Quoted(env.flight) + ")"
	                                      : "SELECT acl_flight_serve(" + Quoted(env.flight) + ", " +
	                                            Quoted(env.flight_cert) + ", " + Quoted(env.flight_key) + ")";
	steps.push_back({"8 serve", flight});
	if (!env.quack.empty()) {
		auto quack = env.quack_cert.empty()
		                 ? "SELECT acl_quack_serve(" + Quoted(env.quack) + ", " + Quoted(quack_token) + ")"
		                 : "SELECT acl_quack_serve(" + Quoted(env.quack) + ", " + Quoted(quack_token) + ", " +
		                       Quoted(env.quack_cert) + ", " + Quoted(env.quack_key) + ")";
		steps.push_back({"8 serve", quack});
	}
	return steps;
}

//! a random value made per process for quack's server token (CSPRNG: the kernel's)
string RandomToken() {
	std::ifstream urandom("/dev/urandom", std::ios::binary);
	unsigned char bytes[32];
	if (!urandom.read(reinterpret_cast<char *>(bytes), sizeof(bytes))) {
		throw IOException("hugr_node: no random source for the quack door's server token");
	}
	static const char *hex = "0123456789abcdef";
	string out;
	for (auto b : bytes) {
		out += hex[b >> 4];
		out += hex[b & 0xF];
	}
	return out;
}

} // namespace

namespace {

AgentEnvironment WithDocument(const AgentEnvironment &env, const BootstrapDocument &document) {
	if (!env.Orchestrated()) {
		if (document.version == 2) {
			throw InvalidInputException("hugr_node: a bootstrap document of version 2 is the orchestrator's - set "
			                            "HUGR_ORCHESTRATOR, or answer version 1 from the control plane");
		}
		return env;
	}
	if (document.version != 2) {
		throw InvalidInputException("hugr_node: the orchestrator answered a bootstrap document of version %lld - an "
		                            "orchestrated node reads version 2",
		                            static_cast<long long>(document.version));
	}
	auto merged = env;
	merged.tresor = document.tresor_host;
	merged.issuer = document.tresor_issuer;
	merged.client_id = document.tresor_client_id;
	merged.scope = document.tresor_scope;
	if (merged.tresor_insecure_http) {
		RequireLoopbackTresor(merged.tresor);
	}
	return merged;
}

} // namespace

vector<PlanStep> BootstrapPlan(const AgentEnvironment &env_p, const BootstrapDocument &document,
                               const string &quack_token) {
	auto env = WithDocument(env_p, document);
	auto plan = HardeningSteps(env);
	if (env.Orchestrated()) {
		// spec 010: the orchestrator first, with the pod's own token; tresor from the document
		plan.push_back({"2 bootstrap document", DocumentFetchNote(env)});
		for (auto &step : TresorSteps(env)) {
			step.step = "3 tresor";
			plan.push_back(step);
		}
	} else {
		for (auto &step : TresorSteps(env)) {
			plan.push_back(step);
		}
		plan.push_back({"3 bootstrap document", DocumentFetchSql(env)});
	}
	for (auto &step : RepositoryAndCatalogSteps(document)) {
		plan.push_back(step);
	}
	if (document.has_first_policy) {
		plan.push_back({"5 policy catalog", "-- init only while the database holds no schema: " +
		                                        CatalogSchemaSql(document) + " answers 0"});
	}
	if (document.catalog_migrate) {
		plan.push_back({"5 policy catalog",
		                "-- migrate only a catalog that has its schema: " + CatalogSchemaSql(document) + " answers 1"});
		plan.push_back(MigrateCatalogStep(document));
	}
	plan.push_back(UseCatalogStep(document, document.has_first_policy));
	if (document.has_first_policy) {
		plan.push_back({FIRST_POLICY_STEP, FirstPolicyGateNote(document)});
		for (auto &step : FirstPolicySteps(document, std::set<string>())) {
			plan.push_back(step);
		}
	}
	for (auto &step : EngineSteps(env)) {
		plan.push_back(step);
	}
	plan.push_back(
	    {"6 profile", "-- each item of acl_cluster_effective() in its order: hugr_node_profile_statements()"});
	plan.push_back({"7 lock", "SET GLOBAL lock_configuration = true"});
	plan.push_back({"8 serve", "SELECT acl_cluster_applied(<the profile version read in step 6>)"});
	for (auto &step : ServeSteps(env, quack_token)) {
		plan.push_back(step);
	}
	return plan;
}

//===--------------------------------------------------------------------===//
// The SQL surface
//===--------------------------------------------------------------------===//

namespace {

struct AgentInfo : public TableFunctionInfo {
	explicit AgentInfo(shared_ptr<acl::PolicyStore> store_p) : store(std::move(store_p)) {
	}
	shared_ptr<acl::PolicyStore> store;
};

//! rows of (step, sql) - the plan and the profile's statements share the shape
struct StepsBind : public TableFunctionData {
	vector<PlanStep> steps;
};

struct StepsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

void StepsColumns(vector<LogicalType> &types, vector<Identifier> &names) {
	names = {Identifier("step"), Identifier("sql")};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
}

unique_ptr<GlobalTableFunctionState> StepsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<StepsState>();
}

void StepsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<StepsBind>();
	auto &state = data.global_state->Cast<StepsState>();
	idx_t count = 0;
	while (state.next < bind.steps.size() && count < STANDARD_VECTOR_SIZE) {
		auto &step = bind.steps[state.next++];
		output.data[0].SetValue(count, Value(step.step));
		output.data[1].SetValue(count, Value(step.sql));
		count++;
	}
	output.SetChildCardinality(count);
}

//! hugr_node_bootstrap_plan(document_json [, environment := json]): the bootstrap's statements, nothing run
unique_ptr<FunctionData> PlanBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                  vector<Identifier> &names) {
	StepsColumns(types, names);
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw InvalidInputException("hugr_node_bootstrap_plan: pass the bootstrap document (JSON)");
	}
	AgentEnvironment env;
	auto named = input.named_parameters.find("environment");
	if (named != input.named_parameters.end() && !named->second.IsNull()) {
		env = AgentEnvironment::FromJson(named->second.ToString());
	} else {
		env = AgentEnvironment::FromProcess();
	}
	env.Validate();
	auto document = BootstrapDocument::Parse(input.inputs[0].ToString());
	auto bind = make_uniq<StepsBind>();
	bind->steps = BootstrapPlan(env, document, "<random per process>");
	return std::move(bind);
}

//! a setting item's value as written in its spec
string SettingValueOf(const acl::PolicyStore::ClusterItem &item) {
	JsonDoc doc(item.spec, "a profile item's spec");
	return StrField(doc.Root(), "value");
}

//! spec 010: one effective item's statements, an engine setting cut to the pod's ceiling (`cut` says whether it was)
vector<string> ProfileItemStatements(const acl::PolicyStore::ClusterItem &item, const PodResources &pod, bool &cut) {
	cut = false;
	auto statements = acl::ClusterItemApplyStatements(item);
	if (item.kind != "setting") {
		return statements;
	}
	auto quantity = EngineSettingOf(item.name);
	auto ceiling = EngineCeiling(quantity, pod);
	if (quantity == EngineQuantity::NONE || ceiling == 0) {
		return statements;
	}
	auto kept = ClampToCeiling(RequestedAmount(quantity, SettingValueOf(item)), ceiling, cut);
	if (!cut) {
		return statements;
	}
	return {"SET GLOBAL " + Ident(item.name) + " = " + Quoted(EngineValueOf(quantity, kept))};
}

//! hugr_node_profile_statements([environment := json]): what applies this node's effective profile, in its order -
//! an engine setting cut to the pod the environment describes (spec 010)
unique_ptr<FunctionData> ProfileBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                     vector<Identifier> &names) {
	StepsColumns(types, names);
	auto &store = input.info->Cast<AgentInfo>().store;
	AgentEnvironment env;
	auto named = input.named_parameters.find("environment");
	if (named != input.named_parameters.end() && !named->second.IsNull()) {
		env = AgentEnvironment::FromJson(named->second.ToString());
	} else {
		env = AgentEnvironment::FromProcess();
	}
	auto bind = make_uniq<StepsBind>();
	for (auto &item : store->ClusterEffective()) {
		bool cut = false;
		for (auto &sql : ProfileItemStatements(item, env.pod, cut)) {
			bind->steps.push_back(
			    {"6 profile: " + item.kind + " " + item.name + (cut ? " (cut to the pod)" : ""), sql});
		}
	}
	return std::move(bind);
}

void Log(const string &line) {
	fprintf(stderr, "hugr_node: %s\n", line.c_str());
	fflush(stderr);
}

//! one bootstrap statement on its own connection; a failure names its step. The log names the step, never the
//! statement's values (they are addresses and file paths, but a log line has no business carrying them); a read that
//! decides a step (`logged` false) is not one of its statements
unique_ptr<QueryResult> RunStep(DatabaseInstance &db, const string &step, const string &sql, bool logged = true) {
	Connection con(db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("hugr_node: bootstrap step \"%s\" failed: %s", step, result->GetError());
	}
	if (logged && step != "stop") {
		Log("step " + step + ": ok");
	}
	return result;
}

int64_t CountOf(DatabaseInstance &db, const string &step, const string &sql) {
	auto result = RunStep(db, step, sql, false);
	acl::ResultRows rows(*result);
	return rows.Count() == 0 ? 0 : rows.GetValue(0, 0).GetValue<int64_t>();
}

//! spec 015: the first policy, into a catalog nobody administers yet. A mapping is what lets a token's holder in, and
//! the first policy writes its mappings last: a catalog without one, whose only issuer is the first policy's, has
//! never been administered through it - anything else is left alone, so a later edit is never undone
void WriteFirstPolicy(DatabaseInstance &db, const BootstrapDocument &document) {
	auto mappings = CountOf(db, FIRST_POLICY_STEP, "SELECT count(*) FROM acl_role_mappings()");
	auto other_issuers =
	    CountOf(db, FIRST_POLICY_STEP,
	            "SELECT count(*) FROM acl_issuers() WHERE name <> " + Quoted(document.first_policy.issuer_name));
	if (mappings > 0 || other_issuers > 0) {
		Log("step 5 first policy: the catalog holds its policy - nothing written");
		return;
	}
	std::set<string> held_groups;
	auto groups = RunStep(db, FIRST_POLICY_STEP, "SELECT \"group\" FROM acl_resource_groups()", false);
	acl::ResultRows group_rows(*groups);
	for (idx_t row = 0; row < group_rows.Count(); row++) {
		held_groups.insert(group_rows.GetValue(0, row).ToString());
	}
	try {
		for (auto &step : FirstPolicySteps(document, held_groups)) {
			RunStep(db, step.step, step.sql);
		}
	} catch (std::exception &) {
		// the hatch never stays open past these statements, whatever failed (the bootstrap stops here)
		Connection con(db);
		con.Query("SET GLOBAL acl_allow_anonymous_admin = false");
		throw;
	}
	Log("step 5 first policy: written - issuer " + document.first_policy.issuer_name + ", " +
	    std::to_string(document.first_policy.clients.size()) + " client(s), " +
	    std::to_string(document.first_policy.resource_groups.size()) + " resource group(s)");
}

int64_t LiveSessions(DatabaseInstance &db) {
	auto result = RunStep(db, "stop", "SELECT acl_drain()");
	acl::ResultRows rows(*result);
	return rows.Count() == 0 ? 0 : rows.GetValue(0, 0).GetValue<int64_t>();
}

//! drain, wait for the sessions or the deadline, end the rest, stop the doors; the sessions still live when the doors
//! stopped
int64_t Stop(DatabaseInstance &db, const AgentEnvironment &env) {
	using namespace duckdb_yyjson; // NOLINT
	Log("stopping: draining");
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(env.drain_deadline);
	g_stop_signal.store(0); // a second SIGTERM during the wait skips straight to stopping the doors
	auto live = LiveSessions(db);
	while (live > 0 && std::chrono::steady_clock::now() < deadline && g_stop_signal.load() == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		live = LiveSessions(db);
	}
	int64_t killed = 0;
	if (live > 0) {
		auto listing = RunStep(db, "stop", "SELECT acl_sessions()");
		acl::ResultRows rows(*listing);
		if (rows.Count() > 0) {
			auto text = rows.GetValue(0, 0).ToString();
			auto doc = yyjson_read(text.c_str(), text.size(), 0);
			if (doc) {
				auto root = yyjson_doc_get_root(doc);
				size_t idx, max;
				yyjson_val *session;
				yyjson_arr_foreach(root, idx, max, session) {
					auto id = StrField(session, "id");
					if (!id.empty()) {
						RunStep(db, "stop", "SELECT acl_session_kill(" + Quoted(id) + ")");
						killed++;
					}
				}
				yyjson_doc_free(doc);
			}
		}
		Log("ended " + std::to_string(killed) + " session(s) at the deadline");
	}
	RunStep(db, "stop", "SELECT acl_flight_stop(" + Quoted(env.flight) + ")");
	if (!env.quack.empty()) {
		RunStep(db, "stop", "SELECT acl_quack_stop(" + Quoted(env.quack) + ")");
	}
	Log("stopped");
	return live;
}

//===--------------------------------------------------------------------===//
// spec 010: the orchestrator, the catalog's type, the tick, the heartbeat
//===--------------------------------------------------------------------===//

//! the projected token, read at each call (kubelet renews it); never kept, never logged. A token is one line of
//! printable ASCII (a JWT) of at most 64 KiB - anything else is refused before it reaches a header
string ReadTokenFile(const string &path) {
	static constexpr idx_t MAX_TOKEN = 64 * 1024;
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("hugr_node: the orchestrator's token file cannot be read (HUGR_ORCHESTRATOR_TOKEN_FILE)");
	}
	string token;
	char buffer[4096];
	while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
		token.append(buffer, static_cast<size_t>(in.gcount()));
		if (token.size() > MAX_TOKEN) {
			throw IOException("hugr_node: the orchestrator's token file is larger than a token "
			                  "(HUGR_ORCHESTRATOR_TOKEN_FILE)");
		}
	}
	StringUtil::Trim(token);
	if (token.empty() || std::any_of(token.begin(), token.end(), [](char c) { return c <= ' ' || c > '~'; })) {
		throw IOException("hugr_node: the orchestrator's token file holds no token (HUGR_ORCHESTRATOR_TOKEN_FILE)");
	}
	return token;
}

//! an orchestrator's answer: its status (0 = none) and its body
struct OrchestratorAnswer {
	int status = 0;
	string body;
};

//! one request to the orchestrator through duckdb's HTTP client (so ca_cert_file applies), with the pod's token as the
//! bearer, never redirected; the server is verified by the hardening (enable_server_cert_verification, and neither
//! the client nor its verification is a profile item). `quick` (the heartbeat): one attempt, 5 s - a stalled
//! orchestrator never holds the tick, nor the SIGTERM behind it
OrchestratorAnswer OrchestratorRequest(DatabaseInstance &db, const AgentEnvironment &env, const string &path,
                                       const string *body, bool quick) {
	auto url = env.orchestrator + path;
	auto &http = HTTPUtil::Get(db);
	auto params = http.InitializeParameters(db, url);
	// the bearer is the pod's own token: a redirect would carry it to wherever the answer points
	params->follow_location = false;
	if (quick) {
		params->timeout = 5;
		params->timeout_usec = 0;
		params->retries = 0;
	}
	HTTPHeaders headers;
	headers.Insert("Authorization", "Bearer " + ReadTokenFile(env.orchestrator_token_file));
	unique_ptr<HTTPResponse> response;
	OrchestratorAnswer answer;
	if (body) {
		headers.Insert("Content-Type", "application/json");
		PostRequestInfo request(url, headers, *params, const_data_ptr_cast(body->data()), body->size());
		response = http.Request(request);
		answer.body = request.buffer_out;
	} else {
		GetRequestInfo request(
		    url, headers, *params, [](const HTTPResponse &) { return true; },
		    [&](const_data_ptr_t data, idx_t length) {
			    answer.body.append(const_char_ptr_cast(data), length);
			    return true;
		    });
		response = http.Request(request);
	}
	if (!response) {
		return OrchestratorAnswer();
	}
	answer.status = static_cast<int>(response->status);
	if (answer.body.empty()) {
		answer.body = response->body;
	}
	return answer;
}

//! the body of a 2xx; throws the status otherwise (never the token, never the body)
string OrchestratorCall(DatabaseInstance &db, const AgentEnvironment &env, const string &path, const string *body) {
	auto answer = OrchestratorRequest(db, env, path, body, false);
	if (answer.status == 0) {
		throw IOException("hugr_node: the orchestrator did not answer %s", path);
	}
	if (answer.status < 200 || answer.status > 299) {
		throw IOException("hugr_node: the orchestrator answered %s with %d", path, answer.status);
	}
	return answer.body;
}

//! spec 010 (version 2): the catalog's type is its reserved secret's - tresor lists a descriptor with its type before
//! anything is attached (tresor spec 004: duckdb_secrets() shows descriptors only)
void ResolveCatalogType(DatabaseInstance &db, BootstrapDocument &document) {
	static const case_insensitive_set_t CATALOG_TYPES = {"postgres", "mysql", "mssql"};
	auto result = RunStep(db, "5 policy catalog",
	                      "SELECT type FROM duckdb_secrets() WHERE name = " + Quoted(document.catalog_secret));
	acl::ResultRows rows(*result);
	if (rows.Count() == 0) {
		throw InvalidInputException("hugr_node: bootstrap step \"5 policy catalog\" failed: the secret \"%s\" is not "
		                            "one the node may use",
		                            document.catalog_secret);
	}
	auto type = rows.GetValue(0, 0).ToString();
	if (!CATALOG_TYPES.count(type)) {
		throw InvalidInputException("hugr_node: bootstrap step \"5 policy catalog\" failed: the secret \"%s\" is of "
		                            "type \"%s\", which is not a policy catalog's (postgres, mysql, mssql)",
		                            document.catalog_secret, type);
	}
	document.catalog_type = StringUtil::Lower(type);
	auto extension = acl::ClusterExtensionOfType(document.catalog_type);
	// a build that links the type's extension has it loaded already: INSTALL from the repository only otherwise
	if (!extension.empty()) {
		auto loaded =
		    RunStep(db, "5 policy catalog",
		            "SELECT count(*) FROM duckdb_extensions() WHERE loaded AND extension_name = " + Quoted(extension));
		acl::ResultRows loaded_rows(*loaded);
		if (loaded_rows.Count() == 0 || loaded_rows.GetValue(0, 0).GetValue<int64_t>() == 0) {
			document.catalog_extension = extension;
		}
	}
}

//! the tick's own memory between ticks
struct TickState {
	int64_t applied = -1;
	TickTaken taken; // what this node took from the profile, with the spec it applied
	bool self_drained = false;
	std::vector<TickPending> pending;
	bool needs_restart = false;
	std::map<string, int64_t> drift_counts;
};

//! an item as the tick reads it: an extension by its canonical name (`postgres` is loaded as `postgres_scanner`)
TickItem TickItemOf(const acl::PolicyStore::ClusterItem &item) {
	auto name = item.kind == "extension" ? ExtensionHelper::ApplyExtensionAlias(item.name) : item.name;
	return {item.kind, name, item.spec};
}

TickKey TickKeyOf(const TickItem &item) {
	return {item.kind, TickLower(item.name)};
}

//! spec 010: the drift as the tick reads it - an engine setting the node holds at the pod's ceiling is "cut", not
//! pending: the profile asked for more than the pod has, and the node runs what the pod allows
vector<TickDrift> TickDriftOf(const vector<acl::PolicyStore::DriftRow> &rows, const PodResources &pod) {
	vector<TickDrift> out;
	for (auto &row : rows) {
		auto state = row.state;
		auto quantity = row.kind == "setting" ? EngineSettingOf(row.name) : EngineQuantity::NONE;
		auto ceiling = EngineCeiling(quantity, pod);
		if (state == "differs" && quantity != EngineQuantity::NONE && ceiling > 0) {
			bool cut = false;
			auto kept = ClampToCeiling(RequestedAmount(quantity, row.profile_value), ceiling, cut);
			bool holds;
			if (quantity == EngineQuantity::THREADS) {
				holds = row.node_value == std::to_string(kept);
			} else {
				// the node holds what the bootstrap set - the ceiling in whole MiB - and duckdb shows it rounded
				// (3.5 GiB): the same rendering of the same amount is the same amount
				auto set = kept / MEBIBYTE * MEBIBYTE;
				holds = row.node_value == StringUtil::BytesToHumanReadableString(set);
			}
			if (cut && holds) {
				state = "cut";
			}
		}
		out.push_back({row.kind, row.name, state});
	}
	return out;
}

//! the class of a failed statement's error - never its text, which may carry a path or a DSN
string ErrorClassOf(QueryResult &result) {
	return Exception::ExceptionTypeToString(result.GetErrorType());
}

string ErrorClassOf(std::exception &ex) {
	return Exception::ExceptionTypeToString(ErrorData(ex).Type());
}

//! one of the tick's own statements: no "step ok" line every interval, the error's class only when it fails
unique_ptr<QueryResult> TickQuery(DatabaseInstance &db, const string &sql) {
	Connection con(db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("hugr_node: a tick statement failed (%s)", ErrorClassOf(*result));
	}
	return result;
}

//! one hot item's statements; '' when they hold, else the error's class
string ApplyHot(DatabaseInstance &db, const acl::PolicyStore::ClusterItem &item, const PodResources &pod) {
	try {
		bool cut = false;
		for (auto &sql : ProfileItemStatements(item, pod, cut)) {
			Connection con(db);
			auto result = con.Query(sql);
			if (result->HasError()) {
				return ErrorClassOf(*result);
			}
		}
	} catch (std::exception &ex) {
		return ErrorClassOf(ex);
	}
	return string();
}

//! one convergence tick: the hot items applied, the rest pending, the version recorded when every hot item holds
void Converge(DatabaseInstance &db, acl::PolicyStore &store, const AgentEnvironment &env, TickState &state) {
	auto version = store.ClusterVersion();
	auto items = store.ClusterEffective();
	auto drift = TickDriftOf(store.ClusterDrift(), env.pod);
	vector<TickItem> tick_items;
	for (auto &item : items) {
		tick_items.push_back(TickItemOf(item));
	}
	std::set<TickKey> applied_here;
	for (auto &note : acl::TakeAppliedHere(db)) {
		auto name = note.first == "extension" ? ExtensionHelper::ApplyExtensionAlias(note.second) : note.second;
		applied_here.insert({note.first, TickLower(name)});
	}
	auto decision = DecideTick(tick_items, drift, state.taken, applied_here);
	for (auto index : decision.adopt) {
		state.taken[TickKeyOf(tick_items[index])] = tick_items[index].spec;
	}
	for (auto &key : decision.forget) {
		state.taken.erase(key);
	}
	bool hot_failed = false;
	for (auto index : decision.apply) {
		auto &item = items[index];
		auto error = ApplyHot(db, item, env.pod);
		if (!error.empty()) {
			// listed, and the rest still apply
			decision.pending.push_back({item.kind, item.name, "hot", error});
			hot_failed = true;
			Log("tick: " + item.kind + " " + item.name + " could not be applied (" + error + ")");
			continue;
		}
		state.taken[TickKeyOf(tick_items[index])] = tick_items[index].spec;
		Log("tick: " + item.kind + " " + item.name + " applied (profile version " + std::to_string(version) + ")");
	}
	for (auto &alias : decision.detach) {
		Connection con(db);
		auto result = con.Query("DETACH " + Ident(alias));
		if (result->HasError()) {
			// in use: it goes with a drain, like a re-pointed source
			decision.pending.push_back({"source", alias, "drain", ErrorClassOf(*result)});
		} else {
			state.taken.erase({"source", TickLower(alias)});
			Log("tick: source " + alias + " detached (profile version " + std::to_string(version) + ")");
		}
	}
	if (!hot_failed && version != state.applied) {
		TickQuery(db, "SELECT acl_cluster_applied(" + std::to_string(version) + ")");
		state.applied = version;
	}
	state.drift_counts.clear();
	for (auto &row : drift) {
		state.drift_counts[row.state]++;
	}
	state.pending = decision.pending;
	state.needs_restart = false;
	for (auto &pending : state.pending) {
		state.needs_restart = state.needs_restart || pending.item_class != "hot";
	}
}

//! spec 010: a node whose resource group the policy lacks refuses new sessions already (duckdb-acl spec 096); the tick
//! says so by draining it - and resumes only a drain it made itself, never one an operator ordered (DecideGroup)
void WatchGroup(DatabaseInstance &db, acl::PolicyStore &store, TickState &state) {
	auto has_group = !store.NodeGroup().empty();
	auto decision = DecideGroup(has_group, has_group && store.NodeGroupKnown(), store.Draining(), state.self_drained);
	if (decision.drain) {
		TickQuery(db, "SELECT acl_drain()");
		Log("tick: the node's resource group is not in the policy - draining");
	}
	if (decision.resume) {
		TickQuery(db, "SELECT acl_resume()");
		Log("tick: the node's resource group is back - resumed");
	}
	state.self_drained = decision.self_drained;
}

//! spec 017: the catalog as this build judges it (acl_catalog_schema(), its fields by name - `catalog` is reported as
//! `schema`), its type, and the extension the node installed from the repository for it ('' when its build links it)
string CatalogStateJson(acl::PolicyStore &store, const BootstrapDocument &document) {
	string out = "{\"type\":" + acl::JsonQuote(document.catalog_type) +
	             ",\"extension\":" + acl::JsonQuote(document.catalog_extension);
	try {
		auto state = store.CatalogSchemaState();
		auto &children = StructValue::GetChildren(state);
		for (idx_t i = 0; i < children.size(); i++) {
			auto name = StructType::GetChildName(state.type(), i).GetIdentifierName();
			auto &v = children[i];
			string json;
			if (v.IsNull()) {
				json = "null";
			} else if (v.type().id() == LogicalTypeId::BIGINT) {
				json = std::to_string(v.GetValue<int64_t>());
			} else {
				json = acl::JsonQuote(v.ToString());
			}
			out += "," + acl::JsonQuote(name == "catalog" ? string("schema") : name) + ":" + json;
		}
	} catch (std::exception &) { // NOLINT: the report says what it can
	}
	return out + "}";
}

//! the heartbeat's body: hugr-platform spec 001's contract, and spec 017's build and catalog
string HeartbeatBody(DatabaseInstance &db, acl::PolicyStore &store, const BootstrapDocument &document,
                     const TickState &state) {
	string pending = "[";
	for (idx_t i = 0; i < state.pending.size(); i++) {
		auto &p = state.pending[i];
		pending += (i ? "," : "") + string("{\"kind\":") + acl::JsonQuote(p.kind) +
		           ",\"name\":" + acl::JsonQuote(p.name) + ",\"class\":" + acl::JsonQuote(p.item_class) +
		           ",\"error\":" + acl::JsonQuote(p.error) + "}";
	}
	pending += "]";
	string drift = "{";
	idx_t n = 0;
	for (auto &count : state.drift_counts) {
		drift += (n++ ? "," : "") + acl::JsonQuote(count.first) + ":" + std::to_string(count.second);
	}
	drift += "}";
	auto load = TickQuery(db, "SELECT acl_node_load()");
	acl::ResultRows load_rows(*load);
	auto load_json = load_rows.Count() == 0 ? string("null") : load_rows.GetValue(0, 0).ToString();
	int64_t target = -1;
	try {
		target = store.ClusterVersion();
	} catch (std::exception &) { // NOLINT: the report says what it can
	}
	return "{\"node_id\":" + acl::JsonQuote(document.node_label) +
	       ",\"image\":" + acl::JsonQuote(HugrNodeExtension().Version()) +
	       ",\"duckdb\":" + acl::JsonQuote(DuckDB::SourceID()) +
	       ",\"duckdb_version\":" + acl::JsonQuote(DuckDB::LibraryVersion()) +
	       ",\"platform\":" + acl::JsonQuote(DuckDB::Platform()) + ",\"catalog\":" + CatalogStateJson(store, document) +
	       ",\"config\":{\"target\":" + std::to_string(target) + ",\"applied\":" + std::to_string(state.applied) +
	       ",\"needs_restart\":" + (state.needs_restart ? "true" : "false") + ",\"pending\":" + pending +
	       "},\"drift\":" + drift + ",\"load\":" + load_json + "}";
}

//! one tick: converge, watch the group, beat - each part on its own, so that one failing does not stop the others
void Tick(DatabaseInstance &db, acl::PolicyStore &store, const AgentEnvironment &env, const BootstrapDocument &document,
          TickState &state) {
	try {
		Converge(db, store, env, state);
	} catch (std::exception &ex) {
		Log("tick: convergence failed (" + ErrorClassOf(ex) + ") - the next tick tries again");
	}
	try {
		WatchGroup(db, store, state);
	} catch (std::exception &ex) {
		Log("tick: the resource group could not be checked (" + ErrorClassOf(ex) + ")");
	}
	if (!env.Orchestrated()) {
		return;
	}
	try {
		auto body = HeartbeatBody(db, store, document, state);
		auto answer = OrchestratorRequest(db, env, "/v1/heartbeat", &body, true);
		if (answer.status < 200 || answer.status > 299) {
			// the status is the orchestrator's own (a 409: another pod beats as this node), never a value of ours
			Log("tick: the heartbeat was answered " + std::to_string(answer.status) + " - the next tick sends again");
		}
	} catch (std::exception &ex) {
		Log("tick: the heartbeat failed (" + ErrorClassOf(ex) + ") - the next tick sends again");
	}
}

struct AgentPragmaInfo : public PragmaFunctionInfo {
	explicit AgentPragmaInfo(shared_ptr<acl::PolicyStore> store_p) : store(std::move(store_p)) {
	}
	shared_ptr<acl::PolicyStore> store;
};

//! PRAGMA hugr_node_run: a pragma that returns a query runs in the statement preprocessor, on the calling connection's
//! own thread - never on a scheduler worker. That matters: a thread-count change (`SET threads`, a profile item)
//! joins every worker, and a node blocked on a worker for its whole life would be one of them (found in CI as
//! "Resource deadlock avoided" when the profile set `threads`). Answers a one-row query: why the node stopped and the
//! sessions still live when the doors stopped.
string RunPragma(ClientContext &context, const FunctionParameters &parameters) {
	// once per process, whatever happens next: a bootstrap that failed half-way is not retried in place (the CLI
	// exits non-zero and the pod restarts), and a node that ran is not started again
	if (g_running.exchange(true)) {
		throw InvalidInputException("hugr_node_run: runs once per process - the previous run ended or failed; start a "
		                            "new process");
	}
	auto &store = parameters.function->function_info->Cast<AgentPragmaInfo>().store;
	auto &db = *context.db;
	auto env = AgentEnvironment::FromProcess();
	env.Validate();
	// spec 010: a pod has no more cores than its machine - more is a Downward API divisor other than 1 (millicores)
	auto cores = std::thread::hardware_concurrency();
	if (env.pod.cpu > 0 && cores > 0 && env.pod.cpu > cores) {
		throw InvalidInputException(
		    "hugr_node: HUGR_POD_CPU says more cores than this machine has (%llu) - the Downward "
		    "API's requests.cpu is read with divisor 1",
		    static_cast<unsigned long long>(cores));
	}
	if (env.DevIdentity()) {
		Log("WARNING: the node runs on a DEVELOPMENT identity (HUGR_IDENTITY_PRIVATE_KEY_FILE), never in production");
	}
	if (env.tresor_insecure_http) {
		Log("WARNING: tresor over plain http on loopback (HUGR_TRESOR_INSECURE_HTTP) - development only, never in "
		    "production");
	}
	auto quack_token = env.quack.empty() ? string() : RandomToken();

	for (auto &step : HardeningSteps(env)) {
		RunStep(db, step.step, step.sql);
	}
	BootstrapDocument document;
	if (env.Orchestrated()) {
		// spec 010: the orchestrator first, with the pod's own token; tresor and the node's identity from its answer
		string text;
		try {
			text = OrchestratorCall(db, env, "/v1/bootstrap", nullptr);
		} catch (std::exception &ex) {
			throw InvalidInputException("hugr_node: bootstrap step \"2 bootstrap document\" failed: %s",
			                            ErrorData(ex).RawMessage());
		}
		Log("step 2 bootstrap document: ok");
		document = BootstrapDocument::Parse(text);
		env = WithDocument(env, document);
		for (auto &step : TresorSteps(env)) {
			RunStep(db, "3 tresor", step.sql);
		}
	} else {
		for (auto &step : TresorSteps(env)) {
			RunStep(db, step.step, step.sql);
		}
		auto fetched = RunStep(db, "3 bootstrap document", DocumentFetchSql(env));
		acl::ResultRows fetched_rows(*fetched);
		if (fetched_rows.Count() == 0) {
			throw InvalidInputException("hugr_node: bootstrap step \"3 bootstrap document\" failed: the control plane "
			                            "answered nothing");
		}
		document = BootstrapDocument::Parse(fetched_rows.GetValue(0, 0).ToString());
	}
	if (document.catalog_type.empty()) {
		// the repository first: the type's extension may come from it
		auto steps = RepositoryAndCatalogSteps(document);
		RunStep(db, steps[0].step, steps[0].sql);
		ResolveCatalogType(db, document);
		steps = RepositoryAndCatalogSteps(document);
		for (idx_t i = 1; i < steps.size(); i++) {
			RunStep(db, steps[i].step, steps[i].sql);
		}
	} else {
		for (auto &step : RepositoryAndCatalogSteps(document)) {
			RunStep(db, step.step, step.sql);
		}
	}
	// read only when it decides something: the listing walks every attached catalog, tresor's included
	auto has_schema = (document.has_first_policy || document.catalog_migrate) &&
	                  CountOf(db, "5 policy catalog", CatalogSchemaSql(document)) > 0;
	auto init = document.has_first_policy && !has_schema;
	if (document.catalog_migrate && has_schema) {
		auto migrate = MigrateCatalogStep(document);
		auto answer = RunStep(db, migrate.step, migrate.sql, false);
		acl::ResultRows answer_rows(*answer);
		Log("step 5 policy catalog: migrate - " +
		    (answer_rows.Count() == 0 ? string("no answer") : answer_rows.GetValue(0, 0).ToString()));
	}
	auto use = UseCatalogStep(document, init);
	RunStep(db, use.step, use.sql);
	if (document.has_first_policy) {
		WriteFirstPolicy(db, document);
	}
	for (auto &step : EngineSteps(env)) {
		RunStep(db, step.step, step.sql);
	}
	TickState tick;
	auto version = store->ClusterVersion();
	for (auto &item : store->ClusterEffective()) {
		bool cut = false;
		for (auto &sql : ProfileItemStatements(item, env.pod, cut)) {
			RunStep(db, "6 profile: " + item.kind + " " + item.name, sql);
		}
		if (cut) {
			Log("step 6 profile: " + item.name + " cut to the pod's ceiling");
		}
		auto tick_item = TickItemOf(item);
		tick.taken[TickKeyOf(tick_item)] = tick_item.spec;
	}
	RunStep(db, "7 lock", "SET GLOBAL lock_configuration = true");
	RunStep(db, "8 serve", "SELECT acl_cluster_applied(" + std::to_string(version) + ")");
	tick.applied = version;

	// the doors open last: a TCP probe on the Flight port is the readiness probe
	g_stop_signal.store(0);
	auto previous_term = std::signal(SIGTERM, HugrNodeOnSignal);
	auto previous_int = std::signal(SIGINT, HugrNodeOnSignal);
	for (auto &step : ServeSteps(env, quack_token)) {
		RunStep(db, step.step, step.sql);
	}
	Log("ready - profile version " + std::to_string(version) + ", serving " + env.flight);
	// spec 010: the tick, on this thread (never a worker); the first one right away, so the orchestrator hears of the
	// node as soon as it serves
	auto interval = std::chrono::seconds(document.heartbeat_interval);
	auto next_tick = std::chrono::steady_clock::now();
	while (g_stop_signal.load() == 0) {
		if (std::chrono::steady_clock::now() >= next_tick) {
			Tick(db, *store, env, document, tick);
			// on the interval's own clock: a slow tick does not push every later beat back
			next_tick += interval;
			if (next_tick < std::chrono::steady_clock::now()) {
				next_tick = std::chrono::steady_clock::now();
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	auto signal_number = g_stop_signal.load();
	auto left = Stop(db, env);
	std::signal(SIGTERM, previous_term);
	std::signal(SIGINT, previous_int);
	return "SELECT " + Quoted(signal_number == SIGINT ? "SIGINT" : "SIGTERM") + " AS reason, " + std::to_string(left) +
	       "::BIGINT AS sessions_left";
}

} // namespace

void RegisterHugrNodeAgent(ExtensionLoader &loader, const shared_ptr<acl::PolicyStore> &store) {
	auto info = make_shared_ptr<AgentInfo>(store);

	auto run = PragmaFunction::PragmaStatement(Identifier("hugr_node_run"), RunPragma);
	run.function_info = make_shared_ptr<AgentPragmaInfo>(store);
	loader.RegisterFunction(run);

	TableFunction plan(Identifier("hugr_node_bootstrap_plan"), {LogicalType::VARCHAR}, StepsScan, PlanBind, StepsInit);
	plan.GetSignature().WithTypedKwargs("options",
	                                    [](TypedKwargs &options) { options.Add("environment", LogicalType::VARCHAR); });
	plan.function_info = info;
	loader.RegisterFunction(plan);

	TableFunction profile(Identifier("hugr_node_profile_statements"), {}, StepsScan, ProfileBind, StepsInit);
	// spec 010: the pod the statements are cut to, like the plan's
	profile.GetSignature().WithTypedKwargs(
	    "options", [](TypedKwargs &options) { options.Add("environment", LogicalType::VARCHAR); });
	profile.function_info = info;
	loader.RegisterFunction(profile);
}

} // namespace hugr_node
} // namespace duckdb
