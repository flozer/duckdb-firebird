#include "firebird_profile_table.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_dbt_sources.hpp" // FirebirdMetadataLease, AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias
#include "firebird_scanner.hpp"     // LoadTableSchema, ProbePrimaryKey, PrimaryKeyInfo
#include "firebird_view_analysis.hpp" // LookupObjectType, ViewAnalysis, AnalyzeViewSource

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
//  Qualified-name parsing
// ---------------------------------------------------------------------------
//
// The function takes a `catalog.schema.table` reference and pulls it apart
// into (catalog, table). The Firebird ATTACH path exposes exactly one
// schema, "main", so the schema part is accepted only as 'main' (any case)
// and may be omitted entirely:
//
//   fb.main.CUSTOMER  -> catalog=fb, table=CUSTOMER
//   fb.CUSTOMER       -> catalog=fb, table=CUSTOMER
//   fb.main.X.Y       -> error (too many parts)
//
// We deliberately do NOT support quoted identifiers with embedded dots in
// this first version — legacy Firebird OLTP relation names are
// alphanumeric + underscore. A name that needs quoting is rare enough to
// defer until someone actually hits it.

struct ProfileTarget {
    std::string catalog;
    std::string table;
};

static ProfileTarget ParseQualifiedName(const std::string &qualified) {
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

    ProfileTarget out;
    if (parts.size() == 2) {
        out.catalog = parts[0];
        out.table = parts[1];
    } else if (parts.size() == 3) {
        if (upper(parts[1]) != "MAIN") {
            throw BinderException(
                "firebird_profile_table: schema '%s' is not exposed. The "
                "Firebird ATTACH path exposes exactly one schema, 'main'. "
                "Use '%s.main.%s' or '%s.%s'.",
                parts[1], parts[0], parts[2], parts[0], parts[2]);
        }
        out.catalog = parts[0];
        out.table = parts[2];
    } else {
        throw BinderException(
            "firebird_profile_table(qualified_name VARCHAR): expected "
            "'catalog.schema.table' or 'catalog.table' (e.g. "
            "'fb.main.CUSTOMER'), got '%s'.",
            qualified);
    }
    if (out.catalog.empty() || out.table.empty()) {
        throw BinderException(
            "firebird_profile_table: qualified_name has an empty catalog or "
            "table part ('%s'). Expected 'catalog.schema.table' or "
            "'catalog.table'.",
            qualified);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Profile model
// ---------------------------------------------------------------------------

struct IndexMeta {
    std::string name;
    std::vector<std::string> columns;
    bool unique = false;
    bool is_pk = false;
};

// One structured diagnostic. `code` is a stable public identifier (never
// reused or redefined once shipped); `severity` is LOW | MEDIUM | HIGH,
// reusing the full_scan_risk vocabulary; `message` is the human-readable
// prose (also surfaced verbatim in the legacy `warnings` column).
struct Alert {
    std::string code;
    std::string severity;
    std::string message;
};

struct TableProfile {
    std::string table_name;
    std::string object_type = "TABLE"; // TABLE | VIEW
    bool has_primary_key = false;
    std::vector<std::string> primary_key_columns;
    std::vector<std::string> indexes;             // formatted descriptions
    std::vector<std::string> watermark_candidates;
    std::vector<std::string> filter_candidates;
    std::string full_scan_risk = "MEDIUM"; // LOW | MEDIUM | HIGH
    int32_t recommended_partitions = 1;
    std::vector<Alert> alerts;
};

// Single source for every diagnostic. The legacy `warnings` column is
// derived from these alerts' messages at emit time, so the two lists
// cannot diverge.
static void AddAlert(TableProfile &p, const char *code, const char *severity,
                     std::string message) {
    p.alerts.push_back(Alert{code, severity, std::move(message)});
}

// SQL-quote a single-quoted string literal (doubles embedded quotes). Local
// copy of the scanner's helper so any user-controlled relation name reaching
// a Firebird WHERE clause cannot escape the string literal.
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

// Reads every index on the relation, with its segment columns, uniqueness,
// and whether it backs the PRIMARY KEY constraint. Best-effort: a failed
// metadata query leaves the list empty rather than aborting the profile.
static std::vector<IndexMeta> LoadIndexes(FirebirdConnection &conn,
                                          const std::string &upper_table) {
    std::vector<IndexMeta> out;
    try {
        // One row per index. RDB$UNIQUE_FLAG = 1 means unique. The PK
        // marker comes from RDB$RELATION_CONSTRAINTS (LEFT JOIN — most
        // indexes are not constraint-backed).
        auto cur = conn.OpenCursor(
            "SELECT TRIM(ri.RDB$INDEX_NAME), "
            "       COALESCE(ri.RDB$UNIQUE_FLAG, 0), "
            "       CASE WHEN rc.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY' "
            "            THEN 1 ELSE 0 END "
            "  FROM RDB$INDICES ri "
            "  LEFT JOIN RDB$RELATION_CONSTRAINTS rc "
            "         ON rc.RDB$INDEX_NAME = ri.RDB$INDEX_NAME "
            "        AND rc.RDB$CONSTRAINT_TYPE = 'PRIMARY KEY' "
            " WHERE ri.RDB$RELATION_NAME = " + SqlLiteral(upper_table) + " "
            " ORDER BY ri.RDB$INDEX_NAME");
        while (cur->Fetch()) {
            IndexMeta idx;
            idx.name = cur->GetText(0);
            idx.unique = (cur->GetShort(1) == 1);
            idx.is_pk = (cur->GetShort(2) == 1);
            out.push_back(std::move(idx));
        }
    } catch (...) {
        return {};
    }

    // Fill segments per index. Cheap: one round trip per index, and OLTP
    // relations rarely carry more than a handful.
    for (auto &idx : out) {
        try {
            auto cur = conn.OpenCursor(
                "SELECT TRIM(seg.RDB$FIELD_NAME) "
                "  FROM RDB$INDEX_SEGMENTS seg "
                " WHERE seg.RDB$INDEX_NAME = " + SqlLiteral(idx.name) + " "
                " ORDER BY seg.RDB$FIELD_POSITION");
            while (cur->Fetch()) {
                idx.columns.push_back(cur->GetText(0));
            }
        } catch (...) {
            // Leave this index's columns empty; the formatter copes.
        }
    }
    return out;
}

// "IDX_NAME (COL_A, COL_B) [UNIQUE] [PK]"
//
// Firebird expression / computed indexes (CREATE INDEX ... COMPUTED BY (...))
// store their definition in RDB$INDICES.RDB$EXPRESSION_BLR and carry NO rows
// in RDB$INDEX_SEGMENTS. LoadIndexes therefore leaves `columns` empty for
// them. Rather than print misleading empty parentheses ("IDX ()"), label the
// index "(expression)". We intentionally do not decode or expose the
// expression BLR — the goal is only to avoid an empty-looking index.
static std::string FormatIndex(const IndexMeta &idx) {
    std::string s = idx.name + " (";
    if (idx.columns.empty()) {
        s += "expression";
    } else {
        for (size_t i = 0; i < idx.columns.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += idx.columns[i];
        }
    }
    s += ")";
    if (idx.unique) {
        s += " UNIQUE";
    }
    if (idx.is_pk) {
        s += " PK";
    }
    return s;
}

// ---------------------------------------------------------------------------
//  Heuristics — simple, explicit, factual
// ---------------------------------------------------------------------------
//
// Watermark candidate: a column whose type can drive incremental /
// high-water-mark extraction. We accept INTEGER-family columns (often
// surrogate keys / sequences that grow monotonically) and DATE/TIMESTAMP
// columns (created_at / updated_at patterns). We do NOT claim the column
// IS monotonic — only that its TYPE makes it a plausible watermark. The
// warnings column says exactly that.
static bool IsWatermarkType(const LogicalType &t) {
    switch (t.id()) {
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
        return true;
    default:
        return false;
    }
}

// A "good filter column" here means: it is covered by an index (so a WHERE
// on it can use the index instead of scanning) AND its type is one the
// scanner can push down without charset hazards. We exclude text columns
// on CHARACTER SET NONE storage because text-filter pushdown is gated off
// for those (UTF-8 literals may not round-trip against NONE bytes).
static bool IsCheapFilterType(const FirebirdColumnDesc &desc,
                              const LogicalType &t) {
    const bool is_text =
        (t.id() == LogicalTypeId::VARCHAR);
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

// ---------------------------------------------------------------------------
//  Profile assembly
// ---------------------------------------------------------------------------

static TableProfile BuildProfile(FirebirdConnection &conn,
                                 const std::string &table_name,
                                 NoneEncoding none_encoding) {
    const std::string upper = UpperCopy(table_name);

    TableProfile p;
    p.table_name = upper;

    // 1) Object type + existence. A missing relation is a hard error: the
    //    caller asked to profile something that is not there.
    if (!LookupObjectType(conn, upper, p.object_type)) {
        throw BinderException(
            "firebird_profile_table: relation '%s' not found in the attached "
            "Firebird catalog (RDB$RELATIONS has no matching row).",
            table_name);
    }
    const bool is_view = (p.object_type == "VIEW");

    // 2) Columns + per-column Firebird metadata. LoadTableSchema throws if
    //    the relation has no readable columns.
    duckdb::vector<std::string> col_names;
    duckdb::vector<LogicalType> col_types;
    duckdb::vector<FirebirdColumnDesc> col_descs;
    LoadTableSchema(conn, upper, col_names, col_types, col_descs,
                    none_encoding);

    // 3) Indexes (tables only — views carry no physical indexes).
    std::vector<IndexMeta> indexes;
    if (!is_view) {
        indexes = LoadIndexes(conn, upper);
        for (const auto &idx : indexes) {
            p.indexes.push_back(FormatIndex(idx));
            if (idx.is_pk) {
                p.has_primary_key = true;
                p.primary_key_columns = idx.columns;
            }
        }
    }

    // 4) Set of indexed columns — drives the filter-candidate heuristic.
    std::vector<std::string> indexed_cols;
    for (const auto &idx : indexes) {
        for (const auto &c : idx.columns) {
            indexed_cols.push_back(c);
        }
    }
    auto is_indexed = [&](const std::string &c) {
        return std::find(indexed_cols.begin(), indexed_cols.end(), c) !=
               indexed_cols.end();
    };

    // 5) Watermark + filter candidates.
    for (idx_t i = 0; i < col_names.size(); ++i) {
        if (IsWatermarkType(col_types[i])) {
            p.watermark_candidates.push_back(col_names[i]);
        }
        if (!is_view && is_indexed(col_names[i]) &&
            IsCheapFilterType(col_descs[i], col_types[i])) {
            p.filter_candidates.push_back(col_names[i]);
        }
    }

    // 6) Full-scan risk + recommended partitions.
    //
    //    The single safe parallelism lever the scanner already supports is
    //    a single-column numeric PK with a usable MIN/MAX range (see
    //    ProbePrimaryKey). When that exists we can recommend partitions and
    //    rate the scan risk lower; otherwise a full scan is the only path
    //    and the risk is higher.
    std::unique_ptr<PrimaryKeyInfo> pk;
    if (!is_view) {
        pk = ProbePrimaryKey(conn, upper, col_names, col_types);
    }

    if (is_view) {
        // Views have no PK / index / partition lever and may hide joins or
        // aggregation. Conservative: HIGH risk, serial, with an explicit
        // pointer to the heavy-view guidance.
        p.full_scan_risk = "HIGH";
        p.recommended_partitions = 1;
        AddAlert(p, "view_no_scan_lever", "HIGH",
            "Object is a VIEW: no primary key, indexes, or partition lever. "
            "A scan reads the full view definition. Consider materializing "
            "through DuckDB/dbt/Parquet for repeated analytics.");

        // Heavy-view diagnostics (Phase 4 #2). Shallow, conservative
        // inspection of the stored view SELECT text — never the text
        // itself, only shape flags. When the source can't be read we say
        // so explicitly rather than imply a simple view.
        ViewAnalysis va = AnalyzeViewSource(conn, upper);
        if (!va.inspected) {
            AddAlert(p, "view_definition_not_inspected", "MEDIUM",
                "View definition not inspected (RDB$VIEW_SOURCE unavailable "
                "or unreadable): join/aggregation/filter shape is unknown. "
                "Treat as potentially heavy.");
        } else {
            if (va.has_join) {
                AddAlert(p, "view_contains_join", "HIGH",
                    "View contains a JOIN: a scan may materialize a join "
                    "server-side on every read. Prefer materializing through "
                    "DuckDB/dbt/Parquet for repeated analytics.");
            }
            if (va.has_group_by || va.has_aggregate) {
                AddAlert(p, "view_contains_aggregation", "HIGH",
                    "View contains aggregation (GROUP BY or aggregate "
                    "functions): each scan recomputes the aggregate "
                    "server-side. Materialize the result if queried "
                    "repeatedly.");
            }
            if (!va.has_where) {
                AddAlert(p, "view_no_filter", "MEDIUM",
                    "View has no WHERE filter in its definition: a scan reads "
                    "the full underlying data set. Push a selective filter or "
                    "materialize.");
            }
        }
    } else if (pk) {
        // Single-column numeric PK with a usable MIN/MAX range — the only
        // safe parallelism lever the scanner supports. recommended_partitions
        // is now computed by the SAME PickPartitionCount the scanner itself
        // uses at scan time (firebird_scanner.hpp), so this recommendation
        // can never diverge from what a real scan would actually do. It
        // remains advisory in spirit (row count is never probed — no
        // COUNT(*) / full scan in the analyzer), but the number itself is
        // now the scanner's real answer, not a separate estimate.
        const idx_t parts = PickPartitionCount(pk->min_value, pk->max_value);
        p.recommended_partitions = static_cast<int32_t>(parts);
        p.full_scan_risk = (parts > 1) ? "MEDIUM" : "LOW";
        if (parts > 1) {
            AddAlert(p, "partition_advisory", "LOW",
                "Recommended partitions=" + std::to_string(parts) +
                " is advisory and derived from the PK MIN/MAX range width, "
                "not the row count. The PK range may be sparse (gaps, "
                "deletes), so partitions can be uneven. Validate against the "
                "live server before scanning a production database in "
                "parallel.");
            // Server-side parallelism caveat: Firebird 5 can parallelize a
            // single query server-side, and combining that with client-side
            // PK-range partitions can oversubscribe the server. We have no
            // cheap, reliable probe for the server's ParallelWorkers setting
            // from the catalog, so this is surfaced as a generic caveat
            // rather than a detected condition.
            AddAlert(p, "server_parallelism_caveat", "LOW",
                "If Firebird server-side parallelism is already "
                "enabled/configured (e.g. Firebird 5 ParallelWorkers), prefer "
                "starting with partitions=1 or benchmark before combining "
                "server-side and client-side parallelism.");
        } else {
            // PK is numeric with a real range, but below the scanner's own
            // partitioning threshold (PickPartitionCount's minimum-rows-per-
            // partition floor) — the same function that decides this at
            // scan time returned 1. Be explicit so the caller does not
            // wonder why a numeric PK still recommends serial.
            AddAlert(p, "pk_range_small_serial", "LOW",
                "Primary key range is small relative to the scanner's "
                "partitioning threshold: serial scan recommended; "
                "partitioning would add overhead without meaningful "
                "parallelism.");
        }
    } else {
        // No view, but also no usable parallel lever: composite /
        // non-numeric / missing PK. Single-threaded full scan only.
        p.full_scan_risk = "HIGH";
        p.recommended_partitions = 1;
        if (!p.has_primary_key) {
            AddAlert(p, "no_primary_key", "HIGH",
                "No primary key detected: a scan is a full table scan and "
                "cannot be range-partitioned. Add a selective WHERE on an "
                "indexed column, or materialize the table.");
        } else if (p.primary_key_columns.size() > 1) {
            AddAlert(p, "composite_pk_serial", "LOW",
                "Primary key is composite: the parallel-scan lever needs a "
                "single-column numeric PK. Scans run serially.");
        } else {
            // Single-column PK that ProbePrimaryKey still rejected. Either the
            // column is non-numeric (CHAR/VARCHAR/UUID surrogate, common in
            // legacy ERPs) or it is numeric but has no usable MIN/MAX range
            // (empty or single-row table). Distinguish the two so the message
            // is accurate rather than lumping both under "non-numeric".
            bool pk_numeric = false;
            const auto &pk_col = p.primary_key_columns.front();
            for (idx_t i = 0; i < col_names.size(); ++i) {
                if (col_names[i] == pk_col) {
                    switch (col_types[i].id()) {
                    case LogicalTypeId::SMALLINT:
                    case LogicalTypeId::INTEGER:
                    case LogicalTypeId::BIGINT:
                        pk_numeric = true;
                        break;
                    default:
                        break;
                    }
                    break;
                }
            }
            if (pk_numeric) {
                AddAlert(p, "numeric_pk_no_range_serial", "LOW",
                    "Primary key is single-column numeric but has no usable "
                    "MIN/MAX range (empty or near-empty table): no partition "
                    "lever, scans run serially.");
            } else {
                AddAlert(p, "non_numeric_pk_serial", "LOW",
                    "Primary key is single-column but non-numeric: the "
                    "parallel-scan lever needs a numeric PK. Scans run "
                    "serially.");
            }
        }
    }

    // 7) Generic caveats.
    if (!p.has_primary_key && !is_view) {
        // Already warned above for the no-PK case; nothing extra.
    }
    if (p.filter_candidates.empty() && !is_view) {
        AddAlert(p, "no_indexed_filter_columns", "MEDIUM",
            "No cheap indexed filter columns detected: WHERE clauses may not "
            "use an index and could force a full scan.");
    }
    bool any_none = false;
    for (const auto &d : col_descs) {
        if (d.character_set_id == 0) {
            any_none = true;
            break;
        }
    }
    if (any_none) {
        AddAlert(p, "none_charset_text_columns", "MEDIUM",
            "Relation has CHARACTER SET NONE text columns: text-filter "
            "pushdown is disabled for those columns (UTF-8 literals may not "
            "round-trip against raw NONE bytes).");
    }

    return p;
}

// ---------------------------------------------------------------------------
//  Table function plumbing
// ---------------------------------------------------------------------------

struct ProfileTableBindData : public TableFunctionData {
    std::string catalog_name;
    std::string table_name;
};

struct ProfileTableGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
ProfileTableBind(ClientContext &context, TableFunctionBindInput &input,
                 vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_profile_table(qualified_name VARCHAR): qualified_name "
            "is required (e.g. 'fb.main.CUSTOMER' for an ATTACH aliased "
            "'fb').");
    }
    auto qn = ParseQualifiedName(input.inputs[0].ToString());

    auto bind = make_uniq<ProfileTableBindData>();
    bind->catalog_name = qn.catalog;
    bind->table_name = qn.table;

    // Resolve the alias eagerly so a bad catalog fails at bind time, not
    // mid-scan. Reuses the dbt-sources validator (same "is this a Firebird
    // ATTACH" check), which already emits an actionable message.
    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {
        "table_name",
        "object_type",
        "has_primary_key",
        "primary_key_columns",
        "indexes",
        "watermark_candidates",
        "filter_candidates",
        "full_scan_risk",
        "recommended_partitions",
        "warnings",
        "alerts",
    };
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::VARCHAR,
        LogicalType::INTEGER,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::STRUCT({
            {"code", LogicalType::VARCHAR},
            {"severity", LogicalType::VARCHAR},
            {"message", LogicalType::VARCHAR}})),
    };
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
ProfileTableInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<ProfileTableGlobalState>();
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

