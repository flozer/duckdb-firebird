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

// Cached descriptor built once at ATTACH from the already-loaded PK/UNIQUE
// constraint map and column types — zero new Firebird I/O.
struct PrimaryKeyDescriptor {
    bool                        has_pk        = false;
    duckdb::vector<std::string> columns;        // ordinal order
    bool                        single_numeric = false; // 1 col AND numeric type
};

enum class PkRangeStrategy { SERIAL, PK_RANGE_PARTITIONABLE };

struct PkRangeClassification {
    bool            eligible;
    std::string     column;   // "" when not single-col
    std::string     reason;   // no primary key | composite PK | non-numeric PK | single numeric PK
    PkRangeStrategy strategy;
};

// Pure classifier — derives one of 4 normalized reasons from a descriptor.
// No I/O; safe to call in any context.
PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d);

// Single source of truth for scan-parallelism sizing: given a PK's
// [min_v, max_v] range, returns how many partitions the scanner will
// actually use. Pure/numeric — no Firebird I/O, safe to call from
// anywhere (firebird_profile_table's advisory recommendation and
// firebird_explain_pushdown's planned_partitions column both call this
// so they can never diverge from the scanner's real behavior).
idx_t PickPartitionCount(int64_t min_v, int64_t max_v);

struct FirebirdBindData : public TableFunctionData {
    FirebirdConnectionInfo conn_info;
    std::string table_name;
    duckdb::vector<std::string> column_names;
    duckdb::vector<LogicalType> column_types;
    // Per-column Firebird metadata (charset id, sqltype, etc.). Same
    // length / ordering as column_names. Used at fetch time to know
    // whether a text column is CHARACTER SET NONE and at planning
    // time to gate text-filter pushdown.
    duckdb::vector<FirebirdColumnDesc> column_descs;
    std::unique_ptr<PrimaryKeyInfo> pk;
    // Full descriptor built at ATTACH time from the constraint map + column
    // types (zero I/O). Carries has_pk / columns / single_numeric for
    // ClassifyPkRange; populated by FirebirdTableEntry::GetScanFunction.
    PrimaryKeyDescriptor pk_descriptor;
    idx_t partitions_override = 0;
    // Optional ROWS N hint pushed into every per-partition Firebird query.
    // 0 = unset (no limit). DuckDB's TableFunction API has no built-in
    // LIMIT pushdown hook, so this is opt-in via the `row_limit=N` named
    // parameter for callers who know they only need the first N rows.
    optional_idx limit_override;
    // Optional 0-based offset paired with `row_limit`. Emits Firebird's
    // `ROWS M+1 TO M+N` form. v0.5 deliberately requires `row_limit`
    // alongside `row_offset` (FirebirdScanBind raises a BinderException
    // if only the offset is set) — pure offset is a "skip then drain"
    // pattern that is expensive and surprising.
    optional_idx offset_override;
    // Optional shared connection pool — populated by the ATTACH path
    // (FirebirdTableEntry::GetScanFunction). Direct firebird_scan() calls
    // leave this null, so each LocalState constructs its own connection.
    std::shared_ptr<FirebirdConnectionPool> pool;
    // Pre-translated WHERE fragments lifted from BoundFunctionExpression
    // filters that the TableFilterSet path doesn't carry — LIKE 'prefix%'
    // ESCAPE '\\', NOT IN (?, ?, ...), BETWEEN ? AND ?, and the boolean
    // NOT (...) wrapper around any of the above. Each entry already has
    // its identifiers via QuoteIdent and emits `?` placeholders for the
    // value positions; `params` holds the matching constants in the
    // same order they appear inside `sql`. The scanner concatenates the
    // SQL fragments with AND glue and threads the params through the
    // bind path (FirebirdQueryBuilder + FirebirdConnection::OpenCursor).
    struct ExtraPredicate {
        std::string sql;
        duckdb::vector<Value> params;
    };
    duckdb::vector<ExtraPredicate> extra_predicates;
    // Pushdown explainability (Phase 4 #3). Coarse reasons for complex
    // filters (LIKE / NOT IN) that FirebirdScanPushdownComplexFilter
    // considered but did NOT push because the target column is
    // CHARACTER SET NONE under non-strict transcoding. The filter stays in
    // DuckDB (correctly re-applied above the scan); this list only carries
    // the telemetry so firebird_last_query() can explain *why* the complex
    // predicate did not reach Firebird. Surfaced as NONE_CHARSET entries in
    // not_pushed_reasons, with a matching residual_filters placeholder.
    duckdb::vector<std::string> gated_complex_reasons;
    // How to handle Firebird text columns whose CHARACTER SET is NONE
    // (server does NOT transliterate, so the bytes arriving in our
    // buffers can be anything). The default is WIN1252 because the
    // overwhelming majority of legacy Brazilian / Western-European
    // Firebird databases — Athenas, IBExpert exports, RM Sistemas,
    // delphi-era ERPs — wrote their NONE columns through a
    // Windows-1252 client. STRICT raises on non-UTF-8 and is the
    // safest choice for known-UTF-8-source databases; ISO_8859_1
    // covers Latin-1 inputs; BLOB surfaces the column as DuckDB BLOB
    // (raw bytes). Aligned with fb-cdc-rust's `charset_for_none_fields`
    // default. See enum NoneEncoding.
    NoneEncoding none_encoding = NoneEncoding::WIN1252;
    // True when RDB$DATABASE.RDB$CHARACTER_SET_NAME = NONE. Drives
    // the warning + influences pushdown safety (text-filter literals
    // are UTF-8 in DuckDB and may not round-trip against NONE bytes).
    bool db_charset_none = false;
    // View-shape signals for the ROWS-pagination safety decision (see
    // firebird_scanner.cpp's OpenNextPartitionCursor). Populated at bind
    // time only when the target has no usable single-column numeric PK —
    // computing this for every scan would be wasted I/O when the PK path
    // already gives a safe, cheap ORDER BY column.
    bool is_view = false;
    // Meaningful only when is_view is true: no JOIN / GROUP BY / aggregate
    // detected AND the source was inspectable. A simple pass-through view
    // safely inherits its base table's RDB$DB_KEY (verified empirically);
    // a heavy or uninspectable view does not (RDB$DB_KEY silently returns
    // SQL NULL for a self-JOIN + GROUP BY view — not an error).
    bool is_view_simple_for_pagination = false;
};

