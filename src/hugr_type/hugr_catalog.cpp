// B's virtual catalog as B shows it to A's node role (spec 011, "The catalog"): read-only, a snapshot per transaction.

#include "hugr_catalog.hpp"

#include "hugr_remote.hpp"
#include "hugr_scan.hpp"

#include "duckdb/common/types/type_manager.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {
namespace hugr {

namespace {

//! when a refresh that failed is tried again: B being away must not cost every statement a connect
constexpr int64_t REFRESH_RETRY_MS = 30000;
//! a stale catalog is read again at most this often: every refusal of B's acl is a binder error, and an object B
//! lists but refuses must not cost each statement a reading of B's listings
constexpr int64_t STALE_REFRESH_MS = 1000;

//! B's schemas with their full paths, its MAIN catalog's first - quack's own walk of the (nested) schema tree
const char *SCHEMAS_QUERY = R"(
WITH RECURSIVE schema_tree AS (
	SELECT oid, database_name, [schema_name] AS schema_path
	FROM duckdb_schemas()
	WHERE parent_schema_oid IS NULL
	UNION ALL
	SELECT nested.oid, nested.database_name, list_append(parent.schema_path, nested.schema_name)
	FROM duckdb_schemas() nested
	JOIN schema_tree parent ON nested.parent_schema_oid = parent.oid
)
SELECT oid, database_name, schema_path, current_database()
FROM schema_tree
WHERE database_name NOT IN ('system', 'temp')
ORDER BY (database_name = current_database()) DESC, database_name, length(schema_path), schema_path
)";

//! Every column of every table and view A's role sees, by its schema's oid. Views too: a view is read as a table
//! (B's acl resolves the name), so no function that takes SQL text is ever bound (spec 011, "No state between loans").
const char *COLUMNS_QUERY = R"(
SELECT schema_oid, table_name, column_name, data_type
FROM duckdb_columns()
ORDER BY schema_oid, table_name, column_index
)";

CreateSchemaInfo SchemaInfo(const Identifier &name) {
	CreateSchemaInfo info;
	info.SetQualifiedName(QualifiedName({name}, Identifier()));
	return info;
}

//! The schema at `path` under `top`, every missing level made: a level B has no schema for is the stand-in for one of
//! its catalogs. A schema whose place is taken already is not registered (null): the first one keeps the name - B's
//! MAIN catalog's come first - and the other is left out, its objects with it, never merged into the first.
optional_ptr<HugrSchemaEntry> RegisterSchema(HugrCatalog &catalog,
                                             case_insensitive_map_t<unique_ptr<HugrSchemaEntry>> &top,
                                             const vector<Identifier> &path, const vector<Identifier> &remote_path) {
	reference<case_insensitive_map_t<unique_ptr<HugrSchemaEntry>>> level = top;
	optional_ptr<HugrSchemaEntry> parent;
	for (idx_t i = 0; i < path.size(); i++) {
		auto key = path[i].GetIdentifierName();
		auto found = level.get().find(key);
		auto last = i + 1 == path.size();
		if (found != level.get().end()) {
			if (last) {
				return nullptr;
			}
		} else {
			auto info = SchemaInfo(path[i]);
			auto entry = make_uniq<HugrSchemaEntry>(catalog, info, parent.get());
			// a stand-in is one of B's catalogs: its path there is the catalog alone
			entry->remote_path = last ? remote_path : vector<Identifier> {remote_path[0]};
			found = level.get().emplace(key, std::move(entry)).first;
		}
		parent = found->second.get();
		level = parent->schemas;
	}
	return parent;
}

bool IsVarchar(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR;
}

//! B's listing has the shape the type asked for, or none of it is read: a hostile or a broken B must not make this
//! node read past a row's end or a value of another type
void CheckListing(const ColumnDataCollection &rows, const vector<bool (*)(const LogicalType &)> &columns,
                  const char *what) {
	auto &types = rows.Types();
	if (types.size() < columns.size()) {
		throw InvalidInputException("hugr: B's %s listing has %llu columns, not %llu", what, types.size(),
		                            columns.size());
	}
	for (idx_t i = 0; i < columns.size(); i++) {
		if (!columns[i](types[i])) {
			throw InvalidInputException("hugr: B's %s listing has a column of type %s at %llu", what,
			                            types[i].ToString(), i + 1);
		}
	}
}

