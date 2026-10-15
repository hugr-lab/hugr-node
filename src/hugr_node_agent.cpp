// Spec 006: the node process and its bootstrap.
//
// `duckdb -c "CALL hugr_node_run()"` is a platform node's entrypoint. The function reads the local part from the
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
#include "acl_result_rows.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "yyjson.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace duckdb {
namespace hugr_node {

namespace {

//! The one run of this process and the signal that ends it. Process-wide by nature: a signal reaches the process, and
//! the node is one per process (a second hugr_node_run() is refused). The handler only stores (async-signal-safe).
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

string CutTresorScheme(const string &host) {
	return StringUtil::StartsWith(host, "tresor:") ? host.substr(7) : host;
}

string TrimSlash(string url) {
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	return url;
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
	return env;
}

void AgentEnvironment::Validate() const {
	auto require = [](const string &value, const char *name) {
		if (value.empty()) {
			throw InvalidInputException("hugr_node: %s is not set", name);
		}
	};
	require(tresor, "HUGR_TRESOR");
	require(issuer, "HUGR_IDENTITY_ISSUER");
	require(client_id, "HUGR_IDENTITY_CLIENT_ID");
	if (assertion_file.empty() == private_key_file.empty()) {
		throw InvalidInputException("hugr_node: the node's identity is HUGR_IDENTITY_ASSERTION_FILE (the projected "
		                            "service-account token) or, for development only, HUGR_IDENTITY_PRIVATE_KEY_FILE - "
		                            "exactly one of them");
	}
	require(control_plane, "HUGR_CONTROL_PLANE");
	if (!StringUtil::StartsWith(control_plane, "https://")) {
		throw InvalidInputException("hugr_node: HUGR_CONTROL_PLANE is an https URL, not \"%s\"", control_plane);
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
	if (out.version != 1) {
		throw InvalidInputException("hugr_node: the bootstrap document's version %lld is not one this node reads (1)",
		                            static_cast<long long>(out.version));
	}
	out.node_group = StrField(root, "node_group");
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
	if (!StringUtil::StartsWith(out.repository_prefix, "https://")) {
		throw InvalidInputException("hugr_node: the repository's prefix is an https URL, not \"%s\" - its signing keys "
		                            "are read from it",
		                            out.repository_prefix);
	}
	auto catalog = yyjson_obj_get(root, "catalog");
	if (!catalog || !yyjson_is_obj(catalog)) {
		throw InvalidInputException("hugr_node: the bootstrap document has no catalog");
	}
	out.catalog_type = StrField(catalog, "type");
	out.catalog_extension = StrField(catalog, "extension");
	out.catalog_path = StrField(catalog, "path");
	out.catalog_secret = StrField(catalog, "secret");
	auto schema = StrField(catalog, "schema");
	if (!schema.empty()) {
		out.catalog_schema = schema;
	}
	if (out.catalog_type.empty()) {
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
	if (!env.ca_file.empty()) {
		add("SET GLOBAL ca_cert_file = " + Quoted(env.ca_file));
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
	                         IDENTITY_SECRET + ", ACT_FOR_SESSIONS true)"}};
}

string DocumentFetchSql(const AgentEnvironment &env) {
	return "SELECT content FROM read_text(" + Quoted(env.control_plane + "/v1/bootstrap") + ")";
}

vector<PlanStep> RepositoryAndCatalogSteps(const BootstrapDocument &doc) {
	vector<PlanStep> steps;
	steps.push_back({"4 repository", "CREATE EXTENSION REPOSITORY " + Ident(doc.repository_name) + " WITH PREFIX " +
	                                     Quoted(doc.repository_prefix)});
	if (!doc.catalog_extension.empty()) {
		steps.push_back(
		    {"5 policy catalog", "INSTALL " + Ident(doc.catalog_extension) + " FROM " + Ident(doc.repository_name)});
		steps.push_back(
		    {"5 policy catalog", "LOAD " + Ident(doc.catalog_extension) + " FROM " + Ident(doc.repository_name)});
	}
	steps.push_back({"5 policy catalog", "ATTACH " + Quoted(doc.catalog_path) + " AS " + POLICY_ALIAS + " (TYPE " +
	                                         doc.catalog_type + ", SECRET " + Ident(doc.catalog_secret) + ")"});
	if (!doc.node_group.empty()) {
		steps.push_back({"5 policy catalog", "SET GLOBAL acl_node_group = " + Quoted(doc.node_group)});
	}
	steps.push_back(
	    {"5 policy catalog", "SELECT acl_use_db(" + Quoted(POLICY_ALIAS) + ", " + Quoted(doc.catalog_schema) + ")"});
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

vector<PlanStep> BootstrapPlan(const AgentEnvironment &env, const BootstrapDocument &document,
                               const string &quack_token) {
	auto plan = HardeningSteps(env);
	for (auto &step : TresorSteps(env)) {
		plan.push_back(step);
	}
	plan.push_back({"3 bootstrap document", DocumentFetchSql(env)});
	for (auto &step : RepositoryAndCatalogSteps(document)) {
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

//! hugr_node_profile_statements(): what applies this node's effective profile, in its order
unique_ptr<FunctionData> ProfileBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                     vector<Identifier> &names) {
	StepsColumns(types, names);
	auto &store = input.info->Cast<AgentInfo>().store;
	auto bind = make_uniq<StepsBind>();
	for (auto &item : store->ClusterEffective()) {
		for (auto &sql : acl::ClusterItemApplyStatements(item)) {
			bind->steps.push_back({"6 profile: " + item.kind + " " + item.name, sql});
		}
	}
	return std::move(bind);
}

//! one bootstrap statement on its own connection; a failure names its step
unique_ptr<QueryResult> RunStep(DatabaseInstance &db, const string &step, const string &sql) {
	Connection con(db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("hugr_node: bootstrap step \"%s\" failed: %s", step, result->GetError());
	}
	return result;
}

void Log(const string &line) {
	fprintf(stderr, "hugr_node: %s\n", line.c_str());
	fflush(stderr);
}

struct RunBind : public TableFunctionData {
	shared_ptr<acl::PolicyStore> store;
};

struct RunState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> RunBindFn(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                   vector<Identifier> &names) {
	names = {Identifier("reason"), Identifier("sessions_left")};
	types = {LogicalType::VARCHAR, LogicalType::BIGINT};
	auto bind = make_uniq<RunBind>();
	bind->store = input.info->Cast<AgentInfo>().store;
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> RunInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RunState>();
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

void RunScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RunState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	// once per process, whatever happens next: a bootstrap that failed half-way is not retried in place (the CLI
	// exits non-zero and the pod restarts), and a node that ran is not started again
	if (g_running.exchange(true)) {
		throw InvalidInputException("hugr_node_run: runs once per process - the previous run ended or failed; start a "
		                            "new process");
	}
	auto &bind = data.bind_data->Cast<RunBind>();
	auto &db = *context.db;
	auto env = AgentEnvironment::FromProcess();
	env.Validate();
	if (env.DevIdentity()) {
		Log("WARNING: the node runs on a DEVELOPMENT identity (HUGR_IDENTITY_PRIVATE_KEY_FILE), never in production");
	}
	auto quack_token = env.quack.empty() ? string() : RandomToken();

	for (auto &step : HardeningSteps(env)) {
		RunStep(db, step.step, step.sql);
	}
	for (auto &step : TresorSteps(env)) {
		RunStep(db, step.step, step.sql);
	}
	auto fetched = RunStep(db, "3 bootstrap document", DocumentFetchSql(env));
	acl::ResultRows fetched_rows(*fetched);
	if (fetched_rows.Count() == 0) {
		throw InvalidInputException("hugr_node: bootstrap step \"3 bootstrap document\" failed: the control plane "
		                            "answered nothing");
	}
	auto document = BootstrapDocument::Parse(fetched_rows.GetValue(0, 0).ToString());
	for (auto &step : RepositoryAndCatalogSteps(document)) {
		RunStep(db, step.step, step.sql);
	}
	auto version = bind.store->ClusterVersion();
	for (auto &item : bind.store->ClusterEffective()) {
		for (auto &sql : acl::ClusterItemApplyStatements(item)) {
			RunStep(db, "6 profile: " + item.kind + " " + item.name, sql);
		}
	}
	RunStep(db, "7 lock", "SET GLOBAL lock_configuration = true");
	RunStep(db, "8 serve", "SELECT acl_cluster_applied(" + std::to_string(version) + ")");

	// the doors open last: a TCP probe on the Flight port is the readiness probe
	g_stop_signal.store(0);
	auto previous_term = std::signal(SIGTERM, HugrNodeOnSignal);
	auto previous_int = std::signal(SIGINT, HugrNodeOnSignal);
	for (auto &step : ServeSteps(env, quack_token)) {
		RunStep(db, step.step, step.sql);
	}
	Log("ready - profile version " + std::to_string(version) + ", serving " + env.flight);
	while (g_stop_signal.load() == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	auto signal_number = g_stop_signal.load();
	auto left = Stop(db, env);
	std::signal(SIGTERM, previous_term);
	std::signal(SIGINT, previous_int);
	output.data[0].SetValue(0, Value(signal_number == SIGINT ? "SIGINT" : "SIGTERM"));
	output.data[1].SetValue(0, Value::BIGINT(left));
	output.SetChildCardinality(1);
}

} // namespace

void RegisterHugrNodeAgent(ExtensionLoader &loader, const shared_ptr<acl::PolicyStore> &store) {
	auto info = make_shared_ptr<AgentInfo>(store);

	TableFunction run(Identifier("hugr_node_run"), {}, RunScan, RunBindFn, RunInit);
	run.function_info = info;
	loader.RegisterFunction(run);

	TableFunction plan(Identifier("hugr_node_bootstrap_plan"), {LogicalType::VARCHAR}, StepsScan, PlanBind, StepsInit);
	plan.GetSignature().WithTypedKwargs("options",
	                                    [](TypedKwargs &options) { options.Add("environment", LogicalType::VARCHAR); });
	plan.function_info = info;
	loader.RegisterFunction(plan);

	TableFunction profile(Identifier("hugr_node_profile_statements"), {}, StepsScan, ProfileBind, StepsInit);
	profile.function_info = info;
	loader.RegisterFunction(profile);
}

} // namespace hugr_node
} // namespace duckdb
