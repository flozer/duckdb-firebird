#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/table_filter.hpp"

#include "firebird_client.hpp"  // FirebirdColumnDesc, NoneEncoding

namespace duckdb {

// Builds the SELECT statement that DuckDB will execute against Firebird,
// translating projection pushdown (column_ids) and filter pushdown
// (TableFilterSet) into native Firebird SQL.
//
// Filters that can't be expressed in Firebird SQL stay in DuckDB and are
// reapplied above the scan (the bind data records which filters were
// consumed so the caller knows what to drop from the planner).
//
// Text columns whose Firebird CHARACTER SET is NONE (character_set_id == 0)
// are special-cased: when `none_encoding != STRICT`, any predicate that
// touches such a column is left residual so DuckDB re-evaluates it after
// transcoding. The SqlLiteral we'd otherwise emit is UTF-8 and would not
// compare equal to raw CP1252 / Latin-1 bytes server-side.
class FirebirdQueryBuilder {
public:
    struct Result {
        std::string sql;
        // Indices into the original filter set that were *not* pushed down,
        // i.e. DuckDB still has to re-evaluate them.
        std::vector<idx_t> residual_filter_indices;
        // Parameter values bound positionally to the `?` placeholders in
        // `sql`. Empty when no filters were parametrised (the no-filter
        // case, or filters whose literal types can't be bound — those go
        // residual instead of being interpolated).
        std::vector<Value> params;
    };

    static Result Build(const std::string &table_name,
                        const std::vector<std::string> &all_column_names,
                        const std::vector<LogicalType>  &all_column_types,
                        const std::vector<column_t>     &column_ids,
                        optional_ptr<TableFilterSet>     filters,
                        optional_idx                     limit,
                        const std::string               &extra_predicate = {},
                        const std::vector<FirebirdColumnDesc> *column_descs = nullptr,
                        NoneEncoding                     none_encoding = NoneEncoding::WIN1252,
                        optional_idx                     offset = optional_idx());
};

} // namespace duckdb