// --- discovery + probe helpers ---------------------------------------------

// Reads RDB$RELATION_FIELDS ⋈ RDB$FIELDS for `table_name` and populates the
// output vectors with Firebird → DuckDB type mappings. Throws BinderException
// if the table doesn't exist.
//
// `none_encoding` controls how Firebird CHARACTER SET NONE text columns
// are surfaced when present: STRICT/WIN1252/ISO_8859_1 keep them as
// VARCHAR (transcoded at fetch); BLOB surfaces them as BLOB.
//
// `out_descs` carries the raw Firebird per-column metadata; the caller
// uses it to detect NONE columns and gate filter pushdown accordingly.
void LoadTableSchema(FirebirdConnection &conn,
                     const std::string &table_name,
                     duckdb::vector<std::string> &out_names,
                     duckdb::vector<LogicalType> &out_types,
                     duckdb::vector<FirebirdColumnDesc> &out_descs,
                     NoneEncoding none_encoding = NoneEncoding::WIN1252);

// One resolved table schema, as produced by LoadAllTableSchemas. The three
// column vectors are parallel (same length / ordering), matching the
// out_names / out_types / out_descs triple that LoadTableSchema fills.
struct FirebirdTableSchema {
    std::string table_name;
    duckdb::vector<std::string> names;
    duckdb::vector<LogicalType> types;
    duckdb::vector<FirebirdColumnDesc> descs;
};

// Batch variant of LoadTableSchema: reads the columns for EVERY user table in
// a single RDB$RELATION_FIELDS ⋈ RDB$FIELDS ⋈ RDB$RELATIONS query and groups
// the rows per relation client-side. Collapses the historical N-round-trip
// (one query per table) catalog load into a single round-trip — the dominant
// cost when ATTACHing a large schema over a high-latency link. Applies the
// same blr_* → SQL_* normalisation and NONE-charset handling as
// LoadTableSchema. Throws on query failure; the ATTACH path falls back to the
// per-table loop. Excludes system relations and MON$ snapshots (type 3),
// mirroring the RDB$RELATIONS filter used by the catalog discovery query.
duckdb::vector<FirebirdTableSchema> LoadAllTableSchemas(
    FirebirdConnection &conn,
    NoneEncoding none_encoding = NoneEncoding::WIN1252);

// Reads `RDB$DATABASE.RDB$CHARACTER_SET_NAME`. Returns true when the
// database-level default is NONE. Best-effort: any RDB$ error returns
// false (we don't want a probe failure to break a working scan).
bool DatabaseCharsetIsNone(FirebirdConnection &conn);

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
