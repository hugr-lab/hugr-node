// A read of one of B's tables or views (spec 011): one loan per scan, `SELECT <projection> FROM <path>`, the first
// rows inline with the PREPARE, the rest FETCHed ahead on the ASYNC pool (quack's fetcher). quack's own scan
// (quack_scan.cpp) is the model; what differs is the loan - lent at the scan's start, back when it ends, after its
// remote query is over - and the one retry of a PREPARE whose connection B ended.

#include "hugr_scan.hpp"

#include "hugr_catalog.hpp"
#include "hugr_client.hpp"
#include "hugr_client_fetch_ahead.hpp"
#include "hugr_remote.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_context.hpp"

#include <queue>

namespace duckdb {
namespace hugr {

unique_ptr<FunctionData> HugrScanBindData::Copy() const {
	auto result = make_uniq<HugrScanBindData>();
	result->catalog = catalog;
	result->pool = pool;
	result->remote_name = remote_name;
	result->table_entry = table_entry;
	return std::move(result);
}

bool HugrScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<HugrScanBindData>();
	return other.pool == pool && other.remote_name == remote_name;
}

namespace {

class ChunkResult {
public:
	ChunkResult(DataChunk &chunk_p, bool projected_p) : projected(projected_p) {
		chunk = make_uniq<DataChunk>();
		chunk->InitializeEmpty(chunk_p.GetTypes());
		chunk->Reference(chunk_p);
	}
	DataChunk &Chunk() {
		return *chunk;
	}
	//! the chunk is the scan's projection already (the query sent carried it)
	bool projected;

private:
	unique_ptr<DataChunk> chunk;
};

struct HugrScanGlobalState : public GlobalTableFunctionState {
	~HugrScanGlobalState() override {
		End();
	}
	idx_t MaxThreads() const override {
		return max_threads;
	}

	//! The scan is over (its end, an early stop, an error): stop the read-ahead, end the remote query if it still
	//! runs - B's CANCEL returns once it is (it joins the producer), so the next borrower's PREPARE supersedes
	//! nothing - and give the connection back.
	void End() {
		if (fetcher) {
			fetcher->StopAndDrain();
			fetcher.reset();
		}
		if (loan && !completed && needs_more_fetch) {
			try {
				loan.Client().CancelQuery(query_uuid);
			} catch (std::exception &ex) {
				loan.Answered(ClassifyAnswer(ErrorData(ex).RawMessage()));
				loan.Answered(Answer::RECONNECT); // a connection whose query state is unknown never goes back
			}
		}
		loan.Release();
	}

	Loan loan;
	shared_ptr<HugrFetcher> fetcher;
	idx_t max_threads = 1;
	vector<ColumnIndex> column_ids;
	vector<idx_t> projection_ids;
	hugeint_t query_uuid;
	bool needs_more_fetch = false;
	bool projected = false;
	atomic<bool> completed {false};
	atomic<idx_t> active_scanners {0};

	vector<ChunkResult> TryGetResults() {
		lock_guard<mutex> guard(lock);
		return std::move(results);
	}
	vector<ChunkResult> results;

private:
	mutex lock;
};

struct HugrScanLocalState : public LocalTableFunctionState {
	optional_idx current_batch_index;
	optional_idx fetch_claim;
	bool fetch_exhausted = false;
	bool finish_reported = false;
	std::queue<ChunkResult> results;
};

string BuildQuery(const HugrScanBindData &bind_data, const TableFunctionInitInput &input) {
	string select;
	for (auto &column : input.column_indexes) {
		if (!select.empty()) {
			select += ", ";
		}
		if (column.IsVirtualColumn()) {
			auto virtual_column = column.GetPrimaryIndex();
			if (virtual_column == COLUMN_IDENTIFIER_EMPTY) {
				// count(*): any value will do, of the type HugrGetVirtualColumns declared
				select += "NULL::BOOLEAN";
			} else if (virtual_column == COLUMN_IDENTIFIER_ROW_ID) {
				throw NotImplementedException("hugr does not support rowid on remote tables");
			} else {
				throw InternalException("Unsupported virtual column index");
			}
		} else {
			select += "#" + to_string(column.GetPrimaryIndex() + 1);
		}
	}
	auto from = "FROM " + bind_data.remote_name.ToString();
	return select.empty() ? from : "SELECT " + select + " " + from;
}

unique_ptr<GlobalTableFunctionState> HugrScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HugrScanBindData>();
	auto state = make_uniq<HugrScanGlobalState>();
	state->column_ids = input.column_indexes;
	state->projection_ids = input.projection_ids;
	state->projected = !input.column_indexes.empty();
	auto query = BuildQuery(bind_data, input);

	state->loan = bind_data.pool->Borrow(context);
	state->query_uuid = UUID::GenerateRandomUUID();
	unique_ptr<PrepareResponseMessage> response;
	try {
		response = Prepare(*bind_data.pool, context, state->loan, query, state->query_uuid);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::BINDER || error.Type() == ExceptionType::CATALOG) {
			// B named an object or column that is gone: the next transaction reads B's catalog again. This
			// statement is not retried - its plan was bound to the old shape (spec 011, "The catalog")
			bind_data.catalog->MarkStale();
		}
		throw;
	}
	state->needs_more_fetch = response->NeedsMoreFetch();
	state->query_uuid = response->QueryUUID();
	for (auto &chunk : response->MutableResults()) {
		state->results.emplace_back(chunk->Chunk(), state->projected);
	}
	state->max_threads = state->needs_more_fetch ? GlobalTableFunctionState::MAX_THREADS : 1;
	if (state->needs_more_fetch) {
		state->fetcher = make_shared_ptr<HugrFetcher>(context, state->loan.Client(), state->query_uuid,
		                                              bind_data.pool->Options().read_ahead);
	} else {
		// the whole result came with the PREPARE: B's query is over, the connection may go back now
		state->completed = true;
		state->loan.Release();
	}
	return std::move(state);
}

