#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "ibase.h"

// --- Firebird 4 SQL types -------------------------------------------------
//
// The Firebird 3 ibase.h shipped by most distros doesn't declare these,
// but the byte layout on the wire is stable across server versions, so
// defining them locally lets us scan Firebird 4 servers when the build
// host only has FB3 headers available.

#ifndef SQL_INT128
#define SQL_INT128       32752
#endif
#ifndef SQL_TIMESTAMP_TZ
#define SQL_TIMESTAMP_TZ 32754
#endif
#ifndef SQL_TIME_TZ
#define SQL_TIME_TZ      32756
#endif
#ifndef SQL_TIMESTAMP_TZ_EX
#define SQL_TIMESTAMP_TZ_EX 32748
#endif
#ifndef SQL_TIME_TZ_EX
#define SQL_TIME_TZ_EX      32750
#endif
#ifndef SQL_DEC16
#define SQL_DEC16        32760
#endif
#ifndef SQL_DEC34
#define SQL_DEC34        32762
#endif

namespace duckdb {

struct FirebirdConnectionInfo {
    std::string database;          // "host/port:/path/db.fdb" or local path
    std::string user = "SYSDBA";
    std::string password = "masterkey";
    std::string role;
    std::string charset = "UTF8";
    int dialect = 3;
    // G4 session stability — per-attachment keepalive, carried to the
    // server in the attach DPB as isc_dpb_dummy_packet_interval.
    // Unit: SECONDS (the unit Firebird's own DummyPacketInterval
    // firebird.conf entry and the wire-protocol docs use). 0 (default)
    // = the DPB item is NOT sent, byte-for-byte the historical attach.
    // The field propagates with every copy of this struct: pool-held
    // connections, metadata leases, schema/table catalog entries and
    // scanner connections all attach through FirebirdConnection::Attach().
    int64_t dummy_packet_interval_secs = 0;

    static FirebirdConnectionInfo Parse(const std::string &conn_str);
};

// Validates a user-supplied dummy packet interval (seconds). 0 is the
// "off" default and passes. Throws a BinderException with the unit in
// the message when the value is negative or beyond the 32-bit DPB
// payload range. Shared by the firebird_scan() bind path and the
// ATTACH path so both reject identical values with identical text.
void ValidateDummyPacketInterval(int64_t seconds);

// Throws a BinderException if `charset` would deliver bytes DuckDB's
// UTF-8-only string vectors can't ingest. UTF8, UTF-8, NONE, OCTETS pass;
// anything else is rejected with a hint to keep the default UTF8 (Firebird
// transliterates from the storage charset server-side).
void ValidateClientCharset(const std::string &charset);

struct FirebirdColumnDesc {
    std::string name;
    int16_t sqltype = 0;           // base type (low bit cleared)
    int16_t sqlsubtype = 0;
    int16_t sqlscale = 0;
    int16_t sqllen = 0;
    // Firebird CHARACTER SET id. 0 = NONE (storage charset is "no
    // declared encoding — bytes are raw"). Non-zero = a real
    // server-side charset; Firebird transliterates to the client's
    // lc_ctype (which we keep at UTF8) before fetch, so the bytes
    // arriving in our buffers are valid UTF-8.
    int16_t character_set_id = -1; // -1 = unknown / not applicable
    bool nullable = true;
};

// How to surface text columns whose Firebird CHARACTER SET is NONE.
// Firebird NONE means "bytes have no declared encoding" — the server
// does not transliterate them, so we get whatever the writing app
// stored. DuckDB's VARCHAR requires valid UTF-8, so we have to
// choose what to do at fetch time.
enum class NoneEncoding {
    STRICT,      // require UTF-8; raise an informative error otherwise
    WIN1252,     // decode bytes as Windows-1252 -> UTF-8
    ISO_8859_1,  // decode bytes as ISO-8859-1 (Latin-1) -> UTF-8
    BLOB,        // surface the column as DuckDB BLOB (raw bytes)
};

NoneEncoding ParseNoneEncoding(const std::string &s);

class FirebirdConnection;

// Holds one prepared+executed cursor. Owns a per-statement XSQLDA and the
// per-column data buffers that libfbclient writes into on each fetch.
class FirebirdStatement {
public:
    FirebirdStatement(FirebirdConnection &conn, const std::string &sql);
    // Same shape as the constructor above, but binds the supplied
    // parameters into the prepared statement's input XSQLDA before
    // executing. Each Value is encoded into the format the server
    // described for that placeholder; mismatches throw IOException.
    // `params.size()` must match the number of `?` placeholders in the
    // SQL — fewer or extra parameters is a programming error.
    FirebirdStatement(FirebirdConnection &conn, const std::string &sql,
                      const std::vector<Value> &params);

