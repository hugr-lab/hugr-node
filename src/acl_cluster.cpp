// Spec 093: the cluster profile - the shared part of every node's bootstrap, kept in the policy catalog
// as desired state only. A change is checked, written with a config_version bump in one catalog
// transaction, and - when it can be applied live - applied on this node before the commit, so what
// reaches the cluster has worked on at least one node. Converging the other nodes is not this
// extension's: that is the node agent's (hugr_node). Seam: RegisterAclCluster.

#include "acl_cluster.hpp"

#include "acl_mgmt_rules.hpp"

#include "acl_door_common.hpp"
#include "acl_function_categories.hpp"
#include "acl_platform.hpp"
#include "acl_lineage.hpp"
#include "acl_policy_catalog.hpp"
#include "acl_result_rows.hpp"
#include "acl_sql_text.hpp"
#include "hugr_node_tick.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/parser/peg/passthrough_dialect.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <functional>

namespace duckdb {
namespace acl {

using acl_detail::CatalogBackend;
using acl_detail::Lit;

namespace {

//! Keys that carry a credential, wherever they are written: a conninfo `k=v`, a URI query parameter,
//! an ATTACH option. The profile names secrets; it never holds one (design/017 §3.6a).
const case_insensitive_set_t CREDENTIAL_KEYS = {"password",
                                                "passwd",
                                                "pwd",
                                                "passfile",
                                                "secret",
                                                "client_secret",
                                                "token",
                                                "bearer_token",
                                                "access_token",
                                                "key_id",
                                                "session_token",
                                                "secret_access_key",
                                                "account_key",
                                                "sas_token",
                                                "sig",
                                                "private_key",
                                                "connection_string",
                                                "aws_secret_access_key",
                                                "s3_secret_access_key",
                                                "s3_access_key_id",
                                                "s3_session_token",
                                                "http_proxy_password",
                                                "encryption_key"};

//! The node's hardening, fixed by its bootstrap - never a profile item
const case_insensitive_set_t HARDENING_SETTINGS = {
    "allow_unsigned_extensions", "allow_community_extensions", "allow_extension_repositories",
    "allow_persistent_secrets", "allow_unredacted_secrets", "lock_configuration", "extension_repository_directory",
    "allow_parser_override_extension", "enable_external_access", "secret_directory", "extension_directory",
    "extension_directories", "home_directory", "acl_observe_unauthenticated", "allowed_directories", "allowed_paths",
    // spec 118 review: the node's trust - who authorizes a door's
    // statement, whether the anonymous hatch opens, what a TLS peer
    // must prove, which filesystems exist
    "acl_allow_anonymous_admin", "acl_quack_authentication_function", "acl_quack_authorization_function",
    "enable_server_cert_verification", "enable_curl_server_cert_verification", "disabled_filesystems",
    // spec 006: which certificates the node trusts and where code may come from - the bootstrap sets them from
    // the environment, never a profile item (found 2026-10-15: ca_cert_file could hand every node a new CA)
    "ca_cert_file", "autoinstall_known_extensions", "autoload_known_extensions", "custom_extension_repository",
    "autoinstall_extension_repository", "tresor_http_client",
    // spec 010: where a node spills is its pod's (the spill volume, set by the bootstrap from HUGR_TEMP_DIRECTORY) - a
    // profile item could otherwise point every node's temp files somewhere else
    "temp_directory",
    // spec 010 review: the bearer path - which HTTP client carries the node's tokens (httplib verifies by its own
    // setting) and whether its log shows an Authorization header as written
    "httpfs_client_implementation", "redact_http_logs"};

//! Where the cluster's telemetry goes (spec 005): one destination for the whole cluster (the owner, 2026-10-14) - a
//! profile item of the cluster's scope, never of a group's
const case_insensitive_set_t CLUSTER_WIDE_SETTINGS = {
    "acl_otel_endpoint", "acl_otel_protocol",         "acl_otel_insecure",      "acl_otel_certificate",
    "acl_otel_lineage",  "acl_otel_lineage_endpoint", "acl_otel_lineage_secret"};

//! Source types whose connection needs a credential: written without SECRET, refused
const case_insensitive_set_t TYPES_NEEDING_SECRET = {"postgres", "postgres_scanner", "mysql", "mysql_scanner", "mssql",
                                                     "hugr"};

//! A source's type: its TYPE, else the one its path names - `postgres:dbname=…`, as duckdb reads a path's prefix
//! (ExtractExtensionPrefixFromPath: a scheme's `://` names a filesystem, no type) - '' for neither: a file, `:memory:`
string SourceTypeOf(const string &type, const string &path) {
	if (!type.empty()) {
		return type;
	}
	auto colon = path.find(':');
	if (colon == string::npos || colon < 2 || path.compare(colon, 3, "://") == 0) {
		return string(); // `:memory:`, a drive letter, a URL
	}
	for (idx_t i = 0; i < colon; i++) {
		auto c = path[i];
		if (!StringUtil::CharacterIsAlpha(c) && !StringUtil::CharacterIsDigit(c) && c != '_') {
			return string();
		}
	}
	return StringUtil::Lower(path.substr(0, colon));
}

//! The extension a source type needs loaded ('' = a core type)
string ExtensionOfType(const string &type) {
	auto t = StringUtil::Lower(type);
	if (t == "postgres" || t == "postgres_scanner") {
		return "postgres_scanner";
	}
	if (t == "mysql" || t == "mysql_scanner") {
		return "mysql_scanner";
	}
	if (t == "sqlite" || t == "sqlite_scanner") {
		return "sqlite_scanner";
	}
	// spec 011: another platform - the type is built into hugr_node
	if (t == "duckdb" || t == "hugr" || t.empty()) {
		return "";
	}
	return t; // ducklake, mssql, iceberg, delta, ... are named like their extension
}

[[noreturn]] void RefuseCredential(const string &key, const string &where) {
	throw BinderException("acl cluster: \"%s\" in %s is a credential, and the cluster profile never holds one - "
	                      "create a secret (in the node's secrets service) and name it with SECRET <name>",
	                      key, where);
}

void LintKey(const string &key, const string &where) {
	auto k = key;
	StringUtil::Trim(k);
	if (CREDENTIAL_KEYS.count(k)) {
		RefuseCredential(k, where);
	}
}

//! A path as written: a conninfo list, a URI, a prefixed form (`ducklake:postgres:k=v ...`) - every
//! `k=v` in it is checked by its key, and a URI's userinfo may carry a user but never a password
void LintPath(const string &path) {
	// userinfo: scheme://user:secret@host
	auto scheme = path.find("://");
	if (scheme != string::npos) {
		auto authority_start = scheme + 3;
		auto authority_end = path.find_first_of("/?#", authority_start);
		auto authority = path.substr(authority_start,
		                             authority_end == string::npos ? string::npos : authority_end - authority_start);
		auto at = authority.rfind('@');
		if (at != string::npos && authority.substr(0, at).find(':') != string::npos) {
			RefuseCredential("user:password@", "the source's URI");
		}
	}
	string token;
	auto flush = [&]() {
		auto eq = token.find('=');
		if (eq != string::npos) {
			auto key = token.substr(0, eq);
			auto colon = key.find_last_of(":/");
			if (colon != string::npos) {
				key = key.substr(colon + 1);
			}
			LintKey(key, "the source's path");
		}
		token.clear();
	};
	for (auto c : path) {
		if (c == ' ' || c == '\t' || c == ';' || c == '&' || c == '?' || c == '\n') {
			flush();
		} else {
			token += c;
		}
	}
	flush();
}

string JsonObjectOf(const vector<std::pair<string, string>> &entries) {
	vector<string> out;
	for (auto &entry : entries) {
		out.push_back(JsonQuote(entry.first) + ": " + JsonQuote(entry.second));
	}
	return "{" + StringUtil::Join(out, ", ") + "}";
}

vector<std::pair<string, string>> ParseOptions(const string &json) {
	using namespace duckdb_yyjson; // NOLINT
	vector<std::pair<string, string>> out;
	if (json.empty()) {
		return out;
	}
	auto doc = yyjson_read(json.c_str(), json.size(), 0);
	if (!doc) {
		throw BinderException("acl cluster: the source's options are a JSON object, got \"%s\"", json);
	}
	auto root = yyjson_doc_get_root(doc);
	if (!yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		throw BinderException("acl cluster: the source's options are a JSON object");
	}
	yyjson_val *key;
	yyjson_obj_iter iter = yyjson_obj_iter_with(root);
	while ((key = yyjson_obj_iter_next(&iter))) {
		auto val = yyjson_obj_iter_get_val(key);
		string value;
		if (yyjson_is_str(val)) {
			value = yyjson_get_str(val);
		} else if (yyjson_is_bool(val)) {
			value = yyjson_get_bool(val) ? "true" : "false";
		} else if (yyjson_is_num(val)) {
			value = std::to_string(yyjson_get_num(val));
		}
		out.emplace_back(yyjson_get_str(key), value);
	}
	yyjson_doc_free(doc);
	return out;
}

//! A type or an option key goes into the ATTACH as written: a plain identifier, nothing else
void RequireIdentifier(const string &value, const char *what) {
	for (auto c : value) {
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
			throw BinderException("acl cluster: %s \"%s\" is not a plain identifier", what, value);
		}
	}
}

string Quoted(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

string Ident(const string &value) {
	return "\"" + StringUtil::Replace(value, "\"", "\"\"") + "\"";
}

//! the names of (scope, name) items, a group's written `name (group g)`
vector<string> Names(const vector<std::pair<string, string>> &items) {
	vector<string> names;
	for (auto &item : items) {
		names.push_back(item.first.empty() ? item.second : item.second + " (group " + item.first + ")");
	}
	return names;
}

//! spec 096: whether a resource group has its own item of this kind and name (which overrides the cluster's)
bool GroupHasItem(CatalogBackend &catalog, const string &group, const char *kind, const string &name) {
	return catalog
	           .Query("SELECT 1 FROM " + catalog.Tbl("cluster_items") + " WHERE \"scope\" = " + Lit(group) +
	                  " AND \"kind\" = " + Lit(kind) + " AND lower(\"name\") = lower(" + Lit(name) + ")")
	           ->RowCount() > 0;
}

//! spec 096: the node's resource group ('' = none), as the deployment set it
string NodeGroupOf(DatabaseInstance &db) {
	Value value;
	if (db.TryGetCurrentSetting("acl_node_group", value) && !value.IsNull()) {
		return value.ToString();
	}
	return string();
}

//! spec 096: an item of another group than this node's is written, never applied here - the group's
//! nodes roll it out. True (with the answer's note) when the item is not this node's to apply now.
bool NotThisNodesScope(DatabaseInstance &db, const string &scope, PolicyStore::ClusterAnswer &answer,
                       const std::function<bool(const string &group)> &group_overrides = nullptr) {
	auto node_group = NodeGroupOf(db);
	if (scope.empty()) {
		// a cluster item this node's own group overrides is not this node's either: its group's item is
		// what acl_cluster_effective() says the node runs
		if (!node_group.empty() && group_overrides && group_overrides(node_group)) {
			answer.note =
			    "cluster item overridden by resource group " + node_group + "'s own on this node - not applied here";
			return true;
		}
		return false;
	}
	if (node_group == scope) {
		return false;
	}
	answer.note = "item of resource group " + scope + " - this node is " +
	              (node_group.empty() ? string("in no group") : "in group " + node_group) +
	              "; the group's nodes roll it out";
	return true;
}

//! A query on its own connection of the node: what the live apply and the node-side checks run on
unique_ptr<QueryResult> NodeQuery(DatabaseInstance &db, const string &sql) {
	Connection con(db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		result->ThrowError("acl cluster: ");
	}
	return result;
}

bool NodeHasDatabase(DatabaseInstance &db, const string &alias) {
	auto result = NodeQuery(db, "SELECT 1 FROM duckdb_databases() WHERE database_name = " + Quoted(alias));
	return result->RowCount() > 0;
}

//! The ATTACH this item stands for, as the node runs it (spec 112 §9: LINEAGE names the source, the
//! parser override's sugar declares it)
string AttachStatement(const string &alias, const string &path, const string &type, const string &secret,
                       const vector<std::pair<string, string>> &options, const string &lineage) {
	vector<string> parts;
	if (!type.empty()) {
		parts.push_back("TYPE " + type);
	}
	if (!secret.empty()) {
		parts.push_back("SECRET " + Ident(secret));
	}
	for (auto &option : options) {
		if (StringUtil::CIEquals(option.second, "true")) {
			parts.push_back(option.first);
		} else {
			parts.push_back(option.first + " " + Quoted(option.second));
		}
	}
	return "ATTACH " + Quoted(path) + " AS " + Ident(alias) +
	       (parts.empty() ? string() : " (" + StringUtil::Join(parts, ", ") + ")") +
	       (lineage.empty() ? string() : " LINEAGE " + Quoted(lineage));
}

void RequireClusterCatalog(const unique_ptr<CatalogBackend> &catalog, const char *what) {
	if (!catalog) {
		throw BinderException("%s: the cluster profile lives in a policy catalog - run acl_use_db() first", what);
	}
	if (catalog->FunctionMode()) {
		throw BinderException("%s: the function-driver policy source has no cluster profile", what);
	}
}

using ReadFn = std::function<unique_ptr<QueryResult>(const string &)>;

void RequireScope(const ReadFn &read, CatalogBackend &catalog, const string &scope) {
	if (scope.empty()) {
		return;
	}
	auto exists = read("SELECT 1 FROM " + catalog.Tbl("resource_groups") + " WHERE \"group\" = " + Lit(scope));
	if (exists->RowCount() == 0) {
		throw BinderException("acl cluster: resource group \"%s\" does not exist - IN GROUP names a resource "
		                      "group (CREATE RESOURCE GROUP)",
		                      scope);
	}
}

//! The item's current spec, or "" when it is not in the profile
string CurrentSpec(const ReadFn &read, CatalogBackend &catalog, const string &scope, const string &kind,
                   const string &name) {
	auto row = read("SELECT \"spec\" FROM " + catalog.Tbl("cluster_items") + " WHERE \"scope\" = " + Lit(scope) +
	                " AND \"kind\" = " + Lit(kind) + " AND lower(\"name\") = lower(" + Lit(name) + ")");
	return row->RowCount() == 0 ? string() : row->Collection().GetValue(0, 0).ToString();
}

//! The item's stored comment ("" when it has none)
string CurrentComment(const ReadFn &read, CatalogBackend &catalog, const string &scope, const string &kind,
                      const string &name) {
	auto row = read("SELECT coalesce(\"comment\", '') FROM " + catalog.Tbl("cluster_items") + " WHERE \"scope\" = " +
	                Lit(scope) + " AND \"kind\" = " + Lit(kind) + " AND lower(\"name\") = lower(" + Lit(name) + ")");
	return row->RowCount() == 0 ? string() : row->Collection().GetValue(0, 0).ToString();
}

string UpsertItem(CatalogBackend &catalog, const string &scope, const string &kind, const string &name,
                  const string &spec, const string &item_class, const string &comment, bool keep_pos) {
	auto table = catalog.Tbl("cluster_items");
	auto where = " WHERE \"scope\" = " + Lit(scope) + " AND \"kind\" = " + Lit(kind) + " AND lower(\"name\") = lower(" +
	             Lit(name) + ")";
	string version =
	    "(SELECT CAST(\"value\" AS BIGINT) FROM " + catalog.Tbl("meta") + " WHERE \"key\" = 'config_version') + 1";
	if (keep_pos) {
		return "UPDATE " + table + " SET \"spec\" = " + Lit(spec) + ", \"class\" = " + Lit(item_class) +
		       ", \"version\" = " + version + ", \"comment\" = " + Lit(comment) + where;
	}
	return "INSERT INTO " + table + " VALUES (" + Lit(scope) + ", " + Lit(kind) + ", " + Lit(name) + ", " + Lit(spec) +
	       ", " + Lit(item_class) + ", " + version + ", (SELECT coalesce(max(\"pos\"), 0) + 1 FROM " + table + "), " +
	       Lit(comment) + ")";
}

string DeleteItem(CatalogBackend &catalog, const string &scope, const string &kind, const string &name) {
	return "DELETE FROM " + catalog.Tbl("cluster_items") + " WHERE \"scope\" = " + Lit(scope) +
	       " AND \"kind\" = " + Lit(kind) + " AND lower(\"name\") = lower(" + Lit(name) + ")";
}

//! hugr-node spec 035: the aliases a group's `sources` item names (its spec `{"names": [...]}`)
case_insensitive_set_t SourceSetNames(const string &spec) {
	using namespace duckdb_yyjson; // NOLINT
	auto doc = yyjson_read(spec.c_str(), spec.size(), 0);
	auto list = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "names") : nullptr;
	if (!list || !yyjson_is_arr(list)) {
		if (doc) {
			yyjson_doc_free(doc);
		}
		throw InvalidInputException("acl cluster: a group's source set is not readable");
	}
	case_insensitive_set_t names;
	size_t i, n;
	yyjson_val *value;
	yyjson_arr_foreach(list, i, n, value) {
		if (yyjson_is_str(value)) {
			names.insert(yyjson_get_str(value));
		}
	}
	yyjson_doc_free(doc);
	return names;
}

string JsonField(const string &json, const char *field);

//! spec 035 stage 2b: a set that names no list - every source, attached on demand (`SET SOURCES ALL LAZY`)
bool SourceSetAll(const string &spec) {
	return JsonField(spec, "all") == "true";
}

//! hugr-node spec 035: group's set lists `name`, whose item - from `owner` ('' the cluster, else the group) - must have
//! every source it depends on listed too
void RequireListedDependencies(const std::function<unique_ptr<QueryResult>(const string &)> &read,
                               CatalogBackend &catalog, const string &group, const string &owner, const string &name,
                               const case_insensitive_set_t &listed) {
	auto deps = read("SELECT \"depends_on\" FROM " + catalog.Tbl("cluster_deps") + " WHERE \"scope\" = " + Lit(owner) +
	                 " AND lower(\"name\") = lower(" + Lit(name) + ")");
	ResultRows rows(*deps);
	for (idx_t r = 0; r < rows.Count(); r++) {
		auto dependency = rows.GetValue(0, r).ToString();
		if (!listed.count(dependency)) {
			throw BinderException("acl cluster: group \"%s\"'s source set lists \"%s\", which depends on \"%s\" - "
			                      "list it there too (CLUSTER SET SOURCES)",
			                      group, name, dependency);
		}
	}
}

//! the node's secrets service (tresor): attached at the bootstrap, never lazy
bool IsSecretsServiceSource(const string &type, const string &path) {
	return StringUtil::CIEquals(SourceTypeOf(type, path), "tresor");
}

//! spec 035 stage 2b: whether `group`'s source set attaches its sources on demand
bool GroupSetLazy(CatalogBackend &catalog, const string &group) {
	if (group.empty() || catalog.FunctionMode()) {
		return false;
	}
	auto result = catalog.Query("SELECT \"spec\" FROM " + catalog.Tbl("cluster_items") +
	                            " WHERE \"scope\" = " + Lit(group) + " AND \"kind\" = 'sources'");
	return result->RowCount() > 0 && JsonField(result->Collection().GetValue(0, 0).ToString(), "lazy") == "true";
}

//! hugr-node spec 035: whether `group` declares its sources (its nodes attach only those)
bool GroupDeclaresSources(CatalogBackend &catalog, const string &group) {
	if (group.empty() || catalog.FunctionMode()) {
		return false;
	}
	auto result = catalog.Query("SELECT \"spec\" FROM " + catalog.Tbl("cluster_items") +
	                            " WHERE \"scope\" = " + Lit(group) + " AND \"kind\" = 'sources'");
	// spec 035 stage 2b: ALL LAZY may attach every source - the node is not partial
	return result->RowCount() > 0 && !SourceSetAll(result->Collection().GetValue(0, 0).ToString());
}

string JsonField(const string &json, const char *field) {
	using namespace duckdb_yyjson; // NOLINT
	auto doc = yyjson_read(json.c_str(), json.size(), 0);
	if (!doc) {
		return string();
	}
	auto val = yyjson_obj_get(yyjson_doc_get_root(doc), field);
	string out = val && yyjson_is_str(val) ? string(yyjson_get_str(val)) : string();
	yyjson_doc_free(doc);
	return out;
}

} // namespace

namespace {

//! spec 035 stage 2: one lazy source of this node - its item and its idle time (0 = never detached), both under the
//! registry's lock; when a statement last read it and the lock its attach and its idle detach take (never held with
//! the registry's: an attach may wait on its source for long, the parse of every statement reads the registry)
struct LazySource {
	PolicyStore::ClusterItem item;
	int64_t idle_seconds = 0;
	mutex attach_lock;
	std::chrono::steady_clock::time_point last_use; // under attach_lock
};

//! spec 035 stage 2: the node's lazy sources, from its effective profile - per instance, in its object cache
class LazySources : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "hugr_node_lazy_sources";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // never evicted: the node's own state
	}
	mutex lock;
	map<string, shared_ptr<LazySource>> sources; // by the lowercased alias
};

