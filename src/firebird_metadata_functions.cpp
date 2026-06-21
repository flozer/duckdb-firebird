#include "firebird_metadata_functions.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

// AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias, FirebirdMetadataLease
#include "firebird_dbt_sources.hpp"
// FirebirdStatement (cursor type returned by OpenCursor), FirebirdConnection
#include "firebird_client.hpp"

#include <functional>
#include <string>
#include <vector>

namespace duckdb {

// Value helpers: RDB$ NULL -> SQL NULL, never a silent default.
// FirebirdConnection::OpenCursor returns unique_ptr<FirebirdStatement>;
// helpers take it by reference.
[[maybe_unused]] static Value TextOrNull(FirebirdStatement &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::VARCHAR) : Value(c.GetText(i));
}
// NOTE: uses GetLong (4-byte). Use ONLY for Firebird INTEGER columns.
// For SMALLINT columns use ShortOrNull (GetLong over-reads a 2-byte field).
[[maybe_unused]] static Value IntOrNull(FirebirdStatement &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::INTEGER)
                       : Value::INTEGER(c.GetLong(i));
}
// For Firebird SMALLINT (2-byte) columns — RDB$FIELD_POSITION, RDB$*_TYPE,
// RDB$FIELD_SUB_TYPE, RDB$FIELD_SCALE, RDB$SYSTEM_FLAG, RDB$NULL_FLAG, etc.
// GetLong() would over-read a 2-byte field; use GetShort.
[[maybe_unused]] static Value ShortOrNull(FirebirdStatement &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::INTEGER)
                       : Value::INTEGER(static_cast<int32_t>(c.GetShort(i)));
}
[[maybe_unused]] static Value BoolFromFlag(FirebirdStatement &c, idx_t i) {
    return Value::BOOLEAN(!c.IsNull(i) && c.GetShort(i) != 0);
}

// One descriptor drives the whole function.
struct MetadataFn {
    std::string                 name;
    duckdb::vector<std::string> col_names;
    duckdb::vector<LogicalType> col_types;
    std::string                 sql;
    // Map the current cursor row to one Value per output column.
    std::function<duckdb::vector<Value>(FirebirdStatement &)> map_row;
};

// Carries an owned copy of MetadataFn through the DuckDB function_info slot so
// that MetaBindDispatch (a non-capturing function pointer) can read it without
// depending on the lifetime of the caller's descriptor.
struct MetadataFnInfo : public TableFunctionInfo {
    MetadataFn desc;
    explicit MetadataFnInfo(const MetadataFn &d) : desc(d) {}
};

struct MetaBindData : public TableFunctionData {
    std::string catalog_name;
    const MetadataFn *desc = nullptr;
};
struct MetaGlobalState : public GlobalTableFunctionState {
    duckdb::vector<duckdb::vector<Value>> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

// Non-capturing bind callback — reads the descriptor from function_info.
static unique_ptr<FunctionData>
MetaBindDispatch(ClientContext &context, TableFunctionBindInput &input,
                 vector<LogicalType> &return_types, vector<string> &names) {
    D_ASSERT(input.info);
    auto &info = input.info->Cast<MetadataFnInfo>();
    const MetadataFn &desc = info.desc;

    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException("%s(catalog_name VARCHAR): catalog_name is "
                              "required (the alias from ATTACH ... (TYPE firebird)).",
                              desc.name);
    }
    auto bind = make_uniq<MetaBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    bind->desc = &desc;
    ValidateFirebirdAttachAlias(context, bind->catalog_name);
    names = desc.col_names;
    return_types = desc.col_types;
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
MetaInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<MetaBindData>();
    auto g = make_uniq<MetaGlobalState>();
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    auto cur = lease.conn->OpenCursor(bind.desc->sql);
    while (cur->Fetch()) {
        g->rows.push_back(bind.desc->map_row(*cur));
    }
    return std::move(g);
}

static void MetaFunction(ClientContext &, TableFunctionInput &input,
                         DataChunk &output) {
    auto &g = input.global_state->Cast<MetaGlobalState>();
    idx_t row = 0;
    const idx_t target = STANDARD_VECTOR_SIZE;
    while (row < target && g.cursor < g.rows.size()) {
        auto &vals = g.rows[g.cursor++];
        for (idx_t c = 0; c < vals.size(); ++c) {
            output.data[c].SetValue(row, vals[c]);
        }
        ++row;
    }
    output.SetCardinality(row);
}

