// INSERT INTO fb.main.T ... on an ATTACH opened with (TYPE firebird,
// READ_WRITE): a sink that prepares
//     INSERT INTO "T" ("C1", ...) VALUES (?, ...)
// once, with the columns of the INSERT's column list (all columns when it
// has none), and executes it once per row with FirebirdParamEncoder, in the
// DuckDB transaction's Firebird write transaction. Omitted columns are left
// to Firebird (defaults, triggers, generators). Returns the row count like
// any INSERT.

#include "firebird_insert.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"

#include "firebird_batch.hpp"
#include "firebird_cancel.hpp"
#include "firebird_client.hpp"
#include "firebird_observability.hpp"
#include "firebird_param_encoder.hpp"
#include "firebird_query_function.hpp"
#include "firebird_types.hpp"

#include <chrono>

namespace duckdb {

namespace {

// v2.0 names catalog objects with Identifier; v1.5 with string.
#ifdef FIREBIRD_DUCKDB_V2
std::string Str(const Identifier &name) {
    return name.GetIdentifierName();
}
#endif
const std::string &Str(const std::string &name) {
    return name;
}

// The Firebird objects an INSERT keeps on the write connection between its
// steps. They must go before the connection does: Finalize releases them,
// and the write transaction releases them before its connection goes back
// to the pool on every other path (an error, a failing source).
struct FirebirdInsertResources : public FirebirdWriteResources {
    std::unique_ptr<FirebirdStatement> stmt;
    std::unique_ptr<FirebirdParamEncoder> encoder;
    std::unique_ptr<FirebirdBatchWriter> batch;  // Firebird 4+, when batch_size > 0

    void ReleaseFirebird() override {
        batch.reset();
        encoder.reset();
        stmt.reset();
    }
};

struct FirebirdInsertGlobalState : public GlobalSinkState {
    std::shared_ptr<FirebirdInsertResources> res = std::make_shared<FirebirdInsertResources>();
    int64_t batch_size = 0;
    int64_t rows = 0;   // rows inserted (executed)
    int64_t added = 0;  // rows encoded
    // Record counts the batches returned (batch mode only; -1 = not counted).
    int64_t affected = -1;
    // Why a batch_size > 0 INSERT runs one row at a time (empty otherwise).
    std::string batch_fallback;
    int64_t lock_timeout_s = 10;
    // This INSERT's entry in the DuckDB transaction (see OpenFirebirdInsert).
    int64_t open_id = 0;
    // The newest telemetry record when the INSERT opened: a failure
    // recorded after it, in this statement, stays in firebird_last_query()
    // (see Report).
    uint64_t open_seq = 0;
    // The transaction's write connection, once Prepare got it (it outlives
    // the INSERT).
    FirebirdConnection *conn = nullptr;
    // For the telemetry record, made when the INSERT ends: a Firebird scan
    // in its source records itself while the INSERT runs.
    int64_t connection_id = -1;
    int64_t firebird_time_us = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

struct FirebirdInsertSourceState : public GlobalSourceState {
    bool finished = false;
};

class FirebirdInsert final : public PhysicalOperator {
public:
    FirebirdInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
                   Catalog &catalog, std::string table_name, std::string sql, vector<LogicalType> value_types,
                   vector<idx_t> input_columns, NoneEncoding none_encoding)
        : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
          catalog_(catalog), table_name_(std::move(table_name)), sql_(std::move(sql)),
          value_types_(std::move(value_types)), input_columns_(std::move(input_columns)),
          none_encoding_(none_encoding) {
    }

    string GetName() const override {
        return "FIREBIRD_INSERT";
    }

    InsertionOrderPreservingMap<string> ParamsToString() const override {
        InsertionOrderPreservingMap<string> result;
        result["Table"] = table_name_;
        return result;
    }

    // --- Sink: one execution per row ---------------------------------------
    // The INSERT opens its entry in the DuckDB transaction when the pipeline
    // starts, so a firebird_execute in its source that runs first doesn't
    // commit (see OpenFirebirdInsert). The write connection, and with it the
    // Firebird write transaction, waits for the first row: a source that
    // blocks (a sort, an aggregate) holds no Firebird transaction open while
    // it computes.
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
        auto state = make_uniq<FirebirdInsertGlobalState>();
        Value v;
        if (context.TryGetCurrentSetting("firebird_write_lock_timeout", v) && !v.IsNull()) {
            state->lock_timeout_s = v.GetValue<int64_t>();
        }
        auto writes = LockFirebirdWrites(context, catalog_);
        state->open_seq = GetObservabilityState(context)->CurrentSeq();
        state->open_id = OpenFirebirdInsert(context, catalog_, sql_, table_name_);
        return std::move(state);
    }

