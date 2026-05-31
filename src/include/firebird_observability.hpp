#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

// Telemetry record for the last remote Firebird query *attempted* by this
// ClientContext. The capture point lives in the scanner just before
// OpenCursor, so this is the "last SQL we tried to send" - if OpenCursor
// itself raises, firebird_last_query() will still surface the attempted
// SQL. Timings / rows_read / connection_id / error status are Chunk C+.
struct FirebirdQueryTelemetry {
    std::string remote_sql;
    std::vector<std::string> binds_redacted;
    std::string table_name;
    std::vector<std::string> projected_columns;
    std::vector<std::string> pushed_filters;
    std::vector<std::string> residual_filters;
    int64_t rows_read = 0;
    int64_t firebird_time_us = 0;
    int64_t total_time_us = 0;
    int64_t connection_id = -1;
    bool connection_reused = false;
    bool parallel_scan = false;
    int32_t partitions = 1;
    // Pushdown explainability (Phase 4 #3). limit_pushed / offset_pushed
    // mirror the ROWS clause actually emitted to Firebird; invalid
    // (optional_idx default) means no paging was pushed and the column
    // surfaces as SQL NULL. not_pushed_reasons carries one coarse reason
    // per residual filter (NONE_CHARSET / UNSUPPORTED_OP /
    // ROWID_OR_INVALID_COLUMN / UNSUPPORTED_PROJECTION_MAPPING), parallel
    // to residual_filters.
    optional_idx limit_pushed;
    optional_idx offset_pushed;
    std::vector<std::string> not_pushed_reasons;
    timestamp_t captured_at = timestamp_t(0);
    // Empty string when the scan ran cleanly; set to the exception text
    // when OpenCursor or Fetch raised. The "last query attempted" slot
    // stays populated so callers can correlate the failed SQL with the
    // error text.
    std::string error_message;
    bool valid = false;
};

// Per-ClientContext observability state. Registered on demand via
// ClientContext::registered_state->GetOrCreate<FirebirdObservabilityState>(KEY).
//
// PM directive (Chunk A review): this state must NOT be process-wide.
// Each DuckDB connection / session gets its own telemetry slot so the
// last query of one session never leaks into another's
// firebird_last_query() result.
//
// Future redaction policy (NOT yet implemented in Chunk A/B):
//   * SET firebird_observability_redaction = 'strict' | 'debug'
//   * strict (default): numeric/temporal also redacted by category
//   * debug: surface raw values for diagnostics
class FirebirdObservabilityState : public ClientContextState {
public:
    static constexpr const char *KEY = "firebird.observability";

    FirebirdQueryTelemetry GetLastQuery() const;
    // Chunk E: snapshot the ring buffer most-recent-first. Empty when the
    // log is disabled (size = 0) or has never been written to.
    std::vector<FirebirdQueryTelemetry> SnapshotLog() const;
    // Push a new record into the slot. When log_size > 0 the record is
    // also appended to the ring buffer; the buffer is trimmed (oldest
    // dropped) until it fits log_size. log_size = 0 disables logging
    // and clears any leftover entries so the off state is truly empty.
    void RecordQuery(const FirebirdQueryTelemetry &rec, int64_t log_size);

    // Chunk C - cumulative metrics for the *current* last_ record.
    // Each method grabs the same mutex as RecordQuery; the cost per call
    // is one uncontended lock, called once per fetched chunk (not per
    // row), so it stays off the hot path even for million-row scans.
    void AddRows(int64_t n);
    void AddFirebirdTimeUs(int64_t us);
    void UpdateTotalTimeUs();
    void SetError(const std::string &msg);

private:
    mutable std::mutex lock_;
    FirebirdQueryTelemetry last_;
    // Ring buffer for firebird_query_log(). Push-back / pop-front,
    // bounded by the current `firebird_query_log_size` setting at
    // RecordQuery time. std::deque keeps the head/tail O(1).
    std::deque<FirebirdQueryTelemetry> log_;
    // Wall-clock origin for total_time_us. Reset on RecordQuery so
    // every new query gets a fresh clock; UpdateTotalTimeUs computes
    // (now - start_) and stamps last_.total_time_us. Under parallel
    // scan multiple workers update through the same lock, so the
    // surfaced value reflects "scan still in progress" if read mid-flight
    // and "wall time of the slowest partition" once everything settles.
    std::chrono::steady_clock::time_point start_;
};

// Fetch (or create) the observability state attached to this ClientContext.
// Always returns a non-null shared_ptr; lifetime is tied to the context.
shared_ptr<FirebirdObservabilityState> GetObservabilityState(ClientContext &ctx);

// Redact a single bind value for surface in observability output.
//
// PM-approved Phase 1 policy:
//   * NULL                                       -> "<null>"
//   * VARCHAR / CHAR / BLOB                      -> "<text:redacted>"
//     (length intentionally not exposed - it can proxy for sensitive
//     content such as CPF length, e-mail length).
//   * Numeric / temporal / boolean               -> raw Value::ToString()
std::string RedactBindValue(const Value &v);

// Sanitize an exception string before surfacing it through
// firebird_last_query().error_message. Strips obvious credential
// patterns ("password=...", "scheme://user:pass@host") so a raised
// libfbclient diagnostic that happened to echo a connection string
// cannot leak the password.
std::string SanitizeErrorMessage(std::string msg);

// firebird_last_query() table function. Returns at most one row, with the
// most recently captured FirebirdQueryTelemetry FOR THIS CONNECTION.
// Empty result when no query has been captured yet on this context.
TableFunction GetFirebirdLastQueryFunction();

// firebird_query_log() table function. Returns 0..N rows from the
// per-ClientContext ring buffer, most-recent-first. Opt-in via
// `SET firebird_query_log_size = N`; default 0 disables the log.
TableFunction GetFirebirdQueryLogFunction();

} // namespace duckdb
