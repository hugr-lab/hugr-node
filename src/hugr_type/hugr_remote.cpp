// A statement sent to B on a lent connection (spec 011): the PREPARE and its one retry, and a listing read whole.

#include "hugr_remote.hpp"

#include "hugr_client.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hugr {

unique_ptr<PrepareResponseMessage> Prepare(Pool &pool, ClientContext &context, Loan &loan, const string &query,
                                           hugeint_t query_uuid) {
	for (bool retried = false;; retried = true) {
		auto &connection = loan.Client();
		try {
			auto client = connection.GetClient(context);
			return client->GetClient().Request<PrepareResponseMessage>(
			    context, make_uniq<PrepareRequestMessage>(connection.ConnectionId(), query, query_uuid));
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERRUPT) {
				throw;
			}
			auto answer = ClassifyAnswer(error.RawMessage());
			loan.Answered(answer);
			if (EndsConnection(answer)) {
				pool.Count("hugr.pool.reconnects", AnswerName(answer));
			}
			if (!MayRetry(answer, false, retried)) {
				throw;
			}
			pool.Count("hugr.pool.retries");
			loan = pool.Borrow(context, true, true);
		}
	}
}

unique_ptr<ColumnDataCollection> ReadAll(Pool &pool, ClientContext &context, const string &query, bool wait) {
	auto loan = pool.Borrow(context, wait);
	// internal traffic carries uuid 0: it never displaces a client's retained result on B, and FETCH names it too
	auto response = Prepare(pool, context, loan, query, hugeint_t {0, 0});
	auto result = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator());
	result->Initialize(response->Types());
	for (auto &chunk : response->MutableResults()) {
		result->Append(chunk->Chunk());
	}
	if (response->NeedsMoreFetch()) {
		auto &connection = loan.Client();
		auto client = connection.GetClient(context);
		try {
			for (idx_t batch = 1;; batch++) {
				// every batch before this one acknowledged: a listing never asks for an earlier batch again
				auto fetch = client->GetClient().Request<FetchResponseMessage>(
				    context, make_uniq<FetchRequestMessage>(connection.ConnectionId(), response->QueryUUID(), batch,
				                                            batch - 1));
				if (fetch->MutableResults().empty()) {
					break;
				}
				for (auto &chunk : fetch->MutableResults()) {
					result->Append(*chunk);
				}
			}
		} catch (std::exception &ex) {
			loan.Answered(ClassifyAnswer(ErrorData(ex).RawMessage()));
			throw;
		}
	}
	return result;
}

} // namespace hugr
} // namespace duckdb
