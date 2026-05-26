#include "firebird_scanner.hpp"

#include <algorithm>
#include <cctype>
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
#include "duckdb/planner/operator/logical_get.hpp"

#include "firebird_client.hpp"
#include "firebird_query.hpp"
#include "firebird_types.hpp"

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
    bool exhausted = false;

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

void LoadTableSchema(FirebirdConnection &conn,
                     const std::string &table_name,
                     duckdb::vector<std::string> &out_names,
                     duckdb::vector<LogicalType> &out_types) {
    // Firebird stores identifiers upper-cased unless quoted at creation; we
    // upper-case here so callers can pass either form.
    std::string upper = table_name;
    for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const std::string sql =
        "SELECT TRIM(rf.RDB$FIELD_NAME), "
        "       f.RDB$FIELD_TYPE, "
        "       COALESCE(f.RDB$FIELD_SUB_TYPE, 0), "
        "       COALESCE(f.RDB$FIELD_SCALE, 0), "
        "       COALESCE(f.RDB$FIELD_LENGTH, 0) "
        "  FROM RDB$RELATION_FIELDS rf "
        "  JOIN RDB$FIELDS f ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE "
        " WHERE rf.RDB$RELATION_NAME = " + SqlLiteral(upper) + " "
        " ORDER BY rf.RDB$FIELD_POSITION";

    auto cursor = conn.OpenCursor(sql);
    while (cursor->Fetch()) {
        FirebirdColumnDesc desc;
        desc.name        = cursor->GetText(0);
        desc.sqltype     = cursor->GetShort(1);
        desc.sqlsubtype  = cursor->GetShort(2);
        desc.sqlscale    = cursor->GetShort(3);
        desc.sqllen      = cursor->GetShort(4);

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

        out_names.push_back(desc.name);
        out_types.push_back(FirebirdToDuckDBType(desc));
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
        return pk;
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
    }

    FirebirdConnection conn(bind->conn_info);
    LoadTableSchema(conn, bind->table_name, bind->column_names, bind->column_types);

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
        // ATTACH path — reuse a warm connection if the pool has one.
        local->pool = bind.pool;
        local->conn = bind.pool->Acquire();
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

static bool OpenNextPartitionCursor(ClientContext & /*ctx*/,
                                    TableFunctionInput &data,
                                    FirebirdLocalState &local) {
    auto &bind   = data.bind_data->Cast<FirebirdBindData>();
    auto &gstate = data.global_state->Cast<FirebirdGlobalState>();

    PartitionSpec spec;
    if (!gstate.NextPartition(spec)) return false;

    // Concatenate the partition range predicate with any extra predicates
    // lifted from BoundFunctionExpression pushdown (currently just LIKE
    // prefix). All fragments are already SQL-safe (literals quoted via
    // SqlLiteral, identifiers via QuoteIdent).
    std::string combined = spec.where_clause;
    for (auto &ep : bind.extra_predicates) {
        if (!combined.empty()) combined += " AND ";
        combined += "(" + ep + ")";
    }

    auto query = FirebirdQueryBuilder::Build(
        bind.table_name,
        bind.column_names,
        bind.column_types,
        gstate.column_ids,
        gstate.filters,
        bind.limit_override,
        combined);

    local.cursor = local.conn->OpenCursor(query.sql);
    return true;
}

static void FirebirdScanFunction(ClientContext &ctx,
                                 TableFunctionInput &data,
                                 DataChunk &output) {
    auto &local = data.local_state->Cast<FirebirdLocalState>();

    const idx_t target = STANDARD_VECTOR_SIZE;
    const idx_t n_out_cols = output.ColumnCount();
    idx_t row = 0;

    while (row < target) {
        if (!local.cursor) {
            if (!OpenNextPartitionCursor(ctx, data, local)) break;
        }
        if (!local.cursor->Fetch()) {
            // Current partition done — drop the cursor and try to fetch
            // the next one on the next iteration.
            local.cursor.reset();
            continue;
        }
        for (idx_t c = 0; c < n_out_cols; ++c) {
            FirebirdAppendValue(*local.cursor, c, output.data[c], row);
        }
        ++row;
    }
    output.SetCardinality(row);
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

    auto it = filters.begin();
    while (it != filters.end()) {
        idx_t projected_col = 0;
        std::string pattern;
        if (!TryExtractLikePrefix(**it, projected_col, pattern)) {
            ++it;
            continue;
        }
        if (projected_col >= col_ids.size()) { ++it; continue; }
        auto src_col_idx = col_ids[projected_col].GetPrimaryIndex();
        if (src_col_idx == COLUMN_IDENTIFIER_ROW_ID ||
            src_col_idx >= bind.column_names.size()) {
            ++it;
            continue;
        }
        std::string frag = QuoteIdent(bind.column_names[src_col_idx]) +
                           " LIKE " + SqlLiteral(pattern) +
                           " ESCAPE '\\'";
        bind.extra_predicates.push_back(std::move(frag));
        // Tell DuckDB we have the filter covered — it won't re-evaluate it.
        it = filters.erase(it);
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
    fn.named_parameters["user"]       = LogicalType::VARCHAR;
    fn.named_parameters["password"]   = LogicalType::VARCHAR;
    fn.named_parameters["charset"]    = LogicalType::VARCHAR;
    fn.named_parameters["role"]       = LogicalType::VARCHAR;
    fn.named_parameters["dialect"]    = LogicalType::INTEGER;
    fn.named_parameters["partitions"] = LogicalType::BIGINT;
    fn.named_parameters["row_limit"]  = LogicalType::BIGINT;

    // Pushdown advertisements — DuckDB's planner now knows it can hand us
    // narrowed column lists and TableFilterSet entries.
    fn.projection_pushdown = true;
    fn.filter_pushdown     = true;
    fn.filter_prune        = false;  // not yet — we still need to recheck residuals
    // LIKE-prefix pushdown (BoundFunctionExpression that the TableFilter
    // path can't express). See FirebirdScanPushdownComplexFilter.
    fn.pushdown_complex_filter = FirebirdScanPushdownComplexFilter;

    return fn;
}

} // namespace duckdb
