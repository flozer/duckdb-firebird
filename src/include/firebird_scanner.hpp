#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// --- shared types -----------------------------------------------------------
//
// These are exposed so the storage_extension layer (ATTACH path) can build
// FunctionData up-front in TableCatalogEntry::GetScanFunction without
// re-running the bind callback.

struct PrimaryKeyInfo {
    std::string column;
    int64_t min_value = 0;
    int64_t max_value = 0;
};

struct FirebirdBindData : public TableFunctionData {
    FirebirdConnectionInfo conn_info;
    std::string table_name;
    duckdb::vector<std::string> column_names;
    duckdb::vector<LogicalType> column_types;
    std::unique_ptr<PrimaryKeyInfo> pk;
    idx_t partitions_override = 0;
    // Optional ROWS N hint pushed into every per-partition Firebird query.
    // 0 = unset (no limit). DuckDB's TableFunction API has no built-in
    // LIMIT pushdown hook, so this is opt-in via the `row_limit=N` named
    // parameter for callers who know they only need the first N rows.
    optional_idx limit_override;
    // Optional shared connection pool — populated by the ATTACH path
    // (FirebirdTableEntry::GetScanFunction). Direct firebird_scan() calls
    // leave this null, so each LocalState constructs its own connection.
    std::shared_ptr<FirebirdConnectionPool> pool;
};

// --- discovery + probe helpers ---------------------------------------------

// Reads RDB$RELATION_FIELDS ⋈ RDB$FIELDS for `table_name` and populates the
// output vectors with Firebird → DuckDB type mappings. Throws BinderException
// if the table doesn't exist.
void LoadTableSchema(FirebirdConnection &conn,
                     const std::string &table_name,
                     duckdb::vector<std::string> &out_names,
                     duckdb::vector<LogicalType> &out_types);

// Best-effort PK probe — returns nullptr for tables without a single-column
// numeric PK (composite, non-numeric, missing, or any RDB$ access error).
std::unique_ptr<PrimaryKeyInfo> ProbePrimaryKey(
    FirebirdConnection &conn,
    const std::string &table_name,
    const duckdb::vector<std::string> &all_column_names,
    const duckdb::vector<LogicalType> &all_column_types);

// --- table-function factories ----------------------------------------------

// firebird_scan(connection_string, table_name) — table function entry point.
TableFunction GetFirebirdScanFunction();

// firebird_tables(connection_string) — list user tables for discoverability.
TableFunction GetFirebirdTablesFunction();

// firebird_attach_sql(connection_string [, schema]) — emits CREATE SCHEMA
// + CREATE OR REPLACE VIEW DDL the user can pipe back into DuckDB. A
// lightweight stand-in for the full ATTACH StorageExtension.
TableFunction GetFirebirdAttachFunction();

} // namespace duckdb