shared_ptr<LazySources> LazySourcesOf(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<LazySources>(LazySources::ObjectType());
}

//! the source and a copy of its item and idle time, read under the registry's lock
shared_ptr<LazySource> FindLazySource(DatabaseInstance &db, const string &alias, PolicyStore::ClusterItem &item,
                                      int64_t &idle_seconds) {
	auto lazy = LazySourcesOf(db);
	lock_guard<mutex> guard(lazy->lock);
	auto found = lazy->sources.find(StringUtil::Lower(alias));
	if (found == lazy->sources.end()) {
		return nullptr;
	}
	item = found->second->item;
	idle_seconds = found->second->idle_seconds;
	return found->second;
}

} // namespace

bool ClusterSourceLazy(const PolicyStore::ClusterItem &item, int64_t &idle_seconds);

void RefreshLazySources(DatabaseInstance &db, const vector<PolicyStore::ClusterItem> &items) {
	auto lazy = LazySourcesOf(db);
	lock_guard<mutex> guard(lazy->lock);
	map<string, shared_ptr<LazySource>> next;
	for (auto &item : items) {
		int64_t idle_seconds = 0;
		if (!ClusterSourceLazy(item, idle_seconds)) {
			continue;
		}
		auto key = StringUtil::Lower(item.name);
		auto found = lazy->sources.find(key);
		auto source = found != lazy->sources.end() ? found->second : make_shared_ptr<LazySource>();
		if (found == lazy->sources.end()) {
			source->last_use = std::chrono::steady_clock::now(); // attached by hand before it was marked: idle from now
		}
		source->item = item; // an attach in flight runs with the copy it took
		source->idle_seconds = idle_seconds;
		next[key] = source;
	}
	lazy->sources = std::move(next);
}

void ForgetLazySource(DatabaseInstance &db, const string &alias) {
	auto lazy = LazySourcesOf(db);
	lock_guard<mutex> guard(lazy->lock);
	lazy->sources.erase(StringUtil::Lower(alias));
}

vector<string> LazySourceAliases(DatabaseInstance &db) {
	auto lazy = LazySourcesOf(db);
	lock_guard<mutex> guard(lazy->lock);
	vector<string> out;
	for (auto &source : lazy->sources) {
		out.push_back(source.second->item.name);
	}
	return out;
}

void AttachLazySource(DatabaseInstance &db, const string &alias) {
	PolicyStore::ClusterItem item;
	int64_t idle_seconds = 0;
	auto source = FindLazySource(db, alias, item, idle_seconds);
	if (!source) {
		return; // the profile no longer keeps it lazy: nothing of ours
	}
	lock_guard<mutex> attach(source->attach_lock);
	source->last_use = std::chrono::steady_clock::now();
	if (DatabaseManager::Get(db).GetDatabase(Identifier(item.name))) {
		return;
	}
	Connection con(db);
	for (auto &statement : ClusterItemApplyStatements(item)) {
		auto result = con.Query(statement);
		if (result->HasError()) {
			if (DatabaseManager::Get(db).GetDatabase(Identifier(item.name))) {
				break; // attached meanwhile by another path (the tick's apply, a write's hot apply): there it is
			}
			// the class only: a source's error may carry its connection string. The operator's log names the source;
			// the statement's refusal does not - a principal reads virtual names
			auto error_class = Exception::ExceptionTypeToString(result->GetErrorType());
			fprintf(stderr, "hugr_node: lazy source %s could not be attached (%s)\n", item.name.c_str(),
			        error_class.c_str());
			fflush(stderr);
			throw InvalidInputException("acl: a source this statement reads could not be attached (%s)", error_class);
		}
	}
	source->last_use = std::chrono::steady_clock::now();
	// the tick takes it as this node's: a profile that drops or re-points it then detaches it or waits for a drain
	NoteAppliedHere(db, "source", item.name);
}

vector<string> DetachIdleLazySources(DatabaseInstance &db, const std::set<string> &taken) {
	vector<std::pair<shared_ptr<LazySource>, PolicyStore::ClusterItem>> sources;
	vector<int64_t> idle_times;
	{
		auto lazy = LazySourcesOf(db);
		lock_guard<mutex> guard(lazy->lock);
		for (auto &source : lazy->sources) {
			sources.emplace_back(source.second, source.second->item);
			idle_times.push_back(source.second->idle_seconds);
		}
	}
	vector<string> detached;
	auto now = std::chrono::steady_clock::now();
	for (idx_t i = 0; i < sources.size(); i++) {
		auto &source = sources[i].first;
		auto &item = sources[i].second;
		// a source being attached is in use: never wait on it from the tick (its heartbeat with it)
		unique_lock<mutex> attach(source->attach_lock, std::try_to_lock);
		if (!attach.owns_lock()) {
			continue;
		}
		if (!taken.count(StringUtil::Lower(item.name))) {
			continue; // not the node's: attached by hand (ACL NATIVE) - drift, never the tick's to detach
		}
		hugr_node::LazyIdle state;
		state.idle_seconds = idle_times[i];
		state.seconds_since_use = std::chrono::duration_cast<std::chrono::seconds>(now - source->last_use).count();
		{
			auto attached = DatabaseManager::Get(db).GetDatabase(Identifier(item.name));
			state.attached = attached != nullptr;
			// duckdb's own rule (InvokeCloseIfLastReference): the manager's reference and this one are all there
			// is unless a transaction - a running statement, an open one - still uses the database
			state.held = attached && attached.use_count() > 2;
		}
		if (!hugr_node::DecideIdleDetach(state)) {
			continue;
		}
		Connection con(db);
		auto result = con.Query("DETACH " + Ident(item.name));
		if (!result->HasError()) {
			detached.push_back(item.name);
		}
	}
	return detached;
}

void RefreshLazySourcesAfterWrite(PolicyStore &store, DatabaseInstance &db) {
	// the writing node attaches a source it marked lazy on demand at once; the others at their next tick
	try {
		RefreshLazySources(db, store.ClusterEffective());
	} catch (std::exception &) { // NOLINT: the next tick refreshes them
	}
}

//! spec 035 stage 2: an idle time in seconds, read by duckdb's own INTERVAL (never a reader of ours) - '' = none
int64_t IdleSecondsOf(DatabaseInstance &db, bool lazy, const string &idle) {
	if (idle.empty()) {
		return 0;
	}
	if (!lazy) {
		throw BinderException("acl cluster: IDLE is a lazy source's - write LAZY IDLE '<interval>'");
	}
	Connection con(db);
	auto read = con.Query("SELECT CAST(epoch(CAST(" + Quoted(idle) + " AS INTERVAL)) AS BIGINT)");
	if (read->HasError() || read->RowCount() == 0 || read->Collection().GetValue(0, 0).IsNull()) {
		throw BinderException("acl cluster: IDLE '%s' is not an interval duckdb reads ('30 min', '2 hours')", idle);
	}
	auto seconds = read->Collection().GetValue(0, 0).GetValue<int64_t>();
	if (seconds < 60) {
		throw BinderException("acl cluster: IDLE '%s' is under a minute - a lazy source idles a minute at least", idle);
	}
	return seconds;
}

//! the lazy marks of a spec as JSON members, '' when not lazy
string LazyMarks(bool lazy, int64_t idle_seconds) {
	if (!lazy) {
		return string();
	}
	return ", " + JsonQuote("lazy") + ": " + JsonQuote("true") +
	       (idle_seconds > 0 ? ", " + JsonQuote("idle_seconds") + ": " + JsonQuote(std::to_string(idle_seconds))
	                         : string());
}

string ClusterSourceIdentity(const string &spec);

//! spec 035 stage 2b: a source item with its group set's lazy marks in place of its own
string WithLazyMarks(const string &spec, bool lazy, int64_t idle_seconds) {
	auto identity = ClusterSourceIdentity(spec);
	auto close = identity.rfind('}');
	if (close == string::npos) {
		return spec;
	}
	return identity.substr(0, close) + LazyMarks(lazy, idle_seconds) + identity.substr(close);
}

string ClusterSourceIdentity(const string &spec) {
	using namespace duckdb_yyjson; // NOLINT
	auto doc = yyjson_read(spec.c_str(), spec.size(), 0);
	if (!doc) {
		return spec;
	}
	auto copy = yyjson_doc_mut_copy(doc, nullptr);
	yyjson_doc_free(doc);
	if (!copy) {
		return spec;
	}
	auto root = yyjson_mut_doc_get_root(copy);
	yyjson_mut_obj_remove_key(root, "lazy");
	yyjson_mut_obj_remove_key(root, "idle_seconds");
	size_t length = 0;
	auto text = yyjson_mut_write(copy, 0, &length);
	string out = text ? string(text, length) : spec;
	free(text);
	yyjson_mut_doc_free(copy);
	return out;
}

bool ClusterSourceLazy(const PolicyStore::ClusterItem &item, int64_t &idle_seconds) {
	idle_seconds = 0;
	if (item.kind != "source" || JsonField(item.spec, "lazy") != "true") {
		return false;
	}
	auto idle = JsonField(item.spec, "idle_seconds");
	int64_t seconds = 0;
	idle_seconds = !idle.empty() && TryCast::Operation<string_t, int64_t>(string_t(idle), seconds) ? seconds : 0;
	return true;
}

string ClusterExtensionOfType(const string &type) {
	return ExtensionOfType(type);
}

namespace {

//! spec 010: what this node's own profile writes applied here before their commit - per instance, in its object cache
class AppliedHere : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "hugr_node_applied_here";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // never evicted: a note between a write and the next tick
	}
	mutex lock;
	vector<std::pair<string, string>> items;
};

} // namespace

void NoteAppliedHere(DatabaseInstance &db, const string &kind, const string &name) {
	auto here = db.GetObjectCache().GetOrCreate<AppliedHere>(AppliedHere::ObjectType());
	lock_guard<mutex> guard(here->lock);
	here->items.emplace_back(kind, name);
}

vector<std::pair<string, string>> TakeAppliedHere(DatabaseInstance &db) {
	auto here = db.GetObjectCache().GetOrCreate<AppliedHere>(AppliedHere::ObjectType());
	lock_guard<mutex> guard(here->lock);
	auto out = std::move(here->items);
	here->items.clear();
	return out;
}

vector<string> ClusterItemApplyStatements(const PolicyStore::ClusterItem &item) {
	using namespace duckdb_yyjson; // NOLINT
	if (item.kind == "setting") {
		return {"SET GLOBAL " + Ident(item.name) + " = " + Quoted(JsonField(item.spec, "value"))};
	}
	if (item.kind == "extension") {
		auto version = JsonField(item.spec, "version");
		auto repo = JsonField(item.spec, "repository");
		if (version.empty() || repo.empty()) {
			throw InvalidInputException("acl cluster: extension \"%s\" in the profile names no version or repository",
			                            item.name);
		}
		return {"INSTALL " + Ident(item.name) + " FROM " + Ident(repo) + " VERSION " + Quoted(version),
		        "LOAD " + Ident(item.name) + " FROM " + Ident(repo)};
	}
	if (item.kind == "source") {
		vector<std::pair<string, string>> options;
		auto doc = yyjson_read(item.spec.c_str(), item.spec.size(), 0);
		if (!doc) {
			throw InvalidInputException("acl cluster: source \"%s\" has an unreadable spec", item.name);
		}
		auto opts = yyjson_obj_get(yyjson_doc_get_root(doc), "options");
		if (opts && yyjson_is_obj(opts)) {
			size_t idx, max;
			yyjson_val *key, *val;
			yyjson_obj_foreach(opts, idx, max, key, val) {
				options.emplace_back(yyjson_get_str(key), yyjson_is_str(val) ? yyjson_get_str(val) : "");
			}
		}
		yyjson_doc_free(doc);
		return {AttachStatement(item.name, JsonField(item.spec, "path"), JsonField(item.spec, "type"),
		                        JsonField(item.spec, "secret"), options, JsonField(item.spec, "lineage"))};
	}
	throw InvalidInputException("acl cluster: profile item \"%s\" has an unknown kind \"%s\"", item.name, item.kind);
}

bool ClusterBundleMaySet(const string &name) {
	// an allowlist (review 118.2: a denylist of data paths left the ACL's own trust settings open) - the
	// node's resources and tuning; everything else is passthrough's
	static const case_insensitive_set_t TUNING_SETTINGS = {
	    // duckdb: resources, durability, ordering, the http client's patience, rendering
	    "threads", "worker_threads", "memory_limit", "max_memory", "max_temp_directory_size",
	    "preserve_insertion_order", "checkpoint_threshold", "wal_autocheckpoint", "default_order", "default_null_order",
	    "enable_object_cache", "http_timeout", "http_retries", "http_retry_wait_ms", "http_retry_backoff",
	    "http_keep_alive", "timezone", "calendar", "arrow_large_buffer_size",
	    // acl: limits, windows, timeouts, exposure and lineage naming
	    "acl_alias_types", "acl_enum_types", "acl_flight_stream_idle", "acl_flight_ingest_idle",
	    "acl_lineage_namespace", "acl_lineage_level", "acl_lineage_max_edges", "acl_lineage_buffer",
	    "acl_max_result_rows", "acl_max_ingest_rows", "acl_max_sessions", "acl_node_admit_memory",
	    "acl_node_stream_budget", "acl_profile_level", "acl_quack_cache_max_rows", "acl_quack_client_depth",
	    "acl_quack_fetch_producer_buffer_bytes", "acl_quack_fetch_window", "acl_quack_fetch_window_max",
	    "acl_quack_prepare_inline_rows", "acl_quack_rebalance_buffer_bytes", "acl_quack_result_ttl",
	    "acl_quack_server_keep_alive_timeout", "acl_quack_server_max_connections", "acl_quack_stream_reserve_bytes",
	    "acl_quack_target_batch_bytes", "acl_session_idle_timeout", "acl_stream_queue_timeout",
	    "acl_version_check_interval"};
	return TUNING_SETTINGS.count(name) > 0;
}

