#pragma once

#include <mutex>

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// What a function that runs its own SQL on an ATTACH needs from the
// catalog: the connection pool and the type options the ATTACH's scans use.
struct FirebirdAttachHandle {
    std::shared_ptr<FirebirdConnectionPool> pool;
    Catalog *catalog = nullptr;
    NoneEncoding none_encoding = NoneEncoding::WIN1252;
    bool numeric_widen_int64 = false;
    // ATTACH ... (TYPE firebird, READ_WRITE)
    bool write_enabled = false;
    // ATTACH ... (READ_ONLY), or a read-only DuckDB database
    bool read_only = false;
};

// The connection holding the current DuckDB transaction's Firebird write
// transaction on this ATTACH, started on first use (read committed, record
// versions, lock wait up to lock_timeout_s seconds, 0 = no wait). It is
// committed or rolled back with the DuckDB transaction.
FirebirdConnection &GetFirebirdWriteConnection(ClientContext &context, Catalog &catalog,
                                               int64_t lock_timeout_s);

// One write at a time on the current DuckDB transaction's write connection
// on this ATTACH: hold the returned lock while using it. Two firebird_execute
// calls in one statement can run in parallel pipelines; they wait for each
// other (one DuckDB transaction is one Firebird transaction, so a second
// connection would not see the first call's rows nor share its locks).
std::unique_lock<std::mutex> LockFirebirdWrites(ClientContext &context, Catalog &catalog);

// Commits the current DuckDB transaction's Firebird write transaction on
// this ATTACH now, if one was started (a later write starts a new one).
void CommitFirebirdWriteNow(ClientContext &context, Catalog &catalog);

// firebird_execute ran a DDL statement in the current DuckDB transaction:
// the ATTACH's table cache is cleared when that transaction commits.
void NoteFirebirdDdl(ClientContext &context, Catalog &catalog);

// Forget an ATTACH's cached tables (the next use loads them again) and
// close its idle connections.
void ClearFirebirdCatalogCache(Catalog &catalog);

// Close an ATTACH's idle pooled connections (read and write pools): they
// keep the tables they used in their metadata cache, so Firebird refuses
// DROP / ALTER of those tables while they live.
void ReleaseFirebirdIdleConnections(Catalog &catalog);

// firebird_clear_cache(alias)
TableFunction GetFirebirdClearCacheFunction();

// firebird_execute(alias, sql): runs one statement in the DuckDB transaction's
// Firebird write transaction (ATTACH ... READ_WRITE); the rows of a statement
// that returns rows are read and discarded.
TableFunction GetFirebirdExecuteFunction();

#ifdef FIREBIRD_DUCKDB_V2
// DuckDB 2.0: the optimizer hook that tells firebird_execute whether it is
// the whole statement (its autocommit commit depends on it).
void RegisterFirebirdExecuteOptimizer(DBConfig &config);
#endif

// Resolves `alias` to a Firebird ATTACH (BinderException naming
// `function_name` otherwise). Leases nothing.
FirebirdAttachHandle GetFirebirdAttachHandle(ClientContext &context, const std::string &alias,
                                             const std::string &function_name);

// firebird_query(alias, sql): runs a SELECT on the attached database as
// written (no pushdown, no partitions) and returns its rows.
TableFunction GetFirebirdQueryFunction();

// For firebird_explain_pushdown: the SQL and the column names of a bound
// firebird_query call.
void DescribeFirebirdQueryBind(const FunctionData &bind_data, std::string &sql,
                               std::vector<std::string> &columns);

} // namespace duckdb
