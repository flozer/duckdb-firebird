#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// firebird_index_profile(qualified_name VARCHAR)
//
// Per-index, read-only diagnostic for a Firebird relation reachable
// through an attached Firebird catalog. The argument is a qualified name
// in `catalog.schema.table` form (e.g. 'fb.main.CUSTOMER'); the schema
// part is accepted only as 'main' (the Firebird ATTACH path exposes
// exactly one schema) and may be omitted ('fb.CUSTOMER'). Missing
// relation -> BinderException.
//
// Grain: ONE ROW PER EXISTING INDEX. If the table has zero indexes, emits
// exactly ONE synthetic row instead of zero rows, so the "no indexes at
// all" signal is never silently lost to the per-index grain:
//   - index_name IS NULL marks that synthetic row (never a real index)
//   - columns is an empty list []; every other index-scoped column is a
//     typed SQL NULL on that row
//   - alerts carries {code: 'no_indexes_on_table', severity: 'HIGH', ...}
//
// Columns:
//   - index_name                  VARCHAR, nullable (NULL = synthetic row)
//   - columns                     LIST(VARCHAR)  index segment columns,
//                                 ordered; empty for an expression index
//                                 or the synthetic row
//   - is_unique                   BOOLEAN, nullable
//   - is_active                   BOOLEAN, nullable
//   - is_primary_key              BOOLEAN, nullable
//   - is_foreign_key              BOOLEAN, nullable
//   - selectivity                 DOUBLE, nullable (RDB$STATISTICS; a
//                                 LOWER value tends to indicate a MORE
//                                 selective index; NULL means the
//                                 statistic was never computed — not
//                                 "stale". No numeric threshold-based
//                                 alert exists for this value yet.)
//   - alerts                      LIST(STRUCT(code, severity, message))
//                                 codes (stable public contract):
//                                 no_indexes_on_table (HIGH),
//                                 index_inactive (MEDIUM),
//                                 missing_statistics (LOW)
//   - unindexed_filter_candidates LIST(VARCHAR) table-level fact,
//                                 repeated on every row. Excludes any
//                                 column covered by ANY index segment,
//                                 including an inactive index's segments.
//
// This is a factual diagnostic, not a cost-based advisor: there is no
// numeric selectivity threshold in this version.
TableFunction GetFirebirdIndexProfileFunction();

} // namespace duckdb