PolicyStore::ClusterAnswer PolicyStore::ClusterExtension(const string &verb, const string &scope, const string &name_p,
                                                         const string &version, const string &repository,
                                                         const string &comment) {
	// spec 010 review: one extension, one item - `postgres` is the item `postgres_scanner`, as duckdb loads it
	auto name = ExtensionHelper::ApplyExtensionAlias(name_p);
	RequireClusterCatalog(catalog, "acl_cluster_extension");
	auto db = instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	auto v = StringUtil::Lower(verb);
	if (v != "install" && v != "update" && v != "remove") {
		throw BinderException("acl_cluster_extension: the verb is install, update or remove, not \"%s\"", verb);
	}
	if (name.empty() || name.find_first_of("/\\:. '\"") != string::npos) {
		throw BinderException("acl cluster: \"%s\" is not an extension name - the profile installs an extension "
		                      "by its name from a trusted repository, never by a path or URL",
		                      name);
	}
	ClusterAnswer answer;
	answer.item_class = v == "install" ? "hot" : "restart";
	string repo = repository;
	vector<string> trusted; // the node's user-provided repositories
	auto require_trusted = [&](const string &candidate) {
		for (auto &n : trusted) {
			if (StringUtil::CIEquals(n, candidate)) {
				return;
			}
		}
		throw BinderException("acl cluster: \"%s\" is not a trusted repository of this node - the profile installs "
		                      "only from a repository created with CREATE EXTENSION REPOSITORY",
		                      candidate);
	};
	if (v != "remove") {
		if (version.empty()) {
			throw BinderException("acl cluster: an extension in the profile names its VERSION - every node installs "
			                      "the same build");
		}
		// a trusted repository of the node's (duckdb 2.0 CREATE EXTENSION REPOSITORY): core and community
		// are never the profile's source, and neither is a path or URL
		auto repos = NodeQuery(*db, "SELECT repository_name FROM duckdb_extension_repositories() WHERE type = "
		                            "'USER_PROVIDED' ORDER BY 1");
		ResultRows rows(*repos);
		for (idx_t i = 0; i < rows.Count(); i++) {
			trusted.push_back(rows.GetValue(0, i).ToString());
		}
		// spec 019: an update without FROM keeps the item's repository - read with the item, below
		if (repo.empty() && v == "install") {
			if (trusted.size() != 1) {
				throw BinderException("acl cluster: name the repository with FROM <name> - this node trusts %s",
				                      trusted.empty()
				                          ? string("no user-provided repository (CREATE EXTENSION REPOSITORY)")
				                          : StringUtil::Join(trusted, ", "));
			}
			repo = trusted[0];
		}
		if (!repo.empty()) {
			require_trusted(repo);
		}
	}
	answer.version = catalog->WriteWithReads(
	    [&](const ReadFn &read, vector<string> &statements) {
		    RequireScope(read, *catalog, scope);
		    auto current = CurrentSpec(read, *catalog, scope, "extension", name);
		    if (v == "install" && !current.empty()) {
			    throw BinderException("acl cluster: extension \"%s\" is already in the profile - UPDATE EXTENSION "
			                          "changes its version",
			                          name);
		    }
		    if (v != "install" && current.empty()) {
			    throw BinderException("acl cluster: extension \"%s\" is not in the profile", name);
		    }
		    if (v == "remove") {
			    // a source of the profile that needs it would leave a node unable to start
			    auto sources = read("SELECT \"name\", \"spec\" FROM " + catalog->Tbl("cluster_items") +
			                        " WHERE \"kind\" = 'source'");
			    ResultRows rows(*sources);
			    for (idx_t i = 0; i < rows.Count(); i++) {
				    auto spec = rows.GetValue(1, i).ToString();
				    auto type = SourceTypeOf(JsonField(spec, "type"), JsonField(spec, "path"));
				    if (StringUtil::CIEquals(ExtensionOfType(type), name)) {
					    throw BinderException("acl cluster: source \"%s\" needs extension \"%s\" - DETACH it first",
					                          rows.GetValue(0, i).ToString(), name);
				    }
			    }
			    statements.push_back(DeleteItem(*catalog, scope, "extension", name));
			    return;
		    }
		    // spec 019: an update keeps what it does not name - the item's repository (still one this node
		    // trusts) and its comment
		    auto note = comment;
		    if (v == "update") {
			    if (repository.empty()) {
				    repo = JsonField(current, "repository");
				    bool known = false;
				    for (auto &n : trusted) {
					    known = known || StringUtil::CIEquals(n, repo);
				    }
				    if (!known) {
					    throw BinderException("acl cluster: extension \"%s\" comes from \"%s\", which this node no "
					                          "longer trusts - name its repository with FROM",
					                          name, repo);
				    }
			    }
			    if (note.empty()) {
				    note = CurrentComment(read, *catalog, scope, "extension", name);
			    }
		    }
		    // spec 103: version and repository only - DuckDB verifies the repository's signature at every
		    // INSTALL and LOAD, and a released version is immutable there; a hash of the signed file would
		    // change with every key rotation and differ per channel for the same code
		    auto spec = JsonObjectOf({{"version", version}, {"repository", repo}});
		    statements.push_back(
		        UpsertItem(*catalog, scope, "extension", name, spec, answer.item_class, note, !current.empty()));
	    },
	    "config_version",
	    [&](int64_t) {
		    if (NotThisNodesScope(*db, scope, answer, [&](const string &group) {
			        return GroupHasItem(*catalog, group, "extension", name);
		        })) {
			    return;
		    }
		    if (v != "install") {
			    answer.note = "restart class - a running process cannot unload or reload an extension; each node "
			                  "reports it pending until it restarts (the orchestrator rolls it)";
			    return;
		    }
		    NodeQuery(*db, "INSTALL " + Ident(name) + " FROM " + Ident(repo) + " VERSION " + Quoted(version));
		    NodeQuery(*db, "LOAD " + Ident(name) + " FROM " + Ident(repo));
		    answer.applied_here = true;
		    NoteAppliedHere(*db, "extension", name);
	    });
	return answer;
}

PolicyStore::ClusterAnswer PolicyStore::ClusterAttach(const string &scope, const string &alias, const string &path,
                                                      const string &type, const string &secret,
                                                      const string &options_json, const vector<string> &depends_on,
                                                      const string &comment, const string &lineage, bool lazy,
                                                      const string &idle) {
	RequireClusterCatalog(catalog, "acl_cluster_attach");
	auto db = instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	if (alias.empty()) {
		throw BinderException("acl cluster: ATTACH names its alias (AS <name>)");
	}
	if (StringUtil::CIEquals(alias, catalog->db_name)) {
		throw BinderException("acl cluster: \"%s\" is the policy catalog's database - the node attaches it from its "
		                      "local bootstrap, never from the profile it holds",
		                      alias);
	}
	RequireIdentifier(type, "the source's TYPE");
	LintPath(path);
	auto options = ParseOptions(options_json);
	for (auto &option : options) {
		RequireIdentifier(option.first, "an ATTACH option");
		LintKey(option.first, "the source's options");
		if (StringUtil::CIEquals(option.first, "type") || StringUtil::CIEquals(option.first, "secret")) {
			// the call's own arguments: written among the options they would skip the SECRET requirement
			throw BinderException("acl cluster: %s is the attach's own argument, never one of its options",
			                      StringUtil::Upper(option.first));
		}
	}
	case_insensitive_set_t depended;
	for (auto &dep : depends_on) {
		if (dep.empty() || !depended.insert(dep).second) {
			throw BinderException("acl cluster: DEPENDS ON names each source once - \"%s\" %s", dep,
			                      dep.empty() ? "names none" : "twice");
		}
	}
	if (!lineage.empty() && (lineage.find("://") == string::npos || !LineageIdentityClean(lineage))) {
		throw BinderException("acl cluster: LINEAGE names the source as others know it, "
		                      "<scheme>://<host:port>[/<database>] - never a credential (no user:password@)");
	}
	// a source whose path names its type (`postgres:dbname=…`, no TYPE) needs a secret as one with TYPE does
	auto source_type = SourceTypeOf(type, path);
	if (secret.empty() && TYPES_NEEDING_SECRET.count(source_type)) {
		throw BinderException("acl cluster: a %s source connects with a credential - name the secret that holds it: "
		                      "(TYPE %s, SECRET <name>)",
		                      source_type, source_type);
	}
	// spec 035 stage 2: a lazy source attaches alone, when a statement reads it - nothing it loads through is attached
	// with it, and the node's secrets service is never lazy
	if (lazy && !depends_on.empty()) {
		throw BinderException(
		    "acl cluster: a lazy source attaches alone - LAZY and DEPENDS ON are not written together");
	}
	if (lazy && IsSecretsServiceSource(type, path)) {
		throw BinderException("acl cluster: the secrets service is attached at the bootstrap, never LAZY");
	}
	auto idle_seconds = IdleSecondsOf(*db, lazy, idle);
	auto spec = "{" + JsonQuote("type") + ": " + JsonQuote(type) + ", " + JsonQuote("path") + ": " + JsonQuote(path) +
	            ", " + JsonQuote("secret") + ": " + JsonQuote(secret) + ", " + JsonQuote("options") + ": " +
	            JsonObjectOf(options) +
	            (lineage.empty() ? string() : ", " + JsonQuote("lineage") + ": " + JsonQuote(lineage)) +
	            LazyMarks(lazy, idle_seconds) + "}";
	ClusterAnswer answer;
	bool repoint = false;
	bool exists = false;
	// a group's source named like a cluster's overrides it on the group's nodes: there it is a re-point
	bool overrides = false;
	vector<string> left_out; // spec 035: groups whose source set does not list it
	answer.version = catalog->WriteWithReads(
	    [&](const ReadFn &read, vector<string> &statements) {
		    RequireScope(read, *catalog, scope);
		    auto current = CurrentSpec(read, *catalog, scope, "source", alias);
		    exists = !current.empty();
		    // spec 035 stage 2: a change of the lazy marks alone moves nothing - the source is where it was
		    repoint = exists && ClusterSourceIdentity(current) != ClusterSourceIdentity(spec);
		    overrides = !scope.empty() && !CurrentSpec(read, *catalog, "", "source", alias).empty();
		    answer.item_class = repoint || overrides ? "drain" : "hot";
		    // spec 035 stage 2: nothing loads through a lazy source, and a source others load through is not lazy
		    for (auto &dep : depends_on) {
			    for (auto &dep_scope : {scope, string()}) {
				    if (JsonField(CurrentSpec(read, *catalog, dep_scope, "source", dep), "lazy") == "true") {
					    throw BinderException("acl cluster: DEPENDS ON \"%s\" - a lazy source; nothing loads through "
					                          "one",
					                          dep);
				    }
			    }
		    }
		    if (lazy) {
			    auto dependents = read("SELECT \"name\" FROM " + catalog->Tbl("cluster_deps") +
			                           " WHERE lower(\"depends_on\") = lower(" + Lit(alias) + ") ORDER BY \"name\"");
			    if (dependents->RowCount() > 0) {
				    ResultRows rows(*dependents);
				    throw BinderException("acl cluster: \"%s\" is what %s loads through - a source others depend on "
				                          "is never LAZY",
				                          alias, rows.GetValue(0, 0).ToString());
			    }
		    }
		    // dependencies: sources of this scope or of the whole cluster, never a cycle
		    for (auto &dep : depends_on) {
			    auto exists = read("SELECT 1 FROM " + catalog->Tbl("cluster_items") +
			                       " WHERE \"kind\" = 'source' AND lower(\"name\") = lower(" + Lit(dep) +
			                       ") AND (\"scope\" = " + Lit(scope) + " OR \"scope\" = '')");
			    if (exists->RowCount() == 0) {
				    throw BinderException("acl cluster: DEPENDS ON \"%s\" - no such source in the profile%s", dep,
				                          scope.empty() ? string() : " of group " + scope + " or the cluster");
			    }
		    }
		    // cycle: from each dependency, can we reach `alias` again through the existing edges?
		    auto edges_result =
		        read("SELECT lower(\"name\"), lower(\"depends_on\") FROM " + catalog->Tbl("cluster_deps"));
		    ResultRows edges(*edges_result);
		    std::function<bool(const string &, case_insensitive_set_t &)> reaches = [&](const string &from,
		                                                                                case_insensitive_set_t &seen) {
			    if (StringUtil::CIEquals(from, alias)) {
				    return true;
			    }
			    if (!seen.insert(from).second) {
				    return false;
			    }
			    for (idx_t i = 0; i < edges.Count(); i++) {
				    if (StringUtil::CIEquals(edges.GetValue(0, i).ToString(), from) &&
				        reaches(edges.GetValue(1, i).ToString(), seen)) {
					    return true;
				    }
			    }
			    return false;
		    };
		    for (auto &dep : depends_on) {
			    case_insensitive_set_t seen;
			    if (reaches(dep, seen)) {
				    throw BinderException("acl cluster: DEPENDS ON \"%s\" would make a cycle through \"%s\"", dep,
				                          alias);
			    }
		    }
		    // spec 035: the groups whose source set this source reaches - a set that lists it needs what it depends on
		    // listed too, and one that does not list it leaves it off its nodes (said in the answer)
		    auto sets = read("SELECT \"scope\", \"spec\" FROM " + catalog->Tbl("cluster_items") +
		                     " WHERE \"kind\" = 'sources'" +
		                     (scope.empty() ? string() : " AND \"scope\" = " + Lit(scope)) + " ORDER BY \"scope\"");
		    ResultRows set_rows(*sets);
		    for (idx_t r = 0; r < set_rows.Count(); r++) {
			    auto group = set_rows.GetValue(0, r).ToString();
			    if (scope.empty() && !CurrentSpec(read, *catalog, group, "source", alias).empty()) {
				    continue; // the group reads its own item of that name, not this one
			    }
			    auto set_spec = set_rows.GetValue(1, r).ToString();
			    if (SourceSetAll(set_spec)) {
				    continue; // every source - attached on demand there
			    }
			    auto listed = SourceSetNames(set_spec);
			    if (!listed.count(alias)) {
				    left_out.push_back(group);
				    continue;
			    }
			    for (auto &dep : depends_on) {
				    if (!listed.count(dep)) {
					    throw BinderException(
					        "acl cluster: group \"%s\"'s source set lists \"%s\", which would depend on "
					        "\"%s\" - list it there first (CLUSTER SET SOURCES)",
					        group, alias, dep);
				    }
			    }
		    }
		    statements.push_back(
		        UpsertItem(*catalog, scope, "source", alias, spec, answer.item_class, comment, exists));
		    statements.push_back("DELETE FROM " + catalog->Tbl("cluster_deps") + " WHERE \"scope\" = " + Lit(scope) +
		                         " AND lower(\"name\") = lower(" + Lit(alias) + ")");
		    for (auto &dep : depends_on) {
			    statements.push_back("INSERT INTO " + catalog->Tbl("cluster_deps") + " VALUES (" + Lit(scope) + ", " +
			                         Lit(alias) + ", " + Lit(dep) + ")");
		    }
	    },
	    "config_version",
	    [&](int64_t) {
		    if (NotThisNodesScope(*db, scope, answer, [&](const string &group) {
			        return GroupHasItem(*catalog, group, "source", alias);
		        })) {
			    return;
		    }
		    if (std::find(left_out.begin(), left_out.end(), NodeGroupOf(*db)) != left_out.end()) {
			    return; // spec 035: this node's group does not attach it (the note says so)
		    }
		    if (repoint || (overrides && NodeHasDatabase(*db, alias))) {
			    answer.note = string(overrides && !repoint ? "drain class - it overrides the cluster's source of that "
			                                                 "name on this group's nodes; "
			                                               : "drain class - ") +
			                  "re-pointing an attached source is DETACH + ATTACH under the same name; each node "
			                  "reports it pending for a drain";
			    return;
		    }
		    if (NodeHasDatabase(*db, alias)) {
			    answer.note = "\"" + alias + "\" is already attached on this node - the profile now describes it";
			    NoteAppliedHere(*db, "source", alias);
			    if (!lineage.empty()) {
				    NodeQuery(*db, "SELECT acl_lineage_source(" + Quoted(alias) + ", " + Quoted(lineage) + ")");
				    answer.note += "; its lineage identity is declared";
			    }
			    return;
		    }
		    // spec 035 stage 2b: this node's group attaches its sources on demand - but a DEPENDS ON chain's and the
		    // secrets service, as its effective profile says (ClusterEffective's rule, on what is committed)
		    if (!lazy && GroupSetLazy(*catalog, NodeGroupOf(*db))) {
			    auto effective = ClusterEffective();
			    bool depended = false;
			    for (auto &item : effective) {
				    for (auto &dep : item.depends_on) {
					    depended = depended || (item.kind == "source" && StringUtil::CIEquals(dep, alias));
				    }
			    }
			    if (depends_on.empty() && !depended && !IsSecretsServiceSource(type, path)) {
				    answer.note = "lazy on this node - its group's source set attaches each source when a statement "
				                  "first reads it";
				    return;
			    }
			    for (auto &dep : depends_on) {
				    if (!NodeHasDatabase(*db, dep)) {
					    // what it loads through is lazily detached here: the tick attaches both, in their order
					    answer.note = "attached on this node by its next tick, after \"" + dep + "\" it depends on";
					    return;
				    }
			    }
		    }
		    if (lazy) {
			    answer.note = "lazy - attached on this node by the first statement that reads it" +
			                  string(idle_seconds > 0 ? ", detached after it idles" : ", never detached by itself");
			    return;
		    }
		    NodeQuery(*db, AttachStatement(alias, path, type, secret, options, lineage));
		    answer.applied_here = true;
		    NoteAppliedHere(*db, "source", alias);
	    });
	RefreshLazySourcesAfterWrite(*this, *db);
	if (!left_out.empty()) {
		auto said = "not attached by the nodes of group(s) " + StringUtil::Join(left_out, ", ") +
		            " - their source set does not list it (CLUSTER SET SOURCES names what they attach)";
		// a group's own item its set leaves out: that is all there is to say about it
		answer.note = scope.empty() && !answer.note.empty() ? answer.note + "; " + said : said;
	}
	return answer;
}

