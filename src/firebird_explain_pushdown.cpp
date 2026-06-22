// PK-range pushdown explainability helpers.
//
// ClassifyPkRange: pure function — derives one of four normalized reasons
// from a PrimaryKeyDescriptor that was populated at ATTACH time (zero new I/O).
// The four normalized reason strings are the stable contract used by Task 5's
// SQL tests.
//
// firebird_explain_pushdown(sql VARCHAR): table function (Task 3 skeleton).
// Parses the given SQL, enforces the read-only SELECT allow-list, rejects
// direct firebird_scan() references, then returns the 14-column schema with
// zero rows (plan walk added in Task 4).

#include "firebird_explain_pushdown.hpp"
#include "firebird_scanner.hpp"
#include "firebird_query.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
//  ClassifyPkRange (Task 2)
// ---------------------------------------------------------------------------

PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d) {
    if (!d.has_pk)
        return {false, "",           "no primary key",   PkRangeStrategy::SERIAL};
    if (d.columns.size() > 1)
        return {false, "",           "composite PK",     PkRangeStrategy::SERIAL};
    if (!d.single_numeric)
        return {false, d.columns[0], "non-numeric PK",   PkRangeStrategy::SERIAL};
    return     {true,  d.columns[0], "single numeric PK", PkRangeStrategy::PK_RANGE_PARTITIONABLE};
}

// ---------------------------------------------------------------------------
//  firebird_explain_pushdown() — Task 3 skeleton
// ---------------------------------------------------------------------------
//
// ExplainRow: holds one row of the 14-column output schema. Task 4 fills it;
// here the vector stays empty so valid SELECTs return 0 rows.

struct ExplainRow {
    int64_t      scan_ordinal      = 0;
    std::string  table_name;
    std::string  remote_sql;
    std::vector<std::string> projected_columns;
    std::vector<std::string> pushed_filters;
    std::vector<std::string> residual_filters;
    std::vector<std::string> not_pushed_reasons;
    // Paging columns surface as SQL NULL (not 0) when no ROWS clause was
    // pushed, so a real limit/offset of 0 is never confused with "no limit".
    bool         limit_pushed_valid  = false;
    int64_t      limit_pushed        = 0;
    bool         offset_pushed_valid = false;
    int64_t      offset_pushed       = 0;
    bool         rows_clause_valid   = false;
    std::string  rows_clause;
    bool         pk_range_eligible = false;
    std::string  pk_range_column;
    std::string  pk_range_reason;
    std::string  scan_strategy;
};

// ---------------------------------------------------------------------------
//  firebird_scan detection: walk the parsed TableRef tree recursively.
//  Returns true if any TableFunctionRef with function_name == "firebird_scan"
//  (case-insensitive) is found.
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// Forward declare so TableRef walker and QueryNode walker can call each other.
static bool HasFirebirdScanInNode(const QueryNode &node);

static bool HasFirebirdScanInTableRef(const TableRef *ref) {
    if (!ref) return false;
    switch (ref->type) {
    case TableReferenceType::TABLE_FUNCTION: {
        const auto &tf = ref->Cast<TableFunctionRef>();
        if (tf.function &&
            tf.function->GetExpressionClass() == ExpressionClass::FUNCTION) {
            const auto &fe = tf.function->Cast<FunctionExpression>();
            if (ToLower(fe.function_name) == "firebird_scan")
                return true;
        }
        // Also recurse into a nested subquery argument if present.
        if (tf.subquery)
            return HasFirebirdScanInNode(*tf.subquery->node);
        return false;
    }
    case TableReferenceType::JOIN: {
        const auto &jr = ref->Cast<JoinRef>();
        return HasFirebirdScanInTableRef(jr.left.get()) ||
               HasFirebirdScanInTableRef(jr.right.get());
    }
    case TableReferenceType::SUBQUERY: {
        const auto &sq = ref->Cast<SubqueryRef>();
        if (sq.subquery)
            return HasFirebirdScanInNode(*sq.subquery->node);
        return false;
    }
    default:
        return false;
    }
}

