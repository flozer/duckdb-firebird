#include "firebird_client.hpp"
#include "firebird_client_loader.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

// Route every ISC entry-point this TU calls through the runtime-loaded
// FirebirdClientApi table. The redirect is local to this translation
// unit (the loader header declares the function table using the
// original names via decltype, so it must be included BEFORE these
// macros take effect). String literals naming these symbols in error
// messages are unaffected because the preprocessor does not expand
// inside `"..."`.
#define isc_attach_database          (::duckdb::fbapi().isc_attach_database)
#define isc_detach_database          (::duckdb::fbapi().isc_detach_database)
#define isc_start_transaction        (::duckdb::fbapi().isc_start_transaction)
#define isc_rollback_transaction     (::duckdb::fbapi().isc_rollback_transaction)
#define isc_dsql_allocate_statement  (::duckdb::fbapi().isc_dsql_allocate_statement)
#define isc_dsql_prepare             (::duckdb::fbapi().isc_dsql_prepare)
#define isc_dsql_describe            (::duckdb::fbapi().isc_dsql_describe)
#define isc_dsql_describe_bind       (::duckdb::fbapi().isc_dsql_describe_bind)
#define isc_dsql_execute             (::duckdb::fbapi().isc_dsql_execute)
#define isc_dsql_fetch               (::duckdb::fbapi().isc_dsql_fetch)
#define isc_dsql_free_statement      (::duckdb::fbapi().isc_dsql_free_statement)
#define isc_open_blob2               (::duckdb::fbapi().isc_open_blob2)
#define isc_close_blob               (::duckdb::fbapi().isc_close_blob)
#define isc_get_segment              (::duckdb::fbapi().isc_get_segment)
#define isc_sqlcode                  (::duckdb::fbapi().isc_sqlcode)
#define fb_interpret                 (::duckdb::fbapi().fb_interpret)

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
        // host[:port]/path -> libfbclient form "host[/port]:/path". We keep
        // the leading slash from the URL path so the result is unambiguous;
        // libfbclient accepts an aliased database (no slash) only when the
        // caller uses the key=value form.
        auto slash = rest.find('/');
        std::string host = slash == std::string::npos ? rest          : rest.substr(0, slash);
        std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
        if (!host.empty()) {
            auto port = host.rfind(':');
            if (port != std::string::npos && host.find(']') == std::string::npos) {
                host = host.substr(0, port) + "/" + host.substr(port + 1);
            }
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

// --- charset validation ------------------------------------------------------

void ValidateClientCharset(const std::string &charset) {
    std::string upper;
    upper.reserve(charset.size());
    for (char c : charset) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    // Accept names that produce UTF-8 wire bytes, plus the byte-pass-through
    // variants (NONE / OCTETS). Anything else would hand non-UTF-8 bytes to
    // DuckDB's string vectors and trip utf8proc later.
    if (upper == "UTF8" || upper == "UTF-8" || upper == "NONE" || upper == "OCTETS") {
        return;
    }
    throw IOException(
        "firebird: charset='" + charset + "' would deliver non-UTF-8 bytes to "
        "DuckDB. Use the default UTF8 — Firebird transliterates from " +
        charset + "-stored data to UTF-8 on the wire automatically.");
}

// none_encoding parser. Case-insensitive, accepts "iso8859_1" or
// "iso-8859-1" or "latin1" as the Latin-1 spelling.
NoneEncoding ParseNoneEncoding(const std::string &s) {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s) {
        if (c == '-') continue;            // collapse "ISO-8859-1"
        if (c == '_') continue;            // collapse "iso8859_1"
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "STRICT" || upper == "UTF8" || upper == "UTF8STRICT") {
        return NoneEncoding::STRICT;
    }
    if (upper == "WIN1252" || upper == "WINDOWS1252" || upper == "CP1252") {
        return NoneEncoding::WIN1252;
    }
    if (upper == "ISO88591" || upper == "LATIN1") {
        return NoneEncoding::ISO_8859_1;
    }
    if (upper == "BLOB" || upper == "OCTETS") {
        return NoneEncoding::BLOB;
    }
    throw IOException(
        "firebird: unknown none_encoding='" + s + "'. Accepted: "
        "'win1252' (default), 'iso8859_1' (alias 'latin1'), 'strict', 'blob'.");
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
    return AcquireWithInfo().conn;
}