    // On the first row: the transaction's write connection (a
    // firebird_execute in the source shares it) and the prepared statement.
    void Prepare(ClientContext &context, FirebirdInsertGlobalState &state) const {
        try {
            auto &conn = GetFirebirdWriteConnection(context, catalog_, state.lock_timeout_s);
            state.conn = &conn;
            state.connection_id = conn.Id();
            // The prepare can wait on the server (a metadata lock); Fail
            // turns the error a cancel causes into the interrupt.
            FirebirdCancelGuard cancel(context, &conn);
            HoldFirebirdWriteResources(context, catalog_, state.res);
            state.res->stmt = make_uniq<FirebirdStatement>(conn, sql_, FirebirdStatement::PrepareOnlyTag {});
            Value b;
            if (context.TryGetCurrentSetting("firebird_write_batch_size", b) && !b.IsNull()) {
                state.batch_size = b.GetValue<int64_t>();
            }
            if (state.batch_size > 0) {
                state.res->encoder = make_uniq<FirebirdParamEncoder>(*state.res->stmt, value_types_, none_encoding_, true);
                std::string why;
                state.res->batch = FirebirdBatchWriter::TryCreate(conn, *state.res->stmt, *state.res->encoder,
                                                             isc_info_sql_stmt_insert, true, &why);
                if (!state.res->batch) {
                    state.res->encoder.reset();
                    state.batch_fallback = why;
                }
            }
            if (state.res->batch) {
                state.affected = 0;  // a batch returns record counts at no extra cost
            }
            if (!state.res->encoder) {
                state.res->encoder = make_uniq<FirebirdParamEncoder>(*state.res->stmt, value_types_, none_encoding_);
            }
        } catch (std::exception &ex) {
            Fail(context, state, "INSERT into Firebird table \"" + table_name_ + "\": " + ErrorData(ex).RawMessage());
        }
    }

    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
        auto &state = input.global_state.Cast<FirebirdInsertGlobalState>();
        auto writes = LockFirebirdWrites(context.client, catalog_);
        if (!state.res->encoder) {
            Prepare(context.client, state);
        }
        const auto t0 = std::chrono::steady_clock::now();
        // A DuckDB interrupt cancels the Firebird request this chunk waits on;
        // Fail turns the error it causes into the interrupt.
        FirebirdCancelGuard cancel(context.client, &state.res->stmt->Connection());
        for (idx_t r = 0; r < chunk.size(); ++r) {
            try {
                state.res->encoder->Encode(chunk, r, input_columns_);
                if (state.res->batch) {
                    state.res->batch->Add();
                } else {
                    state.res->stmt->ExecuteWithInput(sql_);
                }
            } catch (std::exception &ex) {
                // The rows before it go first, so a failure among them is
                // named, as one row at a time would; not on a connection the
                // error showed is gone (a cancelled one is marked broken).
                const std::string error = ErrorData(ex).RawMessage();
                if (!state.res->stmt->Connection().IsBroken()) {
                    Flush(context.client, state, t0);
                }
                AddTime(state, t0);
                FailRow(context.client, state, state.added + 1, error);
            }
            ++state.added;
            if (!state.res->batch) {
                ++state.rows;
            } else if (static_cast<int64_t>(state.res->batch->Pending()) >= state.batch_size ||
                       state.res->batch->Full()) {
                Flush(context.client, state, t0);
            }
        }
        AddTime(state, t0);
        return SinkResultType::NEED_MORE_INPUT;
    }

    SinkFinalizeType Finalize(Pipeline &, Event &, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override {
        auto &state = input.global_state.Cast<FirebirdInsertGlobalState>();
        auto writes = LockFirebirdWrites(context, catalog_);
        const auto t0 = std::chrono::steady_clock::now();
        // Only a batch has rows left to send (one row at a time, every call
        // ran in Sink). An INSERT that got no row never prepared: it has no
        // statement nor connection.
        if (state.res->batch) {
            FirebirdCancelGuard cancel(context, &state.res->stmt->Connection());
            Flush(context, state, t0);
        }
        AddTime(state, t0);
        const auto record_seq = Report(context, state, "");
        // The statement goes before the connection does (the transaction
        // releases it too, on every other path).
        state.res->ReleaseFirebird();
        // The rows commit with the DuckDB transaction; a failing commit
        // marks this record as failed.
        CloseFirebirdInsert(context, catalog_, state.open_id, record_seq);
        return SinkFinalizeType::READY;
    }

    static void AddTime(FirebirdInsertGlobalState &state, std::chrono::steady_clock::time_point since) {
        state.firebird_time_us +=
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count();
    }

    // The INSERT's telemetry record, made when it ends (success or error).
    // One row at a time, record counts would cost one round trip per row:
    // rows_affected stays NULL. On an error no row is kept: rows_written is
    // 0, and so is rows_affected when counted. A failure doesn't take
    // firebird_last_query() from one that another part of the statement
    // recorded first (an interrupt the INSERT got because DuckDB stopped
    // the query): it goes to the query log only. Returns the record's seq.
    uint64_t Report(ClientContext &context, const FirebirdInsertGlobalState &state, const std::string &error) const {
        auto rec = MakeFirebirdWriteRecord(sql_, "insert", table_name_, state.connection_id);
        rec.write_mode = state.res->batch ? "batch" : "row";
        rec.rows_written = error.empty() ? state.rows : 0;
        rec.rows_affected = error.empty() || state.affected < 0 ? state.affected : 0;
        if (!state.batch_fallback.empty()) {
            rec.not_pushed_reasons = {"batch: " + state.batch_fallback};
        }
        rec.firebird_time_us = state.firebird_time_us;
        rec.total_time_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - state.started)
                .count();
        auto obs = GetObservabilityState(context);
        if (error.empty()) {
            return obs->RecordQuery(rec, FirebirdQueryLogSize(context));
        }
        rec.error_message = error;
        return obs->RecordFailureAfter(rec, FirebirdQueryLogSize(context), state.open_seq);
    }