static bool HasFirebirdScanInNode(const QueryNode &node) {
    // Walk CTE definitions (WITH clauses).
    for (auto &kv : node.cte_map.map) {
        if (kv.second && kv.second->query)
            if (HasFirebirdScanInNode(*kv.second->query->node))
                return true;
    }
    // Walk the node itself.
    if (node.type == QueryNodeType::SELECT_NODE) {
        const auto &sn = node.Cast<SelectNode>();
        if (HasFirebirdScanInTableRef(sn.from_table.get()))
            return true;
    }
    // UNION / INTERSECT / EXCEPT: recurse both branches so a firebird_scan
    // hidden inside a set operation can't evade rejection.
    if (node.type == QueryNodeType::SET_OPERATION_NODE) {
        const auto &so = node.Cast<SetOperationNode>();
        for (const auto &child : so.children) {
            if (child && HasFirebirdScanInNode(*child))
                return true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Bind / GlobalState / Function
// ---------------------------------------------------------------------------

struct ExplainPushdownBindData : public TableFunctionData {
    std::string sql;
};

struct ExplainPushdownGlobalState : public GlobalTableFunctionState {
    std::vector<ExplainRow> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
ExplainPushdownBind(ClientContext &,
                    TableFunctionBindInput &input,
                    vector<LogicalType> &return_types,
                    vector<string> &names) {
    // Require exactly one VARCHAR argument.
    if (input.inputs.size() != 1 ||
        input.inputs[0].type().id() != LogicalTypeId::VARCHAR) {
        throw BinderException(
            "firebird_explain_pushdown(sql VARCHAR): exactly one VARCHAR "
            "argument required");
    }
    const std::string sql = input.inputs[0].GetValue<std::string>();

    // --- Guard 1: parse + require exactly one SELECT statement ---
    Parser p;
    p.ParseQuery(sql);
    if (p.statements.size() != 1 ||
        p.statements[0]->type != StatementType::SELECT_STATEMENT) {
        throw BinderException(
            "firebird_explain_pushdown: only a single read-only "
            "SELECT/WITH...SELECT is allowed");
    }

    // --- Guard 2: reject direct firebird_scan() references ---
    auto &sel = p.statements[0]->Cast<SelectStatement>();
    if (HasFirebirdScanInNode(*sel.node)) {
        throw BinderException(
            "firebird_explain_pushdown: direct firebird_scan(...) is not "
            "supported here (it would open a Firebird connection at bind). "
            "ATTACH … (TYPE firebird) and reference alias.main.table "
            "instead.");
    }

    // --- Schema: 14 columns ---
    names = {
        "scan_ordinal",
        "table_name",
        "remote_sql",
        "projected_columns",
        "pushed_filters",
        "residual_filters",
        "not_pushed_reasons",
        "limit_pushed",
        "offset_pushed",
        "rows_clause",
        "pk_range_eligible",
        "pk_range_column",
        "pk_range_reason",
        "scan_strategy",
    };
    return_types = {
        LogicalType::BIGINT,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
        LogicalType::VARCHAR,
    };

    auto data = make_uniq<ExplainPushdownBindData>();
    data->sql = sql;
    return std::move(data);
}

// ---------------------------------------------------------------------------
//  Plan walk (Task 4)
// ---------------------------------------------------------------------------
//
// Recurse the optimized LogicalOperator plan in preorder (node before its
// children). For every Firebird LogicalGet (function.name == "firebird_scan")
// emit one ExplainRow, assigning scan_ordinal 1-based in visitation order.
// Build() is called capture-only — it produces the remote SQL + pushdown
// telemetry without touching Firebird (no OpenCursor). This mirrors the
// scanner's own observability capture (firebird_scanner.cpp ~696-769).

// Limit/offset inherited from an enclosing LogicalLimit (SQL LIMIT/OFFSET).
// DuckDB has no LIMIT-pushdown hook for table functions, so a SQL LIMIT lands
// in a LogicalLimit parent rather than in FirebirdBindData. We carry the
// nearest enclosing constant LIMIT/OFFSET down to the firebird scan below it,
// matching what the scanner's ROWS clause would express for that slice.
struct InheritedPaging {
    bool         has_limit  = false;
    idx_t        limit      = 0;
    bool         has_offset = false;
    idx_t        offset     = 0;
};

static void WalkPlan(LogicalOperator &op,
                     std::vector<ExplainRow> &rows,
                     int64_t &next_ordinal,
                     InheritedPaging paging) {
    // Capture a constant SQL LIMIT/OFFSET to apply to firebird scans below.
    if (op.type == LogicalOperatorType::LOGICAL_LIMIT) {
        auto &lim = op.Cast<LogicalLimit>();
        if (lim.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
            paging.has_limit = true;
            paging.limit     = lim.limit_val.GetConstantValue();
        }
        if (lim.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
            paging.has_offset = true;
            paging.offset     = lim.offset_val.GetConstantValue();
        }
    }
    if (op.type == LogicalOperatorType::LOGICAL_GET) {
        auto &get = op.Cast<LogicalGet>();
        if (get.function.name == "firebird_scan" && get.bind_data) {
            auto &bd = get.bind_data->Cast<FirebirdBindData>();

            // Effective paging: bind-data override (row_limit=) wins; else the
            // enclosing SQL LIMIT/OFFSET.
            optional_idx eff_limit  = bd.limit_override;
            optional_idx eff_offset = bd.offset_override;
            if (!eff_limit.IsValid() && paging.has_limit) {
                eff_limit = optional_idx(paging.limit);
                if (paging.has_offset) {
                    eff_offset = optional_idx(paging.offset);
                }
            }

            // Project the ColumnIndex list down to raw column ids (column_t),
            // matching the scanner's gstate.column_ids contract. RowId comes
            // through as COLUMN_IDENTIFIER_ROW_ID.
            std::vector<column_t> column_ids;
            const auto &col_idx = get.GetColumnIds();
            column_ids.reserve(col_idx.size());
            for (const auto &ci : col_idx) {
                column_ids.push_back(ci.GetPrimaryIndex());
            }

            // Capture-only Build — serial WHERE, no PK bounds, no extra
            // predicate string (extra_predicates are appended below as
            // pushed_filters telemetry, exactly as the scanner does).
            auto result = FirebirdQueryBuilder::Build(
                bd.table_name,
                bd.column_names,
                bd.column_types,
                column_ids,
                &get.table_filters,
                eff_limit,
                /*extra_predicate=*/"",
                &bd.column_descs,
                bd.none_encoding,
                eff_offset);

            ExplainRow r;
            r.scan_ordinal = next_ordinal++;
            r.table_name   = bd.table_name;
            r.remote_sql   = result.sql;

            // projected_columns: map column_ids -> column_names; rowid sentinel
            // surfaces as "<rowid>".
            r.projected_columns.reserve(column_ids.size());
            for (auto cid : column_ids) {
                if (cid == COLUMN_IDENTIFIER_ROW_ID) {
                    r.projected_columns.push_back("<rowid>");
                } else if (cid < bd.column_names.size()) {
                    r.projected_columns.push_back(bd.column_names[cid]);
                }
            }

            // pushed_filters: builder's pushed TableFilterSet conditions, then
            // each complex extra_predicate (LIKE / NOT IN / BETWEEN) lifted at
            // pushdown-complex time.
            r.pushed_filters = result.pushed_filter_sql;
            for (auto &ep : bd.extra_predicates) {
                r.pushed_filters.push_back(ep.sql);
            }

            // residual_filters: one placeholder per residual TableFilterSet
            // index, parallel reasons; then NONE-gated complex filters.
            r.residual_filters.reserve(result.residual_filter_indices.size());
            for (auto idx : result.residual_filter_indices) {
                r.residual_filters.push_back("filter[" + std::to_string(idx) + "]");
            }
            r.not_pushed_reasons = result.residual_filter_reasons;
            for (const auto &reason : bd.gated_complex_reasons) {
                r.residual_filters.push_back("complex_filter[none_gated]");
                r.not_pushed_reasons.push_back(reason);
            }

            // paging hints + the ROWS clause the scanner would emit.
            r.limit_pushed_valid  = eff_limit.IsValid();
            if (r.limit_pushed_valid) {
                r.limit_pushed = static_cast<int64_t>(eff_limit.GetIndex());
            }
            r.offset_pushed_valid = eff_offset.IsValid();
            if (r.offset_pushed_valid) {
                r.offset_pushed = static_cast<int64_t>(eff_offset.GetIndex());
            }
            // ROWS clause — copy firebird_query.cpp:390-396 formatting.
            if (eff_limit.IsValid()) {
                if (eff_offset.IsValid()) {
                    const idx_t start = eff_offset.GetIndex() + 1;
                    const idx_t end   = eff_offset.GetIndex() +
                                        eff_limit.GetIndex();
                    r.rows_clause = "ROWS " + std::to_string(start) +
                                    " TO " + std::to_string(end);
                } else {
                    r.rows_clause = "ROWS " +
                                    std::to_string(eff_limit.GetIndex());
                }
                r.rows_clause_valid = true;
            }

            rows.push_back(std::move(r));
        }
    }
    for (auto &child : op.children) {
        WalkPlan(*child, rows, next_ordinal, paging);
    }
}

static unique_ptr<GlobalTableFunctionState>
ExplainPushdownInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto gstate = make_uniq<ExplainPushdownGlobalState>();
    const auto &bind = input.bind_data->Cast<ExplainPushdownBindData>();

    // CRITICAL (spike verdict): ExtractPlan takes the context's NON-recursive
    // lock. Calling it on the outer ClientContext (the one running this
    // function) would deadlock. A fresh Connection over the same
    // DatabaseInstance has its own ClientContext+lock and shares the ATTACHed
    // Firebird catalogs, so extraction here is safe — and execute-phase is
    // outside any held context lock.
    Connection con(*context.db);

    // Guard the optimizer: an un-optimized plan would lack filter/projection
    // pushdown and the pushdown-complex callback, defeating the report. Force
    // it on for this throwaway connection.
    if (!con.context->config.enable_optimizer) {
        con.context->config.enable_optimizer = true;
    }

    auto plan = con.context->ExtractPlan(bind.sql);
    if (plan) {
        int64_t next_ordinal = 1;
        WalkPlan(*plan, gstate->rows, next_ordinal, InheritedPaging{});
    }
    return std::move(gstate);
}

static void ExplainPushdownFunction(ClientContext &,
                                    TableFunctionInput &input,
                                    DataChunk &output) {
    auto &g = input.global_state->Cast<ExplainPushdownGlobalState>();
    const idx_t target = STANDARD_VECTOR_SIZE;
    idx_t row = 0;

    auto make_varchar_list = [](const std::vector<std::string> &xs) {
        vector<Value> vals;
        vals.reserve(xs.size());
        for (auto &s : xs)
            vals.emplace_back(Value(s));
        return Value::LIST(LogicalType::VARCHAR, std::move(vals));
    };

    while (row < target && g.cursor < g.rows.size()) {
        const auto &r = g.rows[g.cursor++];
        output.data[0].SetValue(row, Value::BIGINT(r.scan_ordinal));
        output.data[1].SetValue(row, Value(r.table_name));
        output.data[2].SetValue(row, Value(r.remote_sql));
        output.data[3].SetValue(row, make_varchar_list(r.projected_columns));
        output.data[4].SetValue(row, make_varchar_list(r.pushed_filters));
        output.data[5].SetValue(row, make_varchar_list(r.residual_filters));
        output.data[6].SetValue(row, make_varchar_list(r.not_pushed_reasons));
        output.data[7].SetValue(row, r.limit_pushed_valid
            ? Value::BIGINT(r.limit_pushed)
            : Value(LogicalType::BIGINT));
        output.data[8].SetValue(row, r.offset_pushed_valid
            ? Value::BIGINT(r.offset_pushed)
            : Value(LogicalType::BIGINT));
        output.data[9].SetValue(row, r.rows_clause_valid
            ? Value(r.rows_clause)
            : Value(LogicalType::VARCHAR));
        output.data[10].SetValue(row, Value::BOOLEAN(r.pk_range_eligible));
        output.data[11].SetValue(row, Value(r.pk_range_column));
        output.data[12].SetValue(row, Value(r.pk_range_reason));
        output.data[13].SetValue(row, Value(r.scan_strategy));
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdExplainPushdownFunction() {
    TableFunction fn("firebird_explain_pushdown",
                     {LogicalType::VARCHAR},
                     ExplainPushdownFunction,
                     ExplainPushdownBind,
                     ExplainPushdownInitGlobal);
    return fn;
}

} // namespace duckdb
