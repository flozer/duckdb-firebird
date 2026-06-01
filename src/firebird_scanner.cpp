#include "firebird_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include "firebird_client.hpp"
#include "firebird_observability.hpp"
#include "firebird_query.hpp"
#include "firebird_types.hpp"

#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
//  Primary-key partitioning
// ---------------------------------------------------------------------------
//
// For single-column INTEGER/BIGINT primary keys we partition the scan into
// N ranges of equal numeric width and hand each one to its own worker. The
// PK probe runs once at bind time; the partition queue lives on the global
// state. Tables without a usable PK collapse to a single partition (= the
// pre-parallel behaviour).

struct PartitionSpec {
    // Optional filter applied to the cursor for this partition. Empty for
    // the "no PK" / single-partition case, which scans the whole table.
    std::string where_clause;
};

// ---------------------------------------------------------------------------
//  Global state — owns the partition queue + planner pushdown context.
//
// Stored once per query (in InitGlobal) so every worker can rebuild its
// per-partition SELECT with the same projection / filter pushdown the
// planner asked for.
// ---------------------------------------------------------------------------
struct FirebirdGlobalState : public GlobalTableFunctionState {
    std::mutex lock;
    std::vector<PartitionSpec> partitions;
    idx_t next_partition = 0;
    idx_t max_threads = 1;

    // Pushdown context captured at init time.
    std::vector<column_t>       column_ids;
    std::vector<idx_t>          projection_ids;
    optional_ptr<TableFilterSet> filters;

    idx_t MaxThreads() const override { return max_threads; }

    bool NextPartition(PartitionSpec &out) {
        std::lock_guard<std::mutex> g(lock);
        if (next_partition >= partitions.size()) return false;
        out = std::move(partitions[next_partition++]);
        return true;
    }
};

// ---------------------------------------------------------------------------
//  Local state — one connection + at most one active cursor per worker.
// ---------------------------------------------------------------------------
struct FirebirdLocalState : public LocalTableFunctionState {
    std::unique_ptr<FirebirdConnection> conn;
    std::unique_ptr<FirebirdStatement>  cursor;
    // When non-null, conn was leased from this pool; release it back on
    // destruction so it's available for the next query against the same
    // attached database.
    std::shared_ptr<FirebirdConnectionPool> pool;
    // When filter_prune=true and projection_ids is non-empty, DuckDB
    // expects the output chunk to carry only the projected columns —
    // but column_ids may include additional columns needed for filter
    // evaluation. We fetch into this scratch chunk (one slot per
    // column_id) and then ReferenceColumns into the caller's chunk
    // using projection_ids.
    DataChunk fetch_chunk;
    bool exhausted = false;
    // Phase 2 Chunk C - observability: pulled from the pool lease at
    // InitLocal time. connection_reused stays false for the direct
    // firebird_scan() path (no pool), true when AcquireWithInfo handed
    // back a recycled idle connection.
    bool connection_reused = false;

    ~FirebirdLocalState() override {
        // Order matters: the cursor has to be torn down before the
        // connection is returned to the pool (otherwise the cursor's
        // dsql_free_statement runs against a connection someone else
        // already grabbed).
        cursor.reset();
        if (pool && conn) {
            pool->Release(std::move(conn));
        }
    }
};

// ---------------------------------------------------------------------------
//  Schema discovery.
// ---------------------------------------------------------------------------
//
// Query RDB$RELATION_FIELDS to recover (name, sqltype, subtype, scale, length)
// for every column. We use the same JOIN-with-RDB$FIELDS shape as the
// peregrine extractor: one round-trip per table.

// SQL-quote a single-quoted string literal (doubles embedded quotes). Used
// for any user-controlled value reaching a Firebird WHERE clause — without
// this an attacker who controls the table name (e.g. through a
// firebird_scan('conn', $TBL) call) could escape the string literal and
// inject extra metadata queries.
static std::string SqlLiteral(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) { if (c == '\'') out.push_back('\''); out.push_back(c); }
    out.push_back('\'');
    return out;
}

