#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// firebird_scan(connection_string, table_name) — table function entry point.
TableFunction GetFirebirdScanFunction();

} // namespace duckdb
