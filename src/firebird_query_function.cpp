// firebird_query(alias, sql): runs one SELECT on an attached Firebird
// database exactly as written and returns its rows.
//
// The statement is prepared at bind time to learn its columns
// (isc_dsql_prepare + describe); only SELECT statements (including
// selectable procedures and EXECUTE BLOCK with SUSPEND) are accepted.
// Columns map to DuckDB types the same way a scan maps the same Firebird
// types, with the ATTACH's none_encoding and numeric_widen_int64 options.
// It runs on a connection of the ATTACH's pool in that connection's
// read-only transaction, so a procedure that writes fails on the server;
// with firebird_transaction_snapshot on it reads the snapshot of the DuckDB
// transaction, like the scans. No pushdown, no partitions.

#include "firebird_query_function.hpp"

#include <chrono>

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"

#include "firebird_client.hpp"
#include "firebird_dbt_sources.hpp"
#include "firebird_observability.hpp"
#include "firebird_transaction_snapshot.hpp"
#include "firebird_types.hpp"

namespace duckdb {

namespace {

constexpr const char *kFunctionName = "firebird_query";
// DECFLOAT(34) needs at most 42 characters as text; the scan casts to
// VARCHAR(64), so the same width keeps the two identical.
constexpr int16_t kDecfloatTextLength = 64;

struct FirebirdQueryBindData : public TableFunctionData {
    std::string alias;
    std::string sql;
    FirebirdAttachHandle attach;
    // As described at bind, after the DECFLOAT coercion; the execution
    // re-describes and must find the same layout.
    duckdb::vector<FirebirdColumnDesc> descs;
    duckdb::vector<bool> as_text;  // DECFLOAT columns fetched as VARCHAR
    duckdb::vector<LogicalType> types;
    duckdb::vector<std::string> names;
};

bool IsDecfloat(int16_t sqltype) {
    return sqltype == SQL_DEC16 || sqltype == SQL_DEC34;
}

// A text BLOB's character set travels in sqlscale (the blob subtype is in
// sqlsubtype); the fetch path reads character_set_id for the NONE handling.
void FixBlobCharset(FirebirdColumnDesc &desc) {
    if (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1) {
        desc.character_set_id = static_cast<int16_t>(desc.sqlscale & 0xFF);
    }
}

// Prepares `sql` without executing it and checks it is a SELECT.
std::unique_ptr<FirebirdStatement> PrepareSelect(FirebirdConnection &conn, const std::string &sql) {
    auto stmt = make_uniq<FirebirdStatement>(conn, sql, FirebirdStatement::PrepareOnlyTag {});
    const int type = stmt->StatementType();
    if (type != isc_info_sql_stmt_select && type != isc_info_sql_stmt_select_for_upd) {
        throw BinderException(
            "%s: only SELECT statements are accepted (a selectable procedure, a CTE or an "
            "EXECUTE BLOCK with SUSPEND are SELECTs too); this statement's type is %d. Use "
            "firebird_execute() for other statements.",
            kFunctionName, type);
    }
    return stmt;
}

unique_ptr<FunctionData> FirebirdQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types,
#ifdef FIREBIRD_DUCKDB_V2
                                           vector<Identifier> &names) {
#else
                                           vector<string> &names) {
#endif
    if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
        throw BinderException("%s(alias, sql) requires two non-NULL arguments", kFunctionName);
    }
    auto bind = make_uniq<FirebirdQueryBindData>();
    bind->alias = input.inputs[0].ToString();
    bind->sql = input.inputs[1].ToString();
    bind->attach = GetFirebirdAttachHandle(context, bind->alias, kFunctionName);

