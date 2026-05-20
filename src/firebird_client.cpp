#include "firebird_client.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "duckdb/common/exception.hpp"

namespace duckdb {

// --- connection string parsing -----------------------------------------------
//
// Accepted forms:
//   "firebird://user:pass@host:3050/path/db.fdb?charset=UTF8&dialect=3"
//   "user=SYSDBA;password=masterkey;database=server:/path/db.fdb;charset=UTF8"
//   "/path/to/db.fdb"   (defaults: SYSDBA / masterkey, UTF8, dialect 3)
//
// We deliberately keep parsing tolerant: the goal is "configure libfbclient",
// not enforce a URL grammar. Unknown options are ignored.

static std::string UrlDecode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = s.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

FirebirdConnectionInfo FirebirdConnectionInfo::Parse(const std::string &conn_str) {
    FirebirdConnectionInfo info;

    if (conn_str.rfind("firebird://", 0) == 0) {
        // URL form.
        std::string rest = conn_str.substr(std::string("firebird://").size());
        // Split off query string.
        std::string query;
        auto qpos = rest.find('?');
        if (qpos != std::string::npos) {
            query = rest.substr(qpos + 1);
            rest  = rest.substr(0, qpos);
        }
        // Split userinfo from host/path.
        std::string userinfo;
        auto at = rest.find('@');
        if (at != std::string::npos) {
            userinfo = rest.substr(0, at);
            rest     = rest.substr(at + 1);
        }
        if (!userinfo.empty()) {
            auto colon = userinfo.find(':');
            if (colon != std::string::npos) {
                info.user     = UrlDecode(userinfo.substr(0, colon));
                info.password = UrlDecode(userinfo.substr(colon + 1));
            } else {
                info.user = UrlDecode(userinfo);
            }
        }
        // host[:port]/path -> libfbclient form "host[:port]:/path". We keep
        // the leading slash from the URL path so the result is unambiguous;
        // libfbclient accepts an aliased database (no slash) only when the
        // caller uses the key=value form.
        //
        // NOTE on ports: legacy libfbclient prefers "host/port" while modern
        // builds also accept "host:port"; we pass through whatever the user
        // wrote.
        auto slash = rest.find('/');
        std::string host = slash == std::string::npos ? rest          : rest.substr(0, slash);
        std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
        if (!host.empty()) {
            info.database = host + ":" + path;
        } else {
            info.database = path;
        }
        // Parse query options.
        std::stringstream ss(query);
        std::string kv;
        while (std::getline(ss, kv, '&')) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            auto key = kv.substr(0, eq);
            auto val = UrlDecode(kv.substr(eq + 1));
            if      (key == "charset")  info.charset  = val;
            else if (key == "role")     info.role     = val;
            else if (key == "dialect")  info.dialect  = std::atoi(val.c_str());
            else if (key == "user")     info.user     = val;
            else if (key == "password") info.password = val;
        }
        return info;
    }

    // Key=value form (semicolon separated).
    if (conn_str.find('=') != std::string::npos &&
        conn_str.find(';') != std::string::npos) {
        std::stringstream ss(conn_str);
        std::string kv;
        while (std::getline(ss, kv, ';')) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            auto key = kv.substr(0, eq);
            auto val = kv.substr(eq + 1);
            // Trim.
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(0, 1);
            while (!val.empty() && (val.back()  == ' ' || val.back()  == '\t')) val.pop_back();
            if      (key == "database") info.database = val;
            else if (key == "user")     info.user     = val;
            else if (key == "password") info.password = val;
            else if (key == "charset")  info.charset  = val;
            else if (key == "role")     info.role     = val;
            else if (key == "dialect")  info.dialect  = std::atoi(val.c_str());
        }
        return info;
    }

    // Bare path — treat as local database file.
    info.database = conn_str;
    return info;
}

// --- error reporting ---------------------------------------------------------

void FirebirdConnection::Check(const ISC_STATUS *status, const std::string &context) {
    if (status[0] != 1 || status[1] == 0) return;
    // Decode the full error vector.
    std::string msg = context + ": ";
    const ISC_STATUS *p = status;
    char buf[512];
    long sql_code = isc_sqlcode(status);
    while (fb_interpret(buf, sizeof(buf), &p)) {
        msg.append(buf);
        msg.push_back('\n');
    }
    msg.append("[isc sqlcode=").append(std::to_string(sql_code)).append("]");
    throw IOException(msg);
}