FirebirdConnectionLease FirebirdConnectionPool::AcquireWithInfo() {
    if (!config_.enabled) {
        // Pool disabled: every Acquire creates a fresh connection and
        // Release drops it. Idle queue is never touched.
        auto fresh = std::make_unique<FirebirdConnection>(info_);
        total_created_.fetch_add(1, std::memory_order_relaxed);
        return { std::move(fresh), false };
    }

    {
        std::lock_guard<std::mutex> g(lock_);

        // Drain expired entries first. idle_ is filled with push_back at
        // Release time, so the front is the oldest by released_at.
        if (config_.idle_timeout_ms > 0 && !idle_.empty()) {
            const auto now = std::chrono::steady_clock::now();
            const auto cutoff = std::chrono::milliseconds(config_.idle_timeout_ms);
            size_t drop = 0;
            while (drop < idle_.size() &&
                   now - idle_[drop].released_at > cutoff) {
                ++drop;
            }
            if (drop > 0) {
                idle_.erase(idle_.begin(), idle_.begin() + drop);
                total_discarded_.fetch_add(static_cast<int64_t>(drop),
                                           std::memory_order_relaxed);
            }
        }

        if (!idle_.empty()) {
            auto conn = std::move(idle_.back().conn);
            idle_.pop_back();
            total_reused_.fetch_add(1, std::memory_order_relaxed);
            return { std::move(conn), true };
        }
    }

    auto fresh = std::make_unique<FirebirdConnection>(info_);
    total_created_.fetch_add(1, std::memory_order_relaxed);
    return { std::move(fresh), false };
}

