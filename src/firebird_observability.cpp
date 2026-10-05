#include "firebird_observability.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>

namespace duckdb {

// ---------------------------------------------------------------------------
//  Per-ClientContext state
// ---------------------------------------------------------------------------
//
// PM directive (Chunk A review): observability must be scoped to a
// DuckDB connection, not process-wide. The state lives in the context's
// RegisteredStateManager so its lifetime ends with the connection and
// one session never reads another's last query.

FirebirdQueryTelemetry FirebirdObservabilityState::GetLastQuery() const {
    std::lock_guard<std::mutex> g(lock_);
    return last_;
}

void FirebirdObservabilityState::RecordQuery(const FirebirdQueryTelemetry &rec,
                                              int64_t log_size) {
    std::lock_guard<std::mutex> g(lock_);
    last_ = rec;
    last_.valid = true;
    start_ = std::chrono::steady_clock::now();

    if (log_size <= 0) {
        // Logging disabled: drop any leftover entries so the off state
        // is truly empty even if the user toggled the setting mid-session.
        log_.clear();
        return;
    }
    log_.push_back(last_);
    while (log_.size() > static_cast<size_t>(log_size)) {
        log_.pop_front();
    }
}

std::vector<FirebirdQueryTelemetry> FirebirdObservabilityState::SnapshotLog() const {
    std::lock_guard<std::mutex> g(lock_);
    // Most-recent-first.
    std::vector<FirebirdQueryTelemetry> out(log_.rbegin(), log_.rend());
    return out;
}

void FirebirdObservabilityState::AddRows(int64_t n) {
    std::lock_guard<std::mutex> g(lock_);
    last_.rows_read += n;
    // Mirror the metric onto the most recent ring-buffer entry so
    // firebird_query_log() reflects the in-flight totals for the
    // current query. Older entries are frozen.
    if (!log_.empty()) {
        log_.back().rows_read = last_.rows_read;
    }
}

void FirebirdObservabilityState::AddFirebirdTimeUs(int64_t us) {
    std::lock_guard<std::mutex> g(lock_);
    last_.firebird_time_us += us;
    if (!log_.empty()) {
        log_.back().firebird_time_us = last_.firebird_time_us;
    }
}

void FirebirdObservabilityState::UpdateTotalTimeUs() {
    std::lock_guard<std::mutex> g(lock_);
    auto now = std::chrono::steady_clock::now();
    last_.total_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
    if (!log_.empty()) {
        log_.back().total_time_us = last_.total_time_us;
    }
}

void FirebirdObservabilityState::SetError(const std::string &msg) {
    auto sanitized = SanitizeErrorMessage(msg);
    std::lock_guard<std::mutex> g(lock_);
    last_.error_message = sanitized;
    // Mirror the sanitized error onto the most recent ring-buffer entry
    // so firebird_query_log() never carries a raw exception text when
    // the user has the log enabled.
    if (!log_.empty()) {
        log_.back().error_message = sanitized;
    }
}

shared_ptr<FirebirdObservabilityState> GetObservabilityState(ClientContext &ctx) {
    return ctx.registered_state->GetOrCreate<FirebirdObservabilityState>(
        FirebirdObservabilityState::KEY);
}

// ---------------------------------------------------------------------------
//  firebird_unpushed_mode — residual-filter guard (G1 observability wave)
// ---------------------------------------------------------------------------
//
// The scanner calls HandleUnpushedFilters once per scan when a scan ends up
// with residual (not pushed) filters — the same codes telemetry surfaces in
// not_pushed_reasons. The codes are a stable API; the table below only adds
// a human-facing remedy on top of each one.

static const char *RemedyForReason(const std::string &reason) {
    if (reason == "NONE_CHARSET") {
        return "columns declared CHARACTER SET NONE never push text "
               "filters; the decode is transcoded client-side - use "
               "none_encoding='strict' or, in the next release, "
               "none_pushdown";
    }
    if (reason == "UNSUPPORTED_OP") {
        return "the filter operator/shape/constant type has no Firebird SQL "
               "translation yet; simplify the predicate or accept "
               "DuckDB-side filtering";
    }
    if (reason == "ROWID_OR_INVALID_COLUMN") {
        return "the filter targets the virtual rowid or a column outside "
               "the resolved schema; filter on real columns instead";
    }
    if (reason == "UNSUPPORTED_PROJECTION_MAPPING") {
        return "the filter's projected column could not be mapped back to "
               "a source column; check the query's column list";
    }
    return "unknown reason; inspect firebird_last_query()";
}

void HandleUnpushedFilters(ClientContext &ctx,
                           const std::vector<std::string> &reasons) {
    // Load the mode. A missing setting (a stripped-down embed that never
    // registered the option) behaves exactly like 'silent'.
    std::string mode = "silent";
    Value setting_val;
    if (ctx.TryGetCurrentSetting("firebird_unpushed_mode", setting_val) &&
        !setting_val.IsNull()) {
        mode = StringUtil::Lower(setting_val.ToString());
    }
    if (mode == "silent") {
        return;
    }
    if (mode != "warn" && mode != "error") {
        // Fail loud rather than silently ignoring a typo'd value: the
        // user believes a guard is armed when it is not. This only ever
        // fires when residual filters exist, i.e. when the mode matters.
        throw InvalidInputException(
            "firebird_unpushed_mode='%s' is not valid; use 'silent', "
            "'warn' or 'error'", mode);
    }

    // Count occurrences per code, preserving first-seen order so the
    // message is deterministic across parallel workers.
    std::vector<std::string> order;
    std::map<std::string, idx_t> counts;
    for (const auto &reason : reasons) {
        if (counts.find(reason) == counts.end()) {
            order.push_back(reason);
        }
        ++counts[reason];
    }

    std::ostringstream msg;
    msg << "firebird_unpushed_mode='" << mode << "': this scan kept "
        << reasons.size() << " filter(s) in DuckDB instead of pushing them "
                           "down to Firebird.";
    for (const auto &reason : order) {
        msg << " " << reason << " (" << counts[reason]
            << "): " << RemedyForReason(reason) << ".";
    }
    msg << " Verify with firebird_explain_pushdown() / "
           "firebird_last_query(). Reset with SET firebird_unpushed_mode = "
           "'silent'.";

    if (mode == "error") {
        throw InvalidInputException(msg.str());
    }
    // 'warn': DuckDB's native warning channel — the exact mechanism the
    // engine itself uses for deprecation notices (see
    // duckdb/logging/logger.hpp, DUCKDB_LOG_WARNING). The DuckDB CLI
    // registers a stdout log storage at LOG_WARNING level, so the warning
    // prints to the console out of the box; embedded hosts receive it
    // through the logging subsystem, and can promote it to an error with
    // warnings_as_errors. Limitation: hosts that never enable logging
    // (default NopLogger) do not see the warning — that is DuckDB's
    // platform behaviour, not something the extension can bypass without
    // falling back to raw Printer output.
    DUCKDB_LOG_WARNING(ctx, msg.str());
}

// ---------------------------------------------------------------------------
//  Redaction
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  Error-message sanitizer
// ---------------------------------------------------------------------------
//
// Defensive layer (PM Chunk C finding media). libfbclient diagnostics
// commonly carry SQL/identifiers but, in some failure modes, can echo
// fragments of a connection string. Strip the two obvious credential
// patterns before the error reaches firebird_last_query().error_message
// or the ring buffer:
//   * "password=..." (case-insensitive) up to the next separator
//   * "scheme://user:pass@host" -> user:pass replaced with <redacted>
// Anything else flows through unchanged - we deliberately do NOT try to
// guess additional shapes here; that would risk over-redacting legitimate
// error text (table names, IDs, dates) and hurt debuggability.

std::string SanitizeErrorMessage(std::string msg) {
    try {
        static const std::regex pw_re(
            R"((password\s*=\s*)[^\s;'"]*)",
            std::regex_constants::icase);
        msg = std::regex_replace(msg, pw_re, "$1<redacted>");
        static const std::regex up_re(R"((://)[^/@\s:]+:[^/@\s]+(@))");
        msg = std::regex_replace(msg, up_re, "$1<redacted>$2");
    } catch (...) {
        // Regex implementations differ across platforms; we never want
        // sanitization to swallow the error path itself.
    }
    return msg;
}

