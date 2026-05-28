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

// Whether the value's LogicalType can be safely bound as an input
// parameter to a Firebird prepared statement (matches the encoding
// branches in FirebirdStatement::BindInputParameters). NULL is fine
// here too — the indicator slot carries the NULL flag and the
// XSQLVAR's described sqltype tells the encoder which buffer shape
// to use, so even a NULL VARCHAR / DATE / etc. binds correctly.
//
// Unsigned integer types are deliberately *not* in this whitelist:
// Firebird has no unsigned counterpart, so the XSQLVAR for a column
// or parameter is always signed and our encoder casts to the matching
// signed width. A UBIGINT > INT64_MAX would silently wrap to a
// negative value — better to let those filters stay residual in DuckDB.
static bool IsBindableType(const LogicalType &t) {
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
    // HUGEINT, DECIMAL (variable width), unsigned ints, TIMESTAMP_TZ,
    // TIME_TZ, BLOB, INTERVAL — not in v0.5 parameter encoding yet.
    // Some still inline as literals through SafeLiteralInline below.
    default:
        return false;
    }
}

// Fallback path for types we can't bind through XSQLDA but can safely
// stringify as literals (HUGEINT, DECIMAL with any precision). Strings
// are deliberately *not* included here — a VARCHAR with an embedded
// apostrophe would corrupt the SQL even with quote-doubling on the
// Firebird side if the storage charset doesn't match, so VARCHARs
// always go through bind variables.
//
// TIMESTAMP_TZ / TIME_TZ are *also* excluded: DuckDB's `Value::ToString()`
// emits a form like '2026-05-25 17:30:00+00' that Firebird's literal
// parser does not accept uniformly across dialects (FB4 added the type
// but the literal syntax varies). Filters with TZ literals stay
// residual in DuckDB until we have a verified bind/literal story
// (tracked in docs/en/roadmap.md "Milestone v0.5").
static bool SafeLiteralInline(const Value &v, std::string &out) {
    if (v.IsNull()) { out = "NULL"; return true; }
    switch (v.type().id()) {
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UHUGEINT:
    case LogicalTypeId::DECIMAL:
        out = v.ToString();
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

// Recursively try to translate a TableFilter on `column_name`. Writes the
// SQL fragment into `out` and any bindable constants into `params_out` on
// success. Returns true on success; on failure (unsupported shape or
// non-bindable constant) `out` / `params_out` are left in an
// indeterminate state and the caller marks the filter residual.
//
// Why we never fall back to literal interpolation here: mixing bound and
// inlined values in the same prepared statement is awkward (we'd need a
// second branch in the encoder, and CHARACTER SET NONE strings would
// still be a foot-gun on Firebird's side). Cleaner to either fully
// parameterise the predicate or to leave the whole thing residual.
static bool TranslateFilter(const std::string &column_name,
                            const TableFilter &filter,
                            std::string &out,
                            std::vector<Value> &params_out) {
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
        if (IsBindableType(c.constant.type())) {
            out = column_name + " " + op + " ?";
            params_out.push_back(c.constant);
            return true;
        }
        // Fallback for types we can safely stringify (HUGEINT, DECIMAL,
        // TIMESTAMP_TZ) but don't yet support as bind parameters. Strings
        // are *not* in this fallback — they always parametrise.
        std::string lit;
        if (SafeLiteralInline(c.constant, lit)) {
            out = column_name + " " + op + " " + lit;
            return true;
        }
        return false;
    }
    case TableFilterType::CONJUNCTION_AND: {
        auto &c = filter.Cast<ConjunctionAndFilter>();
        std::string acc;
        // Snapshot the params accumulator so an unsupported child can
        // roll back any params siblings already pushed for this branch.
        const size_t saved = params_out.size();
        for (auto &child : c.child_filters) {
            std::string part;
            if (!TranslateFilter(column_name, *child, part, params_out)) {
                params_out.resize(saved);
                return false;
            }
            if (!acc.empty()) acc += " AND ";
            acc += "(" + part + ")";
        }
        out = acc;
        return !out.empty();
    }
    case TableFilterType::CONJUNCTION_OR: {
        auto &c = filter.Cast<ConjunctionOrFilter>();
        std::string acc;
        const size_t saved = params_out.size();
        for (auto &child : c.child_filters) {
            std::string part;
            if (!TranslateFilter(column_name, *child, part, params_out)) {
                params_out.resize(saved);
                return false;
            }
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
        const size_t saved = params_out.size();
        std::string acc = column_name + " IN (";
        for (size_t i = 0; i < c.values.size(); ++i) {
            if (i) acc += ", ";
            if (IsBindableType(c.values[i].type())) {
                acc += "?";
                params_out.push_back(c.values[i]);
                continue;
            }
            std::string lit;
            if (!SafeLiteralInline(c.values[i], lit)) {
                params_out.resize(saved);
                return false;
            }
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
        return TranslateFilter(column_name, *c.child_filter, out, params_out);
    }
    default:
        return false;  // unsupported: re-evaluated in DuckDB
    }
}

// A predicate that touches a Firebird CHARACTER SET NONE text column
// CANNOT be safely pushed down when `none_encoding != STRICT`: the
// SqlLiteral we'd emit is UTF-8 (DuckDB's internal encoding), but the
// server compares against the raw bytes the writing application stored,
// so a row containing the correct value can silently fail the predicate.
//
// In STRICT mode the row would fail UTF-8 validation at fetch time
// anyway (the scanner raises an IOException there), so leaving the
// predicate pushable is fine — Firebird does the filtering, DuckDB
// never sees the offending bytes. Hence we return false for STRICT.
// A future refactor could short-circuit STRICT NONE pushdown to
// guarantee the error surfaces at the same point regardless of
// pushdown decisions, but that needs a verified reverse-transcode
// plan and is tracked in the v0.5 roadmap.
static bool IsNoneTextColumnGated(const FirebirdColumnDesc &desc,
                                   NoneEncoding mode) {
    if (mode == NoneEncoding::STRICT) return false;
    if (desc.character_set_id != 0) return false;
    return desc.sqltype == SQL_TEXT ||
           desc.sqltype == SQL_VARYING ||
           (desc.sqltype == SQL_BLOB && desc.sqlsubtype == 1);
}

FirebirdQueryBuilder::Result FirebirdQueryBuilder::Build(
    const std::string &table_name,
    const std::vector<std::string> &all_column_names,
    const std::vector<LogicalType>  &/*all_column_types*/,
    const std::vector<column_t>     &column_ids,
    optional_ptr<TableFilterSet>     filters,
    optional_idx                     limit,
    const std::string               &extra_predicate,
    const std::vector<FirebirdColumnDesc> *column_descs,
    NoneEncoding                     none_encoding,
    optional_idx                     offset) {

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
            // CHARACTER SET NONE text + caller is transcoding bytes
            // client-side: drop the predicate and let DuckDB filter
            // post-transcode. See IsNoneTextColumnGated() comment.
            if (column_descs && source_col < column_descs->size() &&
                IsNoneTextColumnGated((*column_descs)[source_col], none_encoding)) {
                r.residual_filter_indices.push_back(filter_idx++);
                continue;
            }
            std::string frag;
            // Snapshot the params accumulator: if the filter can't be
            // fully parametrised the partial bindings have to roll back
            // so we don't ship a mismatched (params.size() != '?' count)
            // statement to the server.
            const size_t saved_params = r.params.size();
            if (TranslateFilter(QuoteIdent(all_column_names[source_col]), filter,
                                frag, r.params)) {
                conds.push_back("(" + frag + ")");
                r.pushed_filter_sql.push_back(frag);
            } else {
                r.params.resize(saved_params);
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

    // --- LIMIT / OFFSET pushdown --------------------------------------------
    // Firebird >= 1.5 uses `ROWS N` for top-N and `ROWS m TO n` (1-based,
    // inclusive) for offset+limit. We never emit offset without limit —
    // FirebirdScanBind rejects that combination at bind time so the caller
    // sees an actionable error instead of an over-fetch.
    if (limit.IsValid()) {
        if (offset.IsValid()) {
            const idx_t start = offset.GetIndex() + 1;       // 1-based
            const idx_t end   = offset.GetIndex() + limit.GetIndex();
            sql << " ROWS " << start << " TO " << end;
        } else {
            sql << " ROWS " << limit.GetIndex();
        }
    }

    r.sql = sql.str();
    return r;
}

} // namespace duckdb
