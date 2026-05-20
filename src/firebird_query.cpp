#include "firebird_query.hpp"
#include "firebird_types.hpp"

#include <sstream>

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

namespace duckdb {

// --- value formatting --------------------------------------------------------
//
// We only emit SQL for filters we are *confident* Firebird will accept. The
// safe set covers the common cases (numerics, dates, strings, BOOLEAN) and
// matches what DuckDB itself produces in its other scanners. Anything outside
// the safe set falls through as a "residual" filter that DuckDB re-applies
// above the scan.

static bool FormatLiteral(const Value &v, std::string &out) {
    if (v.IsNull()) { out = "NULL"; return true; }

    switch (v.type().id()) {
    case LogicalTypeId::BOOLEAN:
        out = v.GetValue<bool>() ? "TRUE" : "FALSE";
        return true;
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::DECIMAL:
        out = v.ToString();
        return true;
    case LogicalTypeId::VARCHAR: {
        // Single-quoted, doubling embedded quotes.
        std::string s = v.GetValue<std::string>();
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) { if (c == '\'') out.push_back('\''); out.push_back(c); }
        out.push_back('\'');
        return true;
    }
    case LogicalTypeId::DATE:
        out = "DATE '" + v.ToString() + "'";
        return true;
    case LogicalTypeId::TIME:
        out = "TIME '" + v.ToString() + "'";
        return true;
    case LogicalTypeId::TIMESTAMP:
        out = "TIMESTAMP '" + v.ToString() + "'";
        return true;
    default:
        return false;
    }
}

static const char *ComparisonToSql(ExpressionType t) {
    switch (t) {
    case ExpressionType::COMPARE_EQUAL:           return "=";
    case ExpressionType::COMPARE_NOTEQUAL:        return "<>";
    case ExpressionType::COMPARE_LESSTHAN:        return "<";
    case ExpressionType::COMPARE_GREATERTHAN:     return ">";
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:    return "<=";
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: return ">=";
    default: return nullptr;
    }
}

// Recursively try to translate a TableFilter on `column_name`. Writes into
// `out` on success and returns true. Caller is responsible for placing the
// generated fragment inside the outer WHERE clause.
static bool TranslateFilter(const std::string &column_name,
                            const TableFilter &filter,
                            std::string &out) {
    switch (filter.filter_type) {
    case TableFilterType::IS_NULL:
        out = column_name + " IS NULL";
        return true;
    case TableFilterType::IS_NOT_NULL:
        out = column_name + " IS NOT NULL";
        return true;
    case TableFilterType::CONSTANT_COMPARISON: {
        auto &c = filter.Cast<ConstantFilter>();
        const char *op = ComparisonToSql(c.comparison_type);
        if (!op) return false;
        std::string lit;
        if (!FormatLiteral(c.constant, lit)) return false;
        out = column_name + " " + op + " " + lit;
        return true;
    }
    case TableFilterType::CONJUNCTION_AND: {
        auto &c = filter.Cast<ConjunctionAndFilter>();
        std::string acc;
        for (auto &child : c.child_filters) {
            std::string part;
            if (!TranslateFilter(column_name, *child, part)) return false;
            if (!acc.empty()) acc += " AND ";
            acc += "(" + part + ")";
        }
        out = acc;
        return !out.empty();
    }
    case TableFilterType::CONJUNCTION_OR: {
        auto &c = filter.Cast<ConjunctionOrFilter>();
        std::string acc;
        for (auto &child : c.child_filters) {
            std::string part;
            if (!TranslateFilter(column_name, *child, part)) return false;
            if (!acc.empty()) acc += " OR ";
            acc += "(" + part + ")";
        }
        out = acc;
        return !out.empty();
    }
    case TableFilterType::IN_FILTER: {
        auto &c = filter.Cast<InFilter>();
        if (c.values.empty()) {
            // `WHERE col IN ()` is unsatisfiable — emit something that
            // guarantees zero rows without round-tripping every literal.
            out = "(1=0)";
            return true;
        }
        std::string acc = column_name + " IN (";
        for (size_t i = 0; i < c.values.size(); ++i) {
            std::string lit;
            if (!FormatLiteral(c.values[i], lit)) return false;
            if (i) acc += ", ";
            acc += lit;
        }
        acc += ")";
        out = acc;
        return true;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        // "Optional" means DuckDB will still re-check this filter, so
        // pushing it down is a pure win when we *can*. Unwrap and try the
        // child; if we can't translate it, just signal "not pushed" — no
        // correctness impact (DuckDB still applies it).
        auto &c = filter.Cast<OptionalFilter>();
        if (!c.child_filter) return false;
        return TranslateFilter(column_name, *c.child_filter, out);
    }
    default:
        return false;  // unsupported: re-evaluated in DuckDB
    }
}

