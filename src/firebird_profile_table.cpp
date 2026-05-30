#include "firebird_profile_table.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_dbt_sources.hpp" // FirebirdMetadataLease, AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias
#include "firebird_scanner.hpp"     // LoadTableSchema, ProbePrimaryKey, PrimaryKeyInfo

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
    std::vector<std::string> warnings;
};

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

// RDB$RELATIONS.RDB$RELATION_TYPE: 0/NULL persistent table, 1 view, 2
// external, 3 MON$ snapshot, 4/5 GTT. RDB$VIEW_BLR being non-NULL is the
// canonical "this is a view" signal across Firebird versions, so we lean
// on it as the primary discriminator and fall back to RELATION_TYPE.
// Returns false (with no rows) when the relation does not exist — the
// caller turns that into a BinderException.
static bool LookupObjectType(FirebirdConnection &conn,
                             const std::string &upper_table,
                             std::string &out_type) {
    auto cur = conn.OpenCursor(
        "SELECT CASE WHEN r.RDB$VIEW_BLR IS NOT NULL "
        "            OR r.RDB$RELATION_TYPE = 1 THEN 'VIEW' "
        "            ELSE 'TABLE' END "
        "  FROM RDB$RELATIONS r "
        " WHERE r.RDB$RELATION_NAME = " + SqlLiteral(upper_table));
    if (!cur->Fetch()) {
        return false;
    }
    out_type = cur->GetText(0);
    return true;
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
        p.warnings.push_back(
            "Object is a VIEW: no primary key, indexes, or partition lever. "
            "A scan reads the full view definition. Consider materializing "
            "through DuckDB/dbt/Parquet for repeated analytics.");
    } else if (pk) {
        // Numeric single-column PK with a real range — the partition
        // heuristic is the same shape the scanner's auto path uses: cap at
        // a small, predictable number so we never recommend runaway
        // parallelism against an OLTP server.
        const int64_t span = pk->max_value - pk->min_value;
        int32_t parts = 1;
        if (span >= 1000000) {
            parts = 8;
        } else if (span >= 100000) {
            parts = 4;
        } else if (span >= 10000) {
            parts = 2;
        }
        p.recommended_partitions = parts;
        p.full_scan_risk = (parts > 1) ? "MEDIUM" : "LOW";
        if (parts > 1) {
            p.warnings.push_back(
                "Recommended partitions is advisory and derived from the PK "
                "MIN/MAX range, not row count. Validate against the live "
                "server before scanning a production database in parallel.");
        }
    } else {
        // No view, but also no usable parallel lever: composite /
        // non-numeric / missing PK. Single-threaded full scan only.
        p.full_scan_risk = "HIGH";
        p.recommended_partitions = 1;
        if (!p.has_primary_key) {
            p.warnings.push_back(
                "No primary key detected: a scan is a full table scan and "
                "cannot be range-partitioned. Add a selective WHERE on an "
                "indexed column, or materialize the table.");
        } else if (p.primary_key_columns.size() > 1) {
            p.warnings.push_back(
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
                p.warnings.push_back(
                    "Primary key is single-column numeric but has no usable "
                    "MIN/MAX range (empty or near-empty table): no partition "
                    "lever, scans run serially.");
            } else {
                p.warnings.push_back(
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
        p.warnings.push_back(
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
        p.warnings.push_back(
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
    output.data[9].SetValue(0, VarcharList(p.warnings));
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