bool IsIntegral(const LogicalType &type) {
	return type.IsIntegral();
}

bool IsVarcharList(const LogicalType &type) {
	return type.id() == LogicalTypeId::LIST && ListType::GetChildType(type).id() == LogicalTypeId::VARCHAR;
}

} // namespace

// ---- entries ------------------------------------------------------------------------------------------------------

HugrTableEntry::HugrTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)) {
}

unique_ptr<BaseStatistics> HugrTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction HugrTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data_p) {
	auto &hugr_catalog = catalog.Cast<HugrCatalog>();
	auto bind_data = make_uniq<HugrScanBindData>();
	bind_data->catalog = &hugr_catalog;
	bind_data->pool = hugr_catalog.GetPool();
	bind_data->remote_name = schema.Cast<HugrSchemaEntry>().RemoteName(name);
	auto local_path = schema.GetSchemaPath();
	local_path.insert(local_path.begin(), catalog.GetName());
	bind_data->local_name = QualifiedName(std::move(local_path), name).ToString();
	for (auto &column : columns.Physical()) {
		bind_data->column_names.push_back(column.Name().GetIdentifierName());
		bind_data->column_types.push_back(column.Type());
	}
	bind_data->table_entry = this;
	bind_data_p = std::move(bind_data);
	return HugrScanFunction();
}

TableStorageInfo HugrTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

HugrSchemaEntry::HugrSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, optional_ptr<SchemaCatalogEntry> parent)
    : SchemaCatalogEntry(catalog, info, parent) {
}

unique_ptr<CreateInfo> HugrSchemaEntry::GetInfo() const {
	auto result = make_uniq<CreateSchemaInfo>();
	// a nested schema roots its path at the catalog so it can be navigated: [catalog, parents..., name]
	auto path = GetSchemaPath();
	if (path.size() > 1) {
		path.insert(path.begin(), catalog.GetName());
	}
	result->SetQualifiedName(QualifiedName(std::move(path), Identifier()));
	result->comment = comment;
	result->tags = tags;
	return std::move(result);
}

QualifiedName HugrSchemaEntry::RemoteName(const Identifier &entry_name) const {
	// catalog-qualified: B's MAIN catalog's schema `hr` and B's catalog `hr` never read as each other on B
	return QualifiedName(remote_path, entry_name);
}

void HugrSchemaEntry::Scan(ClientContext &context, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, callback);
}

void HugrSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	switch (type) {
	case CatalogType::SCHEMA_ENTRY:
		for (auto &entry : schemas) {
			callback(*entry.second);
		}
		break;
	case CatalogType::TABLE_ENTRY:
		for (auto &entry : tables) {
			callback(*entry.second);
		}
		break;
	default:
		break;
	}
}

optional_ptr<CatalogEntry> HugrSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	auto &name = lookup_info.GetEntryName();
	switch (lookup_info.GetCatalogType()) {
	case CatalogType::SCHEMA_ENTRY: {
		auto found = schemas.find(name);
		return found == schemas.end() ? nullptr : found->second.get();
	}
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		auto found = tables.find(name);
		return found == tables.end() ? nullptr : found->second.get();
	}
	default:
		// B's functions are not in the catalog: stage 2's (spec 011)
		return nullptr;
	}
}

optional_ptr<CatalogEntry> HugrSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
optional_ptr<CatalogEntry> HugrSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
optional_ptr<CatalogEntry> HugrSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
optional_ptr<CatalogEntry> HugrSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
optional_ptr<CatalogEntry> HugrSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
optional_ptr<CatalogEntry> HugrSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
void HugrSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}
void HugrSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ParentCatalog().Cast<HugrCatalog>().RefuseWrite();
}

// ---- transactions -------------------------------------------------------------------------------------------------

Transaction &HugrTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<HugrTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData HugrTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	transactions.erase(transaction);
	return ErrorData();
}

void HugrTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	transactions.erase(transaction);
}

// ---- the catalog --------------------------------------------------------------------------------------------------

HugrCatalog::HugrCatalog(AttachedDatabase &db, ClientContext &context, shared_ptr<Pool> pool_p, string server_p)
    : Catalog(db), pool(std::move(pool_p)), server(std::move(server_p)) {
	// the ATTACH reads B's catalog now, and fails with B's first refusal rather than waiting it out
	current = Load(context, false);
	loaded_at = NowMs();
}