FirebirdQueryBuilder::Result FirebirdQueryBuilder::Build(
    const std::string &table_name,
    const std::vector<std::string> &all_column_names,
    const std::vector<LogicalType>  &/*all_column_types*/,
    const std::vector<column_t>     &column_ids,
    optional_ptr<TableFilterSet>     filters,
    optional_idx                     limit,
    const std::string               &extra_predicate) {

    Result r;
    std::ostringstream sql;
    sql << "SELECT ";

    // --- projection pushdown -------------------------------------------------
    //
    // Important: we always emit *exactly one SELECT expression per entry
    // in column_ids*, in the same order. Otherwise the scan-time loop
    // (output column i ⇄ Firebird column i) gets misaligned. For
    // COLUMN_IDENTIFIER_ROW_ID we don't have a real rowid in Firebird, so
    // we emit a typed NULL placeholder. BIGINT matches DuckDB's
    // virtual-rowid type and keeps the cell shape valid for COUNT(*)-
    // style queries that project only the rowid.
    if (column_ids.empty()) {
        sql << "*";
    } else {
        for (size_t i = 0; i < column_ids.size(); ++i) {
            if (i) sql << ", ";
            auto cid = column_ids[i];
            if (cid == COLUMN_IDENTIFIER_ROW_ID) {
                sql << "CAST(NULL AS BIGINT)";
            } else {
                sql << QuoteIdent(all_column_names[cid]);
            }
        }
    }

    sql << " FROM " << QuoteIdent(table_name);

    // --- filter pushdown -----------------------------------------------------
    if (filters && !filters->filters.empty()) {
        std::vector<std::string> conds;
        idx_t filter_idx = 0;
        for (auto &kv : filters->filters) {
            column_t projected_col = kv.first;            // index into column_ids
            auto &filter = *kv.second;
            // Resolve projected column back to the original column name.
            if (projected_col >= column_ids.size()) {
                r.residual_filter_indices.push_back(filter_idx++);
                continue;
            }
            column_t source_col = column_ids[projected_col];
            if (source_col == COLUMN_IDENTIFIER_ROW_ID ||
                source_col >= all_column_names.size()) {
                r.residual_filter_indices.push_back(filter_idx++);
                continue;
            }
            std::string frag;
            if (TranslateFilter(QuoteIdent(all_column_names[source_col]), filter, frag)) {
                conds.push_back("(" + frag + ")");
            } else {
                r.residual_filter_indices.push_back(filter_idx);
            }
            ++filter_idx;
        }
        if (!conds.empty()) {
            sql << " WHERE ";
            for (size_t i = 0; i < conds.size(); ++i) {
                if (i) sql << " AND ";
                sql << conds[i];
            }
        }
    }

    // --- Partition predicate (PK-range parallel scan) -----------------------
    // Injected by the scanner once per worker. The text is built locally
    // ("pk >= a AND pk <= b" with numeric literals only) so no escaping is
    // needed.
    if (!extra_predicate.empty()) {
        if (sql.str().find(" WHERE ") == std::string::npos) {
            sql << " WHERE ";
        } else {
            sql << " AND ";
        }
        sql << "(" << extra_predicate << ")";
    }

    // --- LIMIT pushdown ------------------------------------------------------
    // Firebird ≥ 1.5 uses ROWS N; older dialects use FIRST N. We emit ROWS.
    if (limit.IsValid()) {
        sql << " ROWS " << limit.GetIndex();
    }

    r.sql = sql.str();
    return r;
}

} // namespace duckdb
