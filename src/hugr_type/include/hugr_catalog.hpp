//===----------------------------------------------------------------------===//
// hugr_catalog.hpp - B's virtual catalog as B shows it to A's node role (spec 011, "The catalog")
//
// Read-only. A snapshot of B's listings - duckdb_schemas() and duckdb_columns(), answered by B's acl as A's role's
// own - read at ATTACH and again after catalog_ttl, or after B named an object or column that is gone. A transaction
// reads the snapshot it first looked at; a new one replaces it whole for the transactions that start after.
//===----------------------------------------------------------------------===//

#pragma once

#include "hugr_pool.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {
namespace hugr {

class HugrCatalog;

//! A table or a view of B: both are read as `SELECT <columns> FROM <path>`, B's acl resolving the name.
class HugrTableEntry : public TableCatalogEntry {
public:
	HugrTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

	const ColumnList &GetColumns() const override {
		return columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

private:
	ColumnList columns;
};

//! A schema of B, or - for a catalog of B other than its MAIN one - the catalog itself, holding its schemas.
class HugrSchemaEntry : public SchemaCatalogEntry {
public:
	HugrSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, optional_ptr<SchemaCatalogEntry> parent);

	unique_ptr<CreateInfo> GetInfo() const override;

	//! The name of an entry here as B sees it: B's catalog, then the schema's path there.
	QualifiedName RemoteName(const Identifier &entry_name) const;
	//! B's catalog and this schema's path in it (a stand-in for one of B's catalogs: just the catalog)
	vector<Identifier> remote_path;

	case_insensitive_map_t<unique_ptr<HugrSchemaEntry>> schemas;
	case_insensitive_map_t<unique_ptr<HugrTableEntry>> tables;

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
};

//! One read of B's listings.
struct HugrSnapshot {
	case_insensitive_map_t<unique_ptr<HugrSchemaEntry>> schemas;
	//! objects B listed whose columns this node could not type (named, for the log), left out
	vector<string> skipped;
};

class HugrTransaction : public Transaction {
public:
	HugrTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
	//! what this transaction reads, from its first lookup on - pinned under the lock: a transaction's catalog is
	//! looked at by several pipelines' inits at once
	mutex pin_lock;
	shared_ptr<HugrSnapshot> snapshot;
};

class HugrTransactionManager : public TransactionManager {
public:
	HugrTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}
	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override {
	}

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<HugrTransaction>> transactions;
};

class HugrCatalog : public Catalog {
public:
	HugrCatalog(AttachedDatabase &db, ClientContext &context, shared_ptr<Pool> pool, string server);
	~HugrCatalog() override;

	void Initialize(bool load_builtin) override {
	}
	string GetCatalogType() override {
		return "hugr";
	}
	shared_ptr<Pool> GetPool() const {
		return pool;
	}
	//! B named an object or column that is gone: the next transaction reads B's listings again.
	void MarkStale() {
		stale = true;
	}
	[[noreturn]] void RefuseWrite() const;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;
	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return server;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;
	//! the snapshot of a lookup: the transaction's own, pinned at its first lookup
	shared_ptr<HugrSnapshot> SnapshotOf(CatalogTransaction transaction);
	shared_ptr<HugrSnapshot> SnapshotOf(ClientContext &context);
	//! the current snapshot, read again first when it is due and `context` may read B
	shared_ptr<HugrSnapshot> Current(optional_ptr<ClientContext> context);
	shared_ptr<HugrSnapshot> Load(ClientContext &context, bool wait);

	shared_ptr<Pool> pool;
	string server;
	mutex snapshot_lock;
	shared_ptr<HugrSnapshot> current;
	int64_t loaded_at = 0;
	bool refreshing = false;
	atomic<bool> stale {false};
};

} // namespace hugr
} // namespace duckdb