// --- DPB / TPB helpers -------------------------------------------------------

static void DpbAppend(std::vector<char> &dpb, char tag, const std::string &val) {
    dpb.push_back(tag);
    dpb.push_back(static_cast<char>(val.size()));
    dpb.insert(dpb.end(), val.begin(), val.end());
}

// --- FirebirdConnectionPool --------------------------------------------------

std::unique_ptr<FirebirdConnection> FirebirdConnectionPool::Acquire() {
    {
        std::lock_guard<std::mutex> g(lock_);
        if (!idle_.empty()) {
            auto conn = std::move(idle_.back());
            idle_.pop_back();
            return conn;
        }
    }
    return std::make_unique<FirebirdConnection>(info_);
}

void FirebirdConnectionPool::Release(std::unique_ptr<FirebirdConnection> conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> g(lock_);
    idle_.push_back(std::move(conn));
}

size_t FirebirdConnectionPool::IdleCount() {
    std::lock_guard<std::mutex> g(lock_);
    return idle_.size();
}

// --- FirebirdConnection ------------------------------------------------------

FirebirdConnection::FirebirdConnection(const FirebirdConnectionInfo &info) : info_(info) {
    Attach();
    try {
        StartReadOnlyTransaction();
    } catch (...) {
        if (db_) {
            ISC_STATUS s[20] = {};
            isc_detach_database(s, &db_);
        }
        throw;
    }
}

FirebirdConnection::~FirebirdConnection() {
    ISC_STATUS s[20] = {};
    if (tr_) isc_rollback_transaction(s, &tr_);
    if (db_) isc_detach_database(s, &db_);
}

void FirebirdConnection::Attach() {
    std::vector<char> dpb;
    dpb.push_back(isc_dpb_version1);
    DpbAppend(dpb, isc_dpb_user_name,    info_.user);
    DpbAppend(dpb, isc_dpb_password,     info_.password);
    if (!info_.charset.empty()) {
        DpbAppend(dpb, isc_dpb_lc_ctype, info_.charset);
    }
    if (!info_.role.empty()) {
        DpbAppend(dpb, isc_dpb_sql_role_name, info_.role);
    }
    {
        // SQL dialect (1-byte value, but libfbclient expects it length-tagged).
        dpb.push_back(isc_dpb_sql_dialect);
        dpb.push_back(1);
        dpb.push_back(static_cast<char>(info_.dialect));
    }

    ISC_STATUS status[20] = {};
    isc_attach_database(status,
                        static_cast<short>(info_.database.size()),
                        const_cast<char *>(info_.database.c_str()),
                        &db_,
                        static_cast<short>(dpb.size()),
                        dpb.data());
    Check(status, "isc_attach_database(" + info_.database + ")");
}

void FirebirdConnection::StartReadOnlyTransaction() {
    // Read-only, read-committed, no wait, no auto-undo — same shape used by
    // the peregrine extractor for its long-running scans.
    static const unsigned char tpb[] = {
        isc_tpb_version3,
        isc_tpb_read,
        isc_tpb_read_committed,
        isc_tpb_rec_version,
        isc_tpb_nowait,
        isc_tpb_no_auto_undo,
    };
    ISC_STATUS status[20] = {};
    isc_start_transaction(status, &tr_, 1, &db_, sizeof(tpb), const_cast<unsigned char *>(tpb));
    Check(status, "isc_start_transaction");
}

std::unique_ptr<FirebirdStatement> FirebirdConnection::OpenCursor(const std::string &sql) {
    return make_uniq<FirebirdStatement>(*this, sql);
}

// --- XSQLDA helpers ----------------------------------------------------------

static XSQLDA *AllocXSQLDA(int n) {
    auto *x = static_cast<XSQLDA *>(std::malloc(XSQLDA_LENGTH(n)));
    if (!x) throw std::bad_alloc();
    std::memset(x, 0, XSQLDA_LENGTH(n));
    x->version = SQLDA_VERSION1;
    x->sqln    = n;
    return x;
}

// --- FirebirdStatement -------------------------------------------------------