unique_ptr<LocalTableFunctionState> HugrScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                      GlobalTableFunctionState *global_state_p) {
	auto &state = global_state_p->Cast<HugrScanGlobalState>();
	auto local = make_uniq<HugrScanLocalState>();
	state.active_scanners++;
	for (auto &chunk : state.TryGetResults()) {
		local->results.push(std::move(chunk));
	}
	return std::move(local);
}

void FinishScan(HugrScanGlobalState &state, HugrScanLocalState &local) {
	if (local.finish_reported) {
		return;
	}
	local.finish_reported = true;
	if (state.active_scanners.fetch_sub(1) != 1) {
		return;
	}
	// the last scanner: B's stream ended (a FETCH past its total), nothing is in flight
	state.completed = true;
}

void Emit(HugrScanGlobalState &state, ChunkResult &chunk, DataChunk &output) {
	auto &response_chunk = chunk.Chunk();
	if (chunk.projected) {
		output.Reference(response_chunk);
		return;
	}
	auto &projection_ids = state.projection_ids;
	auto columns = projection_ids.empty() ? state.column_ids.size() : projection_ids.size();
	for (idx_t i = 0; i < columns; i++) {
		auto &index = projection_ids.empty() ? state.column_ids[i] : state.column_ids[projection_ids[i]];
		if (index.IsVirtualColumn()) {
			if (index.GetPrimaryIndex() != COLUMN_IDENTIFIER_EMPTY) {
				throw NotImplementedException("hugr does not support rowid on remote tables");
			}
			output.data[i].Reference(Value(output.data[i].GetType()), count_t(response_chunk.size()));
			continue;
		}
		output.data[i].Reference(response_chunk.data[index.GetPrimaryIndex()]);
	}
	output.CheckCardinality(response_chunk.size());
}

void HugrScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<HugrScanGlobalState>();
	auto &local = input.local_state->Cast<HugrScanLocalState>();
	while (true) {
		while (!local.results.empty()) {
			auto chunk = std::move(local.results.front());
			local.results.pop();
			if (chunk.Chunk().size() > 0) {
				Emit(state, chunk, output);
				return;
			}
		}
		if (state.fetcher && !local.fetch_exhausted) {
			idx_t batch_index;
			vector<unique_ptr<DataChunk>> chunks;
			HugrFetchResult result;
			try {
				result = state.fetcher->GetBatch(context, input, local.fetch_claim, batch_index, chunks);
			} catch (std::exception &ex) {
				// rows may have flowed: the statement fails (spec 011), and a connection B ended does not go back
				ErrorData error(ex);
				if (error.Type() != ExceptionType::INTERRUPT) {
					auto answer = ClassifyAnswer(error.RawMessage());
					if (EndsConnection(answer)) {
						state.loan.Answered(answer);
					}
				}
				throw;
			}
			switch (result) {
			case HugrFetchResult::BATCH:
				for (auto &chunk : chunks) {
					local.results.emplace(*chunk, state.projected);
				}
				local.current_batch_index = batch_index;
				continue;
			case HugrFetchResult::FINISHED:
				local.fetch_exhausted = true;
				FinishScan(state, local);
				return;
			case HugrFetchResult::BLOCKED:
				return;
			}
		}
		FinishScan(state, local);
		return;
	}
}

OperatorPartitionData HugrScanGetPartitionData(ClientContext &, TableFunctionGetPartitionInput &input) {
	auto &local = input.local_state->Cast<HugrScanLocalState>();
	return OperatorPartitionData(local.current_batch_index.IsValid() ? local.current_batch_index.GetIndex() : 0);
}

InsertionOrderPreservingMap<string> HugrScanToString(TableFunctionToStringInput &input) {
	auto &bind_data = input.bind_data->Cast<HugrScanBindData>();
	InsertionOrderPreservingMap<string> result;
	result["Catalog"] = bind_data.pool->Catalog();
	result["Object"] = bind_data.remote_name.ToString();
	return result;
}

void HugrScanSerialize(Serializer &, const optional_ptr<FunctionData>, const BoundTableFunction &) {
	throw NotImplementedException("hugr scans cannot be serialized");
}

unique_ptr<FunctionData> HugrScanDeserialize(Deserializer &, BoundTableFunction &) {
	throw NotImplementedException("hugr scans cannot be deserialized");
}

//! count(*)'s marker, so it does not pick rowid; rowid declared too, so binding it fails cleanly at the scan
virtual_column_map_t HugrGetVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	virtual_column_map_t result;
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::ROW_TYPE)));
	return result;
}

optional_ptr<TableCatalogEntry> HugrScanGetTableEntry(optional_ptr<const FunctionData> bind_data_p) {
	return bind_data_p->Cast<HugrScanBindData>().table_entry;
}

} // namespace

TableFunction HugrScanFunction() {
	TableFunction function("hugr_scan", {}, HugrScan, nullptr, HugrScanInitGlobal, HugrScanInitLocal);
	function.projection_pushdown = true;
	function.get_partition_data = HugrScanGetPartitionData;
	function.to_string = HugrScanToString;
	function.serialize = HugrScanSerialize;
	function.deserialize = HugrScanDeserialize;
	function.get_virtual_columns = HugrGetVirtualColumns;
	function.get_table_entry = HugrScanGetTableEntry;
	return function;
}

} // namespace hugr
} // namespace duckdb
