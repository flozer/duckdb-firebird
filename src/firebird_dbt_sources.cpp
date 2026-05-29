#include "firebird_dbt_sources.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_scanner.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_set>

namespace duckdb {

// ---------------------------------------------------------------------------
//  Catalog-alias validation
// ---------------------------------------------------------------------------
//
// We deliberately do NOT throw the bare DuckDB-internal CatalogException
// path here - that surfaces as "Catalog with name X does not exist" with
// no hint that the function expects a Firebird ATTACH. Wrapping the
// lookup in BinderException with the function name gives the user a
// direct, actionable message.

void ValidateFirebirdAttachAlias(ClientContext &context,
                                 const std::string &catalog_name) {
    auto catalog_ptr = Catalog::GetCatalogEntry(context, catalog_name);
    if (!catalog_ptr) {
        throw BinderException(
            "firebird_generate_dbt_sources: no attached catalog named '%s'. "
            "ATTACH first: ATTACH '<path-or-uri>' AS %s (TYPE firebird);",
            catalog_name, catalog_name);
    }
    if (catalog_ptr->GetCatalogType() != "firebird") {
        throw BinderException(
            "firebird_generate_dbt_sources: catalog '%s' is not a Firebird "
            "ATTACH (GetCatalogType() = '%s'). Use the alias of an ATTACH "
            "created with (TYPE firebird).",
            catalog_name, catalog_ptr->GetCatalogType());
    }
}

// ---------------------------------------------------------------------------
//  YAML emission helpers
// ---------------------------------------------------------------------------
//
// We only emit double-quoted scalars for any user-controlled or
// database-controlled string (alias, schema, table name, column name,
// data_type, description). Identifiers in production Firebird OLTP
// schemas are alphanumeric + underscore and would survive unquoted, but
// quoting unconditionally is cheaper than auditing every dialect quirk
// and keeps the output reader-safe for any future extension to BLOB
// descriptions or non-ASCII table names.

static std::string YamlQuote(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

// ---------------------------------------------------------------------------
//  Metadata model + query
// ---------------------------------------------------------------------------

struct ColumnMeta {
    std::string name;
    LogicalType type;
    bool        is_pk = false;
};

struct TableMeta {
    std::string             name;
    std::vector<ColumnMeta> columns;
};

// One round-trip listing the user relations we care about. The same
// shape firebird_tables() uses: persistent tables (type 0/NULL) + views
// (type 1). System tables filtered via SYSTEM_FLAG. Plain table_name +
// pk_column_name (NULL when no single-column PK detected). Columns are
// picked up per-table via LoadTableSchema below, which already handles
// the RDB$FIELD_TYPE -> SQL_* mapping that FirebirdToDuckDBType expects.
struct TableListRow {
    std::string table_name;
    std::string pk_column; // empty when no single-column PK
};

static std::vector<TableListRow> ListUserTables(FirebirdConnection &conn) {
    // PK detection emits the column name ONLY when the table's PRIMARY
    // KEY index has exactly one segment. Composite PKs return NULL so
    // BuildSourcesYaml will not emit a misleading `tests: [not_null,
    // unique]` block against a single member of the composite key.
    // The aggregate-with-CASE form folds the count guard and the field
    // name into one scalar subquery; MIN() is safe because a single-
    // segment PK has exactly one matching row.
    const std::string sql =
        "SELECT TRIM(r.RDB$RELATION_NAME) AS NAME, "
        "       (SELECT CASE WHEN COUNT(*) = 1 "
        "                    THEN MIN(TRIM(seg.RDB$FIELD_NAME)) "
        "               END "
        "          FROM RDB$INDICES ri "
        "          JOIN RDB$RELATION_CONSTRAINTS rc ON ri.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        "          JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = ri.RDB$INDEX_NAME "
        "         WHERE ri.RDB$RELATION_NAME = r.RDB$RELATION_NAME "
        "           AND rc.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY') AS PK "
        "  FROM RDB$RELATIONS r "
        " WHERE r.RDB$SYSTEM_FLAG = 0 "
        "   AND (r.RDB$RELATION_TYPE IS NULL OR r.RDB$RELATION_TYPE = 0 OR r.RDB$RELATION_TYPE = 1) "
        " ORDER BY r.RDB$RELATION_NAME";

    std::vector<TableListRow> out;
    auto cursor = conn.OpenCursor(sql);
    while (cursor->Fetch()) {
        TableListRow row;
        row.table_name = cursor->GetText(0);
        if (!cursor->IsNull(1)) {
            row.pk_column = cursor->GetText(1);
        }
        out.push_back(std::move(row));
    }
    return out;
}

static std::vector<TableMeta>
CollectTableMetadata(FirebirdConnection &conn,
                      NoneEncoding none_encoding) {
    auto tables = ListUserTables(conn);
    std::vector<TableMeta> out;
    out.reserve(tables.size());
    for (const auto &row : tables) {
        TableMeta tm;
        tm.name = row.table_name;

        duckdb::vector<std::string>        col_names;
        duckdb::vector<LogicalType>        col_types;
        duckdb::vector<FirebirdColumnDesc> col_descs;
        LoadTableSchema(conn, tm.name, col_names, col_types, col_descs,
                        none_encoding);

        tm.columns.reserve(col_names.size());
        for (idx_t i = 0; i < col_names.size(); ++i) {
            ColumnMeta cm;
            cm.name  = col_names[i];
            cm.type  = col_types[i];
            cm.is_pk = !row.pk_column.empty() && row.pk_column == col_names[i];
            tm.columns.push_back(std::move(cm));
        }
        out.push_back(std::move(tm));
    }
    return out;
}

// ---------------------------------------------------------------------------
//  YAML composition
// ---------------------------------------------------------------------------

static const char *FIREBIRD_MAIN_SCHEMA = "main";

static std::string BuildSourcesYaml(FirebirdConnection &conn,
                                    const std::string &catalog_name,
                                    NoneEncoding none_encoding) {
    auto tables = CollectTableMetadata(conn, none_encoding);

    std::ostringstream os;
    os << "version: 2\n";
    os << "\n";
    os << "sources:\n";
    os << "  - name: " << YamlQuote(catalog_name) << "\n";
    os << "    schema: " << YamlQuote(FIREBIRD_MAIN_SCHEMA) << "\n";
    os << "    description: \"\"\n";
    os << "    tables:\n";
    if (tables.empty()) {
        os << "      []\n";
        return os.str();
    }
    for (const auto &t : tables) {
        os << "      - name: " << YamlQuote(t.name) << "\n";
        os << "        description: \"\"\n";
        if (t.columns.empty()) {
            os << "        columns: []\n";
            continue;
        }
        os << "        columns:\n";
        for (const auto &c : t.columns) {
            os << "          - name: " << YamlQuote(c.name) << "\n";
            os << "            data_type: " << YamlQuote(c.type.ToString()) << "\n";
            os << "            description: \"\"\n";
            if (c.is_pk) {
                os << "            tests:\n";
                os << "              - not_null\n";
                os << "              - unique\n";
            }
        }
    }
    return os.str();
}

// ---------------------------------------------------------------------------
//  firebird_generate_dbt_sources(catalog_name VARCHAR) — table function
// ---------------------------------------------------------------------------
//
// Chunk B replaces the Chunk A placeholder with a real walk of the
// attached catalog's RDB$ tables. The lease's destructor returns the
// connection to the pool even when BuildSourcesYaml throws mid-scan,
// so a failed metadata query never leaks a connection.

struct DbtSourcesBindData : public TableFunctionData {
    std::string catalog_name;
};

struct DbtSourcesGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
DbtSourcesBind(ClientContext &context,
               TableFunctionBindInput &input,
               vector<LogicalType> &return_types,
               vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_generate_dbt_sources(catalog_name VARCHAR): "
            "catalog_name is required (the alias from ATTACH '...' AS "
            "<alias> (TYPE firebird)).");
    }
    auto bind = make_uniq<DbtSourcesBindData>();
    bind->catalog_name = input.inputs[0].ToString();

    // Resolve the alias eagerly so a bad name fails at bind time, not
    // mid-scan.
    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {"yaml"};
    return_types = {LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
DbtSourcesInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<DbtSourcesGlobalState>();
}

static void DbtSourcesFunction(ClientContext &context,
                                TableFunctionInput &input,
                                DataChunk &output) {
    auto &g = input.global_state->Cast<DbtSourcesGlobalState>();
    if (g.emitted) {
        output.SetCardinality(0);
        return;
    }
    auto &bind = input.bind_data->Cast<DbtSourcesBindData>();

    // The lease releases the leased connection on scope exit, even if
    // BuildSourcesYaml throws while iterating RDB$ metadata.
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    std::string yaml = BuildSourcesYaml(*lease.conn, bind.catalog_name,
                                         lease.none_encoding);

    output.SetCardinality(1);
    output.data[0].SetValue(0, Value(yaml));
    g.emitted = true;
}

TableFunction GetFirebirdDbtSourcesFunction() {
    TableFunction fn("firebird_generate_dbt_sources",
                     {LogicalType::VARCHAR},
                     DbtSourcesFunction,
                     DbtSourcesBind,
                     DbtSourcesInitGlobal);
    return fn;
}

} // namespace duckdb