// Bind a concrete descriptor (stored in static storage by each Get*Function()
// body in later tasks) into a TableFunction using the function_info slot so
// MetaBindDispatch can retrieve it without a capturing lambda.
[[maybe_unused]] static TableFunction MakeMetadataFunction(const MetadataFn &desc) {
    TableFunction fn(desc.name, {LogicalType::VARCHAR}, MetaFunction,
                     MetaBindDispatch,
                     MetaInitGlobal);
    fn.function_info = make_shared_ptr<MetadataFnInfo>(desc);
    return fn;
}

TableFunction GetFirebirdIndexesFunction() {
    static const MetadataFn desc{
        "firebird_indexes",
        {"table_schema", "table_name", "index_name", "is_unique", "is_active",
         "segment_position", "column_name", "expression_source"},
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
         LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::INTEGER,
         LogicalType::VARCHAR, LogicalType::VARCHAR},
        "SELECT TRIM(i.RDB$RELATION_NAME), TRIM(i.RDB$INDEX_NAME), "
        "       i.RDB$UNIQUE_FLAG, i.RDB$INDEX_INACTIVE, "
        "       seg.RDB$FIELD_POSITION, TRIM(seg.RDB$FIELD_NAME), "
        "       CAST(i.RDB$EXPRESSION_SOURCE AS VARCHAR(4000)) "
        "  FROM RDB$INDICES i "
        "  LEFT JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = i.RDB$INDEX_NAME "
        " WHERE i.RDB$SYSTEM_FLAG = 0 "
        " ORDER BY i.RDB$RELATION_NAME, i.RDB$INDEX_NAME, seg.RDB$FIELD_POSITION",
        [](FirebirdStatement &c) -> duckdb::vector<Value> {
            // RDB$INDEX_INACTIVE: SMALLINT; NULL or 0 = active, 1 = inactive
            Value active = Value::BOOLEAN(c.IsNull(3) || c.GetShort(3) == 0);
            // RDB$UNIQUE_FLAG: SMALLINT — BoolFromFlag uses GetShort
            // RDB$FIELD_POSITION: SMALLINT, 0-based — ShortOrNull
            return {Value("main"), TextOrNull(c, 0), TextOrNull(c, 1),
                    BoolFromFlag(c, 2), active, ShortOrNull(c, 4),
                    TextOrNull(c, 5), TextOrNull(c, 6)};
        }};
    return MakeMetadataFunction(desc);
}

TableFunction GetFirebirdForeignKeysFunction() {
    static const MetadataFn desc{
        "firebird_foreign_keys",
        {"fk_schema", "fk_table", "fk_constraint", "ordinal_position",
         "fk_column", "pk_table", "pk_constraint", "update_rule", "delete_rule"},
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR,
         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
        "       fkseg.RDB$FIELD_POSITION, TRIM(fkseg.RDB$FIELD_NAME), "
        "       TRIM(uq.RDB$RELATION_NAME), TRIM(ref.RDB$CONST_NAME_UQ), "
        "       TRIM(ref.RDB$UPDATE_RULE), TRIM(ref.RDB$DELETE_RULE) "
        "  FROM RDB$RELATION_CONSTRAINTS rc "
        "  JOIN RDB$REF_CONSTRAINTS ref ON ref.RDB$CONSTRAINT_NAME = rc.RDB$CONSTRAINT_NAME "
        "  JOIN RDB$RELATION_CONSTRAINTS uq ON uq.RDB$CONSTRAINT_NAME = ref.RDB$CONST_NAME_UQ "
        "  JOIN RDB$INDEX_SEGMENTS fkseg ON fkseg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        " WHERE rc.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
        " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, fkseg.RDB$FIELD_POSITION",
        [](FirebirdStatement &c) -> duckdb::vector<Value> {
            // RDB$FIELD_POSITION is SMALLINT — use ShortOrNull, not IntOrNull.
            return {Value("main"), TextOrNull(c, 0), TextOrNull(c, 1),
                    ShortOrNull(c, 2), TextOrNull(c, 3), TextOrNull(c, 4),
                    TextOrNull(c, 5), TextOrNull(c, 6), TextOrNull(c, 7)};
        }};
    return MakeMetadataFunction(desc);
}

