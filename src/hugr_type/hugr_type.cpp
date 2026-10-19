// The `hugr` catalog type (spec 011): the ATTACH, the `hugr` secret type, hugr_pools() and the pool's gauges.

#include "hugr_type.hpp"

#include "hugr_catalog.hpp"
#include "hugr_pool.hpp"

#include "acl_audit.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {
namespace hugr {

namespace {

idx_t PositiveOption(const string &key, const Value &value) {
	auto number = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
	if (number == 0) {
		throw InvalidInputException("hugr: %s must be greater than zero", key);
	}
	return number;
}

//! B's door as written: `host[:port]`, optionally `hugr:` in front
string DoorOf(const string &path) {
	auto door = path;
	StringUtil::Trim(door);
	for (auto prefix : {"hugr://", "hugr:"}) {
		if (StringUtil::StartsWith(StringUtil::Lower(door), prefix)) {
			door = door.substr(strlen(prefix));
			break;
		}
	}
	if (door.empty()) {
		throw InvalidInputException("hugr: ATTACH names B's door - 'host[:port]'");
	}
	return door;
}

unique_ptr<Catalog> HugrAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                               const string &name, AttachInfo &info, AttachOptions &attach_options) {
	LinkOptions options;
	string secret;
	bool disable_ssl = false;
	for (auto &entry : attach_options.options) {
		auto key = StringUtil::Lower(entry.first);
		auto &value = entry.second;
		if (key == "secret") {
			secret = value.ToString();
		} else if (key == "pool_size") {
			options.pool_size = PositiveOption(key, value);
		} else if (key == "pool_wait") {
			options.pool_wait_s = PositiveOption(key, value);
		} else if (key == "idle_timeout") {
			options.idle_timeout_s = PositiveOption(key, value);
		} else if (key == "max_lifetime") {
			options.max_lifetime_s = PositiveOption(key, value);
		} else if (key == "read_ahead") {
			options.read_ahead = PositiveOption(key, value);
		} else if (key == "catalog_ttl") {
			options.catalog_ttl_s = PositiveOption(key, value);
		} else if (key == "heartbeat_timeout") {
			options.heartbeat_timeout_s = PositiveOption(key, value);
		} else if (key == "disable_ssl") {
			disable_ssl = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else {
			throw InvalidInputException("hugr: unknown ATTACH option \"%s\" (the options: secret, pool_size, pool_wait, "
			                            "idle_timeout, max_lifetime, read_ahead, catalog_ttl, heartbeat_timeout, "
			                            "disable_ssl)",
			                            entry.first);
		}
	}
	if (secret.empty()) {
		// the token is never an option: it is read from the secret at every connect (spec 011)
		throw InvalidInputException("hugr: name the secret that holds B's token - ATTACH '<door>' AS %s (TYPE hugr, "
		                            "SECRET <name>)",
		                            name);
	}
	QuackUri uri("quack:" + DoorOf(info.path), !disable_ssl);
	if (disable_ssl && !uri.IsLocal()) {
		throw InvalidInputException("hugr: disable_ssl is for a loopback B only - %s is reached over TLS", uri.Host());
	}
	// duckdb refuses every write to a read-only database before it reaches the catalog
	db.SetReadOnlyDatabase();
	auto pool = make_shared_ptr<Pool>(*context.db, name, uri, secret, options);
	PoolRegistry::Get(*context.db)->Add(pool);
	return make_uniq<HugrCatalog>(db, context, std::move(pool), uri.CanonicalUri());
}

unique_ptr<TransactionManager> HugrCreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                            Catalog &) {
	return make_uniq<HugrTransactionManager>(db);
}

class HugrStorageExtension : public StorageExtension {
public:
	HugrStorageExtension() {
		attach = HugrAttach;
		create_transaction_manager = HugrCreateTransactionManager;
	}
};

//! CREATE SECRET (TYPE hugr, TOKEN '...'): the node's own form; on a platform node the secret lives in tresor, whose
//! generated `token` is minted for the node at every read (spec 011, "The ATTACH")
unique_ptr<BaseSecret> CreateHugrSecret(ClientContext &, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.emplace_back("hugr:");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (auto &option : input.options) {
		if (StringUtil::Lower(option.first) != "token") {
			throw InvalidInputException("hugr: unknown secret parameter \"%s\" (the one parameter: token)",
			                            option.first);
		}
		secret->secret_map["token"] = option.second.ToString();
	}
	secret->redact_keys = {"token"};
	return std::move(secret);
}

// ---- hugr_pools() -------------------------------------------------------------------------------------------------

struct PoolsState : public GlobalTableFunctionState {
	vector<PoolView> views;
	idx_t next = 0;
};

LogicalType ConnectionsType() {
	child_list_t<LogicalType> fields;
	fields.emplace_back("label", LogicalType::VARCHAR);
	fields.emplace_back("age_s", LogicalType::BIGINT);
	fields.emplace_back("idle_s", LogicalType::BIGINT);
	fields.emplace_back("busy", LogicalType::BOOLEAN);
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

unique_ptr<FunctionData> PoolsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &types,
                                   vector<Identifier> &names) {
	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		types.push_back(std::move(type));
	};
	column("catalog", LogicalType::VARCHAR);
	column("server", LogicalType::VARCHAR);
	column("pool_size", LogicalType::UBIGINT);
	column("busy", LogicalType::UBIGINT);
	column("idle", LogicalType::UBIGINT);
	column("connecting", LogicalType::UBIGINT);
	column("waiting", LogicalType::UBIGINT);
	column("last_refusal", LogicalType::VARCHAR);
	column("connections", ConnectionsType());
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PoolsInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<PoolsState>();
	for (auto &pool : PoolRegistry::Get(*context.db)->Pools()) {
		state->views.push_back(pool->View());
	}
	return std::move(state);
}

void PoolsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<PoolsState>();
	idx_t count = 0;
	while (state.next < state.views.size() && count < STANDARD_VECTOR_SIZE) {
		auto &view = state.views[state.next++];
		vector<Value> connections;
		for (auto &connection : view.connections) {
			child_list_t<Value> fields;
			fields.emplace_back("label", connection.label.empty() ? Value() : Value(connection.label));
			fields.emplace_back("age_s", Value::BIGINT(connection.age_s));
			fields.emplace_back("idle_s", Value::BIGINT(connection.idle_s));
			fields.emplace_back("busy", Value::BOOLEAN(connection.busy));
			connections.push_back(Value::STRUCT(std::move(fields)));
		}
		output.data[0].SetValue(count, Value(view.catalog));
		output.data[1].SetValue(count, Value(view.server));
		output.data[2].SetValue(count, Value::UBIGINT(view.size));
		output.data[3].SetValue(count, Value::UBIGINT(view.busy));
		output.data[4].SetValue(count, Value::UBIGINT(view.idle));
		output.data[5].SetValue(count, Value::UBIGINT(view.connecting));
		output.data[6].SetValue(count, Value::UBIGINT(view.waiting));
		output.data[7].SetValue(count, view.last_refusal.empty() ? Value() : Value(view.last_refusal));
		output.data[8].SetValue(count, Value::LIST(ListType::GetChildType(ConnectionsType()), std::move(connections)));
		count++;
	}
	output.SetChildCardinality(count);
}

//! hugr.pool.connections{catalog, state} and hugr.pool.waiting{catalog}, read when the registry is snapshot
void RegisterGauges(DatabaseInstance &db) {
	string why;
	auto hooks = acl::AuditHooks::Reach(db.GetObjectCache(), why);
	if (!hooks) {
		return;
	}
	auto registry = PoolRegistry::Get(db);
	hooks->Gauges().RegisterDynamic(
	    "hugr.pool.connections", "1", "connections of a hugr link, by state", [registry] {
		    vector<std::pair<vector<std::pair<string, string>>, int64_t>> rows;
		    for (auto &pool : registry->Pools()) {
			    auto view = pool->View();
			    for (auto state : {std::make_pair("busy", view.busy), std::make_pair("idle", view.idle),
			                       std::make_pair("connecting", view.connecting)}) {
				    rows.push_back({{{"catalog", view.catalog}, {"state", state.first}},
				                    static_cast<int64_t>(state.second)});
			    }
		    }
		    return rows;
	    });
	hooks->Gauges().RegisterDynamic("hugr.pool.waiting", "1", "loans waiting for a connection of a hugr link",
	                                [registry] {
		                                vector<std::pair<vector<std::pair<string, string>>, int64_t>> rows;
		                                for (auto &pool : registry->Pools()) {
			                                auto view = pool->View();
			                                rows.push_back({{{"catalog", view.catalog}},
			                                                static_cast<int64_t>(view.waiting)});
		                                }
		                                return rows;
	                                });
}

} // namespace

void RegisterHugrType(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	auto storage = make_shared_ptr<HugrStorageExtension>();
	StorageExtension::Register(db.config, "hugr", storage);

	SecretType secret_type;
	secret_type.name = Identifier("hugr");
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "hugr_node";
	loader.RegisterSecretType(secret_type);
	CreateSecretFunction create_secret = {"hugr", "config", CreateHugrSecret};
	create_secret.named_parameters["token"] = LogicalType::VARCHAR;
	loader.RegisterFunction(create_secret);

	TableFunction pools(Identifier("hugr_pools"), {}, PoolsScan, PoolsBind, PoolsInit);
	loader.RegisterFunction(pools);

	RegisterGauges(db);
}

} // namespace hugr
} // namespace duckdb