HugrCatalog::~HugrCatalog() {
	if (pool) {
		pool->Close();
	}
}

void HugrCatalog::RefuseWrite() const {
	throw PermissionException("hugr: the catalog %s is read-only", const_cast<HugrCatalog &>(*this).GetName());
}

shared_ptr<HugrSnapshot> HugrCatalog::Load(ClientContext &context, bool wait) {
	auto schemas = ReadAll(*pool, context, SCHEMAS_QUERY, wait);
	auto columns = ReadAll(*pool, context, COLUMNS_QUERY, wait);
	auto snapshot = make_shared_ptr<HugrSnapshot>();

	CheckListing(*schemas, {IsIntegral, IsVarchar, IsVarcharList, IsVarchar}, "schema");
	CheckListing(*columns, {IsIntegral, IsVarchar, IsVarchar, IsVarchar}, "column");
	unordered_map<int64_t, reference<HugrSchemaEntry>> by_oid;
	for (auto &row : schemas->Rows()) {
		if (row.GetValue(0).IsNull() || row.GetValue(1).IsNull() || row.GetValue(2).IsNull() ||
		    row.GetValue(3).IsNull()) {
			continue;
		}
		auto oid = row.GetValue(0).GetValue<int64_t>();
		auto database = row.GetValue(1).GetValue<string>();
		auto main = row.GetValue(3).GetValue<string>();
		vector<Identifier> path;
		vector<Identifier> remote_path {Identifier(database)};
		if (!StringUtil::CIEquals(database, main)) {
			// another catalog of the role: the catalog is a schema holding its schemas (quack's layout)
			path.emplace_back(database);
		}
		bool complete = true;
		auto parts = row.GetValue(2); // held: GetChildren answers a reference into it
		for (auto &part : ListValue::GetChildren(parts)) {
			if (part.IsNull()) {
				complete = false;
				break;
			}
			path.emplace_back(part.GetValue<string>());
			remote_path.emplace_back(part.GetValue<string>());
		}
		if (!complete || remote_path.size() < 2) {
			continue;
		}
		auto schema = RegisterSchema(*this, snapshot->schemas, path, remote_path);
		if (schema) {
			by_oid.emplace(oid, *schema);
		}
	}

	// one table per (schema oid, name), its columns in order; a type this node cannot spell leaves the object out
	auto &types = TypeManager::Get(*context.db);
	optional_ptr<HugrSchemaEntry> schema;
	unique_ptr<CreateTableInfo> info;
	bool broken = false;
	auto finish = [&] {
		if (info && schema && !broken) {
			auto name = info->GetQualifiedName().Name().GetIdentifierName();
			schema->tables[name] = make_uniq<HugrTableEntry>(*this, *schema, *info);
		}
		info.reset();
		broken = false;
	};
	int64_t last_oid = -1;
	string last_table;
	for (auto &row : columns->Rows()) {
		if (row.GetValue(0).IsNull() || row.GetValue(1).IsNull() || row.GetValue(2).IsNull() ||
		    row.GetValue(3).IsNull()) {
			continue;
		}
		auto oid = row.GetValue(0).GetValue<int64_t>();
		auto table = row.GetValue(1).GetValue<string>();
		if (!info || oid != last_oid || table != last_table) {
			finish();
			last_oid = oid;
			last_table = table;
			auto found = by_oid.find(oid);
			schema = found == by_oid.end() ? nullptr : &found->second.get();
			if (schema) {
				info = make_uniq<CreateTableInfo>(*schema, Identifier(table));
			}
		}
		if (!info || broken) {
			continue;
		}
		auto type_text = row.GetValue(3).GetValue<string>();
		try {
			auto type = types.ParseLogicalType(type_text);
			info->columns.AddColumn(ColumnDefinition(Identifier(row.GetValue(2).GetValue<string>()), std::move(type)));
		} catch (std::exception &ex) {
			broken = true;
			snapshot->skipped.push_back(schema->RemoteName(Identifier(table)).ToString() + " (" + type_text + ")");
		}
	}
	finish();
	if (!snapshot->skipped.empty()) {
		DUCKDB_LOG_WARNING(context, StringUtil::Format("hugr: %s: left out, a type this node cannot spell: %s",
		                                               GetName(), StringUtil::Join(snapshot->skipped, ", ")));
	}
	return snapshot;
}

