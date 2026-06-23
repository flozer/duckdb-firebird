#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// firebird_health(alias VARCHAR)
//
// Single-row, read-only health diagnostic for an attached Firebird
// catalog. `alias` is the ATTACH alias (e.g. 'fb'). Emits one row of
// factual server/database metrics plus a structured `warnings` list.
//
// Columns: engine_version, ods_version, sql_dialect, default_charset,
// page_size, forced_writes, sweep_interval, oldest_transaction,
// oldest_active, oldest_snapshot, next_transaction, oit_gap, oat_gap,
// attachments, warnings.
//
// Factual diagnostic, not an opaque advisor: every warning maps to an
// explicit documented condition, and the driving counters are columns.
// Monitoring-table reads are privilege-filtered, not gated: a partial
// attachments count is a faithful result. The `mon_unavailable` warning
// is emitted only on a real monitoring-query failure.
TableFunction GetFirebirdHealthFunction();

} // namespace duckdb
