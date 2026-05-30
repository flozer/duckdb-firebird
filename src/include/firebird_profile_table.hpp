#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// firebird_profile_table(qualified_name VARCHAR)
//
// Factual single-table diagnostic for a Firebird relation reachable
// through an attached Firebird catalog. The argument is a qualified
// name in `catalog.schema.table` form (e.g. 'fb.main.CUSTOMER'). The
// schema part is accepted only as 'main' — the Firebird ATTACH path
// exposes exactly one schema — and may be omitted ('fb.CUSTOMER').
//
// Emits a single row describing what the extension can learn cheaply
// and safely from the RDB$ system tables, without touching business
// data beyond a best-effort PK MIN/MAX probe:
//
//   - object_type           TABLE | VIEW
//   - has_primary_key       BOOLEAN
//   - primary_key_columns   LIST(VARCHAR)
//   - indexes               LIST(VARCHAR)  ("NAME (COL, COL) [UNIQUE]")
//   - watermark_candidates  LIST(VARCHAR)  monotonic-ish columns
//   - filter_candidates     LIST(VARCHAR)  indexed / cheap WHERE columns
//   - full_scan_risk        LOW | MEDIUM | HIGH
//   - recommended_partitions INTEGER       advisory only
//   - warnings              LIST(VARCHAR)
//
// This is a *factual* diagnostic, not a cost-based advisor. Heuristics
// are simple and explicit; the warnings column carries the caveats.
TableFunction GetFirebirdProfileTableFunction();

} // namespace duckdb
