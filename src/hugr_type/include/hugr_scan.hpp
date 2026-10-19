//===----------------------------------------------------------------------===//
// hugr_scan.hpp - a read of one of B's tables or views (spec 011)
//===----------------------------------------------------------------------===//

#pragma once

#include "hugr_pool.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class TableCatalogEntry;

namespace hugr {

class HugrCatalog;

struct HugrScanBindData : public FunctionData {
	HugrCatalog *catalog = nullptr;
	shared_ptr<Pool> pool;
	//! the object as B sees it (B's catalog, then the schema's path there), and as A names it
	QualifiedName remote_name;
	string local_name;
	//! its columns as the snapshot read them: what the scan asks B for by name, and what B's answer must be
	vector<string> column_names;
	vector<LogicalType> column_types;
	optional_ptr<TableCatalogEntry> table_entry;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! The scan behind every table of a `hugr` catalog: one loan per scan, `SELECT <projection> FROM <path>` (the
//! projection only - filters are stage 2's), the first rows inline, the rest FETCHed ahead.
TableFunction HugrScanFunction();

} // namespace hugr
} // namespace duckdb