    // Prepares and describes `sql` WITHOUT executing it — no cursor is
    // opened, no rows are ever fetchable from an instance constructed
    // this way. Used to cheaply learn the ACTUAL runtime column layout
    // (via XSQLDA) for a query whose static catalog metadata might be
    // stale (e.g. a view's computed/aggregate column, whose type was
    // frozen at CREATE VIEW time and can legitimately disagree with
    // what Firebird's live DSQL compiler produces for the identical
    // expression today). Call .columns() to read the described layout.
    struct PrepareOnlyTag {};
    FirebirdStatement(FirebirdConnection &conn, const std::string &sql,
                      PrepareOnlyTag);

    ~FirebirdStatement();

    FirebirdStatement(const FirebirdStatement &) = delete;
    FirebirdStatement &operator=(const FirebirdStatement &) = delete;

    const std::vector<FirebirdColumnDesc> &columns() const { return columns_; }

    // G5 bytes-estimate support. RowWidthBytes() is the sum of the
    // per-column fetch buffer sizes of the output XSQLDA (each column's
    // descriptor width: CHAR/VARCHAR sqllen + length prefix, fixed-size
    // temporal/numeric widths, 8-byte BLOB id frame). Computed once in
    // AllocateBuffers() (i.e. at prepare/describe time), so it is stable
    // for the whole cursor and identical for every partition cursor of
    // the same scan. bytes_estimate() uses it as "payload bytes per
    // fetched row"; see BytesRead() / TakeBytesRead() for the counter.
    int64_t RowWidthBytes() const { return row_width_bytes_; }

    // Running estimate of Firebird payload bytes this cursor has pulled:
    // RowWidthBytes() per successful Fetch() plus the actual BLOB segment
    // bytes returned by ReadBlob() (BLOB columns carry only an 8-byte id
    // frame in the XSQLDA — the content arrives through isc_get_segment,
    // so it is counted where it is actually read). Protocol headers,
    // prepare/bind round-trips and the paged-rows metadata are NOT
    // counted; this is a payload estimate, not a wire-traffic meter.
    int64_t BytesRead() const { return bytes_read_; }

    // Drain variant: returns the accumulated estimate and resets the
    // counter to zero. The scanner drains per output chunk so the number
    // can be folded into the per-query telemetry record and (on the
    // ATTACH path) into the catalog's lifetime pool counter without the
    // statement having to know about either.
    int64_t TakeBytesRead() {
        const int64_t b = bytes_read_;
        bytes_read_ = 0;
        return b;
    }

    // Set the character_set_id on column `col`. Used when the caller
    // wants to augment what XSQLDA gives us with metadata pulled
    // from RDB$FIELDS — needed for text BLOBs (sqltype=SQL_BLOB,
    // subtype=1) where the XSQLVAR sqlsubtype is the *blob* subtype
    // (1 = text), not the INTL character_set_id.
    void OverrideCharsetId(idx_t col, int16_t cs_id) {
        columns_[col].character_set_id = cs_id;
    }

    // Returns false when no more rows are available.
    bool Fetch();