PolicyStore::ClusterAnswer PolicyStore::ClusterDetach(const string &scope, const string &alias, bool cascade,
                                                      bool force) {
	RequireClusterCatalog(catalog, "acl_cluster_detach");
	auto db = instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	if (StringUtil::CIEquals(alias, catalog->db_name)) {
		throw BinderException("acl cluster: \"%s\" is the policy catalog's database - never the profile's", alias);
	}
	ClusterAnswer answer;
	answer.item_class = "hot";
	// what goes, each with its scope: the source and (CASCADE) whatever depends on it
	vector<std::pair<string, string>> removed {{scope, alias}};
	// group items whose name the cluster also has: on the group's nodes the cluster's comes back
	case_insensitive_set_t falls_back;
	// what of it is in effect on THIS node (the cluster's item, unless this node's group has its own)
	case_insensitive_set_t in_effect;
	auto node_group = NodeGroupOf(*db);
	auto key = [](const std::pair<string, string> &item) {
		return item.first + "\x1f" + item.second;
	};
	answer.version = catalog->WriteWithReads(
	    [&](const ReadFn &read, vector<string> &statements) {
		    RequireScope(read, *catalog, scope);
		    if (CurrentSpec(read, *catalog, scope, "source", alias).empty()) {
			    throw BinderException("acl cluster: source \"%s\" is not in the profile", alias);
		    }
		    // spec 035: a group's own source its set lists gives way to the cluster's of that name - which must find
		    // what it depends on listed too
		    if (!scope.empty() && !CurrentSpec(read, *catalog, "", "source", alias).empty()) {
			    auto set = CurrentSpec(read, *catalog, scope, "sources", "sources");
			    if (!set.empty() && !SourceSetAll(set)) {
				    auto listed = SourceSetNames(set);
				    if (listed.count(alias)) {
					    RequireListedDependencies(read, *catalog, scope, "", alias, listed);
				    }
			    }
		    }
		    // who depends on it (transitively, for CASCADE). An edge is of its dependent's scope and names
		    // a source of that scope, else the cluster's: a group edge depends on a cluster source only
		    // while the group has none of that name, and a cluster edge never on a group's source.
		    auto edges_result = read("SELECT \"scope\", \"name\", \"depends_on\" FROM " + catalog->Tbl("cluster_deps"));
		    ResultRows edges(*edges_result);
		    auto group_sources_result = read("SELECT \"scope\", \"name\" FROM " + catalog->Tbl("cluster_items") +
		                                     " WHERE \"kind\" = 'source' AND \"scope\" <> ''");
		    ResultRows group_sources(*group_sources_result);
		    auto group_has = [&](const string &group, const string &name) {
			    for (idx_t i = 0; i < group_sources.Count(); i++) {
				    if (group_sources.GetValue(0, i).ToString() == group &&
				        StringUtil::CIEquals(group_sources.GetValue(1, i).ToString(), name)) {
					    return true;
				    }
			    }
			    return false;
		    };
		    auto depends_on_removed = [&](const string &edge_scope, const string &dep,
		                                  const std::pair<string, string> &r) {
			    if (!StringUtil::CIEquals(dep, r.second)) {
				    return false;
			    }
			    if (edge_scope == r.first) {
				    return true;
			    }
			    return r.first.empty() && !group_has(edge_scope, dep);
		    };
		    auto removing = [&](const string &item_scope, const string &name) {
			    return std::find_if(removed.begin(), removed.end(), [&](const std::pair<string, string> &r) {
				           return r.first == item_scope && StringUtil::CIEquals(r.second, name);
			           }) != removed.end();
		    };
		    // a group's source whose name the cluster's keeps: its dependents fall back to the cluster's
		    auto keeps_fallback = [&](const std::pair<string, string> &r) {
			    return !r.first.empty() && !removing("", r.second) &&
			           !CurrentSpec(read, *catalog, "", "source", r.second).empty();
		    };
		    auto walk_dependents = [&](idx_t at) {
			    auto of = removed[at]; // a copy: emplace_back below may move the vector
			    if (keeps_fallback(of)) {
				    return;
			    }
			    for (idx_t i = 0; i < edges.Count(); i++) {
				    auto edge_scope = edges.GetValue(0, i).ToString();
				    auto dependent = edges.GetValue(1, i).ToString();
				    if (!depends_on_removed(edge_scope, edges.GetValue(2, i).ToString(), of) ||
				        removing(edge_scope, dependent)) {
					    continue;
				    }
				    if (!cascade) {
					    throw BinderException("acl cluster: source \"%s\"%s DEPENDS ON \"%s\" - detach it first, or "
					                          "DETACH %s CASCADE",
					                          dependent, edge_scope.empty() ? string() : " of group " + edge_scope,
					                          of.second, alias);
				    }
				    removed.emplace_back(edge_scope, dependent);
			    }
		    };
		    // to a fixpoint: a group source skipped because the cluster's of its name stays loses that
		    // fallback when the walk reaches the cluster's later - then its dependents go too
		    idx_t walked = 0;
		    do {
			    walked = removed.size();
			    for (idx_t at = 0; at < removed.size(); at++) {
				    walk_dependents(at);
			    }
		    } while (removed.size() != walked);
		    for (auto &r : removed) {
			    if (!r.first.empty() && !removing("", r.second) &&
			        !CurrentSpec(read, *catalog, "", "source", r.second).empty()) {
				    falls_back.insert(key(r));
			    }
			    bool here = r.first.empty() ? node_group.empty() || !group_has(node_group, r.second) ||
			                                      removing(node_group, r.second)
			                                : r.first == node_group;
			    if (here) {
				    in_effect.insert(key(r));
			    }
		    }
		    // the policy reads through it: the policy goes first (design/017 §3.2a)
		    vector<string> readers;
		    for (auto &r : removed) {
			    if (falls_back.count(key(r))) {
				    continue; // the cluster's source of that name stays: the policy still reads through it
			    }
			    auto &source = r.second;
			    // a physical name as stored: bare (src.main.t) or quoted ("src".main.t)
			    auto lower = StringUtil::Lower(source);
			    auto through = [&](const char *column) {
				    // spec 116: folded as StringUtil::Lower folded `lower` - ASCII, never SQL's Unicode lower()
				    auto c = KeyFoldSql(string("\"") + column + "\"");
				    return "(" + c + " LIKE " + Lit(lower + ".%") + " OR " + c + " LIKE " + Lit("\"" + lower + "\".%") +
				           " OR " + c + " = " + Lit(lower) + " OR " + c + " = " + Lit("\"" + lower + "\"") + ")";
			    };
			    auto rel = read("SELECT \"vcat\" || '.' || \"vname\" FROM " + catalog->Tbl("relations") + " WHERE " +
			                    through("phys"));
			    auto sch = read("SELECT \"vcat\" || '.' || \"path\" FROM " + catalog->Tbl("schemas") + " WHERE " +
			                    through("phys_path"));
			    auto fun = read("SELECT \"vcat\" || '.' || \"vname\" FROM " + catalog->Tbl("functions") + " WHERE " +
			                    through("target"));
			    for (auto *result : {rel.get(), sch.get(), fun.get()}) {
				    ResultRows rows(*result);
				    for (idx_t i = 0; i < rows.Count(); i++) {
					    readers.push_back(rows.GetValue(0, i).ToString());
				    }
			    }
			    // spec 118: a virtual view or a macro whose body reads the source
			    auto views =
			        read("SELECT \"vcat\" || '.' || \"vname\", \"view_sql\" FROM " + catalog->Tbl("relations") +
			             " WHERE \"view_sql\" IS NOT NULL UNION ALL SELECT \"vcat\" || '.' || \"vname\", "
			             "\"template\" FROM " +
			             catalog->Tbl("functions") + " WHERE \"template\" IS NOT NULL");
			    ResultRows view_rows(*views);
			    for (idx_t i = 0; i < view_rows.Count(); i++) {
				    if (SqlNamesSource(view_rows.GetValue(1, i).ToString(), source)) {
					    readers.push_back(view_rows.GetValue(0, i).ToString());
				    }
			    }
		    }
		    if (!readers.empty() && !force) {
			    throw BinderException("acl cluster: the policy reads through %s: %s - remove those first, or DETACH %s "
			                          "FORCE (acl_check_catalog then reports them as source_missing)",
			                          StringUtil::Join(Names(removed), ", "), StringUtil::Join(readers, ", "), alias);
		    }
		    for (auto &r : removed) {
			    statements.push_back(DeleteItem(*catalog, r.first, "source", r.second));
			    statements.push_back("DELETE FROM " + catalog->Tbl("cluster_deps") + " WHERE \"scope\" = " +
			                         Lit(r.first) + " AND lower(\"name\") = lower(" + Lit(r.second) + ")");
		    }
	    },
	    "config_version",
	    [&](int64_t) {
		    // dependents first; only what is in effect on this node, and never a name the cluster's item
		    // takes back on it (a re-point, the agent's through drain). The source itself may be another
		    // group's, or a cluster item this node's group overrides, while what depends on it is here.
		    vector<string> busy;
		    vector<string> repointed;
		    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
			    if (!in_effect.count(key(*it)) || !NodeHasDatabase(*db, it->second)) {
				    continue;
			    }
			    if (falls_back.count(key(*it))) {
				    repointed.push_back(it->second);
				    continue;
			    }
			    ForgetLazySource(*db, it->second); // spec 035 stage 2: no statement attaches it again meanwhile
			    Connection con(*db);
			    auto result = con.Query("DETACH " + Ident(it->second));
			    if (result->HasError()) {
				    busy.push_back(it->second);
			    }
		    }
		    bool head_here = in_effect.count(key(removed[0])) > 0;
		    if (!head_here) {
			    NotThisNodesScope(*db, scope, answer, [&](const string &) { return true; });
		    }
		    answer.applied_here = head_here && busy.empty() && repointed.empty();
		    if (removed.size() > 1) {
			    auto names = Names(removed);
			    answer.note += string(answer.note.empty() ? "" : "; ") + "detached with it: " +
			                   StringUtil::Join(vector<string>(names.begin() + 1, names.end()), ", ");
		    }
		    if (!repointed.empty()) {
			    answer.item_class = "drain";
			    answer.note += string(answer.note.empty() ? "" : "; ") +
			                   "the cluster's source of that name is in "
			                   "effect again on this node: " +
			                   StringUtil::Join(repointed, ", ") + " - the node reports it pending for a drain";
		    }
		    if (!busy.empty()) {
			    answer.item_class = "drain";
			    answer.note += string(answer.note.empty() ? "" : "; ") +
			                   "in use on this node: " + StringUtil::Join(busy, ", ") +
			                   " - the node reports it pending for a drain";
		    }
	    });
	RefreshLazySourcesAfterWrite(*this, *db);
	return answer;
}

//! hugr-node spec 035: a group's source set - `set` the aliases its nodes attach (the cluster's or the group's own
//! sources), `reset` it (every source again). One item per group, kind `sources`; a cluster-wide one is refused: the
//! cluster attaches every source. Its nodes converge at their next tick: what left is detached (a source in use waits
//! for a drain), what is listed and missing is attached.
static PolicyStore::ClusterAnswer ClusterSources(PolicyStore &store, const string &verb, const string &scope,
                                                 const string &names_json, bool lazy, const string &idle) {
	RequireClusterCatalog(store.catalog, "acl_cluster_sources");
	auto &catalog = *store.catalog;
	auto v = StringUtil::Lower(verb);
	if (v != "set" && v != "reset") {
		throw BinderException("acl_cluster_sources: the verb is set or reset, not \"%s\"", verb);
	}
	if (scope.empty()) {
		throw BinderException("acl cluster: a source set is a group's - write it IN GROUP <group>: the cluster "
		                      "attaches every source");
	}
	auto db = store.instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	// spec 035 stage 2b: ALL names every source - only with LAZY (without it, it is RESET SOURCES)
	bool all = v == "set" && StringUtil::CIEquals(names_json, "all");
	if (all && !lazy) {
		throw BinderException("acl cluster: SOURCES ALL is written with LAZY - every source attached at once is what "
		                      "RESET SOURCES gives the group");
	}
	if (v == "reset" && (lazy || !idle.empty())) {
		throw BinderException("acl cluster: RESET SOURCES takes no LAZY - it gives the group every source again");
	}
	auto idle_seconds = IdleSecondsOf(*db, lazy, idle);
	vector<string> names;
	if (v == "set" && !all) {
		using namespace duckdb_yyjson; // NOLINT
		auto doc = yyjson_read(names_json.c_str(), names_json.size(), 0);
		auto root = doc ? yyjson_doc_get_root(doc) : nullptr;
		if (!root || !yyjson_is_arr(root)) {
			if (doc) {
				yyjson_doc_free(doc);
			}
			throw BinderException("acl_cluster_sources: the sources are a JSON array of aliases");
		}
		case_insensitive_set_t seen;
		size_t i, n;
		yyjson_val *value;
		yyjson_arr_foreach(root, i, n, value) {
			if (!yyjson_is_str(value)) {
				continue;
			}
			string name = yyjson_get_str(value);
			if (seen.insert(name).second) {
				names.push_back(name);
			}
		}
		yyjson_doc_free(doc);
		if (names.empty()) {
			throw BinderException("acl cluster: a source set names at least one source - RESET SOURCES gives the "
			                      "group every source again");
		}
	}
	PolicyStore::ClusterAnswer answer;
	answer.item_class = "hot";
	answer.version = catalog.WriteWithReads(
	    [&](const ReadFn &read, vector<string> &statements) {
		    RequireScope(read, catalog, scope);
		    auto current = CurrentSpec(read, catalog, scope, "sources", "sources");
		    if (v == "reset") {
			    if (current.empty()) {
				    throw BinderException("acl cluster: group \"%s\" declares no source set", scope);
			    }
			    statements.push_back(DeleteItem(catalog, scope, "sources", "sources"));
			    return;
		    }
		    if (all) {
			    statements.push_back(UpsertItem(catalog, scope, "sources", "sources",
			                                    "{" + acl::JsonQuote("all") + ": " + acl::JsonQuote("true") +
			                                        LazyMarks(true, idle_seconds) + "}",
			                                    answer.item_class, "", !current.empty()));
			    return;
		    }
		    vector<string> json;
		    for (auto &name : names) {
			    // a source of the cluster or of this group, by its alias as written there
			    auto found = read("SELECT \"name\" FROM " + catalog.Tbl("cluster_items") +
			                      " WHERE \"kind\" = 'source' AND \"scope\" IN ('', " + Lit(scope) +
			                      ") AND lower(\"name\") = lower(" + Lit(name) + ")");
			    if (found->RowCount() == 0) {
				    throw BinderException("acl cluster: \"%s\" is no source of the cluster's nor of group \"%s\"'s",
				                          name, scope);
			    }
			    json.push_back(acl::JsonQuote(found->Collection().GetValue(0, 0).ToString()));
		    }
		    // a listed source that loads through another (DEPENDS ON) needs it attached too
		    case_insensitive_set_t listed(names.begin(), names.end());
		    for (auto &name : names) {
			    // the item the group's nodes read: its own of that name, else the cluster's
			    auto owner = CurrentSpec(read, catalog, scope, "source", name).empty() ? string() : scope;
			    RequireListedDependencies(read, catalog, scope, owner, name, listed);
		    }
		    statements.push_back(
		        UpsertItem(catalog, scope, "sources", "sources",
		                   "{\"names\": [" + StringUtil::Join(json, ", ") + "]" + LazyMarks(lazy, idle_seconds) + "}",
		                   answer.item_class, "", !current.empty()));
	    },
	    "config_version", [](int64_t) {});
	if (v == "reset") {
		answer.note = "the group's nodes attach every source again from their next tick";
	} else if (lazy) {
		answer.note = string(all ? "the group's nodes attach every source"
		                         : "the group's nodes attach only these "
		                           "sources") +
		              ", each when a statement first reads it" +
		              (idle_seconds > 0 ? ", detached after it idles" : ", never detached by itself") +
		              " - a source in a DEPENDS ON chain is attached at once";
	} else {
		answer.note = "the group's nodes attach only these sources from their next tick - what left is detached (a "
		              "source in use waits for a drain)";
	}
	RefreshLazySourcesAfterWrite(store, *db);
	return answer;
}