shared_ptr<HugrSnapshot> HugrCatalog::Current(optional_ptr<ClientContext> context) {
	{
		lock_guard<mutex> guard(snapshot_lock);
		auto age = NowMs() - loaded_at;
		auto due =
		    (stale && age >= STALE_REFRESH_MS) || age >= static_cast<int64_t>(pool->Options().catalog_ttl_s) * 1000;
		if (!due || !context || refreshing) {
			return current;
		}
		refreshing = true;
		stale = false;
	}
	shared_ptr<HugrSnapshot> fresh;
	try {
		fresh = Load(*context, false);
	} catch (std::exception &ex) {
		// B away: the snapshot stays, and the next try waits - a statement that reads B fails there, with B's answer
		DUCKDB_LOG_WARNING(*context,
		                   StringUtil::Format("hugr: %s: the catalog was not read again: %s", GetName(), ex.what()));
	}
	lock_guard<mutex> guard(snapshot_lock);
	refreshing = false;
	auto now = NowMs();
	if (fresh) {
		current = std::move(fresh);
		loaded_at = now;
	} else {
		loaded_at = now - static_cast<int64_t>(pool->Options().catalog_ttl_s) * 1000 + REFRESH_RETRY_MS;
	}
	return current;
}

shared_ptr<HugrSnapshot> HugrCatalog::SnapshotOf(CatalogTransaction transaction) {
	if (!transaction.transaction) {
		return Current(nullptr);
	}
	auto &hugr_transaction = transaction.transaction->Cast<HugrTransaction>();
	lock_guard<mutex> guard(hugr_transaction.pin_lock);
	if (!hugr_transaction.snapshot) {
		hugr_transaction.snapshot = Current(transaction.context);
	}
	return hugr_transaction.snapshot;
}

shared_ptr<HugrSnapshot> HugrCatalog::SnapshotOf(ClientContext &context) {
	return SnapshotOf(GetCatalogTransaction(context));
}

optional_ptr<SchemaCatalogEntry> HugrCatalog::LookupSchema(CatalogTransaction transaction,
                                                           const EntryLookupInfo &schema_lookup,
                                                           OnEntryNotFound if_not_found) {
	// [catalog, schema...] when qualified, [schema] when bare; the catalog part and empty placeholders dropped
	auto &qualified = schema_lookup.GetQualifiedName().Path();
	vector<Identifier> path;
	for (idx_t i = 0; i < qualified.size(); i++) {
		if ((i == 0 && qualified.size() > 1) || qualified[i].empty()) {
			continue;
		}
		path.push_back(qualified[i]);
	}
	auto snapshot = SnapshotOf(transaction);
	reference<case_insensitive_map_t<unique_ptr<HugrSchemaEntry>>> level = snapshot->schemas;
	optional_ptr<HugrSchemaEntry> entry;
	for (auto &part : path) {
		auto found = level.get().find(part.GetIdentifierName());
		if (found == level.get().end()) {
			if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw BinderException("Schema with name \"%s\" not found", part.GetIdentifierName());
			}
			return nullptr;
		}
		entry = found->second.get();
		level = entry->schemas;
	}
	return entry.get();
}

void HugrCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	auto snapshot = SnapshotOf(context);
	std::function<void(HugrSchemaEntry &)> visit = [&](HugrSchemaEntry &schema) {
		callback(schema);
		for (auto &nested : schema.schemas) {
			visit(*nested.second);
		}
	};
	for (auto &schema : snapshot->schemas) {
		visit(*schema.second);
	}
}

optional_ptr<CatalogEntry> HugrCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	RefuseWrite();
}
void HugrCatalog::DropSchema(ClientContext &, DropInfo &) {
	RefuseWrite();
}
PhysicalOperator &HugrCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                 PhysicalOperator &) {
	RefuseWrite();
}
PhysicalOperator &HugrCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                          optional_ptr<PhysicalOperator>) {
	RefuseWrite();
}
PhysicalOperator &HugrCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                          PhysicalOperator &) {
	RefuseWrite();
}
PhysicalOperator &HugrCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                          PhysicalOperator &) {
	RefuseWrite();
}
unique_ptr<LogicalOperator> HugrCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                         unique_ptr<LogicalOperator>) {
	RefuseWrite();
}

DatabaseSize HugrCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

} // namespace hugr
} // namespace duckdb