void FirebirdConnectionPool::Release(std::unique_ptr<FirebirdConnection> conn) {
    if (!conn) return;

    if (!config_.enabled) {
        // Pool disabled: never park, destroy on release.
        total_discarded_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::lock_guard<std::mutex> g(lock_);

    // max_size caps the idle queue, not the number of active leases.
    // Past the cap we drop the connection that was just returned.
    if (config_.max_size > 0 &&
        static_cast<int64_t>(idle_.size()) >= config_.max_size) {
        total_discarded_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    IdleEntry e;
    e.conn = std::move(conn);
    e.released_at = std::chrono::steady_clock::now();
    idle_.push_back(std::move(e));
}

size_t FirebirdConnectionPool::IdleCount() {
    std::lock_guard<std::mutex> g(lock_);
    return idle_.size();
}

// --- FirebirdConnection ------------------------------------------------------

// Process-wide monotonic counter feeding FirebirdConnection::Id(). Used
// only for observability correlation; not a Firebird attachment id.
static std::atomic<int64_t> g_firebird_connection_id_counter{0};

FirebirdConnection::FirebirdConnection(const FirebirdConnectionInfo &info)
    : info_(info),
      id_(g_firebird_connection_id_counter.fetch_add(1, std::memory_order_relaxed)) {
    ValidateClientCharset(info_.charset);
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

std::unique_ptr<FirebirdStatement>
FirebirdConnection::OpenCursor(const std::string &sql,
                                const std::vector<Value> &params) {
    return make_uniq<FirebirdStatement>(*this, sql, params);
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

// Firebird epoch (Modified Julian Date) is 1858-11-17. DuckDB stores
// dates as days since 1970-01-01 (DATE_EPOCH = 40587 in MJD).
static constexpr int32_t MJD_EPOCH_TO_UNIX_EPOCH_DAYS = 40587;

void FirebirdStatement::BindInputParameters(const std::vector<Value> &params,
                                            const std::string &sql_for_error) {
    ISC_STATUS status[20] = {};

    in_sqlda_ = AllocXSQLDA(static_cast<int>(params.size()));
    isc_dsql_describe_bind(status, &stmt_, 1, in_sqlda_);
    FirebirdConnection::Check(status, "isc_dsql_describe_bind:\n" + sql_for_error);

    if (in_sqlda_->sqld > in_sqlda_->sqln) {
        int n = in_sqlda_->sqld;
        std::free(in_sqlda_);
        in_sqlda_ = AllocXSQLDA(n);
        std::memset(status, 0, sizeof(status));
        isc_dsql_describe_bind(status, &stmt_, 1, in_sqlda_);
        FirebirdConnection::Check(status, "isc_dsql_describe_bind (re):\n" + sql_for_error);
    }

    if (static_cast<size_t>(in_sqlda_->sqld) != params.size()) {
        throw IOException(
            "firebird: parameter count mismatch — SQL has " +
            std::to_string(in_sqlda_->sqld) + " placeholder(s), caller passed " +
            std::to_string(params.size()) + ".\n" + sql_for_error);
    }

    const int n = in_sqlda_->sqld;
    in_buffers_.resize(n);
    in_indicators_.assign(n, 0);

    for (int i = 0; i < n; ++i) {
        XSQLVAR &v = in_sqlda_->sqlvar[i];
        const Value &val = params[i];

        // Strip the nullable bit and remember it; we always allow NULL
        // bound parameters via the indicator slot.
        const auto base_sqltype = v.sqltype & ~1;
        v.sqltype = base_sqltype | 1;
        v.sqlind  = &in_indicators_[i];

        if (val.IsNull()) {
            // libfbclient still needs a valid sqldata pointer + buffer
            // even for NULL — allocate a 1-byte zero and flag via the
            // indicator.
            in_buffers_[i].assign(1, 0);
            v.sqldata = in_buffers_[i].data();
            in_indicators_[i] = -1;
            continue;
        }
        in_indicators_[i] = 0;

        switch (base_sqltype) {
        case SQL_TEXT:
        case SQL_VARYING: {
            // Coerce to string regardless of the DuckDB source type
            // (covers the case where DuckDB passes us an INTEGER
            // literal that bound to a VARCHAR column).
            auto s = val.ToString();
            if (base_sqltype == SQL_VARYING) {
                // VARYING expects a 2-byte length prefix + payload.
                in_buffers_[i].assign(s.size() + sizeof(ISC_USHORT), 0);
                ISC_USHORT len = static_cast<ISC_USHORT>(s.size());
                std::memcpy(in_buffers_[i].data(), &len, sizeof(len));
                std::memcpy(in_buffers_[i].data() + sizeof(len), s.data(), s.size());
                v.sqllen = static_cast<short>(s.size());
            } else {
                // CHAR(N): pad with spaces up to declared length.
                size_t cap = std::max<size_t>(v.sqllen, s.size());
                in_buffers_[i].assign(cap, ' ');
                std::memcpy(in_buffers_[i].data(), s.data(), s.size());
                v.sqllen = static_cast<short>(cap);
            }
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_SHORT: {
            in_buffers_[i].assign(sizeof(int16_t), 0);
            int16_t iv = static_cast<int16_t>(val.GetValue<int64_t>());
            std::memcpy(in_buffers_[i].data(), &iv, sizeof(iv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_LONG: {
            in_buffers_[i].assign(sizeof(int32_t), 0);
            int32_t iv = static_cast<int32_t>(val.GetValue<int64_t>());
            std::memcpy(in_buffers_[i].data(), &iv, sizeof(iv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_INT64: {
            in_buffers_[i].assign(sizeof(int64_t), 0);
            int64_t iv = val.GetValue<int64_t>();
            std::memcpy(in_buffers_[i].data(), &iv, sizeof(iv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_FLOAT: {
            in_buffers_[i].assign(sizeof(float), 0);
            float fv = static_cast<float>(val.GetValue<double>());
            std::memcpy(in_buffers_[i].data(), &fv, sizeof(fv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_DOUBLE:
        case SQL_D_FLOAT: {
            in_buffers_[i].assign(sizeof(double), 0);
            double dv = val.GetValue<double>();
            std::memcpy(in_buffers_[i].data(), &dv, sizeof(dv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_BOOLEAN: {
            in_buffers_[i].assign(sizeof(ISC_UCHAR), 0);
            ISC_UCHAR bv = val.GetValue<bool>() ? 1 : 0;
            std::memcpy(in_buffers_[i].data(), &bv, sizeof(bv));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_TYPE_DATE: {
            in_buffers_[i].assign(sizeof(ISC_DATE), 0);
            date_t d = val.GetValue<date_t>();
            ISC_DATE fb_date =
                static_cast<ISC_DATE>(d.days + MJD_EPOCH_TO_UNIX_EPOCH_DAYS);
            std::memcpy(in_buffers_[i].data(), &fb_date, sizeof(fb_date));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_TYPE_TIME: {
            in_buffers_[i].assign(sizeof(ISC_TIME), 0);
            // DuckDB TIME is microseconds since midnight; Firebird TIME is
            // 1/10000 second (100 microseconds) since midnight.
            dtime_t t = val.GetValue<dtime_t>();
            ISC_TIME fb_time = static_cast<ISC_TIME>(t.micros / 100);
            std::memcpy(in_buffers_[i].data(), &fb_time, sizeof(fb_time));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        case SQL_TIMESTAMP: {
            in_buffers_[i].assign(sizeof(ISC_TIMESTAMP), 0);
            timestamp_t ts = val.GetValue<timestamp_t>();
            date_t d;
            dtime_t t;
            Timestamp::Convert(ts, d, t);
            ISC_TIMESTAMP fb_ts;
            fb_ts.timestamp_date =
                static_cast<ISC_DATE>(d.days + MJD_EPOCH_TO_UNIX_EPOCH_DAYS);
            fb_ts.timestamp_time =
                static_cast<ISC_TIME>(t.micros / 100);
            std::memcpy(in_buffers_[i].data(), &fb_ts, sizeof(fb_ts));
            v.sqldata = in_buffers_[i].data();
            break;
        }
        default:
            // Type the server expects can't be encoded by this build
            // (FB4 INT128 / DECFLOAT / TZ on the bind path are not yet
            // supported here). Caller should treat as residual.
            throw IOException(
                "firebird: cannot bind parameter " + std::to_string(i) +
                " — server requires sqltype=" + std::to_string(base_sqltype) +
                " which is not yet supported as an input parameter. The "
                "caller's planner should leave this filter as a residual "
                "DuckDB predicate.\n" + sql_for_error);
        }
    }
}

void FirebirdStatement::Prepare(const std::string &sql) {
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
        int n = out_sqlda_->sqld;
        std::free(out_sqlda_);
        out_sqlda_ = AllocXSQLDA(n);
        std::memset(status, 0, sizeof(status));
        isc_dsql_describe(status, &stmt_, 1, out_sqlda_);
        FirebirdConnection::Check(status, "isc_dsql_describe");
    }

    AllocateBuffers();
}

FirebirdStatement::FirebirdStatement(FirebirdConnection &conn, const std::string &sql)
    : conn_(conn) {
    Prepare(sql);

    ISC_STATUS status[20] = {};
    isc_dsql_execute(status, &conn_.tr(), &stmt_, 1, nullptr);
    FirebirdConnection::Check(status, "isc_dsql_execute:\n" + sql);
}

FirebirdStatement::FirebirdStatement(FirebirdConnection &conn,
                                     const std::string &sql,
                                     const std::vector<Value> &params)
    : conn_(conn) {
    Prepare(sql);

    if (params.empty()) {
        // No placeholders → same code path as the no-parameter constructor.
        ISC_STATUS status[20] = {};
        isc_dsql_execute(status, &conn_.tr(), &stmt_, 1, nullptr);
        FirebirdConnection::Check(status, "isc_dsql_execute:\n" + sql);
        return;
    }

    BindInputParameters(params, sql);

    ISC_STATUS status[20] = {};
    isc_dsql_execute(status, &conn_.tr(), &stmt_, 1, in_sqlda_);
    FirebirdConnection::Check(status, "isc_dsql_execute (with params):\n" + sql);
}

FirebirdStatement::FirebirdStatement(FirebirdConnection &conn,
                                     const std::string &sql,
                                     PrepareOnlyTag)
    : conn_(conn) {
    Prepare(sql);
    // Deliberately no isc_dsql_execute call — describe-only.
}

FirebirdStatement::~FirebirdStatement() {
    if (stmt_) {
        ISC_STATUS s[20] = {};
        isc_dsql_free_statement(s, &stmt_, DSQL_drop);
    }
    if (out_sqlda_) std::free(out_sqlda_);
    if (in_sqlda_)  std::free(in_sqlda_);
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
        // For SQL_TEXT / SQL_VARYING, Firebird packs the INTL_TTYPE into
        // sqlsubtype: low byte = character_set_id, high byte = collation
        // id. CS_NONE = 0 — the case we have to surface so the fetch
        // path can transcode (or refuse) NONE bytes per none_encoding.
        // For BLOBs, sqlsubtype is the *blob subtype* (1 = text), not
        // a charset id; LoadTableSchema reads RDB$CHARACTER_SET_ID
        // from RDB$FIELDS for those, so we leave it untouched here.
        if (c.sqltype == SQL_TEXT || c.sqltype == SQL_VARYING) {
            c.character_set_id = static_cast<int16_t>(v.sqlsubtype & 0xFF);
        }

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
        case SQL_TIMESTAMP_TZ_EX:
            // Extended TZ adds a 4-byte session GMT offset (in minutes)
            // after the zone id. We don't surface the GMT offset itself
            // (DuckDB's TIMESTAMP_TZ is offset-only on UTC), but the
            // buffer must be sized correctly or fbclient writes past it.
            bufsz = sizeof(ISC_TIMESTAMP) + sizeof(int16_t) + sizeof(int32_t);
            break;
        case SQL_TIME_TZ:
            bufsz = sizeof(ISC_TIME) + sizeof(int16_t);
            break;
        case SQL_TIME_TZ_EX:
            bufsz = sizeof(ISC_TIME) + sizeof(int16_t) + sizeof(int32_t);
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