FirebirdStatement::FirebirdStatement(FirebirdConnection &conn, const std::string &sql)
    : conn_(conn) {
    ISC_STATUS status[20] = {};

    isc_dsql_allocate_statement(status, &conn_.db(), &stmt_);
    FirebirdConnection::Check(status, "isc_dsql_allocate_statement");

    out_sqlda_ = AllocXSQLDA(16);
    std::memset(status, 0, sizeof(status));
    isc_dsql_prepare(status, &conn_.tr(), &stmt_, 0,
                     const_cast<char *>(sql.c_str()),
                     static_cast<unsigned short>(conn_.info().dialect),
                     out_sqlda_);
    FirebirdConnection::Check(status, "isc_dsql_prepare:\n" + sql);

    if (out_sqlda_->sqld > out_sqlda_->sqln) {
        // Reallocate XSQLDA with the right column count and re-describe.
        int n = out_sqlda_->sqld;
        std::free(out_sqlda_);
        out_sqlda_ = AllocXSQLDA(n);
        std::memset(status, 0, sizeof(status));
        isc_dsql_describe(status, &stmt_, 1, out_sqlda_);
        FirebirdConnection::Check(status, "isc_dsql_describe");
    }

    AllocateBuffers();

    std::memset(status, 0, sizeof(status));
    isc_dsql_execute(status, &conn_.tr(), &stmt_, 1, nullptr);
    FirebirdConnection::Check(status, "isc_dsql_execute:\n" + sql);
}

FirebirdStatement::~FirebirdStatement() {
    if (stmt_) {
        ISC_STATUS s[20] = {};
        isc_dsql_free_statement(s, &stmt_, DSQL_drop);
    }
    if (out_sqlda_) std::free(out_sqlda_);
}

void FirebirdStatement::AllocateBuffers() {
    int n = out_sqlda_->sqld;
    buffers_.resize(n);
    indicators_.assign(n, 0);
    columns_.resize(n);

    for (int i = 0; i < n; ++i) {
        XSQLVAR &v = out_sqlda_->sqlvar[i];

        FirebirdColumnDesc &c = columns_[i];
        c.name.assign(v.aliasname, v.aliasname_length);
        // Trim trailing spaces (Firebird pads column names).
        while (!c.name.empty() && c.name.back() == ' ') c.name.pop_back();
        c.sqltype    = v.sqltype & ~1;          // base type
        c.sqlsubtype = v.sqlsubtype;
        c.sqlscale   = v.sqlscale;
        c.sqllen     = v.sqllen;
        c.nullable   = (v.sqltype & 1) != 0;

        // Allocate the data buffer that libfbclient will write into.
        size_t bufsz = 0;
        switch (c.sqltype) {
        case SQL_TEXT:
            bufsz = v.sqllen;
            break;
        case SQL_VARYING:
            bufsz = v.sqllen + sizeof(ISC_USHORT);  // 2-byte length prefix
            break;
        case SQL_SHORT:     bufsz = sizeof(int16_t);       break;
        case SQL_LONG:      bufsz = sizeof(int32_t);       break;
        case SQL_INT64:     bufsz = sizeof(int64_t);       break;
        case SQL_FLOAT:     bufsz = sizeof(float);         break;
        case SQL_DOUBLE:
        case SQL_D_FLOAT:   bufsz = sizeof(double);        break;
        case SQL_TIMESTAMP: bufsz = sizeof(ISC_TIMESTAMP); break;
        case SQL_TYPE_DATE: bufsz = sizeof(ISC_DATE);      break;
        case SQL_TYPE_TIME: bufsz = sizeof(ISC_TIME);      break;
        case SQL_BLOB:
        case SQL_ARRAY:
        case SQL_QUAD:      bufsz = sizeof(ISC_QUAD);      break;
        case SQL_BOOLEAN:   bufsz = sizeof(ISC_UCHAR);     break;
        case SQL_INT128:    bufsz = 16;                    break;
        case SQL_TIMESTAMP_TZ:
            // ISC_TIMESTAMP + 2-byte time-zone id (Firebird 4 ABI).
            bufsz = sizeof(ISC_TIMESTAMP) + sizeof(int16_t);
            break;
        case SQL_TIME_TZ:
            bufsz = sizeof(ISC_TIME) + sizeof(int16_t);
            break;
        case SQL_DEC16:     bufsz = 8;                     break;  // IEEE Decimal64
        case SQL_DEC34:     bufsz = 16;                    break;  // IEEE Decimal128
        default:            bufsz = v.sqllen > 0 ? v.sqllen : 8;
        }
        buffers_[i].assign(bufsz, 0);
        v.sqldata = buffers_[i].data();
        // Force nullable so libfbclient writes into our indicator slot.
        v.sqltype = c.sqltype | 1;
        v.sqlind  = &indicators_[i];
    }
}

