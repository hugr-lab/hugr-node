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
#include "duckdb/parser/keyword_helper.hpp"

#include <queue>

namespace duckdb {
namespace hugr {

unique_ptr<FunctionData> HugrScanBindData::Copy() const {
	auto result = make_uniq<HugrScanBindData>();
	result->catalog = catalog;
	result->pool = pool;
	result->remote_name = remote_name;
	result->local_name = local_name;
	result->column_names = column_names;
	result->column_types = column_types;
	result->table_entry = table_entry;
	return std::move(result);
}

bool HugrScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<HugrScanBindData>();
	return other.pool == pool && other.remote_name == remote_name;
}

namespace {

//! A chunk of B's answer: exactly the scan's projection, as the query asked for it (BuildQuery)
class ChunkResult {
public:
	explicit ChunkResult(DataChunk &chunk_p) {
		chunk = make_uniq<DataChunk>();
		chunk->InitializeEmpty(chunk_p.GetTypes());
		chunk->Reference(chunk_p);
	}
	DataChunk &Chunk() {
		return *chunk;
	}

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

	//! B's answer ended the connection (a scan thread saw it): it does not go back to the pool.
	void Answered(Answer answer) {
		lock_guard<mutex> guard(loan_lock);
		loan.Answered(answer);
	}

	//! B's stream ended - a FETCH past its total: nothing is in flight and B's query is over, so the connection goes
	//! back now, not when the statement ends (a scan's state lives until then, a streamed result's longer).
	void Finished() {
		completed = true;
		lock_guard<mutex> guard(loan_lock);
		loan.Release();
	}

	//! The scan is over before its end (an early stop, an error), or ended: a query still running on B is
	//! cancelled first - B's CANCEL returns once it is over (it joins the producer) and answers the FETCHes in
	//! flight, so the drain after it waits for no batch B was still to produce - then the connection goes back,
	//! and the next borrower's PREPARE supersedes nothing.
	void End() {
		if (loan && !completed && needs_more_fetch) {
			try {
				loan.Client().CancelQuery(query_uuid);
			} catch (std::exception &ex) {
				loan.Answered(ClassifyAnswer(ErrorData(ex).RawMessage()));
				loan.Answered(Answer::RECONNECT); // a connection whose query state is unknown never goes back
			}
		}
		if (fetcher) {
			fetcher->StopAndDrain();
			fetcher.reset();
		}
		loan.Release();
	}

	Loan loan;
	mutex loan_lock;
	shared_ptr<HugrFetcher> fetcher;
	optional_ptr<const HugrScanBindData> bind_data;
	//! the types of B's answer, column by column, as the query asked for them
	vector<LogicalType> expected;
	idx_t max_threads = 1;
	hugeint_t query_uuid;
	bool needs_more_fetch = false;
	atomic<bool> completed {false};

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
	std::queue<ChunkResult> results;
};

//! The query sent to B, and the types its answer must have. Columns are asked for BY NAME, never by position: B's
//! shape may have changed since the snapshot (a column hidden from A's role), and a position would then read
//! another column under this one's masks.
string BuildQuery(const HugrScanBindData &bind_data, const TableFunctionInitInput &input,
                  vector<LogicalType> &expected) {
	string select;
	auto add = [&](const string &item, const LogicalType &type) {
		select += (select.empty() ? "" : ", ") + item;
		expected.push_back(type);
	};
	for (auto &column : input.column_indexes) {
		if (column.IsVirtualColumn()) {
			auto virtual_column = column.GetPrimaryIndex();
			if (virtual_column == COLUMN_IDENTIFIER_EMPTY) {
				// count(*): any value will do, of the type HugrGetVirtualColumns declared
				add("NULL::BOOLEAN", LogicalType::BOOLEAN);
			} else if (virtual_column == COLUMN_IDENTIFIER_ROW_ID) {
				throw NotImplementedException("hugr does not support rowid on remote tables");
			} else {
				throw InternalException("Unsupported virtual column index");
			}
		} else {
			auto index = column.GetPrimaryIndex();
			add(KeywordHelper::WriteQuoted(bind_data.column_names[index], '"'), bind_data.column_types[index]);
		}
	}
	if (select.empty()) {
		// no projection: every column, in the snapshot's order
		for (idx_t i = 0; i < bind_data.column_names.size(); i++) {
			add(KeywordHelper::WriteQuoted(bind_data.column_names[i], '"'), bind_data.column_types[i]);
		}
	}
	return "SELECT " + select + " FROM " + bind_data.remote_name.ToString();
}

//! B answered in another shape than the snapshot's (a type changed, a column moved): the statement fails cleanly -
//! never a chunk referenced over the wrong layout - and the next transaction reads B's catalog again.
void CheckShape(const HugrScanBindData &bind_data, const vector<LogicalType> &expected,
                const vector<LogicalType> &answered) {
	if (answered == expected) {
		return;
	}
	bind_data.catalog->MarkStale();
	throw InvalidInputException("hugr: %s changed on B since this node read B's catalog - the next statement reads "
	                            "it again",
	                            bind_data.local_name);
}

unique_ptr<GlobalTableFunctionState> HugrScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HugrScanBindData>();
	auto state = make_uniq<HugrScanGlobalState>();
	state->bind_data = &bind_data;
	auto query = BuildQuery(bind_data, input, state->expected);

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
	CheckShape(bind_data, state->expected, response->Types());
	for (auto &chunk : response->MutableResults()) {
		CheckShape(bind_data, state->expected, chunk->Chunk().GetTypes());
		state->results.emplace_back(chunk->Chunk());
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
	for (auto &chunk : state.TryGetResults()) {
		local->results.push(std::move(chunk));
	}
	return std::move(local);
}

void HugrScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<HugrScanGlobalState>();
	auto &local = input.local_state->Cast<HugrScanLocalState>();
	while (true) {
		while (!local.results.empty()) {
			auto chunk = std::move(local.results.front());
			local.results.pop();
			if (chunk.Chunk().size() > 0) {
				output.Reference(chunk.Chunk());
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
						state.Answered(answer);
					}
				}
				throw;
			}
			switch (result) {
			case HugrFetchResult::BATCH:
				for (auto &chunk : chunks) {
					CheckShape(*state.bind_data, state.expected, chunk->GetTypes());
					local.results.emplace(*chunk);
				}
				local.current_batch_index = batch_index;
				continue;
			case HugrFetchResult::FINISHED:
				local.fetch_exhausted = true;
				state.Finished();
				return;
			case HugrFetchResult::BLOCKED:
				return;
			}
		}
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
