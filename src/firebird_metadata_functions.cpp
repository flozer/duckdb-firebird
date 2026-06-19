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
[[maybe_unused]] static Value IntOrNull(FirebirdStatement &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::INTEGER)
                       : Value::INTEGER(c.GetLong(i));
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
            // RDB$FIELD_POSITION is SMALLINT — use GetShort, not GetLong.
            Value ordinal = c.IsNull(2) ? Value(LogicalType::INTEGER)
                                        : Value::INTEGER(c.GetShort(2));
            return {Value("main"), TextOrNull(c, 0), TextOrNull(c, 1),
                    ordinal, TextOrNull(c, 3), TextOrNull(c, 4),
                    TextOrNull(c, 5), TextOrNull(c, 6), TextOrNull(c, 7)};
        }};
    return MakeMetadataFunction(desc);
}

} // namespace duckdb