bool FirebirdStatement::Fetch() {
    ISC_STATUS status[20] = {};
    long rc = isc_dsql_fetch(status, &stmt_, 1, out_sqlda_);
    if (rc == 100L) return false;  // no more rows
    if (rc != 0) {
        FirebirdConnection::Check(status, "isc_dsql_fetch");
    }
    return true;
}

bool FirebirdStatement::IsNull(idx_t col) const {
    return indicators_[col] == -1;
}

int16_t  FirebirdStatement::GetShort(idx_t col) const {
    int16_t v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
int32_t  FirebirdStatement::GetLong(idx_t col)  const {
    int32_t v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
int64_t  FirebirdStatement::GetInt64(idx_t col) const {
    int64_t v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
float    FirebirdStatement::GetFloat(idx_t col) const {
    float v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
double   FirebirdStatement::GetDouble(idx_t col) const {
    double v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}

std::string FirebirdStatement::GetText(idx_t col) const {
    const FirebirdColumnDesc &c = columns_[col];
    if (c.sqltype == SQL_VARYING) {
        // 2-byte little-endian length prefix, then the bytes.
        ISC_USHORT len;
        std::memcpy(&len, buffers_[col].data(), sizeof(len));
        return std::string(buffers_[col].data() + sizeof(len), len);
    }
    if (c.sqltype == SQL_TEXT) {
        // Fixed-width CHAR — trim trailing spaces.
        size_t len = c.sqllen;
        const char *p = buffers_[col].data();
        while (len > 0 && p[len - 1] == ' ') --len;
        return std::string(p, len);
    }
    return {};
}

ISC_TIMESTAMP FirebirdStatement::GetTimestamp(idx_t col) const {
    ISC_TIMESTAMP v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
ISC_DATE FirebirdStatement::GetDate(idx_t col) const {
    ISC_DATE v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
ISC_TIME FirebirdStatement::GetTime(idx_t col) const {
    ISC_TIME v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}
bool FirebirdStatement::GetBool(idx_t col) const {
    return buffers_[col][0] != 0;
}

void FirebirdStatement::GetInt128(idx_t col, uint64_t &out_lo, int64_t &out_hi) const {
    // Firebird stores INT128 little-endian as two 64-bit halves: lower
    // 64 bits first, then upper 64 (signed). Matches DuckDB's
    // hugeint_t {uint64_t lower; int64_t upper;} layout one-to-one.
    std::memcpy(&out_lo, buffers_[col].data(),     sizeof(out_lo));
    std::memcpy(&out_hi, buffers_[col].data() + 8, sizeof(out_hi));
}

ISC_TIMESTAMP FirebirdStatement::GetTimestampTzUtc(idx_t col) const {
    ISC_TIMESTAMP v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}

ISC_TIME FirebirdStatement::GetTimeTzUtc(idx_t col) const {
    ISC_TIME v;
    std::memcpy(&v, buffers_[col].data(), sizeof(v));
    return v;
}

std::string FirebirdStatement::ReadBlob(idx_t col) const {
    ISC_QUAD blob_id;
    std::memcpy(&blob_id, buffers_[col].data(), sizeof(blob_id));

    isc_blob_handle blob = 0;
    ISC_STATUS status[20] = {};
    isc_open_blob2(status, &conn_.db(), &conn_.tr(), &blob, &blob_id, 0, nullptr);
    FirebirdConnection::Check(status, "isc_open_blob2");

    std::string out;
    char segment[8192];
    unsigned short seg_len = 0;
    while (true) {
        std::memset(status, 0, sizeof(status));
        ISC_STATUS rc = isc_get_segment(status, &blob, &seg_len, sizeof(segment), segment);
        if (rc == isc_segstr_eof) break;
        if (rc != 0 && rc != isc_segment) {
            isc_close_blob(status, &blob);
            FirebirdConnection::Check(status, "isc_get_segment");
        }
        out.append(segment, seg_len);
        if (rc == 0) break;  // last partial segment
    }
    std::memset(status, 0, sizeof(status));
    isc_close_blob(status, &blob);
    return out;
}

} // namespace duckdb