    // Every failure of the INSERT ends here. One caused by a cancel the
    // watcher sent (the per-row call's InterruptException, a batch's
    // "operation was cancelled" or lost connection) is the interrupt:
    // DuckDB's InterruptException, and its message in error_message. The
    // entry stays open, so the write connection stays pinned until the
    // transaction ends: the INSERT's rows can't commit with anything else.
    [[noreturn]] void Fail(ClientContext &context, const FirebirdInsertGlobalState &state,
                           const std::string &msg) const {
        // Before Prepare got the write connection there is no call a cancel
        // could have ended.
        if (state.conn && state.conn->CancelAnswered()) {
            Report(context, state, FirebirdInterruptMessage(*state.conn));
            NoteFirebirdInsertReported(context, catalog_, state.open_id);
            throw InterruptException();
        }
        Report(context, state, msg);
        NoteFirebirdInsertReported(context, catalog_, state.open_id);
        throw InvalidInputException(msg);
    }

    [[noreturn]] void FailRow(ClientContext &context, const FirebirdInsertGlobalState &state, int64_t row,
                              const std::string &error) const {
        Fail(context, state,
             "INSERT into Firebird table \"" + table_name_ + "\": row " + std::to_string(row) + " failed: " + error);
    }

    // Sends the pending batch (no-op in the per-row mode).
    void Flush(ClientContext &context, FirebirdInsertGlobalState &state,
               std::chrono::steady_clock::time_point t0) const {
        if (!state.res->batch || state.res->batch->Pending() == 0) {
            return;
        }
        const int64_t first = state.rows;
        auto res = state.res->batch->Execute();
        state.rows += static_cast<int64_t>(res.executed);
        if (state.affected >= 0) {
            state.affected = res.affected < 0 ? -1 : state.affected + res.affected;
        }
        if (res.failed && res.whole_batch) {
            AddTime(state, t0);
            Fail(context, state,
                 "INSERT into Firebird table \"" + table_name_ + "\": rows " + std::to_string(first + 1) + " to " +
                     std::to_string(first + static_cast<int64_t>(res.sent)) + " (one batch) failed: " + res.error);
        }
        if (res.failed) {
            AddTime(state, t0);
            FailRow(context, state, first + static_cast<int64_t>(res.failed_index) + 1, res.error);
        }
    }

    bool IsSink() const override {
        return true;
    }

    bool ParallelSink() const override {
        return false;
    }

    bool SinkOrderDependent() const override {
        return true;
    }

    // --- Source: the row count ---------------------------------------------
    unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &) const override {
        return make_uniq<FirebirdInsertSourceState>();
    }

    SourceResultType GetDataInternal(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &input) const override {
        auto &source = input.global_state.Cast<FirebirdInsertSourceState>();
        if (source.finished) {
            return SourceResultType::FINISHED;
        }
        auto &state = sink_state->Cast<FirebirdInsertGlobalState>();
        chunk.SetCardinality(1);
        chunk.SetValue(0, 0, Value::BIGINT(state.rows));
        source.finished = true;
        return SourceResultType::FINISHED;
    }

    bool IsSource() const override {
        return true;
    }

private:
    Catalog &catalog_;
    std::string table_name_;
    std::string sql_;
    vector<LogicalType> value_types_;
    // Input chunk column of each parameter (DuckDB 2.0 passes every table
    // column; only the ones the INSERT names are sent).
    vector<idx_t> input_columns_;
    NoneEncoding none_encoding_;
};

} // namespace