    {
        auto lease = bind->attach.pool->AcquireWithInfo();
        auto conn = std::move(lease.conn);
        try {
            auto stmt = PrepareSelect(*conn, bind->sql);
            const auto &cols = stmt->columns();
            if (cols.empty()) {
                throw BinderException("%s: the statement returns no columns", kFunctionName);
            }
            for (idx_t i = 0; i < cols.size(); ++i) {
                FirebirdColumnDesc desc = cols[i];
                FixBlobCharset(desc);
                const bool text = IsDecfloat(desc.sqltype);
                LogicalType type;
                const bool none_text = desc.character_set_id == 0 &&
                                       (desc.sqltype == SQL_TEXT || desc.sqltype == SQL_VARYING ||
                                        (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1));
                if (text) {
                    type = LogicalType::VARCHAR;
                } else if (none_text && bind->attach.none_encoding == NoneEncoding::BLOB) {
                    type = LogicalType::BLOB;
                } else {
                    type = FirebirdToDuckDBType(desc, bind->attach.numeric_widen_int64);
                }
                bind->descs.push_back(desc);
                bind->as_text.push_back(text);
                bind->types.push_back(type);
                bind->names.push_back(desc.name.empty() ? "column" + std::to_string(i) : desc.name);
            }
            stmt.reset();
        } catch (...) {
            bind->attach.pool->Release(std::move(conn));
            throw;
        }
        bind->attach.pool->Release(std::move(conn));
    }

    QueryResult::DeduplicateColumns(bind->names);
    return_types = bind->types;
#ifdef FIREBIRD_DUCKDB_V2
    names = StringsToIdentifiers(bind->names);
#else
    names = bind->names;
#endif
    return std::move(bind);
}

struct FirebirdQueryGlobalState : public GlobalTableFunctionState {
    std::shared_ptr<FirebirdConnectionPool> pool;
    std::unique_ptr<FirebirdConnection> conn;
    std::unique_ptr<FirebirdStatement> cursor;
    bool done = false;

    idx_t MaxThreads() const override {
        return 1;
    }

    ~FirebirdQueryGlobalState() override {
        cursor.reset();  // before the connection goes back to the pool
        if (pool && conn) {
            try {
                conn->RestoreDefaultTransaction();
            } catch (...) {
                pool->Discard(std::move(conn));
                return;
            }
            pool->Release(std::move(conn));
        }
    }
};