std::string RedactBindValue(const Value &v) {
    if (v.IsNull()) {
        return "<null>";
    }
    switch (v.type().id()) {
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::CHAR:
    case LogicalTypeId::BLOB:
        return "<text:redacted>";
    default:
        return v.ToString();
    }
}

// ---------------------------------------------------------------------------
//  firebird_last_query() — table function
// ---------------------------------------------------------------------------
//
// Output schema (one row, or zero rows when nothing has been captured yet):
//
//   remote_sql         VARCHAR
//   binds              VARCHAR[]
//   table_name         VARCHAR
//   projected_columns  VARCHAR[]
//   pushed_filters     VARCHAR[]
//   residual_filters   VARCHAR[]
//   rows_read          BIGINT
//   firebird_time_us   BIGINT
//   total_time_us      BIGINT
//   connection_id      BIGINT
//   connection_reused  BOOLEAN
//   parallel_scan      BOOLEAN
//   partitions         INTEGER
//   captured_at        TIMESTAMP
//   error_message      VARCHAR   - empty when scan ran cleanly

// Forward declarations - shared by firebird_last_query() and
// firebird_query_log() below.
static void TelemetrySchema(vector<string> &names,
                             vector<LogicalType> &types);
static void EmitTelemetryRow(DataChunk &output, idx_t row,
                              const FirebirdQueryTelemetry &t);

