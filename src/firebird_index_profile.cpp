#include "firebird_index_profile.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_dbt_sources.hpp" // FirebirdMetadataLease, AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias
#include "firebird_scanner.hpp"     // LoadTableSchema, NoneEncoding, FirebirdColumnDesc

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
//  Qualified-name parsing — local copy of firebird_profile_table.cpp's
//  ParseQualifiedName, renamed error-message prefix. Duplicated rather than
//  shared: this codebase's established pattern (see firebird_profile_table's
//  own local copy of SqlLiteral) is small per-file helper duplication over
//  cross-file coupling for these tiny, stable parsers.
// ---------------------------------------------------------------------------

struct IndexProfileTarget {
    std::string catalog;
    std::string table;
};

static IndexProfileTarget ParseQualifiedName(const std::string &qualified) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : qualified) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);

    auto upper = [](std::string s) {
        for (auto &c : s) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    };

    IndexProfileTarget out;
    if (parts.size() == 2) {
        out.catalog = parts[0];
        out.table = parts[1];
    } else if (parts.size() == 3) {
        if (upper(parts[1]) != "MAIN") {
            throw BinderException(
                "firebird_index_profile: schema '%s' is not exposed. The "
                "Firebird ATTACH path exposes exactly one schema, 'main'. "
                "Use '%s.main.%s' or '%s.%s'.",
                parts[1], parts[0], parts[2], parts[0], parts[2]);
        }
        out.catalog = parts[0];
        out.table = parts[2];
    } else {
        throw BinderException(
            "firebird_index_profile(qualified_name VARCHAR): expected "
            "'catalog.schema.table' or 'catalog.table' (e.g. "
            "'fb.main.CUSTOMER'), got '%s'.",
            qualified);
    }
    if (out.catalog.empty() || out.table.empty()) {
        throw BinderException(
            "firebird_index_profile: qualified_name has an empty catalog or "
            "table part ('%s'). Expected 'catalog.schema.table' or "
            "'catalog.table'.",
            qualified);
    }
    return out;
}