PolicyStore::ClusterAnswer PolicyStore::ClusterSetting(const string &verb, const string &scope, const string &name,
                                                       const string &value) {
	RequireClusterCatalog(catalog, "acl_cluster_setting");
	auto db = instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	auto v = StringUtil::Lower(verb);
	if (v != "set" && v != "reset") {
		throw BinderException("acl_cluster_setting: the verb is set or reset, not \"%s\"", verb);
	}
	if (HARDENING_SETTINGS.count(name)) {
		throw BinderException("acl cluster: \"%s\" is the node's hardening, fixed by its bootstrap - never a "
		                      "cluster profile item",
		                      name);
	}
	if (StringUtil::CIEquals(name, "acl_node_group")) {
		// spec 096: which group a node is in is its deployment's, never the policy's - a profile item
		// would let a catalog write move nodes between groups
		throw BinderException("acl cluster: \"acl_node_group\" is the node's own (its deployment sets it) - never a "
		                      "cluster profile item");
	}
	if (v == "set" && !scope.empty() && CLUSTER_WIDE_SETTINGS.count(name)) {
		throw BinderException("acl cluster: \"%s\" is where the cluster's telemetry goes - one destination for the "
		                      "whole cluster: write it without IN GROUP",
		                      name);
	}
	LintKey(name, "a setting's name");
	auto known = NodeQuery(*db, "SELECT scope FROM duckdb_settings() WHERE lower(name) = lower(" + Quoted(name) + ")");
	if (known->RowCount() == 0) {
		throw BinderException("acl cluster: \"%s\" is not a setting of this node", name);
	}
	if (!StringUtil::CIEquals(known->Collection().GetValue(0, 0).ToString(), "GLOBAL")) {
		throw BinderException("acl cluster: \"%s\" is not a GLOBAL setting - the profile carries the node's settings, "
		                      "not a session's",
		                      name);
	}
	ClusterAnswer answer;
	answer.item_class = "restart";
	answer.version = catalog->WriteWithReads(
	    [&](const ReadFn &read, vector<string> &statements) {
		    RequireScope(read, *catalog, scope);
		    auto current = CurrentSpec(read, *catalog, scope, "setting", name);
		    if (v == "reset") {
			    if (current.empty()) {
				    throw BinderException("acl cluster: setting \"%s\" is not in the profile", name);
			    }
			    statements.push_back(DeleteItem(*catalog, scope, "setting", name));
			    return;
		    }
		    statements.push_back(UpsertItem(*catalog, scope, "setting", StringUtil::Lower(name),
		                                    JsonObjectOf({{"value", value}}), answer.item_class, "", !current.empty()));
	    },
	    "config_version",
	    [&](int64_t) {
		    if (NotThisNodesScope(*db, scope, answer, [&](const string &group) {
			        return GroupHasItem(*catalog, group, "setting", StringUtil::Lower(name));
		        })) {
			    return;
		    }
		    answer.note = "restart class - the node's configuration is locked after its bootstrap; each node reports "
		                  "the change pending until it restarts (the orchestrator rolls it)";
	    });
	return answer;
}

//! spec 010 review: an extension's setting the scratch cannot render (it lacks the extension) is the same value when
//! it reads as the same boolean (`yes` as written, `true` on the node) or the same number (`2`, `2.0`)
static bool SameValue(const string &node_value, const string &wanted) {
	bool a = false;
	bool b = false;
	if (TryCastStringBool(node_value.c_str(), node_value.size(), a, false) &&
	    TryCastStringBool(wanted.c_str(), wanted.size(), b, false)) {
		return a == b;
	}
	double x = 0;
	double y = 0;
	return TryCast::Operation<string_t, double>(string_t(node_value), x) &&
	       TryCast::Operation<string_t, double>(string_t(wanted), y) && x == y;
}

//! a setting that starts threads when it is set: never set on the scratch (a profile's `1e6` would start a million
//! there on every tick) - its value is a number, compared as one
static bool StartsThreads(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "threads" || lower == "worker_threads" || lower == "external_threads";
}

vector<PolicyStore::DriftRow> PolicyStore::ClusterDrift() {
	RequireClusterCatalog(catalog, "acl_platform_drift");
	auto db = instance.lock();
	if (!db) {
		throw BinderException("acl cluster: the instance is gone");
	}
	vector<DriftRow> out;
	auto effective = ClusterEffective();
	case_insensitive_map_t<const ClusterItem *> profile[3]; // setting, extension, source
	auto slot = [](const string &kind) {
		return kind == "setting" ? 0 : kind == "extension" ? 1 : 2;
	};
	for (auto &item : effective) {
		// spec 010 review: an extension by its canonical name - `postgres` is loaded as `postgres_scanner`
		auto name = item.kind == "extension" ? ExtensionHelper::ApplyExtensionAlias(item.name) : item.name;
		profile[slot(item.kind)][name] = &item;
	}
	// databases: bootstrap (the default, the policy's, a secrets service) or compared with the sources
	string default_db;
	{
		auto current = NodeQuery(*db, "SELECT current_database()");
		if (current->RowCount() > 0) {
			default_db = current->Collection().GetValue(0, 0).ToString();
		}
	}
	auto databases = NodeQuery(*db, "SELECT database_name, coalesce(path, ':memory:'), type FROM duckdb_databases() "
	                                "WHERE NOT internal");
	ResultRows database_rows(*databases);
	// review 118.2: a path is shown only for a file database - another type's path may be a DSN the operator
	// wrote with its credential (an ACL NATIVE ATTACH); a source is compared by its type (duckdb shows a
	// relative path absolute and a ducklake one without its prefix, so a path compare would say differs)
	auto shown = [](const string &type, const string &path) {
		return StringUtil::CIEquals(type, "duckdb") || StringUtil::CIEquals(type, "sqlite") ? path : "(" + type + ")";
	};
	case_insensitive_set_t on_node;
	// spec 010 review: the policy catalog's extension is loaded at the bootstrap's step 5, before the profile can be
	// read - its version is the repository's then, whatever an item says, and a restart repeats that order
	string catalog_extension;
	for (idx_t i = 0; i < database_rows.Count(); i++) {
		auto name = database_rows.GetValue(0, i).ToString();
		auto path = database_rows.GetValue(1, i).ToString();
		auto type = database_rows.GetValue(2, i).ToString();
		on_node.insert(name);
		if (catalog && StringUtil::CIEquals(name, catalog->db_name)) {
			catalog_extension = ExtensionOfType(type);
		}
		auto source = profile[2].find(name);
		if (source != profile[2].end()) {
			auto wanted = JsonField(source->second->spec, "type");
			bool same = StringUtil::CIEquals(wanted.empty() ? "duckdb" : wanted, type) ||
			            StringUtil::CIEquals(ExtensionOfType(wanted), type);
			out.push_back({"database", name, same ? "same" : "differs", shown(type, path),
			               JsonField(source->second->spec, "path"), source->second->scope});
			continue;
		}
		bool bootstrap = StringUtil::CIEquals(name, default_db) || StringUtil::CIEquals(type, "tresor") ||
		                 (catalog && StringUtil::CIEquals(name, catalog->db_name));
		out.push_back({"database", name, bootstrap ? "bootstrap" : "node_only", shown(type, path), "", ""});
	}
	for (auto &source : profile[2]) {
		if (!on_node.count(source.first)) {
			// spec 035 stage 2: a lazy source not attached is as the profile wants it - attached on demand
			int64_t idle_seconds = 0;
			out.push_back({"database", source.first,
			               ClusterSourceLazy(*source.second, idle_seconds) ? "lazy" : "profile_only", "",
			               JsonField(source.second->spec, "path"), source.second->scope});
		}
	}
	// extensions: the loaded ones; statically linked and acl itself are the build's
	auto extensions = NodeQuery(*db, "SELECT extension_name, coalesce(extension_version, ''), install_mode::VARCHAR "
	                                 "FROM duckdb_extensions() WHERE loaded");
	ResultRows extension_rows(*extensions);
	case_insensitive_set_t loaded;
	for (idx_t i = 0; i < extension_rows.Count(); i++) {
		auto name = extension_rows.GetValue(0, i).ToString();
		auto version = extension_rows.GetValue(1, i).ToString();
		auto mode = extension_rows.GetValue(2, i).ToString();
		loaded.insert(name);
		auto item = profile[1].find(name);
		if (!catalog_extension.empty() && StringUtil::CIEquals(name, catalog_extension)) {
			out.push_back({"extension", name, "bootstrap", version,
			               item == profile[1].end() ? string() : JsonField(item->second->spec, "version"),
			               item == profile[1].end() ? string() : item->second->scope});
			continue;
		}
		if (item != profile[1].end()) {
			auto wanted = JsonField(item->second->spec, "version");
			bool same = wanted.empty() || StringUtil::CIEquals(wanted, version);
			out.push_back({"extension", name, same ? "same" : "differs", version, wanted, item->second->scope});
			continue;
		}
		bool bootstrap = StringUtil::CIEquals(name, "acl") || StringUtil::Contains(StringUtil::Upper(mode), "STATIC");
		out.push_back({"extension", name, bootstrap ? "bootstrap" : "node_only", version, "", ""});
	}
	for (auto &item : profile[1]) {
		if (!loaded.count(item.first)) {
			out.push_back({"extension", item.first, "profile_only", "", JsonField(item.second->spec, "version"),
			               item.second->scope});
		}
	}
	// settings: the profile's compared (a setting the profile does not name has no "desired" value to be
	// told from), and the deployment's own listed as bootstrap
	if (!profile[0].empty()) {
		// one read of the node's values; the profile's value as duckdb would show it - set on a scratch
		// instance (`4GB` reads back as `3.7 GiB`) - else as written (an extension's setting the scratch lacks)
		case_insensitive_map_t<string> node_values;
		auto values = NodeQuery(*db, "SELECT name, value FROM duckdb_settings()");
		ResultRows value_rows(*values);
		for (idx_t i = 0; i < value_rows.Count(); i++) {
			auto value = value_rows.GetValue(1, i);
			node_values[value_rows.GetValue(0, i).ToString()] = value.IsNull() ? string() : value.ToString();
		}
		DBConfig scratch_config; // no extension - acl itself never (a second store and override)
		scratch_config.options.load_extensions = false;
		scratch_config.SetOptionByName("threads", Value::BIGINT(1));
		scratch_config.SetOptionByName("enable_external_access", Value::BOOLEAN(false));
		scratch_config.SetOptionByName("autoload_known_extensions", Value::BOOLEAN(false));
		scratch_config.SetOptionByName("autoinstall_known_extensions", Value::BOOLEAN(false));
		DuckDB scratch(nullptr, &scratch_config);
		Connection scratch_con(scratch);
		for (auto &item : profile[0]) {
			auto value = node_values[item.first];
			auto wanted = JsonField(item.second->spec, "value");
			auto shown_wanted = wanted;
			auto set = StartsThreads(item.first)
			               ? unique_ptr<QueryResult>()
			               : scratch_con.Query("SET GLOBAL " + Ident(item.first) + " = " + Quoted(wanted));
			if (set && !set->HasError()) {
				// duckdb_settings(), not current_setting(): the scratch loads no core_functions
				auto read = scratch_con.Query("SELECT value FROM duckdb_settings() WHERE name = " + Quoted(item.first));
				if (!read->HasError() && read->RowCount() > 0) {
					shown_wanted = read->Collection().GetValue(0, 0).ToString();
				}
			}
			bool same = StringUtil::CIEquals(value, shown_wanted) || StringUtil::CIEquals(value, wanted) ||
			            SameValue(value, wanted);
			out.push_back({"setting", item.first, same ? "same" : "differs", value, wanted, item.second->scope});
		}
	}
	for (auto name : {"acl_node_group"}) {
		Value value;
		db->TryGetCurrentSetting(name, value);
		out.push_back({"setting", name, "bootstrap", value.IsNull() ? string() : value.ToString(), "", ""});
	}
	return out;
}

bool PolicyStore::NodeGroupKnown() {
	// the catalog judged like any read of it (spec 094): one this build may not read throws, and the
	// report says neither the group nor the target
	if (catalog && !catalog->FunctionMode()) {
		catalog->EnsureFresh();
	}
	auto group = NodeGroup();
	if (group.empty()) {
		return true;
	}
	if (!catalog || catalog->FunctionMode()) {
		return false;
	}
	return catalog->Query("SELECT 1 FROM " + catalog->Tbl("resource_groups") + " WHERE \"group\" = " + Lit(group))
	           ->RowCount() > 0;
}

vector<PolicyStore::ClusterItem> PolicyStore::ClusterEffective() {
	// the cluster's items, then this node's group's over them - one row per kind and name, in the
	// profile's order (settings, extensions, sources), each with the scope it came from
	auto group = NodeGroup();
	vector<ClusterItem> out;
	case_insensitive_map_t<idx_t> at;
	for (auto &item : ClusterItems(string())) {
		if (!item.scope.empty() && item.scope != group) {
			continue;
		}
		auto key = item.kind + "\x1f" + item.name;
		auto found = at.find(key);
		if (found == at.end()) {
			at[key] = out.size();
			out.push_back(item);
		} else if (!item.scope.empty()) {
			out[found->second] = item; // the group's item overrides the cluster's
		}
	}
	// hugr-node spec 035: a group that declares its sources attaches only those - its `sources` item is no item to
	// apply, the filter it is
	case_insensitive_set_t declared;
	bool partial = false;
	bool lazy_set = false; // spec 035 stage 2b: the set's sources are attached on demand
	int64_t lazy_idle = 0;
	for (auto &item : out) {
		if (item.kind == "sources" && !item.scope.empty()) {
			partial = !SourceSetAll(item.spec);
			if (partial) {
				declared = SourceSetNames(item.spec);
			}
			lazy_set = JsonField(item.spec, "lazy") == "true";
			auto idle = JsonField(item.spec, "idle_seconds");
			int64_t seconds = 0;
			lazy_idle = !idle.empty() && TryCast::Operation<string_t, int64_t>(string_t(idle), seconds) ? seconds : 0;
		}
	}
	vector<ClusterItem> kept;
	for (auto &item : out) {
		if (item.kind == "sources" || (partial && item.kind == "source" && !declared.count(item.name))) {
			continue;
		}
		kept.push_back(std::move(item));
	}
	out = std::move(kept);
	if (lazy_set) {
		// a source in a DEPENDS ON chain is attached at once: what loads through another needs it attached first; so is
		// the secrets service, which no statement reads (a SECRET of another source does)
		case_insensitive_set_t chained;
		for (auto &item : out) {
			if (item.kind == "source" && !item.depends_on.empty()) {
				chained.insert(item.name);
				chained.insert(item.depends_on.begin(), item.depends_on.end());
			}
		}
		for (auto &item : out) {
			if (item.kind != "source" || chained.count(item.name) ||
			    IsSecretsServiceSource(JsonField(item.spec, "type"), JsonField(item.spec, "path"))) {
				continue;
			}
			// a set that states no IDLE keeps the source's own
			int64_t own_idle = 0;
			ClusterSourceLazy(item, own_idle);
			item.spec = WithLazyMarks(item.spec, true, lazy_idle > 0 ? lazy_idle : own_idle);
		}
	}
	// the profile's order - settings, extensions, sources - whichever scope each came from: a group's own
	// extension must come before the cluster's source that loads through it
	auto rank = [](const string &kind) {
		return kind == "setting" ? 0 : kind == "extension" ? 1 : 2;
	};
	std::stable_sort(out.begin(), out.end(),
	                 [&](const ClusterItem &a, const ClusterItem &b) { return rank(a.kind) < rank(b.kind); });
	return out;
}

int64_t PolicyStore::ClusterVersion() {
	RequireClusterCatalog(catalog, "acl_cluster_version");
	auto result = catalog->Query("SELECT CAST(\"value\" AS BIGINT) FROM " + catalog->Tbl("meta") +
	                             " WHERE \"key\" = 'config_version'");
	return result->RowCount() ? result->Collection().GetValue(0, 0).GetValue<int64_t>() : 0;
}

vector<PolicyStore::ClusterItem> PolicyStore::ClusterItems(const string &scope) {
	RequireClusterCatalog(catalog, "acl_cluster_items");
	auto where = scope.empty() ? string() : " WHERE i.\"scope\" = " + Lit(scope);
	auto result = catalog->Query(
	    "SELECT i.\"scope\", i.\"kind\", i.\"name\", i.\"spec\", i.\"class\", i.\"version\", i.\"comment\", "
	    "(SELECT string_agg(d.\"depends_on\", ',' ORDER BY d.\"depends_on\") FROM " +
	    catalog->Tbl("cluster_deps") +
	    " d WHERE d.\"scope\" = i.\"scope\" AND lower(d.\"name\") = lower(i.\"name\")) " + "FROM " +
	    catalog->Tbl("cluster_items") + " i" + where +
	    " ORDER BY i.\"scope\", CASE i.\"kind\" WHEN 'setting' THEN 0 WHEN 'extension' THEN 1 ELSE 2 END, i.\"pos\"");
	ResultRows rows(*result);
	vector<ClusterItem> out;
	for (idx_t i = 0; i < rows.Count(); i++) {
		ClusterItem item;
		item.scope = rows.GetValue(0, i).ToString();
		item.kind = rows.GetValue(1, i).ToString();
		item.name = rows.GetValue(2, i).ToString();
		item.spec = rows.GetValue(3, i).ToString();
		item.item_class = rows.GetValue(4, i).ToString();
		item.version = rows.GetValue(5, i).GetValue<int64_t>();
		auto comment = rows.GetValue(6, i);
		item.comment = comment.IsNull() ? string() : comment.ToString();
		auto deps = rows.GetValue(7, i);
		if (!deps.IsNull()) {
			item.depends_on = StringUtil::Split(deps.ToString(), ',');
		}
		out.push_back(std::move(item));
	}
	return out;
}

