#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "ibase.h"

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

struct FirebirdColumnDesc {
    std::string name;
    int16_t sqltype = 0;           // base type (low bit cleared)
    int16_t sqlsubtype = 0;
    int16_t sqlscale = 0;
    int16_t sqllen = 0;
    bool nullable = true;
};

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

// Owns one isc_db_handle + a long-running read-only transaction. Cheap to
// create per scan today; will be pooled in a follow-up.
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