// SQL-quote a single-quoted string literal (doubles embedded quotes). Local
// copy of firebird_profile_table.cpp's SqlLiteral.
static std::string SqlLiteral(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out.push_back('\'');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static std::string UpperCopy(const std::string &s) {
    std::string out = s;
    for (auto &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Model
// ---------------------------------------------------------------------------

// One structured diagnostic, same shape as firebird_profile_table's Alert:
// `code` is a stable public identifier (never reused/redefined once
// shipped); `severity` is LOW | MEDIUM | HIGH; `message` is human-readable.
struct Alert {
    std::string code;
    std::string severity;
    std::string message;
};

static void AddAlert(std::vector<Alert> &alerts, const char *code,
                     const char *severity, std::string message) {
    alerts.push_back(Alert{code, severity, std::move(message)});
}

struct IndexProfileRow {
    bool is_synthetic = false;
    std::string index_name;
    std::vector<std::string> columns;
    bool is_unique = false;
    bool is_active = false;
    bool is_primary_key = false;
    bool is_foreign_key = false;
    bool has_selectivity = false;
    double selectivity = 0.0;
    std::vector<Alert> alerts;
};

struct IndexProfileResult {
    std::vector<IndexProfileRow> rows;
    std::vector<std::string> unindexed_filter_candidates;
};

// ---------------------------------------------------------------------------
//  Firebird reads
// ---------------------------------------------------------------------------

static bool RelationExists(FirebirdConnection &conn,
                           const std::string &upper_table) {
    auto cur = conn.OpenCursor(
        "SELECT 1 FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = " +
        SqlLiteral(upper_table));
    return cur->Fetch();
}

// Reads every index on the relation, with segment columns, uniqueness,
// active state, PK/FK backing, and selectivity. Deliberately does NOT
// catch-and-swallow query failures the way firebird_profile_table's
// LoadIndexes does: a silently-empty result here would falsely emit the
// no_indexes_on_table/HIGH alert for a table that actually has indexes,
// which is worse than a visible error.
static std::vector<IndexProfileRow> LoadIndexRows(FirebirdConnection &conn,
                                                   const std::string &upper_table) {
    std::vector<IndexProfileRow> out;
    // RDB$UNIQUE_FLAG / RDB$INDEX_INACTIVE are SMALLINT; the CASE-produced
    // PK/FK flags are integer literals (0/1) — all read via GetShort.
    // RDB$STATISTICS is DOUBLE PRECISION/FLOAT — read via GetDouble.
    auto cur = conn.OpenCursor(
        "SELECT TRIM(ri.RDB$INDEX_NAME), "
        "       COALESCE(ri.RDB$UNIQUE_FLAG, 0), "
        "       CASE WHEN ri.RDB$INDEX_INACTIVE IS NULL "
        "                 OR ri.RDB$INDEX_INACTIVE = 0 THEN 1 ELSE 0 END, "
        "       CASE WHEN pk.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY' "
        "            THEN 1 ELSE 0 END, "
        "       CASE WHEN fk.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
        "            THEN 1 ELSE 0 END, "
        "       ri.RDB$STATISTICS "
        "  FROM RDB$INDICES ri "
        "  LEFT JOIN RDB$RELATION_CONSTRAINTS pk "
        "         ON pk.RDB$INDEX_NAME = ri.RDB$INDEX_NAME "
        "        AND pk.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY' "
        "  LEFT JOIN RDB$RELATION_CONSTRAINTS fk "
        "         ON fk.RDB$INDEX_NAME = ri.RDB$INDEX_NAME "
        "        AND fk.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
        " WHERE ri.RDB$RELATION_NAME = " + SqlLiteral(upper_table) + " "
        " ORDER BY ri.RDB$INDEX_NAME");
    while (cur->Fetch()) {
        IndexProfileRow row;
        row.index_name = cur->GetText(0);
        row.is_unique = (cur->GetShort(1) == 1);
        row.is_active = (cur->GetShort(2) == 1);
        row.is_primary_key = (cur->GetShort(3) == 1);
        row.is_foreign_key = (cur->GetShort(4) == 1);
        row.has_selectivity = !cur->IsNull(5);
        if (row.has_selectivity) {
            row.selectivity = cur->GetDouble(5);
        }
        out.push_back(std::move(row));
    }

    // Fill segment columns per index — one round trip per index, same
    // cost profile as firebird_profile_table's LoadIndexes.
    for (auto &row : out) {
        auto seg_cur = conn.OpenCursor(
            "SELECT TRIM(seg.RDB$FIELD_NAME) "
            "  FROM RDB$INDEX_SEGMENTS seg "
            " WHERE seg.RDB$INDEX_NAME = " + SqlLiteral(row.index_name) + " "
            " ORDER BY seg.RDB$FIELD_POSITION");
        while (seg_cur->Fetch()) {
            row.columns.push_back(seg_cur->GetText(0));
        }
    }
    return out;
}

// A "cheap filter column" here means: its type is one the scanner can push
// down without charset hazards. Excludes text columns on CHARACTER SET
// NONE storage (UTF-8 literals may not round-trip against raw NONE
// bytes). Same heuristic as firebird_profile_table's IsCheapFilterType.
static bool IsCheapFilterType(const FirebirdColumnDesc &desc,
                              const LogicalType &t) {
    const bool is_text = (t.id() == LogicalTypeId::VARCHAR);
    const bool none_charset = (desc.character_set_id == 0);
    if (is_text && none_charset) {
        return false;
    }
    switch (t.id()) {
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
    case LogicalTypeId::DECIMAL:
    case LogicalTypeId::VARCHAR:
        return true;
    default:
        return false;
    }
}

static IndexProfileResult BuildIndexProfile(FirebirdConnection &conn,
                                            const std::string &table_name,
                                            NoneEncoding none_encoding) {
    const std::string upper = UpperCopy(table_name);

    if (!RelationExists(conn, upper)) {
        throw BinderException(
            "firebird_index_profile: relation '%s' not found in the "
            "attached Firebird catalog (RDB$RELATIONS has no matching "
            "row).",
            table_name);
    }

    IndexProfileResult result;
    result.rows = LoadIndexRows(conn, upper);

    // Columns covered by ANY index segment, regardless of active/inactive
    // — "no index structure declares coverage", not "no ACTIVE index
    // covers it" (that condition is signaled via index_inactive instead).
    std::vector<std::string> covered;
    for (const auto &row : result.rows) {
        for (const auto &c : row.columns) {
            covered.push_back(c);
        }
    }
    auto is_covered = [&](const std::string &c) {
        return std::find(covered.begin(), covered.end(), c) != covered.end();
    };

    duckdb::vector<std::string> col_names;
    duckdb::vector<LogicalType> col_types;
    duckdb::vector<FirebirdColumnDesc> col_descs;
    LoadTableSchema(conn, upper, col_names, col_types, col_descs,
                    none_encoding);

    for (idx_t i = 0; i < col_names.size(); ++i) {
        if (!is_covered(col_names[i]) &&
            IsCheapFilterType(col_descs[i], col_types[i])) {
            result.unindexed_filter_candidates.push_back(col_names[i]);
        }
    }

    if (result.rows.empty()) {
        IndexProfileRow synthetic;
        synthetic.is_synthetic = true;
        AddAlert(synthetic.alerts, "no_indexes_on_table", "HIGH",
                "Table has no indexes at all: every scan is a full table "
                "scan and no column has a covering index for filtering.");
        result.rows.push_back(std::move(synthetic));
    } else {
        for (auto &row : result.rows) {
            if (!row.is_active) {
                AddAlert(row.alerts, "index_inactive", "MEDIUM",
                        "Index '" + row.index_name + "' is inactive: the "
                        "optimizer will not use it.");
            }
            if (!row.has_selectivity) {
                AddAlert(row.alerts, "missing_statistics", "LOW",
                        "Index '" + row.index_name + "' has no computed "
                        "selectivity statistic (RDB$STATISTICS is NULL): "
                        "it was never analyzed (SET STATISTICS).");
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
//  Table function plumbing (row-cursor pattern)
// ---------------------------------------------------------------------------

struct IndexProfileBindData : public TableFunctionData {
    std::string catalog_name;
    std::string table_name;
};

struct IndexProfileGlobalState : public GlobalTableFunctionState {
    duckdb::vector<duckdb::vector<Value>> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
IndexProfileBind(ClientContext &context, TableFunctionBindInput &input,
                 vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_index_profile(qualified_name VARCHAR): qualified_name "
            "is required (e.g. 'fb.main.CUSTOMER' for an ATTACH aliased "
            "'fb').");
    }
    auto qn = ParseQualifiedName(input.inputs[0].ToString());

    auto bind = make_uniq<IndexProfileBindData>();
    bind->catalog_name = qn.catalog;
    bind->table_name = qn.table;

    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {
        "index_name", "columns", "is_unique", "is_active",
        "is_primary_key", "is_foreign_key", "selectivity",
        "alerts", "unindexed_filter_candidates",
    };
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::BOOLEAN,
        LogicalType::DOUBLE,
        LogicalType::LIST(LogicalType::STRUCT({
            {"code", LogicalType::VARCHAR},
            {"severity", LogicalType::VARCHAR},
            {"message", LogicalType::VARCHAR}})),
        LogicalType::LIST(LogicalType::VARCHAR),
    };
    return std::move(bind);
}

static Value VarcharList(const std::vector<std::string> &xs) {
    vector<Value> vals;
    vals.reserve(xs.size());
    for (const auto &s : xs) {
        vals.emplace_back(Value(s));
    }
    return Value::LIST(LogicalType::VARCHAR, std::move(vals));
}

static Value AlertStructList(const std::vector<Alert> &alerts) {
    child_list_t<LogicalType> struct_children = {
        {"code", LogicalType::VARCHAR},
        {"severity", LogicalType::VARCHAR},
        {"message", LogicalType::VARCHAR}};
    auto struct_type = LogicalType::STRUCT(struct_children);
    vector<Value> vals;
    vals.reserve(alerts.size());
    for (const auto &a : alerts) {
        child_list_t<Value> sv = {
            {"code", Value(a.code)},
            {"severity", Value(a.severity)},
            {"message", Value(a.message)}};
        vals.emplace_back(Value::STRUCT(std::move(sv)));
    }
    return Value::LIST(struct_type, std::move(vals));
}

static unique_ptr<GlobalTableFunctionState>
IndexProfileInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<IndexProfileBindData>();
    auto g = make_uniq<IndexProfileGlobalState>();

    // The lease releases the connection on scope exit, even if
    // BuildIndexProfile throws mid-load (e.g. relation not found).
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    IndexProfileResult result =
        BuildIndexProfile(*lease.conn, bind.table_name, lease.none_encoding);

    for (const auto &row : result.rows) {
        duckdb::vector<Value> vals;
        vals.push_back(row.is_synthetic ? Value(LogicalType::VARCHAR)
                                        : Value(row.index_name));
        vals.push_back(VarcharList(row.columns));
        vals.push_back(row.is_synthetic ? Value(LogicalType::BOOLEAN)
                                        : Value::BOOLEAN(row.is_unique));
        vals.push_back(row.is_synthetic ? Value(LogicalType::BOOLEAN)
                                        : Value::BOOLEAN(row.is_active));
        vals.push_back(row.is_synthetic ? Value(LogicalType::BOOLEAN)
                                        : Value::BOOLEAN(row.is_primary_key));
        vals.push_back(row.is_synthetic ? Value(LogicalType::BOOLEAN)
                                        : Value::BOOLEAN(row.is_foreign_key));
        vals.push_back((row.is_synthetic || !row.has_selectivity)
                           ? Value(LogicalType::DOUBLE)
                           : Value::DOUBLE(row.selectivity));
        vals.push_back(AlertStructList(row.alerts));
        vals.push_back(VarcharList(result.unindexed_filter_candidates));
        g->rows.push_back(std::move(vals));
    }
    return std::move(g);
}

static void IndexProfileFunction(ClientContext &, TableFunctionInput &input,
                                 DataChunk &output) {
    auto &g = input.global_state->Cast<IndexProfileGlobalState>();
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

TableFunction GetFirebirdIndexProfileFunction() {
    TableFunction fn("firebird_index_profile", {LogicalType::VARCHAR},
                     IndexProfileFunction, IndexProfileBind,
                     IndexProfileInitGlobal);
    return fn;
}

} // namespace duckdb