    bool IsNull(idx_t col) const;
    int16_t  GetShort(idx_t col) const;
    int32_t  GetLong(idx_t col)  const;
    int64_t  GetInt64(idx_t col) const;
    float    GetFloat(idx_t col) const;
    double   GetDouble(idx_t col) const;
    std::string GetText(idx_t col) const;
    ISC_TIMESTAMP GetTimestamp(idx_t col) const;
    ISC_DATE      GetDate(idx_t col) const;
    ISC_TIME      GetTime(idx_t col) const;
    bool          GetBool(idx_t col) const;
    // 128-bit integer (Firebird 4 INT128 / NUMERIC(p,s) with p>18). The
    // value is the raw 16-byte little-endian two's-complement payload
    // libfbclient writes; the caller is responsible for applying scale
    // when the column is a scaled NUMERIC. Returns (lower 64, upper 64).
    void GetInt128(idx_t col, uint64_t &out_lo, int64_t &out_hi) const;
    // Firebird 4 TIMESTAMP WITH TIMEZONE — UTC date/time + a 2-byte
    // time-zone region/offset id. We only surface the UTC component
    // (DuckDB's TIMESTAMP_TZ is microseconds since the UNIX epoch in
    // UTC), so the tz id is intentionally dropped.
    ISC_TIMESTAMP GetTimestampTzUtc(idx_t col) const;
    ISC_TIME      GetTimeTzUtc(idx_t col) const;
    // Reads an entire BLOB into a string (one allocation, segments concatenated).
    std::string   ReadBlob(idx_t col) const;

private:
    FirebirdConnection &conn_;
    isc_stmt_handle stmt_ = 0;
    XSQLDA *out_sqlda_ = nullptr;
    XSQLDA *in_sqlda_  = nullptr;
    std::vector<std::vector<char>> buffers_;
    std::vector<std::vector<char>> in_buffers_;
    std::vector<short> indicators_;
    std::vector<short> in_indicators_;
    std::vector<FirebirdColumnDesc> columns_;
    // G5 — see RowWidthBytes()/BytesRead(). row_width_bytes_ is fixed at
    // AllocateBuffers time; bytes_read_ is mutable because ReadBlob() is
    // const (it is called through const-access paths in the value
    // mapping layer) and still must record the BLOB bytes it pulls.
    int64_t row_width_bytes_ = 0;
    mutable int64_t bytes_read_ = 0;

    void Prepare(const std::string &sql);
    void AllocateBuffers();
    // Encodes `params` into the input XSQLDA buffers described by
    // `isc_dsql_describe_bind`. Throws IOException on type mismatch
    // or unsupported parameter type.
    void BindInputParameters(const std::vector<Value> &params,
                              const std::string &sql_for_error);
};

// Tunable knobs for the connection pool. All defaults reproduce the
// pre-Phase-2 behaviour (unlimited LIFO cache, no expiry, enabled).
//
// max_size limits the idle queue, not the number of active leases - a
// caller can still hand out as many connections as it wants; the cap
// only kicks in on Release(), where surplus connections are dropped
// instead of being parked.
//
// idle_timeout_ms limits how long a released connection may sit in the
// idle queue. The clock starts at Release(), not at connection creation.
struct FirebirdConnectionPoolConfig {
    bool    enabled         = true;
    int64_t max_size        = 0;   // 0 = unlimited
    int64_t idle_timeout_ms = 0;   // 0 = no expiry
};

// Result of FirebirdConnectionPool::AcquireWithInfo(). Carries the
// connection plus whether it came from the idle queue (true) or was
// freshly constructed (false). The legacy Acquire() wrapper drops the
// flag for callers that don't care.
struct FirebirdConnectionLease {
    std::unique_ptr<FirebirdConnection> conn;
    bool reused = false;
};

// Cache of idle FirebirdConnection objects. Cheap to skip when there's
// only one query per ATTACH (each LocalState would open + tear down a
// fresh connection anyway), but saves the ~10-100 ms isc_attach_database
// cost on interactive sessions that hit the same database many times.
class FirebirdConnectionPool {
public:
    explicit FirebirdConnectionPool(FirebirdConnectionInfo info,
                                    FirebirdConnectionPoolConfig config = {})
        : info_(std::move(info)), config_(config) {}

    // Acquire returns either a pooled idle connection (LIFO — warmest
    // first) or a newly constructed one. Never blocks. Preserved for
    // legacy callers that don't need the reused flag.
    std::unique_ptr<FirebirdConnection> Acquire();

    // Acquire variant that exposes whether the returned connection was
    // pulled from the idle queue. Phase 2 wires this into the
    // observability surface (`connection_id`, `connection_reused`).
    FirebirdConnectionLease AcquireWithInfo();

    // Return a connection to the pool. The connection's read-only
    // transaction may still be open — it'll be re-used as-is by the
    // next acquirer. When the pool is disabled, or the idle queue is
    // already at max_size, the connection is destroyed instead of
    // parked.
    void Release(std::unique_ptr<FirebirdConnection> conn);

    // Pool size hint (mostly for testing / introspection).
    size_t IdleCount();

