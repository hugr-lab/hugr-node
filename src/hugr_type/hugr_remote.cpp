// A statement sent to B on a lent connection (spec 011): the PREPARE and its one retry, and a listing read whole.

#include "hugr_remote.hpp"

#include "hugr_client.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace hugr {

namespace {

//! a listing read whole, as acl's own metadata bound (acl_metadata_max_rows' default)
constexpr idx_t LISTING_MAX_ROWS = 1000000;

} // namespace

unique_ptr<PrepareResponseMessage> Prepare(Pool &pool, ClientContext &context, Loan &loan, const string &query,
                                           hugeint_t query_uuid, bool wait) {
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
			if (!MayRetry(answer, false, retried)) {
				throw;
			}
			pool.Count("hugr.pool.retries");
			// the ended connection gives its place back before the new one is asked for: at the pool's size the
			// retry would otherwise wait for its own dead connection
			loan.Release();
			loan = pool.Borrow(context, wait, true);
		}
	}
}

unique_ptr<ColumnDataCollection> ReadAll(Pool &pool, ClientContext &context, const string &query, bool wait) {
	auto loan = pool.Borrow(context, wait);
	// internal traffic carries uuid 0: it never displaces a client's retained result on B, and FETCH names it too
	auto response = Prepare(pool, context, loan, query, hugeint_t {0, 0}, wait);
	auto result = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator());
	auto types = response->Types();
	result->Initialize(types);
	// every chunk of the type the PREPARE announced, and a bound on the whole: B's answer is not trusted
	auto append = [&](DataChunk &chunk) {
		if (chunk.GetTypes() != types) {
			throw InvalidInputException("hugr: B's listing changed its shape while it was read");
		}
		if (result->Count() + chunk.size() > LISTING_MAX_ROWS) {
			throw InvalidInputException("hugr: B's listing passed %llu rows", LISTING_MAX_ROWS);
		}
		result->Append(chunk);
	};
	for (auto &chunk : response->MutableResults()) {
		append(chunk->Chunk());
	}
	if (response->NeedsMoreFetch()) {
		auto &connection = loan.Client();
		auto client = connection.GetClient(context);
		try {
			for (idx_t batch = 1;; batch++) {
				if (context.IsInterrupted()) {
					throw InterruptException();
				}
				// every batch before this one acknowledged: a listing never asks for an earlier batch again
				auto fetch = client->GetClient().Request<FetchResponseMessage>(
				    context,
				    make_uniq<FetchRequestMessage>(connection.ConnectionId(), response->QueryUUID(), batch, batch - 1));
				if (fetch->MutableResults().empty()) {
					break;
				}
				for (auto &chunk : fetch->MutableResults()) {
					append(*chunk);
				}
			}
		} catch (std::exception &ex) {
			// B's query may still run: the connection does not go back (the next PREPARE would supersede it)
			loan.Answered(ClassifyAnswer(ErrorData(ex).RawMessage()));
			loan.Answered(Answer::RECONNECT);
			throw;
		}
	}
	return result;
}

} // namespace hugr
} // namespace duckdb
