#include "firebird_scanner.hpp"

#include <cctype>
#include <memory>
#include <sstream>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_client.hpp"
#include "firebird_query.hpp"
#include "firebird_types.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
//  Bind data — everything we resolved at planning time.
// ---------------------------------------------------------------------------
struct FirebirdBindData : public TableFunctionData {
    FirebirdConnectionInfo conn_info;
    std::string table_name;

    // Full table schema (used to translate projection / filter pushdown back
    // to the canonical column ordering).
    std::vector<std::string> column_names;
    std::vector<LogicalType> column_types;
};

// ---------------------------------------------------------------------------
//  Global state — single-threaded for the prototype.
// ---------------------------------------------------------------------------
struct FirebirdGlobalState : public GlobalTableFunctionState {
    // We emit one scan worker; partitioning by PK range is a follow-up.
    idx_t MaxThreads() const override { return 1; }
};

// ---------------------------------------------------------------------------
//  Local state — owns the cursor + connection actually doing the scan.
// ---------------------------------------------------------------------------
struct FirebirdLocalState : public LocalTableFunctionState {
    std::unique_ptr<FirebirdConnection> conn;
    std::unique_ptr<FirebirdStatement>  cursor;
    bool exhausted = false;
};

// ---------------------------------------------------------------------------
//  Schema discovery.
// ---------------------------------------------------------------------------
//
// Query RDB$RELATION_FIELDS to recover (name, sqltype, subtype, scale, length)
// for every column. We use the same JOIN-with-RDB$FIELDS shape as the
// peregrine extractor: one round-trip per table.

static void LoadTableSchema(FirebirdConnection &conn,
                            const std::string &table_name,
                            std::vector<std::string> &out_names,
                            std::vector<LogicalType> &out_types) {
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
        " WHERE rf.RDB$RELATION_NAME = '" + upper + "' "
        " ORDER BY rf.RDB$FIELD_POSITION";

    auto cursor = conn.OpenCursor(sql);
    while (cursor->Fetch()) {
        FirebirdColumnDesc desc;
        desc.name        = cursor->GetText(0);
        desc.sqltype     = cursor->GetShort(1);
        desc.sqlsubtype  = cursor->GetShort(2);
        desc.sqlscale    = cursor->GetShort(3);
        desc.sqllen      = cursor->GetShort(4);

        // RDB$FIELD_TYPE uses Firebird's *internal* type codes which differ
        // from the SQL-level XSQLDA constants. Translate.
        switch (desc.sqltype) {
        case 7:   desc.sqltype = SQL_SHORT;     break;  // SMALLINT
        case 8:   desc.sqltype = SQL_LONG;      break;  // INTEGER
        case 9:   desc.sqltype = SQL_QUAD;      break;  // QUAD
        case 10:  desc.sqltype = SQL_FLOAT;     break;
        case 11:  desc.sqltype = SQL_D_FLOAT;   break;
        case 12:  desc.sqltype = SQL_TYPE_DATE; break;  // (Dialect 3)
        case 13:  desc.sqltype = SQL_TYPE_TIME; break;
        case 14:  desc.sqltype = SQL_TEXT;      break;  // CHAR
        case 16:  desc.sqltype = SQL_INT64;     break;
        case 23:  desc.sqltype = SQL_BOOLEAN;   break;
        case 27:  desc.sqltype = SQL_DOUBLE;    break;
        case 35:  desc.sqltype = SQL_TIMESTAMP; break;
        case 37:  desc.sqltype = SQL_VARYING;   break;
        case 261: desc.sqltype = SQL_BLOB;      break;
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
        if      (key == "user")     bind->conn_info.user     = val.ToString();
        else if (key == "password") bind->conn_info.password = val.ToString();
        else if (key == "charset")  bind->conn_info.charset  = val.ToString();
        else if (key == "role")     bind->conn_info.role     = val.ToString();
        else if (key == "dialect")  bind->conn_info.dialect  = static_cast<int>(val.GetValue<int32_t>());
    }

    FirebirdConnection conn(bind->conn_info);
    LoadTableSchema(conn, bind->table_name, bind->column_names, bind->column_types);

    return_types = bind->column_types;
    names        = bind->column_names;
    return std::move(bind);
}

// ---------------------------------------------------------------------------
//  Init.
// ---------------------------------------------------------------------------
static unique_ptr<GlobalTableFunctionState> FirebirdScanInitGlobal(
    ClientContext & /*context*/, TableFunctionInitInput & /*input*/) {
    return make_uniq<FirebirdGlobalState>();
}

static unique_ptr<LocalTableFunctionState> FirebirdScanInitLocal(
    ExecutionContext & /*ctx*/,
    TableFunctionInitInput &input,
    GlobalTableFunctionState * /*gstate*/) {

    auto &bind = input.bind_data->Cast<FirebirdBindData>();
    auto local = make_uniq<FirebirdLocalState>();

    auto query = FirebirdQueryBuilder::Build(
        bind.table_name,
        bind.column_names,
        bind.column_types,
        input.column_ids,
        input.filters,
        /*limit=*/optional_idx());

    local->conn   = make_uniq<FirebirdConnection>(bind.conn_info);
    local->cursor = local->conn->OpenCursor(query.sql);
    return std::move(local);
}

// ---------------------------------------------------------------------------
//  Scan.
// ---------------------------------------------------------------------------
static void FirebirdScanFunction(ClientContext & /*context*/,
                                 TableFunctionInput &data,
                                 DataChunk &output) {
    auto &local = data.local_state->Cast<FirebirdLocalState>();
    if (local.exhausted) {
        output.SetCardinality(0);
        return;
    }

    const idx_t target = STANDARD_VECTOR_SIZE;
    const idx_t n_out_cols = output.ColumnCount();

    idx_t row = 0;
    while (row < target) {
        if (!local.cursor->Fetch()) {
            local.exhausted = true;
            break;
        }
        for (idx_t c = 0; c < n_out_cols; ++c) {
            FirebirdAppendValue(*local.cursor, c, output.data[c], row);
        }
        ++row;
    }
    output.SetCardinality(row);
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
    fn.named_parameters["user"]     = LogicalType::VARCHAR;
    fn.named_parameters["password"] = LogicalType::VARCHAR;
    fn.named_parameters["charset"]  = LogicalType::VARCHAR;
    fn.named_parameters["role"]     = LogicalType::VARCHAR;
    fn.named_parameters["dialect"]  = LogicalType::INTEGER;

    // Pushdown advertisements — DuckDB's planner now knows it can hand us
    // narrowed column lists and TableFilterSet entries.
    fn.projection_pushdown = true;
    fn.filter_pushdown     = true;
    fn.filter_prune        = false;  // not yet — we still need to recheck residuals

    return fn;
}

} // namespace duckdb