//===--------------------------------------------------------------------===//
// The SQL surface
//===--------------------------------------------------------------------===//

namespace {

LogicalType AnswerType() {
	child_list_t<LogicalType> children {{"version", LogicalType::BIGINT},
	                                    {"class", LogicalType::VARCHAR},
	                                    {"applied_here", LogicalType::BOOLEAN},
	                                    {"message", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(std::move(children));
}

Value AnswerValue(const PolicyStore::ClusterAnswer &answer) {
	child_list_t<Value> children {{"version", Value::BIGINT(answer.version)},
	                              {"class", Value(answer.item_class)},
	                              {"applied_here", Value::BOOLEAN(answer.applied_here)},
	                              {"message", answer.note.empty() ? Value(LogicalType::VARCHAR) : Value(answer.note)}};
	return Value::STRUCT(std::move(children));
}

string Arg(DataChunk &args, idx_t column, idx_t row) {
	if (column >= args.ColumnCount()) {
		return string();
	}
	auto value = args.data[column].GetValue(row);
	return value.IsNull() ? string() : value.ToString();
}

template <class FN>
void EachRow(DataChunk &args, Vector &result, FN &&fn) {
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, AnswerValue(fn(row)));
	}
}

//! acl_cluster_extension(verb, scope, name, version, repository, comment)
void ClusterExtensionFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	EachRow(args, result, [&](idx_t row) {
		return StoreOf(state).ClusterExtension(Arg(args, 0, row), Arg(args, 1, row), Arg(args, 2, row),
		                                       Arg(args, 3, row), Arg(args, 4, row), Arg(args, 5, row));
	});
}

bool LazyFlag(const string &text) {
	if (text.empty() || StringUtil::CIEquals(text, "false")) {
		return false;
	}
	if (StringUtil::CIEquals(text, "true")) {
		return true;
	}
	throw BinderException("acl cluster: lazy is 'true' or 'false' - not '%s'", text);
}

//! acl_cluster_attach(scope, alias, path, type, secret, options_json, depends_on_csv, comment[, lineage[, lazy[,
//! idle]]]) - lazy 'true' marks the source LAZY (spec 035 stage 2), idle its IDLE interval
void ClusterAttachFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	EachRow(args, result, [&](idx_t row) {
		vector<string> deps;
		auto csv = Arg(args, 6, row);
		if (!csv.empty()) {
			for (auto &dep : StringUtil::Split(csv, ',')) {
				auto d = dep;
				StringUtil::Trim(d);
				deps.push_back(d); // an empty one is refused where the sources are judged
			}
		}
		return StoreOf(state).ClusterAttach(Arg(args, 0, row), Arg(args, 1, row), Arg(args, 2, row), Arg(args, 3, row),
		                                    Arg(args, 4, row), Arg(args, 5, row), deps, Arg(args, 7, row),
		                                    args.ColumnCount() > 8 ? Arg(args, 8, row) : string(),
		                                    args.ColumnCount() > 9 && LazyFlag(Arg(args, 9, row)),
		                                    args.ColumnCount() > 10 ? Arg(args, 10, row) : string());
	});
}

//! acl_cluster_detach(scope, alias, cascade, force)
void ClusterDetachFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	EachRow(args, result, [&](idx_t row) {
		return StoreOf(state).ClusterDetach(Arg(args, 0, row), Arg(args, 1, row),
		                                    StringUtil::CIEquals(Arg(args, 2, row), "true"),
		                                    StringUtil::CIEquals(Arg(args, 3, row), "true"));
	});
}

//! acl_cluster_setting(verb, scope, name, value)
void ClusterSettingFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	EachRow(args, result, [&](idx_t row) {
		return StoreOf(state).ClusterSetting(Arg(args, 0, row), Arg(args, 1, row), Arg(args, 2, row),
		                                     Arg(args, 3, row));
	});
}

//! acl_cluster_sources(verb, scope, names_json[, lazy[, idle]]) - names_json 'ALL' (with lazy 'true') every source
void ClusterSourcesFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	EachRow(args, result, [&](idx_t row) {
		return ClusterSources(StoreOf(state), Arg(args, 0, row), Arg(args, 1, row), Arg(args, 2, row),
		                      args.ColumnCount() > 3 && LazyFlag(Arg(args, 3, row)),
		                      args.ColumnCount() > 4 ? Arg(args, 4, row) : string());
	});
}

void ClusterVersionFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::BIGINT(StoreOf(state).ClusterVersion()), count_t(args.size()));
}

struct ClusterInfo : public TableFunctionInfo {
	explicit ClusterInfo(shared_ptr<PolicyStore> store_p) : store(std::move(store_p)) {
	}
	shared_ptr<PolicyStore> store;
};

struct ItemsBind : public TableFunctionData {
	shared_ptr<PolicyStore> store;
	string scope;
};

struct ItemsState : public GlobalTableFunctionState {
	vector<PolicyStore::ClusterItem> rows;
	idx_t next = 0;
};

unique_ptr<FunctionData> ItemsBindFn(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                     vector<Identifier> &names) {
	for (auto name : {"scope", "kind", "name", "spec", "class", "version", "depends_on", "comment"}) {
		names.push_back(Identifier(name));
	}
	types = {LogicalType::VARCHAR,
	         LogicalType::VARCHAR,
	         LogicalType::VARCHAR,
	         LogicalType::VARCHAR,
	         LogicalType::VARCHAR,
	         LogicalType::BIGINT,
	         LogicalType::LIST(LogicalType::VARCHAR),
	         LogicalType::VARCHAR};
	auto bind = make_uniq<ItemsBind>();
	bind->store = input.info->Cast<ClusterInfo>().store;
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		bind->scope = input.inputs[0].ToString();
	}
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> ItemsInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ItemsBind>();
	auto state = make_uniq<ItemsState>();
	state->rows = bind.store->ClusterItems(bind.scope);
	return std::move(state);
}

void ItemsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ItemsState>();
	idx_t count = 0;
	while (state.next < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.next++];
		output.data[0].SetValue(count, Value(row.scope));
		output.data[1].SetValue(count, Value(row.kind));
		output.data[2].SetValue(count, Value(row.name));
		output.data[3].SetValue(count, Value(row.spec));
		output.data[4].SetValue(count, Value(row.item_class));
		output.data[5].SetValue(count, Value::BIGINT(row.version));
		vector<Value> deps;
		for (auto &d : row.depends_on) {
			deps.emplace_back(d);
		}
		output.data[6].SetValue(count, Value::LIST(LogicalType::VARCHAR, std::move(deps)));
		output.data[7].SetValue(count, row.comment.empty() ? Value(LogicalType::VARCHAR) : Value(row.comment));
		count++;
	}
	output.SetChildCardinality(count);
}

unique_ptr<FunctionData> EffectiveBindFn(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &types, vector<Identifier> &names) {
	return ItemsBindFn(context, input, types, names);
}

unique_ptr<GlobalTableFunctionState> EffectiveInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ItemsBind>();
	auto state = make_uniq<ItemsState>();
	state->rows = bind.store->ClusterEffective();
	return std::move(state);
}

//! spec 035 stage 3: acl_cluster_group_sources([group]) - per resource group, the sources of the profile its roles
//! reach (a grant, a relation, an alias, a body - acl_exposures, as the source gate reads them), whether its source set
//! lists each (NULL: no list - every source), and for a reached source the list leaves out the statement that lists it.
//! Never applied by itself (the owner, 2026-11-16 and 2026-11-20): the operator writes it
//! a name as the grammar reads it back: bare when duckdb's identifier rule takes it, else double-quoted
string QuotedIfNeeded(const string &name) {
	return SQLIdentifier::ToString(name);
}

struct GroupSourceRow {
	string group, source;
	vector<string> roles;
	int listed = -1; // -1 = no list
	string repair;
};

vector<GroupSourceRow> GroupSources(PolicyStore &store, const string &only_group) {
	RequireClusterCatalog(store.catalog, "acl_cluster_group_sources");
	auto &catalog = *store.catalog;
	auto db = store.instance.lock();
	if (!db) {
		throw BinderException("acl_cluster_group_sources: the instance is gone");
	}
	auto rows_of = [&](const string &sql) {
		auto result = catalog.Query(sql);
		vector<vector<Value>> out;
		ResultRows rows(*result);
		for (idx_t r = 0; r < rows.Count(); r++) {
			vector<Value> row;
			for (idx_t c = 0; c < result->ColumnCount(); c++) {
				row.push_back(rows.GetValue(c, r));
			}
			out.push_back(std::move(row));
		}
		return out;
	};
	// the groups: the one asked, else every group whose set is a list (where a reached source can be left out)
	vector<std::pair<string, bool>> groups; // name, is_default
	for (auto &row : rows_of("SELECT g.\"group\", coalesce(g.\"is_default\", false) FROM " +
	                         catalog.Tbl("resource_groups") + " g ORDER BY g.\"group\"")) {
		auto name = row[0].ToString();
		if (!only_group.empty() ? name == only_group : GroupDeclaresSources(catalog, name)) {
			groups.emplace_back(name, row[1].GetValue<bool>());
		}
	}
	if (!only_group.empty() && groups.empty()) {
		throw BinderException("acl_cluster_group_sources: no resource group \"%s\"", only_group);
	}
	// who reaches each source - asked once per source, whatever the groups
	case_insensitive_map_t<vector<string>> readers;
	auto readers_of = [&](const string &source) -> const vector<string> & {
		auto found = readers.find(source);
		if (found != readers.end()) {
			return found->second;
		}
		vector<string> roles;
		Connection con(*db);
		auto result = con.Query("SELECT DISTINCT \"role\" FROM acl_exposures(" + Quoted(source) +
		                        ") WHERE \"role\" IS NOT NULL AND len(\"caps\") > 0 ORDER BY \"role\"");
		if (result->HasError()) {
			result->ThrowError("acl_cluster_group_sources: ");
		}
		ResultRows rows(*result);
		for (idx_t r = 0; r < rows.Count(); r++) {
			roles.push_back(rows.GetValue(0, r).ToString());
		}
		return readers[source] = std::move(roles);
	};
	std::set<string> bound;
	for (auto &row : rows_of("SELECT \"role\" FROM " + catalog.Tbl("role_resource_groups"))) {
		bound.insert(row[0].ToString());
	}
	// every source of the profile, whichever scope - one of another group's is reached but never attachable here
	vector<std::pair<string, string>> profile_sources; // scope, name
	for (auto &row : rows_of("SELECT \"scope\", \"name\" FROM " + catalog.Tbl("cluster_items") +
	                         " WHERE \"kind\" = 'source' ORDER BY \"pos\"")) {
		profile_sources.emplace_back(row[0].ToString(), row[1].ToString());
	}
	// what a source loads through, as group g's nodes read it: its own item of that name, else the cluster's
	auto dependencies = [&](const string &group, const string &name) {
		auto own = rows_of("SELECT 1 FROM " + catalog.Tbl("cluster_items") + " WHERE \"kind\" = 'source' AND " +
		                   "\"scope\" = " + Lit(group) + " AND lower(\"name\") = lower(" + Lit(name) + ")");
		auto owner = own.empty() ? string() : group;
		vector<string> deps;
		for (auto &row :
		     rows_of("SELECT \"depends_on\" FROM " + catalog.Tbl("cluster_deps") + " WHERE \"scope\" = " + Lit(owner) +
		             " AND lower(\"name\") = lower(" + Lit(name) + ") ORDER BY \"depends_on\"")) {
			deps.push_back(row[0].ToString());
		}
		return deps;
	};
	auto OtherGroupAttach = [&](const string &source, const string &group) {
		auto spec =
		    rows_of("SELECT \"spec\" FROM " + catalog.Tbl("cluster_items") + " WHERE \"kind\" = 'source' AND " +
		            "\"scope\" <> '' AND lower(\"name\") = lower(" + Lit(source) + ") ORDER BY \"scope\" LIMIT 1");
		if (spec.empty()) {
			return string();
		}
		// the ATTACH a node runs for it (path, type, secret's name, options, lineage), as the profile writes it
		PolicyStore::ClusterItem item;
		item.kind = "source";
		item.name = source;
		item.spec = spec[0][0].ToString();
		auto statements = ClusterItemApplyStatements(item);
		return statements.empty() ? string() : "CLUSTER " + statements[0] + " IN GROUP " + QuotedIfNeeded(group);
	};
	vector<GroupSourceRow> out;
	for (auto &group : groups) {
		std::set<string> members;
		for (auto &row : rows_of("SELECT \"role\" FROM " + catalog.Tbl("role_resource_groups") +
		                         " WHERE \"group\" = " + Lit(group.first))) {
			members.insert(row[0].ToString());
		}
		// the default group serves the roles bound to none (duckdb-acl spec 096) - whether or not a CREATE ROLE wrote
		// them: a token's role may hold grants with no row of its own
		auto member = [&](const string &role) {
			return members.count(role) > 0 || (group.second && !bound.count(role));
		};
		auto set = rows_of("SELECT \"spec\" FROM " + catalog.Tbl("cluster_items") +
		                   " WHERE \"scope\" = " + Lit(group.first) + " AND \"kind\" = 'sources'");
		bool has_list = !set.empty() && !SourceSetAll(set[0][0].ToString());
		auto set_spec = set.empty() ? string() : set[0][0].ToString();
		auto listed = has_list ? SourceSetNames(set_spec) : case_insensitive_set_t();
		vector<string> sources;   // the group's nodes may attach these: the cluster's and its own
		vector<string> elsewhere; // other groups' own sources: never attached on this group's nodes
		case_insensitive_set_t seen;
		for (auto &source : profile_sources) {
			if ((source.first.empty() || source.first == group.first) && seen.insert(source.second).second) {
				sources.push_back(source.second);
			}
		}
		case_insensitive_set_t seen_elsewhere;
		for (auto &source : profile_sources) {
			if (!seen.count(source.second) && seen_elsewhere.insert(source.second).second) {
				elsewhere.push_back(source.second);
			}
		}
		auto row_of = [&](const string &source, bool attachable) {
			GroupSourceRow row;
			row.group = group.first;
			row.source = source;
			for (auto &role : readers_of(source)) {
				if (member(role)) {
					row.roles.push_back(role);
				}
			}
			row.listed = !attachable ? 0 : has_list ? (listed.count(source) ? 1 : 0) : -1;
			return row;
		};
		vector<GroupSourceRow> rows;
		vector<string> missing;
		for (auto &source : sources) {
			auto row = row_of(source, true);
			if (row.roles.empty() && row.listed != 1) {
				continue; // neither reached nor listed: nothing to say
			}
			if (row.listed == 0) {
				missing.push_back(source);
			}
			rows.push_back(std::move(row));
		}
		for (auto &source : elsewhere) {
			auto row = row_of(source, false);
			if (row.roles.empty()) {
				continue;
			}
			// another group's own: no source set lists it here - attached IN GROUP this group first, as the other
			// group's item writes it (its spec holds no credential: a secret's name, never its value)
			row.repair = OtherGroupAttach(source, group.first);
			rows.push_back(std::move(row));
		}
		if (!missing.empty()) {
			// the set as it is, then what is missing - its lazy marks kept
			vector<string> names;
			for (auto &source : sources) {
				if (listed.count(source)) {
					names.push_back(QuotedIfNeeded(source));
				}
			}
			// what is missing, each after what it loads through (a set that lists a source lists those too)
			case_insensitive_set_t added;
			for (auto &source : sources) {
				if (listed.count(source)) {
					added.insert(source);
				}
			}
			std::function<void(const string &)> add = [&](const string &source) {
				if (!added.insert(source).second) {
					return;
				}
				for (auto &dependency : dependencies(group.first, source)) {
					add(dependency);
				}
				names.push_back(QuotedIfNeeded(source));
			};
			for (auto &source : missing) {
				add(source);
			}
			string marks;
			if (JsonField(set_spec, "lazy") == "true") {
				auto idle = JsonField(set_spec, "idle_seconds");
				marks = " LAZY" + (idle.empty() ? string() : " IDLE '" + idle + " seconds'");
			}
			auto repair = "CLUSTER SET SOURCES (" + StringUtil::Join(names, ", ") + ")" + marks + " IN GROUP " +
			              QuotedIfNeeded(group.first);
			case_insensitive_set_t left_out(missing.begin(), missing.end());
			for (auto &row : rows) {
				if (left_out.count(row.source)) {
					row.repair = repair;
				}
			}
		}
		for (auto &row : rows) {
			out.push_back(std::move(row));
		}
	}
	return out;
}