unique_ptr<GlobalTableFunctionState> FirebirdQueryInitGlobal(ClientContext &context,
                                                             TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<FirebirdQueryBindData>();
    auto state = make_uniq<FirebirdQueryGlobalState>();
    state->pool = bind.attach.pool;
    auto lease = state->pool->AcquireWithInfo();
    state->conn = std::move(lease.conn);

    // Transaction snapshot (ATTACH option or SET firebird_transaction_snapshot):
    // read the DuckDB transaction's snapshot, like the scans do.
    FirebirdTransactionSnapshot *tx_snapshot = nullptr;
    if (auto *ts = GetFirebirdTransactionSnapshot(context, *bind.attach.catalog)) {
        int64_t idle_timeout_s = 60;
        Value v;
        if (context.TryGetCurrentSetting("firebird_transaction_snapshot_idle_timeout", v) && !v.IsNull()) {
            idle_timeout_s = v.GetValue<int64_t>();
        }
        ValidateTransactionSnapshotIdleTimeout(idle_timeout_s);
        const int64_t number = ts->Pin(context, idle_timeout_s);
        if (number > 0) {
            std::string error;
            if (!state->conn->TryStartSnapshotTransaction(number, &error)) {
                state->pool->Discard(std::move(state->conn));
                throw IOException("%s: could not join the snapshot of this DuckDB transaction "
                                  "(number %lld): %s",
                                  kFunctionName, static_cast<long long>(number), error);
            }
            ts->CheckHeld();
            tx_snapshot = ts;
        }
    }

    FirebirdQueryTelemetry rec;
    rec.remote_sql = bind.sql;
    rec.table_name = "";
    rec.projected_columns = bind.names;
    rec.partitions = 1;
    rec.parallel_scan = false;
    rec.snapshot_mode = tx_snapshot ? "transaction" : "serial";
    rec.partition_method = "none";
    rec.captured_at = Timestamp::GetCurrentTimestamp();
    rec.connection_id = state->conn->Id();
    rec.connection_reused = lease.reused;
    int64_t log_size = 0;
    Value setting;
    if (context.TryGetCurrentSetting("firebird_query_log_size", setting) && !setting.IsNull()) {
        log_size = setting.GetValue<int64_t>();
    }
    auto obs = GetObservabilityState(context);
    obs->RecordQuery(rec, log_size);

    const auto t0 = std::chrono::steady_clock::now();
    try {
        auto stmt = PrepareSelect(*state->conn, bind.sql);
        const auto &cols = stmt->columns();
        if (cols.size() != bind.descs.size()) {
            throw InvalidInputException("%s: the statement's columns changed since it was bound "
                                        "(%llu then, %llu now); run it again",
                                        kFunctionName, static_cast<unsigned long long>(bind.descs.size()),
                                        static_cast<unsigned long long>(cols.size()));
        }
        for (idx_t i = 0; i < cols.size(); ++i) {
            if (bind.as_text[i]) {
                stmt->CoerceOutputToText(i, kDecfloatTextLength);
                continue;
            }
            FirebirdColumnDesc desc = cols[i];
            FixBlobCharset(desc);
            const auto &was = bind.descs[i];
            if (desc.sqltype != was.sqltype || desc.sqlscale != was.sqlscale ||
                desc.sqlsubtype != was.sqlsubtype) {
                throw InvalidInputException("%s: the type of column '%s' changed since the statement "
                                            "was bound; run it again",
                                            kFunctionName, bind.names[i]);
            }
            if (desc.sqltype == SQL_BLOB) {
                stmt->OverrideCharsetId(i, desc.character_set_id);
            }
        }
        stmt->ExecuteCursor(bind.sql);
        state->cursor = std::move(stmt);
    } catch (std::exception &ex) {
        obs->SetError(ErrorData(ex).RawMessage());
        throw;
    }
    const auto t1 = std::chrono::steady_clock::now();
    obs->AddFirebirdTimeUs(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    obs->UpdateTotalTimeUs();
    return std::move(state);
}

void FirebirdQueryFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<FirebirdQueryBindData>();
    auto &state = data.global_state->Cast<FirebirdQueryGlobalState>();
    if (state.done) {
        output.SetCardinality(0);
        return;
    }
    idx_t row = 0;
    int64_t fetch_us = 0;
    auto obs = GetObservabilityState(context);
    try {
        while (row < STANDARD_VECTOR_SIZE) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool got = state.cursor->Fetch();
            fetch_us += std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
            if (!got) {
                state.done = true;
                break;
            }
            for (idx_t c = 0; c < output.ColumnCount(); ++c) {
                FirebirdAppendValue(*state.cursor, c, output.data[c], row, bind.attach.none_encoding);
            }
            ++row;
        }
    } catch (std::exception &ex) {
        obs->SetError(ErrorData(ex).RawMessage());
        throw;
    }
    output.SetCardinality(row);
    if (row > 0) {
        obs->AddRows(static_cast<int64_t>(row));
    }
    const int64_t bytes = state.cursor->TakeBytesRead();
    if (bytes > 0) {
        obs->AddBytes(bytes);
        state.pool->AddBytesReadEstimate(bytes);
    }
    if (fetch_us > 0) {
        obs->AddFirebirdTimeUs(fetch_us);
    }
    obs->UpdateTotalTimeUs();
    if (state.done) {
        state.cursor.reset();
    }
}

} // namespace

void DescribeFirebirdQueryBind(const FunctionData &bind_data, std::string &sql,
                               std::vector<std::string> &columns) {
    auto &bind = bind_data.Cast<FirebirdQueryBindData>();
    sql = bind.sql;
    columns.assign(bind.names.begin(), bind.names.end());
}

TableFunction GetFirebirdQueryFunction() {
    TableFunction fn(kFunctionName,
#ifdef FIREBIRD_DUCKDB_V2
                     FunctionSignature()
                         .AddPositionalOnly("alias", LogicalType::VARCHAR)
                         .AddPositionalOnly("sql", LogicalType::VARCHAR),
#else
                     {LogicalType::VARCHAR, LogicalType::VARCHAR},
#endif
                     FirebirdQueryFunction, FirebirdQueryBind, FirebirdQueryInitGlobal);
    fn.projection_pushdown = false;
    fn.filter_pushdown = false;
    return fn;
}

} // namespace duckdb