// ── firebird_generators ────────────────────────────────────────────────────
// Bespoke (not the shared scaffold): current_value requires a per-generator
// GEN_ID("name", 0) query with safe quoting and per-row isolation so that
// a privilege error on one generator NULLs only that row, not the whole call.

// Firebird identifier quoting: wrap in double quotes, double internal quotes.
static std::string QuoteFbIdent(const std::string &id) {
    std::string out = "\"";
    for (char ch : id) {
        if (ch == '"') out += "\"\"";
        else            out += ch;
    }
    out += "\"";
    return out;
}

struct GenRow { std::string name; Value initial; Value current; };

struct GenBindData : public TableFunctionData {
    std::string catalog_name;
};
struct GenGlobalState : public GlobalTableFunctionState {
    std::vector<GenRow> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
GenBind(ClientContext &context, TableFunctionBindInput &input,
        vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull())
        throw BinderException("firebird_generators(catalog_name VARCHAR): catalog_name is required.");
    auto bind = make_uniq<GenBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    ValidateFirebirdAttachAlias(context, bind->catalog_name);
    names        = {"generator_name", "initial_value", "current_value"};
    return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
GenInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<GenBindData>();
    auto g = make_uniq<GenGlobalState>();
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    FirebirdConnection &conn = *lease.conn;

    // Phase 1: list generators + initial_value (BIGINT — use GetInt64).
    auto cur = conn.OpenCursor(
        "SELECT TRIM(RDB$GENERATOR_NAME), RDB$INITIAL_VALUE "
        "  FROM RDB$GENERATORS WHERE RDB$SYSTEM_FLAG = 0 "
        " ORDER BY RDB$GENERATOR_NAME");
    while (cur->Fetch()) {
        GenRow r;
        r.name    = cur->GetText(0);
        r.initial = cur->IsNull(1)
                        ? Value(LogicalType::BIGINT)
                        : Value::BIGINT(cur->GetInt64(1));
        r.current = Value(LogicalType::BIGINT); // default NULL
        g->rows.push_back(std::move(r));
    }

    // Phase 2: per-generator current_value via GEN_ID(<quoted_name>, 0).
    // Step 0 is side-effect-free (does not advance the sequence).
    // Each generator is isolated: a privilege error or failure on one
    // generator sets current_value = NULL for that row only.
    for (auto &r : g->rows) {
        try {
            auto c2 = conn.OpenCursor(
                "SELECT GEN_ID(" + QuoteFbIdent(r.name) + ", 0) FROM RDB$DATABASE");
            if (c2->Fetch() && !c2->IsNull(0)) {
                r.current = Value::BIGINT(c2->GetInt64(0));
            }
        } catch (std::exception &) {
            /* leave r.current = NULL (BIGINT null) */
        }
    }
    return std::move(g);
}