//! spec 035 (duckdb-acl spec 179): the note on a grant's answer - for each group whose source set is a list that leaves
//! out a source the granted role reaches, `group g does not list source s, which role r reads - <repair>`. A note never
//! fails the grant (written already): a catalog that cannot answer says nothing
string GrantSourcesNote(PolicyStore &store, const string &function, const vector<Value> &args) {
	if (function != "acl_grant_catalog" && function != "acl_grant_schema" && function != "acl_grant_object" &&
	    function != "acl_grant_resource_group") {
		return string();
	}
	if (args.empty() || args[0].IsNull() || !store.catalog) {
		return string();
	}
	auto role = args[0].ToString();
	// the sources one statement lists go in one sentence: a group's list names every source it leaves out, another
	// group's own source has its own ATTACH
	vector<std::pair<string, vector<string>>> by_repair; // "<group>\x1f<repair>" -> sources, in the rows' order
	try {
		for (auto &row : GroupSources(store, string())) {
			if (row.listed != 0 || std::find(row.roles.begin(), row.roles.end(), role) == row.roles.end()) {
				continue;
			}
			auto key = row.group + "\x1f" + row.repair;
			auto at = std::find_if(by_repair.begin(), by_repair.end(),
			                       [&](const std::pair<string, vector<string>> &e) { return e.first == key; });
			if (at == by_repair.end()) {
				by_repair.emplace_back(key, vector<string> {row.source});
			} else {
				at->second.push_back(row.source);
			}
		}
	} catch (std::exception &) {
		return string();
	}
	vector<string> notes;
	for (auto &entry : by_repair) {
		auto split = entry.first.find('\x1f');
		auto &sources = entry.second;
		notes.push_back("group " + entry.first.substr(0, split) + " does not list " +
		                (sources.size() == 1 ? "source " : "sources ") + StringUtil::Join(sources, ", ") +
		                ", which role " + role + " reads - " + entry.first.substr(split + 1));
	}
	return StringUtil::Join(notes, "; ");
}

struct GroupSourcesBind : public TableFunctionData {
	shared_ptr<PolicyStore> store;
	string group;
};

struct GroupSourcesState : public GlobalTableFunctionState {
	vector<GroupSourceRow> rows;
	idx_t next = 0;
};

unique_ptr<FunctionData> GroupSourcesBindFn(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                            vector<Identifier> &names) {
	for (auto name : {"group", "source", "roles", "listed", "repair"}) {
		names.push_back(Identifier(name));
	}
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN,
	         LogicalType::VARCHAR};
	auto bind = make_uniq<GroupSourcesBind>();
	bind->store = input.info->Cast<ClusterInfo>().store;
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		bind->group = input.inputs[0].ToString();
	}
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> GroupSourcesInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<GroupSourcesBind>();
	auto state = make_uniq<GroupSourcesState>();
	state->rows = GroupSources(*bind.store, bind.group);
	return std::move(state);
}

void GroupSourcesScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<GroupSourcesState>();
	idx_t count = 0;
	while (state.next < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.next++];
		output.data[0].SetValue(count, Value(row.group));
		output.data[1].SetValue(count, Value(row.source));
		vector<Value> roles;
		for (auto &role : row.roles) {
			roles.emplace_back(role);
		}
		output.data[2].SetValue(count, Value::LIST(LogicalType::VARCHAR, std::move(roles)));
		output.data[3].SetValue(count, row.listed < 0 ? Value(LogicalType::BOOLEAN) : Value::BOOLEAN(row.listed == 1));
		output.data[4].SetValue(count, row.repair.empty() ? Value(LogicalType::VARCHAR) : Value(row.repair));
		count++;
	}
	output.SetChildCardinality(count);
}

//! acl_platform_drift() (spec 118): platform.drift's body - this cluster node against its profile
struct DriftState : public GlobalTableFunctionState {
	vector<PolicyStore::DriftRow> rows;
	idx_t next = 0;
};

unique_ptr<FunctionData> DriftBindFn(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                     vector<Identifier> &names) {
	for (auto name : {"kind", "name", "state", "node_value", "profile_value", "scope"}) {
		names.push_back(Identifier(name));
		types.push_back(LogicalType::VARCHAR);
	}
	auto bind = make_uniq<ItemsBind>();
	bind->store = input.info->Cast<ClusterInfo>().store;
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> DriftInit(ClientContext &, TableFunctionInitInput &input) {
	auto state = make_uniq<DriftState>();
	state->rows = input.bind_data->Cast<ItemsBind>().store->ClusterDrift();
	return std::move(state);
}

void DriftScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<DriftState>();
	idx_t count = 0;
	auto text = [](const string &value) {
		return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
	};
	while (state.next < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.next++];
		output.data[0].SetValue(count, Value(row.kind));
		output.data[1].SetValue(count, Value(row.name));
		output.data[2].SetValue(count, Value(row.state));
		output.data[3].SetValue(count, text(row.node_value));
		output.data[4].SetValue(count, text(row.profile_value));
		output.data[5].SetValue(count, Value(row.scope));
		count++;
	}
	output.SetChildCardinality(count);
}

//! acl_cluster_applied(version) (spec 096): the node agent says which profile version this node has
//! converged to - acl does not converge a node, it reports what the agent says (the load report's
//! config.applied, and readiness when it equals the target)
void ClusterAppliedFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &store = StoreOf(state);
	for (idx_t row = 0; row < args.size(); row++) {
		// a NULL version never arrives here: the default null handling answers NULL and records nothing
		auto version = args.data[0].GetValue(row).GetValue<int64_t>();
		auto target = store.ClusterVersion();
		if (version < 0 || version > target) {
			throw BinderException("acl_cluster_applied: version %lld is not one of the profile's (0 .. %lld)", version,
			                      target);
		}
		store.cluster_applied.store(version);
		result.SetValue(row, Value::BIGINT(version));
	}
}

} // namespace

namespace {

// --- `ACL CLUSTER …` (spec 093): the cluster profile, as rules on duckdb's tokens (spec 020, duckdb-acl spec 150 3g-3:
// RegisterMgmtRules) - each form compiled to its acl_cluster_* call; the authorization (the cluster bundle - it
// changes the cluster's infrastructure) is the one authorizer's ------------------------------------------------------

//! `AclClusterInGroup <- 'IN' 'GROUP' AclNameValue` where an optional clause stands, '' when absent
string ClusterGroup(MgmtCall &call, ParseResult &part) {
	auto clause = MgmtCall::Optional(part);
	return clause ? call.NameValue(clause->Cast<ListParseResult>().GetChild(2), "a resource group name") : string();
}

//! `AclComment <- 'COMMENT' AclQuotedValue` where an optional clause stands, '' when absent
string ClusterComment(MgmtCall &call, ParseResult &part) {
	auto clause = MgmtCall::Optional(part);
	return clause ? call.Quoted(clause->Cast<ListParseResult>().GetChild(1), "comment") : string();
}

// AclClusterExtension <- 'CLUSTER' AclClusterExtensionVerb 'EXTENSION' AclNameValue AclClusterExtensionClause*
//                        AclClusterInGroup? AclComment?
arena_ptr<TransformResultValue> FinalizeClusterExtension(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	MgmtCall call(transformer, "acl_cluster_extension");
	auto verb = MgmtCall::Keyword(list.GetChild(1));
	auto name = call.NameValue(list.GetChild(3), "an extension name");
	string version, repository;
	bool has_version = false, has_repository = false;
	for (auto &entry : MgmtCall::Each(list.GetChild(4))) {
		auto &clause = MgmtCall::Part(entry.get());
		auto keyword = MgmtCall::Keyword(clause);
		if (keyword == "sha256") {
			// spec 103: refused, never ignored - a script that still pins a hash must not believe it does
			throw BinderException("acl admin: SHA256 is no longer part of the cluster profile (spec 103) - DuckDB "
			                      "verifies the repository's signature at every INSTALL and LOAD; name the VERSION and "
			                      "the repository");
		}
		if (verb == "remove") {
			throw BinderException("acl admin: CLUSTER REMOVE EXTENSION names the extension only - %s is an install's "
			                      "or an update's",
			                      StringUtil::Upper(keyword));
		}
		auto &parts = clause.Cast<ListParseResult>();
		if (keyword == "version") {
			if (has_version) {
				throw BinderException("acl admin: VERSION is written once");
			}
			has_version = true;
			version = call.Quoted(parts.GetChild(1), "version");
		} else {
			if (has_repository) {
				throw BinderException("acl admin: FROM is written once");
			}
			has_repository = true; // spec 019: an update may move the item
			repository = call.NameValue(parts.GetChild(1), "a repository name");
		}
	}
	auto scope = ClusterGroup(call, list.GetChild(5));
	auto note = ClusterComment(call, list.GetChild(6));
	if (verb == "remove" && MgmtCall::Optional(list.GetChild(6))) {
		throw BinderException("acl admin: CLUSTER REMOVE EXTENSION names the extension only - COMMENT is an install's "
		                      "or an update's");
	}
	for (auto &value : {verb, scope, name, version, repository, note}) {
		call.Arg(Value(value));
	}
	return call.Result();
}

// AclClusterAttach <- 'CLUSTER' 'ATTACH' AclQuotedValue 'AS' AclNameValue AclClusterOptions? AclClusterLazy?
//                     AclClusterLineage? AclClusterDepends? AclClusterInGroup? AclComment?
arena_ptr<TransformResultValue> FinalizeClusterAttach(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	MgmtCall call(transformer, "acl_cluster_attach");
	auto path = call.Quoted(list.GetChild(2), "the source's path");
	auto alias = call.NameValue(list.GetChild(4), "the source's alias");
	string type, secret;
	vector<std::pair<string, string>> options;
	if (auto parens = MgmtCall::Optional(list.GetChild(5))) {
		// AclClusterOptions <- '(' AclClusterOption AclClusterOptionMore* ','? ')'
		auto &items = parens->Cast<ListParseResult>();
		vector<reference<ParseResult>> each {items.GetChild(1)};
		for (auto &more : MgmtCall::Each(items.GetChild(2))) {
			each.push_back(MgmtCall::Part(more.get()).Cast<ListParseResult>().GetChild(1)); // ',' AclClusterOption
		}
		case_insensitive_set_t named;
		for (auto &entry : each) {
			// AclClusterOption <- AclWordToken AclValue? - a key alone is true
			auto &option = MgmtCall::Part(entry.get()).Cast<ListParseResult>();
			auto key = MgmtCall::Word(option.GetChild(0));
			auto given = MgmtCall::Optional(option.GetChild(1));
			auto value = given ? call.Text(*given, "an option's value") : string("true");
			if (!named.insert(key).second) {
				throw BinderException("acl admin: option %s is written once", StringUtil::Upper(key));
			}
			if (StringUtil::CIEquals(key, "type")) {
				type = StringUtil::Lower(value);
			} else if (StringUtil::CIEquals(key, "secret")) {
				secret = value;
			} else {
				options.emplace_back(StringUtil::Upper(key), value);
			}
		}
	}
	// spec 035 stage 2: AclClusterLazy <- 'LAZY' AclClusterIdle?, AclClusterIdle <- 'IDLE' AclQuotedValue
	string lazy, idle;
	if (auto clause = MgmtCall::Optional(list.GetChild(6))) {
		lazy = "true";
		if (auto given = MgmtCall::Optional(clause->Cast<ListParseResult>().GetChild(1))) {
			idle = call.Quoted(given->Cast<ListParseResult>().GetChild(1), "the source's idle time");
			if (idle.empty()) {
				throw BinderException("acl admin: IDLE names an interval ('30 min') - LAZY alone is never detached");
			}
		}
	}
	string lineage; // spec 112 §9: the name the source is known by, declared where it is attached
	if (auto clause = MgmtCall::Optional(list.GetChild(7))) {
		lineage = call.Quoted(clause->Cast<ListParseResult>().GetChild(1), "the source's lineage identity");
	}
	string deps;
	if (auto clause = MgmtCall::Optional(list.GetChild(8))) {
		// AclClusterDepends <- 'DEPENDS' 'ON' '(' AclNameValue AclClusterDependsMore* ','? ')'
		auto &parts = clause->Cast<ListParseResult>();
		vector<reference<ParseResult>> items {parts.GetChild(3)};
		for (auto &more : MgmtCall::Each(parts.GetChild(4))) {
			items.push_back(MgmtCall::Part(more.get()).Cast<ListParseResult>().GetChild(1)); // ',' AclNameValue
		}
		vector<string> names;
		case_insensitive_set_t seen;
		for (auto &item : items) {
			auto name = call.NameValue(item.get(), "a source it depends on");
			if (name.empty()) {
				throw BinderException("acl admin: DEPENDS ON names a source - '' names none");
			}
			if (!seen.insert(name).second) {
				throw BinderException("acl admin: DEPENDS ON names %s twice", name);
			}
			names.push_back(name);
		}
		deps = StringUtil::Join(names, ",");
	}
	auto scope = ClusterGroup(call, list.GetChild(9));
	auto note = ClusterComment(call, list.GetChild(10));
	vector<string> json;
	for (auto &option : options) {
		json.push_back(JsonQuote(option.first) + ": " + JsonQuote(option.second));
	}
	for (auto &value : {scope, alias, path, type, secret, "{" + StringUtil::Join(json, ", ") + "}", deps, note, lineage,
	                    lazy, idle}) {
		call.Arg(Value(value));
	}
	return call.Result();
}

// AclClusterDetach <- 'CLUSTER' 'DETACH' AclNameValue AclClusterDetachFlag* AclClusterInGroup?
arena_ptr<TransformResultValue> FinalizeClusterDetach(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	MgmtCall call(transformer, "acl_cluster_detach");
	auto alias = call.NameValue(list.GetChild(2), "the source's alias");
	bool cascade = false, force = false;
	for (auto &flag : MgmtCall::Each(list.GetChild(3))) {
		auto keyword = MgmtCall::Keyword(flag.get());
		auto &written = keyword == "cascade" ? cascade : force;
		if (written) {
			throw BinderException("acl admin: %s is written once", StringUtil::Upper(keyword));
		}
		written = true;
	}
	auto scope = ClusterGroup(call, list.GetChild(4));
	for (auto &value : {scope, alias, string(cascade ? "true" : "false"), string(force ? "true" : "false")}) {
		call.Arg(Value(value));
	}
	return call.Result();
}

// AclClusterSet <- 'CLUSTER' 'SET' AclWordToken AclClusterAssign AclValue AclClusterInGroup?
// AclClusterReset <- 'CLUSTER' 'RESET' AclWordToken AclClusterInGroup?
// AclClusterSet <- 'CLUSTER' 'SET' AclClusterSetWhat AclClusterInGroup?
// AclClusterReset <- 'CLUSTER' 'RESET' AclClusterResetWhat AclClusterInGroup? - hugr-node spec 035: SOURCES is a
// group's source set, a word a setting
arena_ptr<TransformResultValue> FinalizeClusterSetting(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto verb = MgmtCall::Keyword(list.GetChild(1));
	auto &what = MgmtCall::Part(list.GetChild(2));
	// RESET's 'SOURCES' is a one-keyword rule - what holds the keyword itself
	if (what.Name() == "AclClusterSources" || (verb == "reset" && MgmtCall::Keyword(what) == "sources")) {
		MgmtCall call(transformer, "acl_cluster_sources");
		vector<string> json;
		string names = "[]", lazy, idle;
		if (verb == "set") {
			// 'SOURCES' AclClusterSourcesWhich AclClusterLazy?
			auto &parts = what.Cast<ListParseResult>();
			auto &which = MgmtCall::Part(parts.GetChild(1));
			if (which.Name() == "AclClusterSourcesList") {
				// '(' AclNameValue AclClusterSourcesMore* ','? ')'
				auto &items = which.Cast<ListParseResult>();
				json.push_back(acl::JsonQuote(call.NameValue(items.GetChild(1), "a source's alias")));
				for (auto &more : MgmtCall::Each(items.GetChild(2))) {
					json.push_back(acl::JsonQuote(call.NameValue(
					    MgmtCall::Part(more.get()).Cast<ListParseResult>().GetChild(1), "a source's alias")));
				}
				names = "[" + StringUtil::Join(json, ", ") + "]";
			} else {
				names = "ALL"; // a one-keyword rule: what holds the keyword itself
			}
			// spec 035 stage 2b: AclClusterLazy <- 'LAZY' AclClusterIdle?
			if (auto clause = MgmtCall::Optional(parts.GetChild(2))) {
				lazy = "true";
				if (auto given = MgmtCall::Optional(clause->Cast<ListParseResult>().GetChild(1))) {
					idle = call.Quoted(given->Cast<ListParseResult>().GetChild(1), "the sources' idle time");
					if (idle.empty()) {
						throw BinderException("acl admin: IDLE names an interval ('30 min') - LAZY alone is never "
						                      "detached");
					}
				}
			}
		}
		auto scope = ClusterGroup(call, list.GetChild(3));
		for (auto &part : {verb, scope, names, lazy, idle}) {
			call.Arg(Value(part));
		}
		return call.Result();
	}
	MgmtCall call(transformer, "acl_cluster_setting");
	string name, value;
	if (verb == "set") {
		auto &setting = what.Cast<ListParseResult>(); // AclWordToken AclClusterAssign AclValue
		name = MgmtCall::Word(setting.GetChild(0));
		value = call.Text(setting.GetChild(2), "a value");
	} else {
		name = MgmtCall::Word(what);
	}
	auto scope = ClusterGroup(call, list.GetChild(3));
	for (auto &part : {verb, scope, name, value}) {
		call.Arg(Value(part));
	}
	return call.Result();
}

void RegisterClusterRules() {
	vector<GrammarChange> rules;
	auto rule = [&](const char *definition) {
		rules.push_back(GrammarChange::AddRule(definition));
	};
	rule("AclClusterInGroup <- 'IN' 'GROUP' AclNameValue");
	rule("AclClusterExtensionVerb <- 'INSTALL' / 'UPDATE' / 'REMOVE'");
	rule("AclClusterVersion <- 'VERSION' AclQuotedValue");
	rule("AclClusterRepository <- 'FROM' AclNameValue");
	// spec 103's refusal whatever follows the word: its tokens up to the statement's end
	rule("AclClusterSha256Token <- Identifier");
	rules.push_back(GrammarChange::AddTerminalRuleOverride(
	    "AclClusterSha256Token",
	    [](const PEGKeywordHelper &) -> unique_ptr<Matcher> { return make_uniq<PassthroughTokenMatcher>(); }));
	rule("AclClusterSha256 <- 'SHA256' AclClusterSha256Token*");
	rule("AclClusterExtensionClause <- AclClusterVersion / AclClusterRepository / AclClusterSha256");
	rules.push_back(GrammarChange::AddRule("AclClusterExtension <- 'CLUSTER' AclClusterExtensionVerb 'EXTENSION' "
	                                       "AclNameValue AclClusterExtensionClause* AclClusterInGroup? AclComment?",
	                                       MgmtStart<FinalizeClusterExtension>));
	rule("AclClusterOption <- AclWordToken AclValue?");
	rule("AclClusterOptionMore <- ',' AclClusterOption");
	rule("AclClusterOptions <- '(' AclClusterOption AclClusterOptionMore* ','? ')'");
	rule("AclClusterIdle <- 'IDLE' AclQuotedValue");
	rule("AclClusterLazy <- 'LAZY' AclClusterIdle?");
	rule("AclClusterLineage <- 'LINEAGE' AclQuotedValue");
	rule("AclClusterDependsMore <- ',' AclNameValue");
	rule("AclClusterDepends <- 'DEPENDS' 'ON' '(' AclNameValue AclClusterDependsMore* ','? ')'");
	rules.push_back(GrammarChange::AddRule(
	    "AclClusterAttach <- 'CLUSTER' 'ATTACH' AclQuotedValue 'AS' AclNameValue AclClusterOptions? AclClusterLazy? "
	    "AclClusterLineage? AclClusterDepends? AclClusterInGroup? AclComment?",
	    MgmtStart<FinalizeClusterAttach>));
	rule("AclClusterDetachFlag <- 'CASCADE' / 'FORCE'");
	rules.push_back(GrammarChange::AddRule(
	    "AclClusterDetach <- 'CLUSTER' 'DETACH' AclNameValue AclClusterDetachFlag* AclClusterInGroup?",
	    MgmtStart<FinalizeClusterDetach>));
	rule("AclClusterAssign <- '=' / 'TO'");
	rule("AclClusterSetting <- AclWordToken AclClusterAssign AclValue");
	rule("AclClusterSourcesMore <- ',' AclNameValue");
	rule("AclClusterSourcesList <- '(' AclNameValue AclClusterSourcesMore* ','? ')'");
	rule("AclClusterSourcesAll <- 'ALL'");
	rule("AclClusterSourcesWhich <- AclClusterSourcesList / AclClusterSourcesAll");
	rule("AclClusterSources <- 'SOURCES' AclClusterSourcesWhich AclClusterLazy?");
	rule("AclClusterSetWhat <- AclClusterSources / AclClusterSetting");
	rule("AclClusterResetSources <- 'SOURCES'");
	rule("AclClusterResetWhat <- AclClusterResetSources / AclWordToken");
	rules.push_back(GrammarChange::AddRule("AclClusterSet <- 'CLUSTER' 'SET' AclClusterSetWhat AclClusterInGroup?",
	                                       MgmtStart<FinalizeClusterSetting>));
	rules.push_back(
	    GrammarChange::AddRule("AclClusterReset <- 'CLUSTER' 'RESET' AclClusterResetWhat AclClusterInGroup?",
	                           MgmtStart<FinalizeClusterSetting>));
	const char *group = "[IN GROUP <group>]";
	vector<MgmtStatementForm> forms {
	    {"AclClusterExtension",
	     {"install", "update", "remove"},
	     StringUtil::Format("write CLUSTER INSTALL | UPDATE EXTENSION <name> [VERSION '<version>'] [FROM <repository>] "
	                        "%s [COMMENT '<text>'], or CLUSTER REMOVE EXTENSION <name> %s",
	                        group, group)},
	    {"AclClusterAttach",
	     {"attach"},
	     StringUtil::Format(
	         "write CLUSTER ATTACH '<path>' AS <alias> [(TYPE <type>, SECRET <secret>, <option> "
	         "[<value>], ...)] [LAZY [IDLE '<interval>']] [LINEAGE '<identity>'] [DEPENDS ON (<source>, ...)] %s "
	         "[COMMENT '<text>']",
	         group)},
	    {"AclClusterDetach",
	     {"detach"},
	     StringUtil::Format("write CLUSTER DETACH <alias> [CASCADE] [FORCE] %s", group)},
	    {"AclClusterSet",
	     {"set"},
	     StringUtil::Format("write CLUSTER SET <setting> = | TO <value> %s, or CLUSTER SET SOURCES (<source>, ...) | "
	                        "ALL [LAZY [IDLE '<interval>']] IN GROUP <group>",
	                        group)},
	    {"AclClusterReset",
	     {"reset"},
	     StringUtil::Format("write CLUSTER RESET <setting> %s, or CLUSTER RESET SOURCES IN GROUP <group>", group)}};
	RegisterMgmtRules("cluster", std::move(forms), std::move(rules),
	                  "ACL CLUSTER expects INSTALL, UPDATE or REMOVE EXTENSION, ATTACH, DETACH, SET or RESET");
}

//! spec 126: the cluster's part of the `platform` catalog - its views and functions, registered at load; the ONE
//! authorizer judges them by their class (CLUSTER views, INFRASTRUCTURE functions: the cluster bundle's)
void RegisterClusterPlatform() {
	constexpr const char *V = "VARCHAR";
	constexpr const char *I = "BIGINT";
	constexpr const char *LV = "VARCHAR[]";
	constexpr const char *MAPV = "MAP";
	constexpr const char *ANSWER = "STRUCT(version BIGINT, class VARCHAR, applied_here BOOLEAN, note VARCHAR)";
	const vector<PlatformColumn> item_columns = {{"scope", V}, {"kind", V},    {"name", V},        {"spec", MAPV},
	                                             {"class", V}, {"version", I}, {"depends_on", LV}, {"comment", V}};
	PlatformView items {"cluster_items", PlatformViewClass::CLUSTER, item_columns,
	                    "the cluster profile as written (spec 093)"};
	items.body =
	    "SELECT \"scope\", \"kind\", \"name\", acl_platform_map(\"spec\") AS \"spec\", \"class\", \"version\", "
	    "\"depends_on\", \"comment\" FROM acl_cluster_items()";
	RegisterPlatformView(std::move(items));
	PlatformView effective {"cluster_effective", PlatformViewClass::CLUSTER, item_columns,
	                        "what of the cluster profile applies to this node (spec 096)"};
	effective.body = "SELECT \"scope\", \"kind\", \"name\", acl_platform_map(\"spec\") AS \"spec\", \"class\", "
	                 "\"version\", \"depends_on\", \"comment\" FROM acl_cluster_effective()";
	RegisterPlatformView(std::move(effective));
	PlatformView drift {"drift",
	                    PlatformViewClass::CLUSTER,
	                    {{"kind", V}, {"name", V}, {"state", V}, {"node_value", V}, {"profile_value", V}, {"scope", V}},
	                    "what this cluster node carries against its profile: same, differs, node_only, profile_only, "
	                    "bootstrap (spec 118)"};
	drift.body = "SELECT * FROM acl_platform_drift()";
	RegisterPlatformView(std::move(drift));

	auto A = [](const char *name) {
		return PlatformParam {name, true, "VARCHAR", ""};
	};
	auto P = [](const char *name) {
		return PlatformParam {name, false, "VARCHAR", ""};
	};
	RegisterPlatformFunction({"cluster_extension",
	                          "acl_cluster_extension",
	                          {A("verb"), A("scope"), A("name"), P("version"), A("repository"), P("comment")},
	                          {6},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          false,
	                          ANSWER,
	                          "install / update / remove an extension of the profile"});
	RegisterPlatformFunction({"cluster_attach",
	                          "acl_cluster_attach",
	                          {A("scope"), A("alias"), A("path"), A("type"), A("secret"), P("options"), A("depends_on"),
	                           P("comment"), P("lineage"), P("lazy"), P("idle")},
	                          {8, 9, 10, 11},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          false,
	                          ANSWER,
	                          "attach a source in the profile"});
	RegisterPlatformFunction({"cluster_detach",
	                          "acl_cluster_detach",
	                          {A("scope"), A("alias"), P("cascade"), P("force")},
	                          {4},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          false,
	                          ANSWER,
	                          "detach a source of the profile"});
	PlatformFunction setting {"cluster_setting",
	                          "acl_cluster_setting",
	                          {A("verb"), A("scope"), A("name"), P("value")},
	                          {4},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          false,
	                          ANSWER,
	                          "set / reset a setting of the profile"};
	// spec 118: the cluster bundle sets only the node's resources and tuning; the rest is passthrough's
	setting.rule_arg = 2;
	setting.rule = ClusterBundleMaySet;
	setting.rule_refusal = "acl admin: \"%s\" is not among the settings the cluster bundle sets (the node's resources "
	                       "and tuning) - a data path, a trust or audit setting is a passthrough scope's";
	RegisterPlatformFunction(std::move(setting));
	// spec 035 stage 3: the check of the groups' source sets - the cluster bundle's, who writes them (it names the
	// roles that reach each source, as platform.exposures does for policy)
	RegisterPlatformFunction({"group_sources",
	                          "acl_cluster_group_sources",
	                          {P("group")},
	                          {0, 1},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          true,
	                          "TABLE",
	                          "what each resource group's roles reach against its source set, with the statement that "
	                          "lists what it leaves out"});
	RegisterPlatformFunction({"cluster_sources",
	                          "acl_cluster_sources",
	                          {A("verb"), A("scope"), P("names"), P("lazy"), P("idle")},
	                          {3, 4, 5},
	                          -1,
	                          PlatformRight::INFRASTRUCTURE,
	                          false,
	                          ANSWER,
	                          "set / reset a group's source set (hugr-node spec 035)"});
}

} // namespace

