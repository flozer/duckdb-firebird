#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

// Builds the SELECT statement that DuckDB will execute against Firebird,
// translating projection pushdown (column_ids) and filter pushdown
// (TableFilterSet) into native Firebird SQL.
//
// Filters that can't be expressed in Firebird SQL stay in DuckDB and are
// reapplied above the scan (the bind data records which filters were
// consumed so the caller knows what to drop from the planner).
class FirebirdQueryBuilder {
public:
    struct Result {
        std::string sql;
        // Indices into the original filter set that were *not* pushed down,
        // i.e. DuckDB still has to re-evaluate them.
        std::vector<idx_t> residual_filter_indices;
    };

    static Result Build(const std::string &table_name,
                        const std::vector<std::string> &all_column_names,
                        const std::vector<LogicalType>  &all_column_types,
                        const std::vector<column_t>     &column_ids,
                        optional_ptr<TableFilterSet>     filters,
                        optional_idx                     limit,
                        const std::string               &extra_predicate = {});
};

} // namespace duckdb
