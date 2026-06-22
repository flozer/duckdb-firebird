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

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
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
    int64_t      limit_pushed      = 0;
    int64_t      offset_pushed     = 0;
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

static unique_ptr<GlobalTableFunctionState>
ExplainPushdownInitGlobal(ClientContext &, TableFunctionInitInput &) {
    // Task 4 will populate rows here; for now the vector stays empty.
    return make_uniq<ExplainPushdownGlobalState>();
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
        output.data[7].SetValue(row, Value::BIGINT(r.limit_pushed));
        output.data[8].SetValue(row, Value::BIGINT(r.offset_pushed));
        output.data[9].SetValue(row, Value(r.rows_clause));
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
