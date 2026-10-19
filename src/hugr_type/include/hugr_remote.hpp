//===----------------------------------------------------------------------===//
// hugr_remote.hpp - a statement sent to B on a lent connection (spec 011)
//===----------------------------------------------------------------------===//

#pragma once

#include "hugr_pool.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {
class PrepareResponseMessage;

namespace hugr {

//! PREPARE `query` on the loan's connection. B's answer that ends the connection (a lapsed lease, a gone node, an
//! ended session, no answer) is retried once on a new connection, which replaces `loan` - a PREPARE is before any
//! row reached A's pipeline. Anything else is the statement's, rethrown as B raised it.
unique_ptr<PrepareResponseMessage> Prepare(Pool &pool, ClientContext &context, Loan &loan, const string &query,
                                           hugeint_t query_uuid);

//! A query read whole on its own loan - the catalog's listings: PREPARE, then FETCH until B says the end. `wait`
//! false: a refused connect is the answer at once (the ATTACH).
unique_ptr<ColumnDataCollection> ReadAll(Pool &pool, ClientContext &context, const string &query, bool wait);

} // namespace hugr
} // namespace duckdb