PhysicalOperator &PlanFirebirdInsert(ClientContext &, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                     optional_ptr<PhysicalOperator> plan, Catalog &catalog, bool write_enabled,
                                     NoneEncoding none_encoding) {
    const auto &table = op.table;
    if (catalog.GetAttached().IsReadOnly()) {
        throw BinderException("INSERT into Firebird table \"%s\": ATTACH '%s' is READ_ONLY", Str(table.name),
                              Str(catalog.GetName()));
    }
    if (!write_enabled) {
        throw BinderException("INSERT into Firebird table \"%s\": ATTACH '%s' does not allow writes. Attach it "
                              "with (TYPE firebird, READ_WRITE) to write to Firebird.",
                              Str(table.name), Str(catalog.GetName()));
    }
    if (op.return_chunk) {
        throw BinderException("INSERT into Firebird table \"%s\": RETURNING is not supported", Str(table.name));
    }
    // ON CONFLICT and INSERT OR REPLACE never get here: DuckDB's binder
    // refuses them, with or without a conflict target, since it checks the
    // target against the unique indexes in the table's storage info and the
    // ATTACH reports none (the primary and unique keys it loads are for
    // information_schema). It refuses MERGE INTO a table of the ATTACH too
    // (only DuckDB's own tables); PlanMergeInto refuses a MERGE that gets
    // past it.
    if (!plan) {
        throw BinderException("INSERT into Firebird table \"%s\": nothing to insert", Str(table.name));
    }

    // Column names, and the input column that carries each one.
    const auto &columns = table.GetColumns();
    vector<std::string> names;
    vector<idx_t> inputs;
    vector<LogicalType> value_types;
#ifdef FIREBIRD_DUCKDB_V2
    // DuckDB 2.0 passes every column in table order, the ones the INSERT
    // doesn't name filled with DuckDB's defaults (NULL here). Send only the
    // named ones, so Firebird applies its own defaults, triggers and
    // generators to the rest. 2.0 keeps the column map in the binder, not
    // in LogicalInsert; what it leaves is this: the binder moves the bound
    // default of every column the INSERT doesn't name into the input
    // projection, so op.bound_defaults[i] is null exactly for those.
    {
        idx_t physical = 0;
        for (auto &col : columns.Physical()) {
            const idx_t index = physical++;
            const bool named = index < op.bound_defaults.size() && op.bound_defaults[index] != nullptr;
            if (!named) {
                continue;
            }
            if (index >= plan->types.size()) {
                throw InternalException("firebird insert: column %llu beyond the input",
                                        static_cast<unsigned long long>(index));
            }
            names.push_back(Str(col.Name()));
            inputs.push_back(index);
            value_types.push_back(plan->types[index]);
        }
        if (names.empty()) {
            throw BinderException("INSERT into Firebird table \"%s\": DEFAULT VALUES is not supported; use "
                                  "firebird_execute() with INSERT ... DEFAULT VALUES",
                                  Str(table.name));
        }
    }
#else
    if (op.column_index_map.empty()) {
        for (auto &col : columns.Physical()) {
            names.push_back(Str(col.Name()));
        }
    } else {
        names.resize(plan->types.size());
        idx_t mapped = 0;
        idx_t physical = 0;
        for (auto &col : columns.Physical()) {
            const idx_t source = op.column_index_map[PhysicalIndex(physical++)];
            if (source == DConstants::INVALID_INDEX) {
                continue;
            }
            if (source >= names.size()) {
                throw InternalException("firebird insert: column map out of range");
            }
            names[source] = Str(col.Name());
            ++mapped;
        }
        if (mapped == 0) {
            throw BinderException("INSERT into Firebird table \"%s\": DEFAULT VALUES is not supported; use "
                                  "firebird_execute() with INSERT ... DEFAULT VALUES",
                                  Str(table.name));
        }
    }
    if (names.size() != plan->types.size()) {
        throw InternalException("firebird insert: %llu columns for %llu values",
                                static_cast<unsigned long long>(names.size()),
                                static_cast<unsigned long long>(plan->types.size()));
    }
    for (idx_t i = 0; i < names.size(); ++i) {
        inputs.push_back(i);
    }
    value_types = plan->types;
#endif

    std::string sql = "INSERT INTO " + QuoteIdent(Str(table.name)) + " (";
    std::string marks;
    for (idx_t i = 0; i < names.size(); ++i) {
        sql += (i ? ", " : "") + QuoteIdent(names[i]);
        marks += i ? ", ?" : "?";
    }
    sql += ") VALUES (" + marks + ")";

    auto &insert = planner.Make<FirebirdInsert>(op.types, op.estimated_cardinality, catalog, Str(table.name), sql,
                                                value_types, inputs, none_encoding);
    insert.children.push_back(*plan);
    return insert;
}

} // namespace duckdb
