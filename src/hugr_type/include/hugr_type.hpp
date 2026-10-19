//===----------------------------------------------------------------------===//
// hugr_type.hpp - the `hugr` catalog type: a platform as a source of another (spec 011)
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {
class ExtensionLoader;

namespace hugr {

//! The type's seam: `ATTACH 'b.example:443' AS b (TYPE hugr, SECRET s)`, the `hugr` secret type, hugr_pools() and
//! the hugr.pool.* gauges.
void RegisterHugrType(ExtensionLoader &loader);

} // namespace hugr
} // namespace duckdb