void RegisterAclCluster(ExtensionLoader &loader, const shared_ptr<PolicyStore> &store) {
	// duckdb-acl spec 178: the platform part's own table functions (the agent's plan, the profile's statements) are the
	// operator's - in the never set, so no grant by name opens one to a principal
	RegisterNeverCallablePrefix("hugr_node_");
	const LogicalType &v = LogicalType::VARCHAR;
	auto scalar = [&](const char *name, vector<LogicalType> arguments, const scalar_function_t &fn,
	                  const LogicalType &returns) {
		ScalarFunction function(Identifier(name), std::move(arguments), returns, fn);
		MarkAclScalar(function, store);
		loader.RegisterFunction(function);
	};
	scalar("acl_cluster_extension", {v, v, v, v, v, v}, ClusterExtensionFunc, AnswerType());
	scalar("acl_cluster_attach", {v, v, v, v, v, v, v, v}, ClusterAttachFunc, AnswerType());
	scalar("acl_cluster_attach", {v, v, v, v, v, v, v, v, v}, ClusterAttachFunc, AnswerType());
	scalar("acl_cluster_attach", {v, v, v, v, v, v, v, v, v, v}, ClusterAttachFunc, AnswerType());
	scalar("acl_cluster_attach", {v, v, v, v, v, v, v, v, v, v, v}, ClusterAttachFunc, AnswerType());
	scalar("acl_cluster_detach", {v, v, v, v}, ClusterDetachFunc, AnswerType());
	scalar("acl_cluster_setting", {v, v, v, v}, ClusterSettingFunc, AnswerType());
	scalar("acl_cluster_sources", {v, v, v}, ClusterSourcesFunc, AnswerType());
	scalar("acl_cluster_sources", {v, v, v, v}, ClusterSourcesFunc, AnswerType());
	scalar("acl_cluster_sources", {v, v, v, v, v}, ClusterSourcesFunc, AnswerType());
	scalar("acl_cluster_version", {}, ClusterVersionFunc, LogicalType::BIGINT);

	auto info = make_shared_ptr<ClusterInfo>(store);
	TableFunctionSet items(Identifier("acl_cluster_items"));
	for (auto &arguments : {vector<LogicalType> {}, vector<LogicalType> {v}}) {
		TableFunction function(Identifier("acl_cluster_items"), arguments, ItemsScan, ItemsBindFn, ItemsInit);
		function.function_info = info;
		items.AddFunction(function);
	}
	loader.RegisterFunction(items);
	// spec 096: what applies to THIS node, and the version its agent reports applied
	// spec 035 stage 3: what each group's roles reach against its source set, with the statement that lists it
	// an acl_* name: in the never set, so no grant opens it to a principal - platform.group_sources is its form
	TableFunctionSet group_sources(Identifier("acl_cluster_group_sources"));
	for (auto &arguments : {vector<LogicalType> {}, vector<LogicalType> {v}}) {
		TableFunction function(Identifier("acl_cluster_group_sources"), arguments, GroupSourcesScan, GroupSourcesBindFn,
		                       GroupSourcesInit);
		function.function_info = info;
		group_sources.AddFunction(function);
	}
	loader.RegisterFunction(group_sources);
	TableFunction effective(Identifier("acl_cluster_effective"), {}, ItemsScan, EffectiveBindFn, EffectiveInit);
	effective.function_info = info;
	loader.RegisterFunction(effective);
	scalar("acl_cluster_applied", {LogicalType::BIGINT}, ClusterAppliedFunc, LogicalType::BIGINT);
	TableFunction drift(Identifier("acl_platform_drift"), {}, DriftScan, DriftBindFn, DriftInit);
	drift.function_info = info;
	loader.RegisterFunction(drift);
	RegisterClusterPlatform();
	RegisterClusterRules();
	RegisterAdminScope(AdminScope::CLUSTER);

	// spec 126: the node's group is the platform part's setting and trait
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(
	    "acl_node_group",
	    "acl: the resource group this node belongs to (spec 096) - it serves the sessions of principals "
	    "that hold the group (or are in none and it is the default); '' serves everyone. Set by the "
	    "deployment, never by the policy",
	    LogicalType::VARCHAR, Value(""),
	    [](ClientContext &, SetScope scope, Value &) {
		    if (scope != SetScope::GLOBAL) {
			    throw InvalidInputException("acl_node_group is global - use SET GLOBAL");
		    }
	    },
	    SetScope::GLOBAL);
	auto *node = store.get(); // the traits live in the store: no reference back to it is kept
	// hugr-node spec 002: a hugr_node node is a cluster node by construction, whatever the number of nodes - the
	// profile, drift, the cluster bundle, secrets only in the service. There is no single-node mode to switch to:
	// the open node is duckdb-acl's own build, which registers no trait
	node->node_traits.cluster_node = []() {
		return true;
	};
	node->node_traits.node_group = [node]() {
		auto db = node->instance.lock();
		Value value;
		if (db && db->TryGetCurrentSetting("acl_node_group", value) && !value.IsNull()) {
			return value.ToString();
		}
		return string();
	};
	node->node_traits.node_group_known = [node]() {
		return node->NodeGroupKnown();
	};
	node->node_traits.config_target = [node]() {
		return node->ClusterVersion();
	};
	// spec 035 (duckdb-acl spec 176): a node whose group declares its sources attaches only part of them - it writes
	// no policy and runs no check of the stored facts against the sources
	node->node_traits.partial_sources = [node]() {
		auto group = node->NodeGroup();
		return node->catalog && GroupDeclaresSources(*node->catalog, group) ? group : string();
	};
	// spec 035 stage 2 (duckdb-acl spec 177): the sources marked LAZY, attached by the statement that reads them
	node->node_traits.lazy_sources = [node]() {
		auto db = node->instance.lock();
		return db ? LazySourceAliases(*db) : vector<string>();
	};
	// spec 035 (duckdb-acl spec 179): a grant that gives a role reach says which resource groups' source sets leave
	// out a source the role now reads, with the statement that lists it - never applied by itself
	node->node_traits.management_note = [node](ClientContext &, const string &function, const vector<Value> &args) {
		return GrantSourcesNote(*node, function, args);
	};
	node->node_traits.attach_on_demand = [node](const string &alias) {
		auto db = node->instance.lock();
		if (db) {
			AttachLazySource(*db, alias);
		}
	};
}

} // namespace acl
} // namespace duckdb