struct LastQueryBindData : public TableFunctionData {};

struct LastQueryGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    FirebirdQueryTelemetry snapshot;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
LastQueryBind(ClientContext &, TableFunctionBindInput &,
              vector<LogicalType> &return_types, vector<string> &names) {
    TelemetrySchema(names, return_types);
    return make_uniq<LastQueryBindData>();
}

static unique_ptr<GlobalTableFunctionState>
LastQueryInitGlobal(ClientContext &context, TableFunctionInitInput &) {
    auto g = make_uniq<LastQueryGlobalState>();
    g->snapshot = GetObservabilityState(context)->GetLastQuery();
    return std::move(g);
}

static Value VarcharList(const std::vector<std::string> &xs) {
    vector<Value> vals;
    vals.reserve(xs.size());
    for (auto &s : xs) {
        vals.emplace_back(Value(s));
    }
    return Value::LIST(LogicalType::VARCHAR, std::move(vals));
}

// Emit one FirebirdQueryTelemetry into the output chunk at index `row`.
// Shared by firebird_last_query() and firebird_query_log() to keep the
// 18-column schema in lockstep.
static void EmitTelemetryRow(DataChunk &output, idx_t row,
                              const FirebirdQueryTelemetry &t) {
    output.data[0].SetValue(row, Value(t.remote_sql));
    output.data[1].SetValue(row, VarcharList(t.binds_redacted));
    output.data[2].SetValue(row, Value(t.table_name));
    output.data[3].SetValue(row, VarcharList(t.projected_columns));
    output.data[4].SetValue(row, VarcharList(t.pushed_filters));
    output.data[5].SetValue(row, VarcharList(t.residual_filters));
    output.data[6].SetValue(row, Value::BIGINT(t.rows_read));
    output.data[7].SetValue(row, Value::BIGINT(t.firebird_time_us));
    output.data[8].SetValue(row, Value::BIGINT(t.total_time_us));
    output.data[9].SetValue(row, Value::BIGINT(t.connection_id));
    output.data[10].SetValue(row, Value::BOOLEAN(t.connection_reused));
    output.data[11].SetValue(row, Value::BOOLEAN(t.parallel_scan));
    output.data[12].SetValue(row, Value::INTEGER(t.partitions));
    output.data[13].SetValue(row, Value::TIMESTAMP(t.captured_at));
    output.data[14].SetValue(row, Value(t.error_message));
    // Phase 4 #3 pushdown explainability. Paging columns surface as SQL
    // NULL (not 0) when no ROWS clause was pushed, so a real limit of 0 is
    // never confused with "no limit".
    output.data[15].SetValue(row, t.limit_pushed.IsValid()
        ? Value::BIGINT(static_cast<int64_t>(t.limit_pushed.GetIndex()))
        : Value(LogicalType::BIGINT));
    output.data[16].SetValue(row, t.offset_pushed.IsValid()
        ? Value::BIGINT(static_cast<int64_t>(t.offset_pushed.GetIndex()))
        : Value(LogicalType::BIGINT));
    output.data[17].SetValue(row, VarcharList(t.not_pushed_reasons));
}