static void GenFunction(ClientContext &, TableFunctionInput &input,
                        DataChunk &output) {
    auto &g = input.global_state->Cast<GenGlobalState>();
    idx_t row = 0;
    const idx_t target = STANDARD_VECTOR_SIZE;
    while (row < target && g.cursor < g.rows.size()) {
        auto &r = g.rows[g.cursor++];
        output.data[0].SetValue(row, Value(r.name));
        output.data[1].SetValue(row, r.initial);
        output.data[2].SetValue(row, r.current);
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdGeneratorsFunction() {
    return TableFunction("firebird_generators", {LogicalType::VARCHAR},
                         GenFunction, GenBind, GenInitGlobal);
}

// ── firebird_domains ──────────────────────────────────────────────────────────
// Maps RDB$FIELD_TYPE (SMALLINT) + sub_type/length/scale/precision to a
// faithful Firebird type string.  Unknown field_type → "TYPE_<n>", never NULL.
// scale: Firebird stores it negative (e.g. -2 for scale=2); expose absolute.
static std::string FormatFbType(int ftype, int sub_type, int length,
                                int scale, int precision) {
    int s = (scale < 0) ? -scale : scale;
    switch (ftype) {
    case 7:  // SMALLINT / NUMERIC(<=4,x) / DECIMAL(<=4,x)
    case 8:  // INTEGER  / NUMERIC / DECIMAL
    case 16: // BIGINT   / NUMERIC / DECIMAL (also INT128 in FB4+)
        if (sub_type == 1) return "NUMERIC(" + std::to_string(precision) + "," + std::to_string(s) + ")";
        if (sub_type == 2) return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(s) + ")";
        if (ftype == 7)  return "SMALLINT";
        if (ftype == 8)  return "INTEGER";
        return "BIGINT";
    case 10:  return "FLOAT";
    case 27:  return "DOUBLE PRECISION";
    case 12:  return "DATE";
    case 13:  return "TIME";
    case 35:  return "TIMESTAMP";
    case 14:  return "CHAR(" + std::to_string(length) + ")";
    case 37:  return "VARCHAR(" + std::to_string(length) + ")";
    case 261: return (sub_type == 1) ? "BLOB SUB_TYPE TEXT" : "BLOB";
    default:  return "TYPE_" + std::to_string(ftype);
    }
}

TableFunction GetFirebirdDomainsFunction() {
    static const MetadataFn desc{
        "firebird_domains",
        {"domain_name", "base_type", "length", "scale", "is_nullable",
         "charset_name", "check_source", "default_source"},
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
         LogicalType::INTEGER, LogicalType::BOOLEAN, LogicalType::VARCHAR,
         LogicalType::VARCHAR, LogicalType::VARCHAR},
        // col indices:  0=domain_name  1=ftype  2=sub_type  3=length
        //               4=scale        5=prec    6=null_flag 7=charset
        //               8=check_src    9=default_src
        "SELECT TRIM(f.RDB$FIELD_NAME), f.RDB$FIELD_TYPE, "
        "       COALESCE(f.RDB$FIELD_SUB_TYPE,0), f.RDB$FIELD_LENGTH, "
        "       COALESCE(f.RDB$FIELD_SCALE,0), COALESCE(f.RDB$FIELD_PRECISION,0), "
        "       f.RDB$NULL_FLAG, TRIM(cs.RDB$CHARACTER_SET_NAME), "
        "       CAST(f.RDB$VALIDATION_SOURCE AS VARCHAR(4000)), "
        "       CAST(f.RDB$DEFAULT_SOURCE AS VARCHAR(4000)) "
        "  FROM RDB$FIELDS f "
        "  LEFT JOIN RDB$CHARACTER_SETS cs ON cs.RDB$CHARACTER_SET_ID = f.RDB$CHARACTER_SET_ID "
        " WHERE COALESCE(f.RDB$SYSTEM_FLAG,0) = 0 "
        "   AND f.RDB$FIELD_NAME NOT STARTING WITH 'RDB$' "
        "   AND f.RDB$FIELD_NAME NOT STARTING WITH 'MON$' "
        " ORDER BY f.RDB$FIELD_NAME",
        [](FirebirdStatement &c) -> duckdb::vector<Value> {
            // All RDB$FIELD_* type/sub_type/scale/precision/null_flag are SMALLINT.
            int ftype    = c.IsNull(1) ? 0  : static_cast<int>(c.GetShort(1));
            int sub_type = c.IsNull(2) ? 0  : static_cast<int>(c.GetShort(2));
            int length   = c.IsNull(3) ? 0  : static_cast<int>(c.GetShort(3));
            int scale    = c.IsNull(4) ? 0  : static_cast<int>(c.GetShort(4));
            int prec     = c.IsNull(5) ? 0  : static_cast<int>(c.GetShort(5));
            // scale is stored negative in Firebird; expose absolute value.
            int abs_scale = (scale < 0) ? -scale : scale;
            std::string bt = FormatFbType(ftype, sub_type, length, scale, prec);
            // is_nullable: NULL flag absent or 0 means nullable.
            Value is_nullable = Value::BOOLEAN(c.IsNull(6) || c.GetShort(6) == 0);
            return {
                TextOrNull(c, 0),                                   // domain_name
                Value(bt),                                          // base_type
                ShortOrNull(c, 3),                                  // length  (SMALLINT)
                c.IsNull(4) ? Value(LogicalType::INTEGER)
                            : Value::INTEGER(abs_scale),            // scale (absolute)
                is_nullable,                                        // is_nullable
                TextOrNull(c, 7),                                   // charset_name
                TextOrNull(c, 8),                                   // check_source
                TextOrNull(c, 9),                                   // default_source
            };
        }};
    return MakeMetadataFunction(desc);
}

} // namespace duckdb