static Value WarningsFromAlerts(const std::vector<Alert> &alerts) {
    vector<Value> vals;
    vals.reserve(alerts.size());
    for (const auto &a : alerts) {
        vals.emplace_back(Value(a.message));
    }
    return Value::LIST(LogicalType::VARCHAR, std::move(vals));
}

static void ProfileTableFunction(ClientContext &context,
                                 TableFunctionInput &input,
                                 DataChunk &output) {
    auto &g = input.global_state->Cast<ProfileTableGlobalState>();
    if (g.emitted) {
        output.SetCardinality(0);
        return;
    }
    auto &bind = input.bind_data->Cast<ProfileTableBindData>();

    // The lease releases the connection on scope exit, even if BuildProfile
    // throws mid-probe (e.g. relation not found).
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    TableProfile p =
        BuildProfile(*lease.conn, bind.table_name, lease.none_encoding);

    output.SetCardinality(1);
    output.data[0].SetValue(0, Value(p.table_name));
    output.data[1].SetValue(0, Value(p.object_type));
    output.data[2].SetValue(0, Value::BOOLEAN(p.has_primary_key));
    output.data[3].SetValue(0, VarcharList(p.primary_key_columns));
    output.data[4].SetValue(0, VarcharList(p.indexes));
    output.data[5].SetValue(0, VarcharList(p.watermark_candidates));
    output.data[6].SetValue(0, VarcharList(p.filter_candidates));
    output.data[7].SetValue(0, Value(p.full_scan_risk));
    output.data[8].SetValue(0, Value::INTEGER(p.recommended_partitions));
    output.data[9].SetValue(0, WarningsFromAlerts(p.alerts));
    output.data[10].SetValue(0, AlertStructList(p.alerts));
    g.emitted = true;
}

TableFunction GetFirebirdProfileTableFunction() {
    TableFunction fn("firebird_profile_table",
                     {LogicalType::VARCHAR},
                     ProfileTableFunction,
                     ProfileTableBind,
                     ProfileTableInitGlobal);
    return fn;
}

} // namespace duckdb