// Returns the canonical (names, types) the two observability table
// functions share. Kept in one place so adding a column never gets out
// of sync between the two.
static void TelemetrySchema(vector<string> &names,
                             vector<LogicalType> &types) {
    names = {
        "remote_sql",
        "binds",
        "table_name",
        "projected_columns",
        "pushed_filters",
        "residual_filters",
        "rows_read",
        "firebird_time_us",
        "total_time_us",
        "connection_id",
        "connection_reused",
        "parallel_scan",
        "partitions",
        "captured_at",
        "error_message",
        "limit_pushed",
        "offset_pushed",
        "not_pushed_reasons",
    };
    types = {
        LogicalType::VARCHAR,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::VARCHAR,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::INTEGER,
        LogicalType::TIMESTAMP,
        LogicalType::VARCHAR,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::LIST(LogicalType::VARCHAR),
    };
}

static void LastQueryFunction(ClientContext &, TableFunctionInput &input,
                              DataChunk &output) {
    auto &g = input.global_state->Cast<LastQueryGlobalState>();
    if (g.emitted || !g.snapshot.valid) {
        output.SetCardinality(0);
        return;
    }
    output.SetCardinality(1);
    EmitTelemetryRow(output, 0, g.snapshot);
    g.emitted = true;
}

TableFunction GetFirebirdLastQueryFunction() {
    TableFunction fn("firebird_last_query",
                     {},
                     LastQueryFunction,
                     LastQueryBind,
                     LastQueryInitGlobal);
    return fn;
}

// ---------------------------------------------------------------------------
//  firebird_query_log() — ring-buffer table function (Chunk E)
// ---------------------------------------------------------------------------
//
// Opt-in via `SET firebird_query_log_size = N`. Default 0 keeps the log
// disabled and returns zero rows. Each captured query (most-recent-first)
// is one row; rotation is per-ClientContext, never process-wide.

struct QueryLogBindData : public TableFunctionData {};

struct QueryLogGlobalState : public GlobalTableFunctionState {
    std::vector<FirebirdQueryTelemetry> snapshot;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
QueryLogBind(ClientContext &, TableFunctionBindInput &,
             vector<LogicalType> &return_types, vector<string> &names) {
    TelemetrySchema(names, return_types);
    return make_uniq<QueryLogBindData>();
}

static unique_ptr<GlobalTableFunctionState>
QueryLogInitGlobal(ClientContext &context, TableFunctionInitInput &) {
    auto g = make_uniq<QueryLogGlobalState>();
    g->snapshot = GetObservabilityState(context)->SnapshotLog();
    return std::move(g);
}

static void QueryLogFunction(ClientContext &, TableFunctionInput &input,
                              DataChunk &output) {
    auto &g = input.global_state->Cast<QueryLogGlobalState>();
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t row = 0;
    while (row < target && g.cursor < g.snapshot.size()) {
        EmitTelemetryRow(output, row, g.snapshot[g.cursor++]);
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdQueryLogFunction() {
    TableFunction fn("firebird_query_log",
                     {},
                     QueryLogFunction,
                     QueryLogBind,
                     QueryLogInitGlobal);
    return fn;
}

} // namespace duckdb