bool DatabaseCharsetIsNone(FirebirdConnection &conn) {
    try {
        auto cur = conn.OpenCursor(
            "SELECT TRIM(RDB$CHARACTER_SET_NAME) FROM RDB$DATABASE");
        if (!cur->Fetch()) return false;
        if (cur->IsNull(0)) return true;  // NONE is sometimes stored NULL
        auto name = cur->GetText(0);
        std::string upper;
        for (char c : name) {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return upper == "NONE";
    } catch (...) {
        return false;
    }
}

void LoadTableSchema(FirebirdConnection &conn,
                     const std::string &table_name,
                     duckdb::vector<std::string> &out_names,
                     duckdb::vector<LogicalType> &out_types,
                     duckdb::vector<FirebirdColumnDesc> &out_descs,
                     NoneEncoding none_encoding) {
    // Firebird stores identifiers upper-cased unless quoted at creation; we
    // upper-case here so callers can pass either form.
    std::string upper = table_name;
    for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Pulls RDB$CHARACTER_SET_ID alongside the rest so the fetch path
    // can distinguish NONE columns (id = 0) from declared-charset
    // columns (Firebird transliterates those to UTF-8 wire). Non-text
    // columns get NULL → -1 sentinel.
    // `RDB$NULL_FLAG` per Firebird convention: 1 means NOT NULL is set
    // somewhere; NULL/0 means the column accepts NULL. The flag lives
    // on RDB$RELATION_FIELDS (per-column override) and on RDB$FIELDS
    // (domain default); COALESCE picks whichever is non-NULL, with the
    // RF override winning when both are set.
    const std::string sql =
        "SELECT TRIM(rf.RDB$FIELD_NAME), "
        "       f.RDB$FIELD_TYPE, "
        "       COALESCE(f.RDB$FIELD_SUB_TYPE, 0), "
        "       COALESCE(f.RDB$FIELD_SCALE, 0), "
        "       COALESCE(f.RDB$FIELD_LENGTH, 0), "
        "       COALESCE(f.RDB$CHARACTER_SET_ID, -1), "
        "       COALESCE(rf.RDB$NULL_FLAG, f.RDB$NULL_FLAG, 0) "
        "  FROM RDB$RELATION_FIELDS rf "
        "  JOIN RDB$FIELDS f ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE "
        " WHERE rf.RDB$RELATION_NAME = " + SqlLiteral(upper) + " "
        " ORDER BY rf.RDB$FIELD_POSITION";

    auto cursor = conn.OpenCursor(sql);
    while (cursor->Fetch()) {
        FirebirdColumnDesc desc;
        desc.name             = cursor->GetText(0);
        desc.sqltype          = cursor->GetShort(1);
        desc.sqlsubtype       = cursor->GetShort(2);
        desc.sqlscale         = cursor->GetShort(3);
        desc.sqllen           = cursor->GetShort(4);
        desc.character_set_id = cursor->GetShort(5);
        // RDB$NULL_FLAG: 1 means NOT NULL; absent / 0 means nullable.
        desc.nullable         = (cursor->GetShort(6) != 1);

        // RDB$FIELD_TYPE uses Firebird's *internal* blr_* type codes which
        // differ from the SQL-level XSQLDA constants. Codes 7..37/261 are
        // legacy (FB <=3); 23..31 are the Firebird 4 additions. Stay in sync
        // with src/include/firebird/impl/blr.h upstream.
        switch (desc.sqltype) {
        case 7:   desc.sqltype = SQL_SHORT;             break;  // SMALLINT
        case 8:   desc.sqltype = SQL_LONG;              break;  // INTEGER
        case 9:   desc.sqltype = SQL_QUAD;              break;  // QUAD
        case 10:  desc.sqltype = SQL_FLOAT;             break;
        case 11:  desc.sqltype = SQL_D_FLOAT;           break;
        case 12:  desc.sqltype = SQL_TYPE_DATE;         break;  // (Dialect 3)
        case 13:  desc.sqltype = SQL_TYPE_TIME;         break;
        case 14:  desc.sqltype = SQL_TEXT;              break;  // CHAR
        case 16:  desc.sqltype = SQL_INT64;             break;
        case 23:  desc.sqltype = SQL_BOOLEAN;           break;
        case 24:  desc.sqltype = SQL_DEC16;             break;  // FB4 blr_dec64  (IEEE Decimal64 / DECFLOAT(16))
        case 25:  desc.sqltype = SQL_DEC34;             break;  // FB4 blr_dec128 (IEEE Decimal128 / DECFLOAT(34))
        case 26:  desc.sqltype = SQL_INT128;            break;  // FB4 INT128 / DECIMAL/NUMERIC(p>18)
        case 27:  desc.sqltype = SQL_DOUBLE;            break;
        case 28:  desc.sqltype = SQL_TIME_TZ;           break;  // FB4 TIME WITH TIME ZONE
        case 29:  desc.sqltype = SQL_TIMESTAMP_TZ;      break;  // FB4 TIMESTAMP WITH TIME ZONE
        case 30:  desc.sqltype = SQL_TIME_TZ_EX;        break;  // FB4 EXTENDED TIME WITH TIME ZONE
        case 31:  desc.sqltype = SQL_TIMESTAMP_TZ_EX;   break;  // FB4 EXTENDED TIMESTAMP WITH TIME ZONE
        case 35:  desc.sqltype = SQL_TIMESTAMP;         break;
        case 37:  desc.sqltype = SQL_VARYING;           break;
        case 261: desc.sqltype = SQL_BLOB;              break;
        default: /* leave as-is; FirebirdToDuckDBType degrades to VARCHAR */ break;
        }

        // Map to a DuckDB LogicalType. When this is a text column on a
        // NONE storage charset and the caller asked for none_encoding=BLOB,
        // hand the bytes through as raw BLOB instead of pretending they
        // are UTF-8.
        LogicalType lt;
        const bool is_text = (desc.sqltype == SQL_TEXT || desc.sqltype == SQL_VARYING);
        const bool is_blob_subtype_text = (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1);
        const bool is_none_charset = (desc.character_set_id == 0);
        if (none_encoding == NoneEncoding::BLOB && is_none_charset &&
            (is_text || is_blob_subtype_text)) {
            lt = LogicalType::BLOB;
        } else {
            lt = FirebirdToDuckDBType(desc);
        }
        out_names.push_back(desc.name);
        out_types.push_back(lt);
        out_descs.push_back(desc);
    }

    if (out_names.empty()) {
        throw BinderException(
            "firebird_scan: table '" + table_name +
            "' not found or has no readable columns "
            "(RDB$RELATION_FIELDS returned no rows)");
    }
}

// ---------------------------------------------------------------------------
//  Primary-key probe (best-effort)
// ---------------------------------------------------------------------------
//
// Returns a PrimaryKeyInfo iff the table has a *single-column* PK on an
// INTEGER-family column. Composite PKs, non-numeric PKs, and tables with
// no PK at all simply fall through (we'll scan them single-threaded). We
// catch all exceptions because metadata access can fail in benign ways
// (missing privileges, special system tables, MON$ unavailable) and we
// never want a PK probe to break a working scan.

std::unique_ptr<PrimaryKeyInfo> ProbePrimaryKey(
    FirebirdConnection &conn,
    const std::string &table,
    const duckdb::vector<std::string> &all_column_names,
    const duckdb::vector<LogicalType> &all_column_types) {

    std::string upper = table;
    for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    try {
        // 1) PK index name.
        std::string pk_index;
        {
            auto cur = conn.OpenCursor(
                "SELECT TRIM(ri.RDB$INDEX_NAME) "
                "  FROM RDB$INDICES ri "
                "  JOIN RDB$RELATION_CONSTRAINTS rc ON ri.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
                " WHERE ri.RDB$RELATION_NAME = " + SqlLiteral(upper) + " "
                "   AND rc.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY'");
            if (!cur->Fetch()) return nullptr;          // no PK
            pk_index = cur->GetText(0);
        }

        // 2) PK columns (we require exactly one).
        std::vector<std::string> pk_cols;
        {
            auto cur = conn.OpenCursor(
                "SELECT TRIM(seg.RDB$FIELD_NAME) "
                "  FROM RDB$INDEX_SEGMENTS seg "
                " WHERE seg.RDB$INDEX_NAME = " + SqlLiteral(pk_index) + " "
                " ORDER BY seg.RDB$FIELD_POSITION");
            while (cur->Fetch()) pk_cols.push_back(cur->GetText(0));
        }
        if (pk_cols.size() != 1) return nullptr;        // composite PK — fall back

        // 3) Confirm the PK column is INTEGER-family in our resolved schema.
        const std::string &pk_name = pk_cols.front();
        auto it = std::find(all_column_names.begin(), all_column_names.end(), pk_name);
        if (it == all_column_names.end()) return nullptr;
        auto idx = static_cast<idx_t>(it - all_column_names.begin());
        switch (all_column_types[idx].id()) {
        case LogicalTypeId::SMALLINT:
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::BIGINT:
            break;
        default:
            return nullptr;                              // not numeric — fall back
        }

        // 4) MIN/MAX for the PK column. One round trip; Firebird hits the
        //    index for both. Empty tables abort.
        int64_t min_v = 0, max_v = 0;
        {
            std::string sql = "SELECT MIN(" + QuoteIdent(pk_name) + "), "
                              "       MAX(" + QuoteIdent(pk_name) + ") "
                              "  FROM " + QuoteIdent(table);
            auto cur = conn.OpenCursor(sql);
            if (!cur->Fetch()) return nullptr;
            if (cur->IsNull(0) || cur->IsNull(1)) return nullptr;
            // The MIN/MAX of an INTEGER-family column comes back as the same
            // SQL type as the column — read accordingly.
            switch (cur->columns()[0].sqltype) {
            case SQL_SHORT: min_v = cur->GetShort(0); max_v = cur->GetShort(1); break;
            case SQL_LONG:  min_v = cur->GetLong(0);  max_v = cur->GetLong(1);  break;
            case SQL_INT64: min_v = cur->GetInt64(0); max_v = cur->GetInt64(1); break;
            default: return nullptr;
            }
        }

        if (max_v <= min_v) return nullptr;             // 0-1 row table

        auto pk = make_uniq<PrimaryKeyInfo>();
        pk->column = pk_name;
        pk->min_value = min_v;
        pk->max_value = max_v;
        return std::move(pk);
    } catch (...) {
        // Any RDB$ access failure → silently fall back to single-thread.
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Bind.
// ---------------------------------------------------------------------------
static unique_ptr<FunctionData> FirebirdScanBind(ClientContext &context,
                                                 TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types,
                                                 vector<string> &names) {
    if (input.inputs.size() < 2) {
        throw BinderException("firebird_scan(connection_string, table_name) "
                              "requires two positional arguments");
    }
    auto bind = make_uniq<FirebirdBindData>();
    bind->conn_info = FirebirdConnectionInfo::Parse(input.inputs[0].ToString());
    bind->table_name = input.inputs[1].ToString();

    // Per-call overrides via named parameters.
    for (auto &kv : input.named_parameters) {
        auto &key = kv.first;
        auto &val = kv.second;
        if      (key == "user")       bind->conn_info.user     = val.ToString();
        else if (key == "password")   bind->conn_info.password = val.ToString();
        else if (key == "charset") {
            // DuckDB strings are UTF-8 internally; if the user asks for a
            // different client charset we'd be handing raw non-UTF-8
            // bytes to DuckDB, which crashes later (utf8proc fatal). The
            // default UTF8 already works for WIN1252 / ISO_8859_1 / Latin1
            // storage because Firebird transliterates server-side.
            auto c = val.ToString();
            std::string upper = c;
            for (auto &ch : upper) ch = static_cast<char>(std::toupper(
                static_cast<unsigned char>(ch)));
            if (upper != "UTF8" && upper != "UTF-8" && upper != "NONE" &&
                upper != "OCTETS") {
                throw BinderException(
                    "firebird_scan: charset='" + c + "' would deliver non-UTF-8 "
                    "bytes to DuckDB. Use the default UTF8 — Firebird "
                    "transliterates from " + c + "-stored data to UTF-8 wire "
                    "automatically. (See README → 'Charset handling'.)");
            }
            bind->conn_info.charset = c;
        }
        else if (key == "role")       bind->conn_info.role     = val.ToString();
        else if (key == "dialect")    bind->conn_info.dialect  = static_cast<int>(val.GetValue<int32_t>());
        else if (key == "partitions") {
            auto p = val.GetValue<int64_t>();
            if (p < 0) throw BinderException("partitions must be >= 0 (0 = auto)");
            bind->partitions_override = static_cast<idx_t>(p);
        }
        else if (key == "row_limit") {
            auto l = val.GetValue<int64_t>();
            if (l <= 0) throw BinderException("row_limit must be > 0");
            bind->limit_override = optional_idx(static_cast<idx_t>(l));
        }
        else if (key == "row_offset") {
            auto o = val.GetValue<int64_t>();
            if (o < 0) throw BinderException("row_offset must be >= 0");
            bind->offset_override = optional_idx(static_cast<idx_t>(o));
        }
        else if (key == "none_encoding") {
            // ParseNoneEncoding throws on unknown values with the list of
            // accepted spellings.
            bind->none_encoding = ParseNoneEncoding(val.ToString());
        }
    }
    // v0.5 contract: offset requires limit. Pure offset is a "skip then
    // drain" pattern that is both expensive (Firebird still streams the
    // skipped rows server-side, just throws them away on the wire) and
    // semantically surprising — the caller almost certainly wanted a
    // bounded slice. Force them to express the bound explicitly.
    if (bind->offset_override.IsValid() && !bind->limit_override.IsValid()) {
        throw BinderException(
            "firebird_scan: row_offset requires row_limit. Firebird's "
            "ROWS clause is bound-pair (`ROWS m TO n`), and pure-offset "
            "paging is expensive and surprising. Pass `row_limit=N` "
            "alongside `row_offset=M` to express the slice as "
            "[M+1, M+N].");
    }
    // Paging is global slice semantics, but partitions=N would replicate
    // the ROWS clause inside each per-partition query — a row_limit=100
    // with partitions=4 would return up to 400 rows, not 100. Two
    // protections:
    //
    // 1. An explicit `partitions=N` (N > 1) with row_limit / row_offset
    //    raises a BinderException — the caller asked for parallelism
    //    *and* paging, which are incompatible, so we make them choose.
    //
    // 2. The implicit case (partitions=0 default, or partitions=1) is
    //    silently *forced* to serial. The user wrote
    //    `firebird_scan(..., row_limit=100)` (which used to work in
    //    v0.4) and expects 100 rows; we honour that by collapsing
    //    partitions to 1 internally and skipping the PK probe below.
    //    This preserves the v0.4 contract without giving up
    //    correctness — paging via the auto-partition heuristic on a
    //    large table would otherwise over-fetch silently.
    const bool paging =
        bind->limit_override.IsValid() || bind->offset_override.IsValid();
    if (paging && bind->partitions_override > 1) {
        throw BinderException(
            "firebird_scan: row_limit / row_offset are global slice "
            "semantics; combining them with partitions > 1 would "
            "replicate the slice inside each partition (e.g. "
            "row_limit=100 + partitions=4 fetches 400 rows, not 100). "
            "Drop the partitions= argument (the scan will run serial "
            "automatically) or set partitions=1 explicitly.");
    }
    if (paging) {
        // Force serial — the partition queue below will get exactly
        // one entry, the PK probe is skipped, and the builder emits
        // one ROWS m TO n statement.
        bind->partitions_override = 1;
    }
    // Overflow guard: row_offset + row_limit must fit in idx_t (the
    // builder forms ROWS (offset+1) TO (offset+limit)). Reject extreme
    // values at bind time so the scanner never builds malformed SQL.
    if (bind->limit_override.IsValid() && bind->offset_override.IsValid()) {
        const idx_t off = bind->offset_override.GetIndex();
        const idx_t lim = bind->limit_override.GetIndex();
        if (off > std::numeric_limits<idx_t>::max() - lim) {
            throw BinderException(
                "firebird_scan: row_offset + row_limit overflow — "
                "their sum exceeds the maximum representable value "
                "(" + std::to_string(std::numeric_limits<idx_t>::max()) +
                "). Tighten one or the other.");
        }
    }

    FirebirdConnection conn(bind->conn_info);
    bind->db_charset_none = DatabaseCharsetIsNone(conn);
    LoadTableSchema(conn, bind->table_name,
                    bind->column_names, bind->column_types, bind->column_descs,
                    bind->none_encoding);

    // PK probe is only worth its three RDB$ round-trips if we might actually
    // parallelize. If the caller forced partitions=1 we skip it entirely;
    // also keeps interactive small-table queries fast on remote servers.
    if (bind->partitions_override != 1) {
        bind->pk = ProbePrimaryKey(conn, bind->table_name,
                                   bind->column_names, bind->column_types);
    }

    return_types = bind->column_types;
    names        = bind->column_names;
    return std::move(bind);
}

// ---------------------------------------------------------------------------
//  Init — slice the PK range into N partitions.
// ---------------------------------------------------------------------------
//
// Heuristic for partition count:
//   - No PK → 1 partition (full-table scan)
//   - PK range / row_estimate ≈ "rows per worker" target of 50k.
//     Bounded by (hardware concurrency, 32) to avoid pathologically many
//     short partitions on huge ranges.
//
// We don't have row_estimate at bind time (no MON$STATS query in the
// probe), so a fixed cap-and-floor is good enough: between 1 and 16
// partitions, scaled by the range width itself.

// Empirical: opening fresh Firebird connections + preparing the XSQLDAs +
// fanning queries through libfbclient's lock manager has measurable cost,
// and on Firebird 3.0 SuperServer (the typical OLTP deployment) the
// server-side scheduler effectively serializes queries against a single
// database file — so naive partitioning actively HURTS performance there.
//
// Defaults are tuned for that pessimistic case: only auto-partition when
// the PK range is large enough that the serial baseline is itself slow
// (a few seconds), and let users opt into finer partitioning via the
// partitions= named parameter when they're hitting Classic mode, multi-
// threaded SuperClassic, or a remote Firebird where parallelism wins.
//
// Concretely:
//   range <  4M rows  → 1 partition  (cheap serial baseline)
//   range >= 4M rows  → min(range / 2M, hardware_concurrency)
static constexpr int64_t MIN_ROWS_PER_PARTITION = 2'000'000;

static idx_t PickPartitionCount(int64_t min_v, int64_t max_v) {
    int64_t range = max_v - min_v + 1;
    if (range < MIN_ROWS_PER_PARTITION * 2) return 1;       // not worth the overhead
    auto hw = static_cast<int64_t>(std::thread::hardware_concurrency());
    if (hw <= 0) hw = 4;
    int64_t by_range = range / MIN_ROWS_PER_PARTITION;
    int64_t result   = std::min<int64_t>(by_range, hw);
    return static_cast<idx_t>(std::max<int64_t>(1, result));
}

static unique_ptr<GlobalTableFunctionState> FirebirdScanInitGlobal(
    ClientContext & /*context*/, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<FirebirdBindData>();
    auto gstate = make_uniq<FirebirdGlobalState>();

    if (bind.pk) {
        int64_t lo = bind.pk->min_value;
        int64_t hi = bind.pk->max_value;
        idx_t n = bind.partitions_override > 0
                      ? bind.partitions_override
                      : PickPartitionCount(lo, hi);
        // Never produce empty partitions for a tiny range.
        int64_t row_capacity = hi - lo + 1;
        if (row_capacity > 0 && static_cast<int64_t>(n) > row_capacity) {
            n = static_cast<idx_t>(row_capacity);
        }
        gstate->partitions.reserve(n);
        // Bucket boundaries via floor-div; the last bucket absorbs the
        // remainder. Using long double for the multiply protects against
        // overflow on near-INT64_MAX PK ranges.
        long double width = (static_cast<long double>(hi) - lo + 1.0L) / n;
        std::string pk = QuoteIdent(bind.pk->column);
        for (idx_t i = 0; i < n; ++i) {
            int64_t part_lo = lo + static_cast<int64_t>(width * i);
            int64_t part_hi = (i == n - 1)
                ? hi
                : lo + static_cast<int64_t>(width * (i + 1)) - 1;
            PartitionSpec spec;
            spec.where_clause = pk + " >= " + std::to_string(part_lo) +
                                " AND " + pk + " <= " + std::to_string(part_hi);
            gstate->partitions.push_back(std::move(spec));
        }
        gstate->max_threads = n;
    } else {
        gstate->partitions.push_back(PartitionSpec{});  // single "whole table" slice
        gstate->max_threads = 1;
    }

    // Capture pushdown context for the workers.
    gstate->column_ids = std::vector<column_t>(input.column_ids.begin(),
                                               input.column_ids.end());
    gstate->projection_ids = input.projection_ids;
    gstate->filters    = input.filters;
    return std::move(gstate);
}

static unique_ptr<LocalTableFunctionState> FirebirdScanInitLocal(
    ExecutionContext & /*ctx*/,
    TableFunctionInitInput &input,
    GlobalTableFunctionState * /*gstate*/) {
    auto &bind = input.bind_data->Cast<FirebirdBindData>();
    auto local = make_uniq<FirebirdLocalState>();
    if (bind.pool) {
        // ATTACH path - reuse a warm connection if the pool has one.
        // Use the info-bearing acquire so observability can surface
        // connection_reused; the direct firebird_scan() path below
        // does not go through the pool and keeps connection_reused
        // at its default false.
        local->pool = bind.pool;
        auto lease = bind.pool->AcquireWithInfo();
        local->conn              = std::move(lease.conn);
        local->connection_reused = lease.reused;
    } else {
        local->conn = make_uniq<FirebirdConnection>(bind.conn_info);
    }
    return std::move(local);
}

// ---------------------------------------------------------------------------
//  Scan.
// ---------------------------------------------------------------------------
//
// Each call to the scan function either continues the worker's current
// cursor or, if it's exhausted (or hasn't been opened yet), pulls the
// next partition from the global queue, opens a fresh cursor for it, and
// continues. The worker is "done" when the queue is empty and the
// current cursor (if any) has been fully drained.

static bool OpenNextPartitionCursor(ClientContext &ctx,
                                    TableFunctionInput &data,
                                    FirebirdLocalState &local) {
    auto &bind   = data.bind_data->Cast<FirebirdBindData>();
    auto &gstate = data.global_state->Cast<FirebirdGlobalState>();

    PartitionSpec spec;
    if (!gstate.NextPartition(spec)) return false;

    // Concatenate the partition range predicate with any extra predicates
    // lifted from BoundFunctionExpression pushdown (LIKE prefix, NOT IN,
    // BETWEEN, boolean NOT). All fragments are already SQL-safe
    // (identifiers via QuoteIdent, literals via SqlLiteral, value
    // positions via `?` placeholders); their params are concatenated
    // *after* whatever TableFilterSet params the builder produced, in
    // the same order they appear in the SQL.
    std::string combined = spec.where_clause;
    duckdb::vector<Value> extra_params;
    for (auto &ep : bind.extra_predicates) {
        if (!combined.empty()) combined += " AND ";
        combined += "(" + ep.sql + ")";
        for (auto &p : ep.params) extra_params.push_back(p);
    }

    auto query = FirebirdQueryBuilder::Build(
        bind.table_name,
        bind.column_names,
        bind.column_types,
        gstate.column_ids,
        gstate.filters,
        bind.limit_override,
        combined,
        &bind.column_descs,
        bind.none_encoding,
        bind.offset_override);

    // Append extra-predicate params to the builder's params, preserving
    // positional order. The Firebird wire protocol resolves `?` slots
    // strictly in textual order, so any predicate spliced into the
    // combined string above must also have its values appear after the
    // TableFilterSet values.
    for (auto &p : extra_params) query.params.push_back(p);

    // --- Observability capture (Phase 1, Chunk B) ---------------------------
    //
    // Record the SQL we are *about to send* to Firebird, the redacted
    // binds, and what pushdown the planner gave us. Capture happens
    // before OpenCursor, so the slot reflects the "last query attempted"
    // for this ClientContext - if OpenCursor itself fails server-side,
    // firebird_last_query() will still show the SQL we tried (this is
    // intentional for debugging; downstream chunks may add an error
    // field when timings/status land).
    //
    // Per-ClientContext slot, so one connection never reads another's
    // last query. Capture runs on every partition cursor open; under
    // parallel scan the surfaced row is literally the most recently
    // opened partition (parallel_scan flag flags it). Timings, rows_read,
    // and connection_id are out of scope for Chunk B per PM directive.
    {
        FirebirdQueryTelemetry rec;
        rec.remote_sql  = query.sql;
        rec.table_name  = bind.table_name;
        rec.binds_redacted.reserve(query.params.size());
        for (auto &p : query.params) {
            rec.binds_redacted.push_back(RedactBindValue(p));
        }
        rec.projected_columns.reserve(gstate.column_ids.size());
        for (auto cid : gstate.column_ids) {
            if (cid == COLUMN_IDENTIFIER_ROW_ID) {
                rec.projected_columns.push_back("<rowid>");
            } else if (cid < bind.column_names.size()) {
                rec.projected_columns.push_back(bind.column_names[cid]);
            }
        }
        rec.pushed_filters = query.pushed_filter_sql;
        for (auto &ep : bind.extra_predicates) {
            rec.pushed_filters.push_back(ep.sql);
        }
        rec.residual_filters.reserve(query.residual_filter_indices.size());
        for (auto idx : query.residual_filter_indices) {
            rec.residual_filters.push_back("filter[" + std::to_string(idx) + "]");
        }
        // Pushdown explainability (Phase 4 #3): coarse reason per residual
        // filter (parallel to residual_filters), plus the ROWS clause
        // paging that was actually pushed to Firebird.
        rec.not_pushed_reasons = query.residual_filter_reasons;
        // Complex filters (LIKE / NOT IN) gated off by CHARACTER SET NONE
        // never reach the Build() residual set — they were lifted out at
        // pushdown-complex time and re-applied by DuckDB. Surface them too,
        // each with a matching residual_filters placeholder so the two
        // lists stay equal length and the gate is visible.
        for (const auto &reason : bind.gated_complex_reasons) {
            rec.residual_filters.push_back("complex_filter[none_gated]");
            rec.not_pushed_reasons.push_back(reason);
        }
        rec.limit_pushed  = bind.limit_override;
        rec.offset_pushed = bind.offset_override;
        rec.partitions    = static_cast<int32_t>(gstate.partitions.size());
        rec.parallel_scan = rec.partitions > 1;
        rec.captured_at   = Timestamp::GetCurrentTimestamp();
        // Phase 2 Chunk C - pool identity from the active lease. For
        // the direct firebird_scan() path the connection was not
        // leased from any pool, but it still carries a process-wide
        // monotonic id assigned at construction; connection_reused
        // stays false because nothing was recycled.
        if (local.conn) {
            rec.connection_id = local.conn->Id();
        }
        rec.connection_reused = local.connection_reused;

        // Ring buffer is opt-in via SET firebird_query_log_size = N.
        // The default registered in firebird_extension.cpp is 0 (off).
        // Missing setting -> treat as 0 so a stripped-down embed of the
        // extension that never registered the option does not crash.
        int64_t log_size = 0;
        Value setting_val;
        if (ctx.TryGetCurrentSetting("firebird_query_log_size", setting_val)) {
            if (!setting_val.IsNull()) {
                log_size = setting_val.GetValue<int64_t>();
            }
        }
        GetObservabilityState(ctx)->RecordQuery(rec, log_size);
    }

    {
        // Time the OpenCursor call so firebird_time_us picks up the
        // server round-trip cost (prepare + execute + first XSQLDA).
        // On exception we tag the observability slot with the error
        // message so firebird_last_query() can correlate the failed SQL
        // with what Firebird returned, then rethrow.
        auto fb_t0 = std::chrono::steady_clock::now();
        try {
            local.cursor = query.params.empty()
                ? local.conn->OpenCursor(query.sql)
                : local.conn->OpenCursor(query.sql, query.params);
        } catch (const std::exception &e) {
            GetObservabilityState(ctx)->SetError(
                std::string("OpenCursor: ") + e.what());
            throw;
        }
        auto fb_t1 = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(fb_t1 - fb_t0).count();
        auto obs = GetObservabilityState(ctx);
        obs->AddFirebirdTimeUs(static_cast<int64_t>(us));
        obs->UpdateTotalTimeUs();
    }
    // The XSQLDA gives us a character_set_id for SQL_TEXT / SQL_VARYING
    // (packed in sqlsubtype), but not for text BLOBs — and column_ids
    // maps cursor-column-index back to the original bind position, so
    // splice the LoadTableSchema-derived charset id onto the cursor's
    // FirebirdColumnDesc here. This is what the fetch path reads to
    // decide whether to transcode / reject / pass-through.
    for (idx_t c = 0; c < gstate.column_ids.size(); ++c) {
        auto src_col = gstate.column_ids[c];
        if (src_col == COLUMN_IDENTIFIER_ROW_ID) continue;
        if (src_col >= bind.column_descs.size()) continue;
        auto cs = bind.column_descs[src_col].character_set_id;
        if (cs >= 0) local.cursor->OverrideCharsetId(c, cs);
    }
    return true;
}

static void FirebirdScanFunction(ClientContext &ctx,
                                 TableFunctionInput &data,
                                 DataChunk &output) {
    auto &local  = data.local_state->Cast<FirebirdLocalState>();
    auto &bind   = data.bind_data->Cast<FirebirdBindData>();
    auto &gstate = data.global_state->Cast<FirebirdGlobalState>();

    // With filter_prune=true, the output chunk only carries the columns
    // listed in projection_ids; the cursor however emits one slot per
    // column_id. Allocate a scratch chunk shaped by column_ids the
    // first time we're called and reference into the output.
    //
    // When projection_ids is empty (the planner kept all columns), or
    // matches column_ids 1:1, we still go through the scratch path —
    // it is uniform and the ReferenceColumns cost is negligible
    // (vector pointer swaps, no copies).
    if (local.fetch_chunk.ColumnCount() == 0) {
        vector<LogicalType> fetch_types;
        fetch_types.reserve(gstate.column_ids.size());
        for (auto cid : gstate.column_ids) {
            if (cid == COLUMN_IDENTIFIER_ROW_ID) {
                fetch_types.push_back(LogicalType::BIGINT);
            } else {
                fetch_types.push_back(bind.column_types[cid]);
            }
        }
        local.fetch_chunk.Initialize(Allocator::DefaultAllocator(), fetch_types);
    }
    local.fetch_chunk.Reset();

    const idx_t target = STANDARD_VECTOR_SIZE;
    const idx_t n_fetch_cols = local.fetch_chunk.ColumnCount();
    idx_t row = 0;

    // Observability (Chunk C): accumulate only the time spent in the
    // wire-level Fetch() calls. OpenCursor is timed separately in
    // OpenNextPartitionCursor; if we also wrapped the whole loop here
    // we would double-count the OpenCursor cost. The value-copy loop
    // (FirebirdAppendValue) is local CPU work and stays outside the
    // FB-time tally.
    int64_t fetch_us = 0;
    try {
        while (row < target) {
            if (!local.cursor) {
                if (!OpenNextPartitionCursor(ctx, data, local)) break;
            }
            auto t0 = std::chrono::steady_clock::now();
            const bool got = local.cursor->Fetch();
            auto t1 = std::chrono::steady_clock::now();
            fetch_us += std::chrono::duration_cast<std::chrono::microseconds>(
                            t1 - t0).count();
            if (!got) {
                local.cursor.reset();
                continue;
            }
            for (idx_t c = 0; c < n_fetch_cols; ++c) {
                FirebirdAppendValue(*local.cursor, c,
                                    local.fetch_chunk.data[c], row,
                                    bind.none_encoding);
            }
            ++row;
        }
    } catch (const std::exception &e) {
        GetObservabilityState(ctx)->SetError(
            std::string("Fetch: ") + e.what());
        throw;
    }
    local.fetch_chunk.SetCardinality(row);

    {
        auto obs = GetObservabilityState(ctx);
        if (row > 0) {
            obs->AddRows(static_cast<int64_t>(row));
        }
        if (fetch_us > 0) {
            obs->AddFirebirdTimeUs(fetch_us);
        }
        obs->UpdateTotalTimeUs();
    }

    if (gstate.projection_ids.empty()) {
        // No projection list — caller wants every fetched column.
        output.Reference(local.fetch_chunk);
    } else {
        duckdb::vector<column_t> proj(gstate.projection_ids.begin(),
                                      gstate.projection_ids.end());
        output.ReferenceColumns(local.fetch_chunk, proj);
    }
}

// ---------------------------------------------------------------------------
//  firebird_tables(connection_string) — discovery table function
// ---------------------------------------------------------------------------
//
// Returns one row per *user* table in the Firebird database, with the
// column count and a flag indicating whether we'd parallelize a scan
// against it. This is the "where do I start?" function for users
// pointed at an unfamiliar legacy database.

struct FirebirdTablesBindData : public TableFunctionData {
    FirebirdConnectionInfo conn_info;
};

struct FirebirdTablesRow {
    std::string table_name;
    std::string kind;               // "table", "view", "external", or "gtt"
    int32_t column_count = 0;
    bool has_pk = false;
    std::string pk_column;          // empty when has_pk is false
};

static const char *RelationKindLabel(int rtype) {
    // RDB$RELATION_TYPE values:
    //   0 (or NULL)        — persistent table
    //   1                  — view
    //   2                  — external table
    //   3                  — virtual / MON$ (excluded by the WHERE clause)
    //   4, 5               — global temporary table
    switch (rtype) {
    case 1:           return "view";
    case 2:           return "external";
    case 4: case 5:   return "gtt";
    default:          return "table";
    }
}

struct FirebirdTablesGlobalState : public GlobalTableFunctionState {
    std::vector<FirebirdTablesRow> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData> FirebirdTablesBind(ClientContext &,
                                                   TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types,
                                                   vector<string> &names) {
    if (input.inputs.empty()) {
        throw BinderException("firebird_tables(connection_string) "
                              "requires the connection string as a positional argument");
    }
    auto bind = make_uniq<FirebirdTablesBindData>();
    bind->conn_info = FirebirdConnectionInfo::Parse(input.inputs[0].ToString());
    for (auto &kv : input.named_parameters) {
        if      (kv.first == "user")     bind->conn_info.user     = kv.second.ToString();
        else if (kv.first == "password") bind->conn_info.password = kv.second.ToString();
        else if (kv.first == "charset")  bind->conn_info.charset  = kv.second.ToString();
        else if (kv.first == "role")     bind->conn_info.role     = kv.second.ToString();
        else if (kv.first == "dialect")  bind->conn_info.dialect  =
            static_cast<int>(kv.second.GetValue<int32_t>());
    }
    names = {"table_name", "kind", "column_count", "has_pk", "pk_column"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::INTEGER, LogicalType::BOOLEAN, LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> FirebirdTablesInitGlobal(
    ClientContext &, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<FirebirdTablesBindData>();
    auto gstate = make_uniq<FirebirdTablesGlobalState>();

    FirebirdConnection conn(bind.conn_info);

    // One query gives us name + relation kind + column count + PK column.
    // System tables filtered via SYSTEM_FLAG; we keep persistent tables
    // (type 0/NULL), views (1), external tables (2), and global
    // temporaries (4,5). Only MON$ virtual snapshots (type 3) are
    // intentionally excluded — they're best accessed via direct SQL.
    const std::string sql =
        "SELECT TRIM(r.RDB$RELATION_NAME) AS NAME, "
        "       COALESCE(r.RDB$RELATION_TYPE, 0) AS RTYPE, "
        "       (SELECT COUNT(*) FROM RDB$RELATION_FIELDS rf "
        "          WHERE rf.RDB$RELATION_NAME = r.RDB$RELATION_NAME) AS CC, "
        "       (SELECT TRIM(seg.RDB$FIELD_NAME) "
        "          FROM RDB$INDICES ri "
        "          JOIN RDB$RELATION_CONSTRAINTS rc ON ri.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        "          JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = ri.RDB$INDEX_NAME "
        "         WHERE ri.RDB$RELATION_NAME = r.RDB$RELATION_NAME "
        "           AND rc.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY' "
        "         ROWS 1) AS PK "
        "  FROM RDB$RELATIONS r "
        " WHERE r.RDB$SYSTEM_FLAG = 0 "
        "   AND (r.RDB$RELATION_TYPE IS NULL OR r.RDB$RELATION_TYPE <> 3) "
        " ORDER BY r.RDB$RELATION_NAME";

    auto cursor = conn.OpenCursor(sql);
    while (cursor->Fetch()) {
        FirebirdTablesRow row;
        row.table_name = cursor->GetText(0);
        int rtype = cursor->IsNull(1) ? 0 : cursor->GetShort(1);
        row.kind = RelationKindLabel(rtype);
        row.column_count = cursor->IsNull(2) ? 0 : cursor->GetLong(2);
        if (!cursor->IsNull(3)) {
            row.has_pk = true;
            row.pk_column = cursor->GetText(3);
        }
        gstate->rows.push_back(std::move(row));
    }
    return std::move(gstate);
}

static void FirebirdTablesFunction(ClientContext &, TableFunctionInput &data,
                                   DataChunk &output) {
    auto &gstate = data.global_state->Cast<FirebirdTablesGlobalState>();
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t row = 0;
    while (row < target && gstate.cursor < gstate.rows.size()) {
        const auto &r = gstate.rows[gstate.cursor++];
        FlatVector::GetData<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], r.table_name);
        FlatVector::GetData<string_t>(output.data[1])[row] =
            StringVector::AddString(output.data[1], r.kind);
        FlatVector::GetData<int32_t>(output.data[2])[row] = r.column_count;
        FlatVector::GetData<bool>(output.data[3])[row]    = r.has_pk;
        if (r.has_pk) {
            FlatVector::GetData<string_t>(output.data[4])[row] =
                StringVector::AddString(output.data[4], r.pk_column);
        } else {
            FlatVector::SetNull(output.data[4], row, true);
        }
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdTablesFunction() {
    TableFunction fn("firebird_tables",
                     {LogicalType::VARCHAR},
                     FirebirdTablesFunction,
                     FirebirdTablesBind,
                     FirebirdTablesInitGlobal);
    fn.named_parameters["user"]     = LogicalType::VARCHAR;
    fn.named_parameters["password"] = LogicalType::VARCHAR;
    fn.named_parameters["charset"]  = LogicalType::VARCHAR;
    fn.named_parameters["role"]     = LogicalType::VARCHAR;
    fn.named_parameters["dialect"]  = LogicalType::INTEGER;
    return fn;
}

// ---------------------------------------------------------------------------
//  firebird_attach_sql(connection_string [, target_schema='fb'])
// ---------------------------------------------------------------------------
//
// Returns the DDL needed to "attach" a Firebird database without writing
// a full StorageExtension. The function does no side effects — it just
// emits one row per CREATE statement: the CREATE SCHEMA first, then one
// CREATE OR REPLACE VIEW per user table that wraps firebird_scan().
//
// Usage in the DuckDB CLI:
//
//   COPY (
//     SELECT sql FROM firebird_attach_sql('firebird://…', 'fb')
//   ) TO 'attach.sql' (FORMAT 'csv', HEADER false, QUOTE '');
//   .read attach.sql
//
// or one-shot from the shell:
//
//   duckdb -c "SELECT sql FROM firebird_attach_sql('…', 'fb')" | duckdb my.db
//
// Direct side-effect execution from inside a table function is unsafe in
// DuckDB (nested ctx.Query() deadlocks the client lock), so we deliver
// the DDL as data and let the user route it through their normal SQL
// execution path.

struct FirebirdAttachBindData : public TableFunctionData {
    std::string connection_string;
    FirebirdConnectionInfo conn_info;
    std::string target_schema = "fb";
    bool overwrite = true;
};

struct FirebirdAttachGlobalState : public GlobalTableFunctionState {
    std::vector<std::string> stmts;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData> FirebirdAttachBind(ClientContext &,
                                                   TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types,
                                                   vector<string> &names) {
    if (input.inputs.empty()) {
        throw BinderException("firebird_attach_sql(connection_string [, schema='fb'])");
    }
    auto bind = make_uniq<FirebirdAttachBindData>();
    bind->connection_string = input.inputs[0].ToString();
    bind->conn_info = FirebirdConnectionInfo::Parse(bind->connection_string);
    if (input.inputs.size() >= 2) {
        bind->target_schema = input.inputs[1].ToString();
    }
    for (auto &kv : input.named_parameters) {
        if      (kv.first == "user")      bind->conn_info.user     = kv.second.ToString();
        else if (kv.first == "password")  bind->conn_info.password = kv.second.ToString();
        else if (kv.first == "charset")   bind->conn_info.charset  = kv.second.ToString();
        else if (kv.first == "role")      bind->conn_info.role     = kv.second.ToString();
        else if (kv.first == "dialect")   bind->conn_info.dialect  =
            static_cast<int>(kv.second.GetValue<int32_t>());
        else if (kv.first == "schema")    bind->target_schema      = kv.second.ToString();
        else if (kv.first == "overwrite") bind->overwrite          = kv.second.GetValue<bool>();
    }
    names = {"sql"};
    return_types = {LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> FirebirdAttachInitGlobal(
    ClientContext &, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<FirebirdAttachBindData>();
    auto gstate = make_uniq<FirebirdAttachGlobalState>();

    // List user-visible relations (tables, views, externals, GTTs);
    // skip MON$ virtual snapshots (type 3) and system tables.
    FirebirdConnection conn(bind.conn_info);
    std::vector<std::string> tables;
    {
        auto cur = conn.OpenCursor(
            "SELECT TRIM(r.RDB$RELATION_NAME) "
            "  FROM RDB$RELATIONS r "
            " WHERE r.RDB$SYSTEM_FLAG = 0 "
            "   AND (r.RDB$RELATION_TYPE IS NULL OR r.RDB$RELATION_TYPE <> 3) "
            " ORDER BY r.RDB$RELATION_NAME");
        while (cur->Fetch()) tables.push_back(cur->GetText(0));
    }

    // Each emitted row is one self-contained statement, terminated with
    // `;` so the user can `.read` the result without further processing.
    gstate->stmts.push_back(
        "CREATE SCHEMA IF NOT EXISTS " + QuoteIdent(bind.target_schema) + ";");

    const std::string verb = bind.overwrite
        ? "CREATE OR REPLACE VIEW "
        : "CREATE VIEW IF NOT EXISTS ";
    for (auto &tbl : tables) {
        std::ostringstream ddl;
        ddl << verb
            << QuoteIdent(bind.target_schema) << "." << QuoteIdent(tbl)
            << " AS SELECT * FROM firebird_scan("
            << SqlLiteral(bind.connection_string) << ", "
            << SqlLiteral(tbl) << ");";
        gstate->stmts.push_back(ddl.str());
    }
    return std::move(gstate);
}

static void FirebirdAttachFunction(ClientContext &, TableFunctionInput &data,
                                   DataChunk &output) {
    auto &gstate = data.global_state->Cast<FirebirdAttachGlobalState>();
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t row = 0;
    while (row < target && gstate.cursor < gstate.stmts.size()) {
        FlatVector::GetData<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], gstate.stmts[gstate.cursor++]);
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdAttachFunction() {
    TableFunction fn("firebird_attach_sql",
                     {LogicalType::VARCHAR},
                     FirebirdAttachFunction,
                     FirebirdAttachBind,
                     FirebirdAttachInitGlobal);
    fn.varargs = LogicalType::VARCHAR;            // optional positional schema name
    fn.named_parameters["user"]      = LogicalType::VARCHAR;
    fn.named_parameters["password"]  = LogicalType::VARCHAR;
    fn.named_parameters["charset"]   = LogicalType::VARCHAR;
    fn.named_parameters["role"]      = LogicalType::VARCHAR;
    fn.named_parameters["dialect"]   = LogicalType::INTEGER;
    fn.named_parameters["schema"]    = LogicalType::VARCHAR;
    fn.named_parameters["overwrite"] = LogicalType::BOOLEAN;
    return fn;
}

// ---------------------------------------------------------------------------
//  Complex-filter pushdown — `col LIKE 'prefix%'`
// ---------------------------------------------------------------------------
//
// `LIKE` reaches the scan as a `BoundFunctionExpression` (scalar function
// `~~`), not a `TableFilter`, so DuckDB hands it over via this hook. We
// only translate the high-value case: a constant right-hand-side whose
// wildcards (`%`, `_`) are all at position >= 1 — Firebird picks an
// index range scan for that. Patterns starting with a wildcard, or any
// pattern we don't recognise, are left in `filters` so DuckDB re-applies
// them above the scan.
//
// Escape rules: we always emit `... ESCAPE '\\'` so a user-supplied
// backslash in the pattern keeps its literal meaning. The pattern
// literal itself goes through SqlLiteral, which doubles embedded
// single quotes; we do not pre-escape `%`/`_` because the user wrote
// them deliberately.

// Returns nullopt if `expr` is not a `col LIKE 'literal'` shape with a
// usable non-wildcard prefix. Otherwise returns the column index and the
// pattern literal.
static bool TryExtractLikePrefix(Expression &expr,
                                 idx_t &out_col,
                                 std::string &out_pattern) {
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
        return false;
    }
    auto &fn = expr.Cast<BoundFunctionExpression>();
    // DuckDB exposes `LIKE` as the scalar function "~~".
    if (fn.function.name != "~~" || fn.children.size() != 2) return false;

    auto &lhs = *fn.children[0];
    auto &rhs = *fn.children[1];
    if (lhs.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) return false;
    if (rhs.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)   return false;

    auto &colref = lhs.Cast<BoundColumnRefExpression>();
    auto &constexpr_ = rhs.Cast<BoundConstantExpression>();
    if (constexpr_.value.IsNull()) return false;
    if (constexpr_.value.type().id() != LogicalTypeId::VARCHAR) return false;

    auto pattern = constexpr_.value.GetValue<std::string>();
    if (pattern.empty()) return false;
    // Require at least one literal character before any wildcard so
    // Firebird can use an index range scan. A leading `%`/`_` would
    // force a full table scan anyway, with no benefit from pushdown.
    char first = pattern.front();
    if (first == '%' || first == '_') return false;

    out_col     = colref.binding.column_index;
    out_pattern = std::move(pattern);
    return true;
}

// Same bindable-type whitelist the FirebirdQueryBuilder uses (kept in
// sync — both files need the same predicate). Strings, signed
// integers, floats, DATE / TIME / TIMESTAMP, BOOLEAN bind through the
// input XSQLDA path; HUGEINT / DECIMAL / unsigned ints / TZ types fall
// back to inline literals where safe and to residual otherwise.
//
// Unsigned ints are deliberately excluded: Firebird has no unsigned
// counterpart, the XSQLVAR is signed, and a UBIGINT > INT64_MAX would
// silently wrap to a negative value on the bind path. Filters with
// such values stay residual.
static bool ExprValueBindable(const LogicalType &t) {
    switch (t.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIME:
    case LogicalTypeId::TIMESTAMP:
        return true;
    default:
        return false;
    }
}

// Detects `col NOT IN (lit, lit, ...)`. DuckDB emits this as a
// BoundOperatorExpression with type COMPARE_NOT_IN; children[0] is the
// column, children[1..] are the constants. We require every constant
// to be bindable so the predicate ships through the input XSQLDA path
// (strings always parametrise, mixed-type lists end up residual).
static bool TryExtractNotIn(Expression &expr,
                             idx_t &out_col,
                             duckdb::vector<Value> &out_values) {
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) return false;
    auto &op = expr.Cast<BoundOperatorExpression>();
    if (op.GetExpressionType() != ExpressionType::COMPARE_NOT_IN) return false;
    if (op.children.size() < 2) return false;
    if (op.children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) return false;

    out_values.clear();
    for (size_t i = 1; i < op.children.size(); ++i) {
        auto &child = *op.children[i];
        if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) return false;
        const auto &v = child.Cast<BoundConstantExpression>().value;
        if (v.IsNull()) return false;                          // NULL in IN-list is squirrelly
        if (!ExprValueBindable(v.type())) return false;
        out_values.push_back(v);
    }
    out_col = op.children[0]->Cast<BoundColumnRefExpression>().binding.column_index;
    return true;
}

// Detects `NOT bool_col` — boolean negation of a column reference. Used
// to push `WHERE NOT ACTIVE` and friends as `NOT (...)` to Firebird.
// Anything more complex than a bare column reference stays residual so
// we don't have to reason about precedence / nullability flipping.
static bool TryExtractNotBoolColumn(Expression &expr, idx_t &out_col) {
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR) return false;
    auto &op = expr.Cast<BoundOperatorExpression>();
    if (op.GetExpressionType() != ExpressionType::OPERATOR_NOT) return false;
    if (op.children.size() != 1) return false;
    auto &inner = *op.children[0];
    if (inner.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) return false;
    if (inner.return_type.id() != LogicalTypeId::BOOLEAN) return false;
    out_col = inner.Cast<BoundColumnRefExpression>().binding.column_index;
    return true;
}

// Whitelist hook called by DuckDB's planner *before* it commits a
// TableFilterSet entry to pushdown. Returning false makes DuckDB add a
// FILTER operator on top of the scan AND drop the filter from the set
// we hand to the builder — exactly the behaviour we need for NONE text
// columns whose bytes have to be transcoded client-side before any
// UTF-8 literal can match them.
static bool FirebirdScanSupportsPushdownType(const FunctionData &bind_data,
                                             idx_t col_idx) {
    auto &bind = bind_data.Cast<FirebirdBindData>();
    if (col_idx >= bind.column_descs.size()) return true;
    const auto &desc = bind.column_descs[col_idx];
    if (bind.none_encoding == NoneEncoding::STRICT) return true;
    if (desc.character_set_id != 0) return true;
    const bool is_text_or_text_blob =
        desc.sqltype == SQL_TEXT || desc.sqltype == SQL_VARYING ||
        (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1);
    return !is_text_or_text_blob;
}

static void FirebirdScanPushdownComplexFilter(
    ClientContext & /*ctx*/, LogicalGet &get, FunctionData *bind_data_p,
    vector<unique_ptr<Expression>> &filters) {

    if (!bind_data_p) return;
    auto &bind = bind_data_p->Cast<FirebirdBindData>();

    // `column_index` in BoundColumnRefExpression is an index into the
    // scan's projected columns (i.e. into LogicalGet::GetColumnIds()),
    // not into bind.column_names. Resolve through GetColumnIds() to find
    // the original column index.
    const auto &col_ids = get.GetColumnIds();

    // Helper: resolve a projected column index from a BoundColumnRef onto
    // the source column index in bind.column_names, returning -1 sentinel
    // (cast to idx_t) when the binding is the virtual row-id or out of
    // range. Returns true on success.
    auto resolve_src_col = [&](idx_t projected_col, idx_t &out) -> bool {
        if (projected_col >= col_ids.size()) return false;
        auto src = col_ids[projected_col].GetPrimaryIndex();
        if (src == COLUMN_IDENTIFIER_ROW_ID || src >= bind.column_names.size())
            return false;
        out = src;
        return true;
    };

    // Helper: NONE-text columns under non-strict transcoding never push.
    auto is_none_text_gated = [&](idx_t src_col_idx) -> bool {
        if (src_col_idx >= bind.column_descs.size()) return false;
        const auto &desc = bind.column_descs[src_col_idx];
        if (bind.none_encoding == NoneEncoding::STRICT) return false;
        if (desc.character_set_id != 0) return false;
        return desc.sqltype == SQL_TEXT || desc.sqltype == SQL_VARYING ||
               (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1);
    };

    // Helper: DECFLOAT(16/34) is exposed to DuckDB as VARCHAR and projected
    // as CAST(col AS VARCHAR(64)). A pushed complex filter must compare
    // against the SAME expression — otherwise Firebird would compare
    // numerically while DuckDB believes the column is text. Since lifted
    // complex predicates are NOT re-checked by DuckDB, the column reference
    // here must mirror the projection's cast.
    auto is_decfloat = [&](idx_t src_col_idx) -> bool {
        if (src_col_idx >= bind.column_descs.size()) return false;
        const auto &desc = bind.column_descs[src_col_idx];
        return desc.sqltype == SQL_DEC16 || desc.sqltype == SQL_DEC34;
    };
    // Column SQL expression for a complex-filter predicate: the casted form
    // for DECFLOAT, the plain quoted identifier otherwise.
    auto col_expr = [&](idx_t src_col_idx) -> std::string {
        if (is_decfloat(src_col_idx)) {
            return "CAST(" + QuoteIdent(bind.column_names[src_col_idx]) +
                   " AS VARCHAR(64))";
        }
        return QuoteIdent(bind.column_names[src_col_idx]);
    };

    auto it = filters.begin();
    while (it != filters.end()) {
        // --- LIKE 'prefix%' ----------------------------------------------
        {
            idx_t projected_col = 0;
            std::string pattern;
            if (TryExtractLikePrefix(**it, projected_col, pattern)) {
                idx_t src;
                if (!resolve_src_col(projected_col, src)) { ++it; continue; }
                if (is_none_text_gated(src)) {
                    // NONE-text gating: the LIKE can't be pushed because a
                    // UTF-8 pattern won't match raw CP1252/Latin-1 bytes
                    // server-side. DuckDB still applies it post-transcode;
                    // record the reason so the pushdown report isn't blind
                    // to the gate. See Phase 4 #3.
                    bind.gated_complex_reasons.push_back("NONE_CHARSET");
                    ++it;
                    continue;
                }
                FirebirdBindData::ExtraPredicate ep;
                ep.sql = col_expr(src) +
                         " LIKE " + SqlLiteral(pattern) +
                         " ESCAPE '\\'";
                bind.extra_predicates.push_back(std::move(ep));
                it = filters.erase(it);
                continue;
            }
        }

        // --- col NOT IN (?, ?, ...) --------------------------------------
        {
            idx_t projected_col = 0;
            duckdb::vector<Value> values;
            if (TryExtractNotIn(**it, projected_col, values)) {
                idx_t src;
                if (!resolve_src_col(projected_col, src)) { ++it; continue; }
                if (is_none_text_gated(src)) {
                    bind.gated_complex_reasons.push_back("NONE_CHARSET");
                    ++it;
                    continue;
                }
                FirebirdBindData::ExtraPredicate ep;
                std::string sql = col_expr(src) + " NOT IN (";
                for (size_t i = 0; i < values.size(); ++i) {
                    if (i) sql += ", ";
                    sql += "?";
                }
                sql += ")";
                ep.sql    = std::move(sql);
                ep.params = std::move(values);
                bind.extra_predicates.push_back(std::move(ep));
                it = filters.erase(it);
                continue;
            }
        }

        // --- NOT bool_col -------------------------------------------------
        {
            idx_t projected_col = 0;
            if (TryExtractNotBoolColumn(**it, projected_col)) {
                idx_t src;
                if (!resolve_src_col(projected_col, src)) { ++it; continue; }
                // Defensive: a DECFLOAT column is VARCHAR to DuckDB and never
                // boolean, so this matcher should not fire for it — but never
                // emit `NOT <decfloat>` server-side; leave it to DuckDB.
                if (is_decfloat(src)) { ++it; continue; }
                FirebirdBindData::ExtraPredicate ep;
                ep.sql = "NOT " + QuoteIdent(bind.column_names[src]);
                bind.extra_predicates.push_back(std::move(ep));
                it = filters.erase(it);
                continue;
            }
        }

        ++it;
    }
}

// ---------------------------------------------------------------------------
//  Registration.
// ---------------------------------------------------------------------------
TableFunction GetFirebirdScanFunction() {
    TableFunction fn("firebird_scan",
                     {LogicalType::VARCHAR, LogicalType::VARCHAR},
                     FirebirdScanFunction,
                     FirebirdScanBind,
                     FirebirdScanInitGlobal,
                     FirebirdScanInitLocal);
    fn.named_parameters["user"]          = LogicalType::VARCHAR;
    fn.named_parameters["password"]      = LogicalType::VARCHAR;
    fn.named_parameters["charset"]       = LogicalType::VARCHAR;
    fn.named_parameters["role"]          = LogicalType::VARCHAR;
    fn.named_parameters["dialect"]       = LogicalType::INTEGER;
    fn.named_parameters["partitions"]    = LogicalType::BIGINT;
    fn.named_parameters["row_limit"]     = LogicalType::BIGINT;
    fn.named_parameters["row_offset"]    = LogicalType::BIGINT;
    fn.named_parameters["none_encoding"] = LogicalType::VARCHAR;

    // Pushdown advertisements — DuckDB's planner now knows it can hand us
    // narrowed column lists and TableFilterSet entries.
    fn.projection_pushdown = true;
    fn.filter_pushdown     = true;
    // filter_prune lets DuckDB tell us, via input.projection_ids, that
    // some columns in input.column_ids are filter-only (not needed in
    // the output). The scan loop fetches into a scratch chunk shaped
    // by column_ids and then ReferenceColumns by projection_ids into
    // the caller's output. Necessary for supports_pushdown_type to
    // work without DataChunk-size mismatches.
    fn.filter_prune        = true;
    // LIKE-prefix pushdown (BoundFunctionExpression that the TableFilter
    // path can't express). See FirebirdScanPushdownComplexFilter.
    fn.pushdown_complex_filter = FirebirdScanPushdownComplexFilter;
    // Per-column opt-out from filter pushdown. Currently used to keep
    // Firebird CHARACTER SET NONE text columns out of the pushed
    // TableFilterSet when the caller is transcoding client-side: the
    // SqlLiteral we'd emit is UTF-8 and would not match the raw bytes
    // server-side. See FirebirdScanSupportsPushdownType.
    fn.supports_pushdown_type = FirebirdScanSupportsPushdownType;

    return fn;
}

} // namespace duckdb