    // Lifetime counters. Useful for tests and for the future
    // pool-introspection surface. Process-relative; no decay.
    int64_t TotalCreated()   const { return total_created_.load(std::memory_order_relaxed); }
    int64_t TotalReused()    const { return total_reused_.load(std::memory_order_relaxed); }
    int64_t TotalDiscarded() const { return total_discarded_.load(std::memory_order_relaxed); }

    // Connections handed out by Acquire/AcquireWithInfo and not yet
    // returned via Release(). Process-relative; a caller that destroyed a
    // connection without Release() would inflate it (all extension paths
    // release through the pool).
    int64_t ActiveCount() const { return active_.load(std::memory_order_relaxed); }

    // Sanitized message of the most recent failed connection creation,
    // empty when none has failed. Credentials are redacted before the
    // message is stored. Populated only for post-ATTACH failures (a
    // failure during ATTACH itself never registers a catalog).
    std::string LastError();

    // G5 — lifetime estimate of Firebird payload bytes fetched by scans
    // that ran through THIS pool, i.e. inside one attached catalog
    // (ATTACH path). Direct firebird_scan() calls have no pool and never
    // touch this counter — they belong to no catalog. The scanner folds
    // each drained per-chunk estimate in via AddBytesReadEstimate();
    // firebird_pool_stats() reads it through BytesReadEstimate().
    // Process-relative, no decay, never reset (same semantics as
    // TotalCreated/TotalReused).
    int64_t BytesReadEstimate() const {
        return bytes_read_estimate_.load(std::memory_order_relaxed);
    }
    void AddBytesReadEstimate(int64_t bytes) {
        if (bytes > 0) {
            bytes_read_estimate_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    // Current config snapshot (immutable after construction in Chunk A;
    // settings-driven mutation lands in Chunk B).
    const FirebirdConnectionPoolConfig &Config() const { return config_; }

private:
    struct IdleEntry {
        std::unique_ptr<FirebirdConnection> conn;
        std::chrono::steady_clock::time_point released_at;
    };

    void RecordConnectionError(const std::string &message);

    FirebirdConnectionInfo       info_;
    FirebirdConnectionPoolConfig config_;
    std::mutex                   lock_;
    std::vector<IdleEntry>       idle_;
    std::atomic<int64_t>         total_created_{0};
    std::atomic<int64_t>         total_reused_{0};
    std::atomic<int64_t>         total_discarded_{0};
    std::atomic<int64_t>         active_{0};
    // G5 — lifetime bytes_read_estimate accumulated by scans that ran on
    // this pool's catalog (see BytesReadEstimate above). Lives here, not
    // in a separate stats object, because the pool is the per-ATTACH
    // singleton the scanner already holds a shared_ptr to via the bind
    // data — no new plumbing, same lifetime as the catalog.
    std::atomic<int64_t>         bytes_read_estimate_{0};
    std::string                  last_error_; // guarded by lock_
};

// Owns one isc_db_handle + a long-running read-only transaction.
class FirebirdConnection {
public:
    explicit FirebirdConnection(const FirebirdConnectionInfo &info);
    ~FirebirdConnection();

    FirebirdConnection(const FirebirdConnection &) = delete;
    FirebirdConnection &operator=(const FirebirdConnection &) = delete;

    isc_db_handle &db() { return db_; }
    isc_tr_handle &tr() { return tr_; }
    const FirebirdConnectionInfo &info() const { return info_; }

    // Process-wide monotonic id used for observability. It is not a
    // Firebird attachment id and is only meant to correlate
    // extension-side pool reuse.
    int64_t Id() const { return id_; }

    // Convenience: prepare+execute a SELECT and return an open cursor.
    std::unique_ptr<FirebirdStatement> OpenCursor(const std::string &sql);
    // Same, with input parameters bound through an XSQLDA. Used by the
    // scanner once filter pushdown emits `?` placeholders.
    std::unique_ptr<FirebirdStatement> OpenCursor(const std::string &sql,
                                                  const std::vector<Value> &params);

    // Throws an IOException carrying isc_interprete'd messages, if status is
    // an error vector.
    static void Check(const ISC_STATUS *status, const std::string &context);

private:
    FirebirdConnectionInfo info_;
    isc_db_handle db_ = 0;
    isc_tr_handle tr_ = 0;
    int64_t id_ = 0;

    void Attach();
    void StartReadOnlyTransaction();
};

} // namespace duckdb
