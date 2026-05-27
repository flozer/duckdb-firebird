#pragma once

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

    static FirebirdConnectionInfo Parse(const std::string &conn_str);
};

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
    ~FirebirdStatement();

    FirebirdStatement(const FirebirdStatement &) = delete;
    FirebirdStatement &operator=(const FirebirdStatement &) = delete;

    const std::vector<FirebirdColumnDesc> &columns() const { return columns_; }

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
    std::vector<std::vector<char>> buffers_;
    std::vector<short> indicators_;
    std::vector<FirebirdColumnDesc> columns_;

    void AllocateBuffers();
};

// Cache of idle FirebirdConnection objects. Cheap to skip when there's
// only one query per ATTACH (each LocalState would open + tear down a
// fresh connection anyway), but saves the ~10-100 ms isc_attach_database
// cost on interactive sessions that hit the same database many times.
class FirebirdConnectionPool {
public:
    explicit FirebirdConnectionPool(FirebirdConnectionInfo info)
        : info_(std::move(info)) {}

    // Acquire returns either a pooled idle connection (LIFO — warmest
    // first) or a newly constructed one. Never blocks.
    std::unique_ptr<FirebirdConnection> Acquire();

    // Return a connection to the pool. The connection's read-only
    // transaction may still be open — it'll be re-used as-is by the
    // next acquirer.
    void Release(std::unique_ptr<FirebirdConnection> conn);

    // Pool size hint (mostly for testing / introspection).
    size_t IdleCount();

private:
    FirebirdConnectionInfo info_;
    std::mutex lock_;
    std::vector<std::unique_ptr<FirebirdConnection>> idle_;
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

    // Convenience: prepare+execute a SELECT and return an open cursor.
    std::unique_ptr<FirebirdStatement> OpenCursor(const std::string &sql);

    // Throws an IOException carrying isc_interprete'd messages, if status is
    // an error vector.
    static void Check(const ISC_STATUS *status, const std::string &context);

private:
    FirebirdConnectionInfo info_;
    isc_db_handle db_ = 0;
    isc_tr_handle tr_ = 0;

    void Attach();
    void StartReadOnlyTransaction();
};

} // namespace duckdb
