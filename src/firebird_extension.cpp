#define DUCKDB_EXTENSION_MAIN

#include "firebird_dbt_sources.hpp"
#include "firebird_explain_pushdown.hpp"
#include "firebird_metadata_functions.hpp"
#include "firebird_extension.hpp"
#include "firebird_observability.hpp"
#include "firebird_health.hpp"
#include "firebird_index_profile.hpp"
#include "firebird_profile_table.hpp"
#include "firebird_scanner.hpp"
#include "firebird_storage.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

namespace {

// Registers a table function with in-band documentation so duckdb_functions()
// exposes real parameter names, a description, and a runnable example.
//
// positional_names covers only the positional parameters: duckdb_functions()
// zips parameter_names against the combined list of positional arguments plus
// fn.named_parameters (iterated in hash-map order), so the named tail must be
// read from the same map here to stay index-aligned.
CreateTableFunctionInfo DescribedTableFunction(TableFunction fn, vector<string> positional_names,
                                               string description, string example, vector<string> categories) {
    FunctionDescription desc;
    desc.parameter_names = std::move(positional_names);
    for (const auto &kv : fn.named_parameters) {
        desc.parameter_names.push_back(kv.first);
    }
    desc.description = std::move(description);
    desc.examples = {std::move(example)};
    desc.categories = std::move(categories);
    CreateTableFunctionInfo info(std::move(fn));
    info.descriptions.push_back(std::move(desc));
    // Matches the bare RegisterFunction(TableFunction) overload, which wraps
    // the function in a TableFunctionSet registered with ALTER_ON_CONFLICT.
    info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
    return info;
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdScanFunction(), {"connection_string", "table_name"},
        "Reads a Firebird table into DuckDB with projection and predicate pushdown, "
        "optional parallel PK-range partitioning (partitions=N), ROWS paging, and "
        "CHARACTER SET NONE decoding.",
        "SELECT * FROM firebird_scan('database=C:/data/erp.fdb user=APP_READONLY password=secret', 'CUSTOMER');",
        {"firebird", "scan"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdTablesFunction(), {"connection_string"},
        "Lists the Firebird tables visible to a connection, without attaching the database.",
        "SELECT * FROM firebird_tables('database=C:/data/erp.fdb user=APP_READONLY password=secret');",
        {"firebird", "catalog"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdAttachFunction(), {"connection_string"},
        "Returns one CREATE VIEW statement per Firebird table, each wrapping "
        "firebird_scan(), for a view-based workflow instead of a storage ATTACH.",
        "SELECT sql FROM firebird_attach_sql('database=C:/data/erp.fdb user=APP_READONLY password=secret');",
        {"firebird", "catalog"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdLastQueryFunction(), {},
        "Returns telemetry for the most recent Firebird scan in the current session: "
        "remote SQL, pushed and residual filters with reasons, pushed paging, timing, "
        "rows read, and parallel scan and connection-reuse details.",
        "SELECT * FROM firebird_last_query();",
        {"firebird", "observability"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdQueryLogFunction(), {},
        "Returns the bounded per-session log of Firebird scans (opt-in via "
        "SET firebird_query_log_size = N), with the same telemetry columns as "
        "firebird_last_query().",
        "SELECT * FROM firebird_query_log();",
        {"firebird", "observability"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdDbtSourcesFunction(), {"catalog_name"},
        "Generates dbt sources.yml content for every Firebird table exposed by an "
        "attached catalog; the YAML is a starting point to review.",
        "SELECT yaml FROM firebird_generate_dbt_sources('fb');",
        {"firebird", "dbt"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdProfileTableFunction(), {"qualified_name"},
        "Returns a single-row factual diagnostic for one table or view behind an "
        "attached catalog: primary key, indexes, watermark and filter candidates, "
        "full-scan risk, advisory recommended_partitions, structured alerts, and a "
        "row estimate (PK-range upper bound, or exact COUNT(*) when "
        "exact_row_count=true).",
        "SELECT * FROM firebird_profile_table('fb.main.CUSTOMER');",
        {"firebird", "diagnostics"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdPoolStatsFunction(), {"catalog_name"},
        "Returns config, idle-queue size, active-lease count, lifetime counters, "
        "and the sanitized last connection error for the connection pool of one "
        "attached Firebird catalog, by explicit alias; it never leases a "
        "connection.",
        "SELECT * FROM firebird_pool_stats('fb');",
        {"firebird", "diagnostics"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdIndexesFunction(), {"catalog_name"},
        "Lists all user indexes with per-segment columns, uniqueness, activity, and "
        "expression source for expression indexes.",
        "SELECT * FROM firebird_indexes('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdForeignKeysFunction(), {"catalog_name"},
        "Lists foreign-key constraints column by column with the real Firebird "
        "update and delete referential rules.",
        "SELECT * FROM firebird_foreign_keys('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdGeneratorsFunction(), {"catalog_name"},
        "Lists user generators/sequences with their initial value and current value "
        "(read per generator via GEN_ID(name, 0)).",
        "SELECT * FROM firebird_generators('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdDomainsFunction(), {"catalog_name"},
        "Lists user-defined domains with formatted type, nullability, charset, and "
        "CHECK/DEFAULT clauses.",
        "SELECT * FROM firebird_domains('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdComputedColumnsFunction(), {"catalog_name"},
        "Lists COMPUTED BY columns of all user tables with their expression source.",
        "SELECT * FROM firebird_computed_columns('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdDependenciesFunction(), {"catalog_name"},
        "Lists dependencies between database objects (tables, views, procedures, "
        "triggers, and others), down to column level when known.",
        "SELECT * FROM firebird_dependencies('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdCommentsFunction(), {"catalog_name"},
        "Lists RDB$DESCRIPTION comments for user tables, views, and columns.",
        "SELECT * FROM firebird_comments('fb');",
        {"firebird", "metadata"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdExplainPushdownFunction(), {"sql"},
        "Analyzes a SELECT over attached Firebird tables plan-only, reporting per "
        "scan what would be pushed down (filters, projection, ROWS paging, PK-range "
        "partitions) without executing the query.",
        "SELECT * FROM firebird_explain_pushdown('SELECT EMP_ID, EMP_NAME FROM fb.main.EMPLOYEE WHERE EMP_ID > 10');",
        {"firebird", "diagnostics"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdTypeAuditFunction(), {"catalog_name"},
        "Reports per-column type and charset fidelity findings (NONE charset, "
        "DECFLOAT as VARCHAR, widenable int64 NUMERIC/DECIMAL, INT128, "
        "timezone types, text BLOBs) for an attached "
        "catalog; only columns with a caveat are emitted.",
        "SELECT * FROM firebird_type_audit('fb');",
        {"firebird", "diagnostics"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdHealthFunction(), {"alias"},
        "Returns a single-row database and server health diagnostic (engine and ODS "
        "version, dialect, charset, page size, transaction counters OIT/OAT/OST, "
        "attachments, warning codes) read from MON$ tables.",
        "SELECT * FROM firebird_health('fb');",
        {"firebird", "diagnostics"}));
    loader.RegisterFunction(DescribedTableFunction(
        GetFirebirdIndexProfileFunction(), {"qualified_name"},
        "Returns one row per index of a Firebird table (columns, uniqueness, "
        "activity, PK/FK backing, raw selectivity, structured alerts) plus "
        "unindexed filter candidates; a table with no indexes emits one synthetic "
        "row.",
        "SELECT * FROM firebird_index_profile('fb.main.CUSTOMER');",
        {"firebird", "diagnostics"}));

    // Register the StorageExtension so DuckDB knows how to handle
    //   ATTACH 'firebird://…' AS fb (TYPE firebird);
    //
    // The registration entry point varies a little across DuckDB versions:
    //   v1.4 had `config.storage_extensions[name] = unique_ptr<>;`
    //   v1.5+ exposes `StorageExtension::Register(config, name, shared_ptr<>)`
    //          (via DBConfig::GetCallbackManager — the field was made
    //          private and moved into the callback manager registry).
    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);
    auto storage_ext = GetFirebirdStorageExtension();
    StorageExtension::Register(config, "firebird",
                               shared_ptr<StorageExtension>(storage_ext.release()));

    // Chunk E - firebird_query_log() ring buffer is opt-in. The default
    // (0) disables capture; users opt in per session via:
    //   SET firebird_query_log_size = 16;
    // Per-ClientContext storage (FirebirdObservabilityState) ensures one
    // session never reads another's ring buffer.
    config.AddExtensionOption(
        "firebird_query_log_size",
        "Maximum entries kept by firebird_query_log() per session. "
        "0 disables the log (default).",
        LogicalType::BIGINT,
        Value::BIGINT(0));

    // Phase 2 - FirebirdConnectionPool tuning. Defaults reproduce the
    // pre-Phase-2 behaviour (enabled, unlimited LIFO, no expiry) so an
    // ATTACH without any SET keeps reusing connections exactly as
    // before. Settings are consumed at ATTACH time by FirebirdAttach,
    // which builds a FirebirdConnectionPoolConfig and hands it to the
    // catalog. Per-session: a SET on one connection does not retune
    // the pool of another ATTACH already in flight.
    config.AddExtensionOption(
        "firebird_pool_enabled",
        "Enable the per-ATTACH FirebirdConnectionPool. When false, every "
        "Acquire opens a fresh connection and Release destroys it.",
        LogicalType::BOOLEAN,
        Value::BOOLEAN(true));
    config.AddExtensionOption(
        "firebird_pool_max_size",
        "Maximum number of idle connections kept in the pool. "
        "0 = unlimited (default). Caps the idle queue, not active leases.",
        LogicalType::BIGINT,
        Value::BIGINT(0));
    config.AddExtensionOption(
        "firebird_pool_idle_timeout_ms",
        "How long (in milliseconds) a released connection may sit in the "
        "idle queue before it is discarded on the next Acquire. "
        "0 = no expiry (default). Clock starts at Release().",
        LogicalType::BIGINT,
        Value::BIGINT(0));

    // G1 observability wave - unpushed-filter guard. Read by the scanner
    // once per scan, at the first partition cursor open that ends up with
    // residual (not pushed) filters — the same signal telemetry records in
    // firebird_last_query().not_pushed_reasons. 'silent' (default) keeps
    // the historical behaviour at zero cost; 'warn' emits a DuckDB
    // warning; 'error' fails the query with an actionable message.
    config.AddExtensionOption(
        "firebird_unpushed_mode",
        "What to do when a Firebird scan keeps filters in DuckDB that were "
        "not pushed down to Firebird (see not_pushed_reasons in "
        "firebird_last_query()). 'silent' (default) does nothing, 'warn' "
        "emits a warning, 'error' fails the query with an actionable "
        "message.",
        LogicalType::VARCHAR,
        Value("silent"));
}

void FirebirdExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string FirebirdExtension::Name() {
    return "firebird";
}

std::string FirebirdExtension::Version() const {
#ifdef EXT_VERSION_FIREBIRD
    return EXT_VERSION_FIREBIRD;
#else
    return "0.1.0";
#endif
}

} // namespace duckdb

// --- C entry points -----------------------------------------------------------
// DuckDB ≥ 1.4 calls the *_duckdb_cpp_init symbol declared by this macro.
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(firebird, loader) {
    duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *firebird_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
