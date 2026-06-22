# firebird_explain_pushdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `firebird_explain_pushdown(sql)` — an a-priori, plan-only explainer that reports the remote query the Firebird scanner *would* produce, without opening a cursor or sending any SQL to Firebird.

**Architecture:** Reject direct `firebird_scan(...)` pre-bind; bind+optimize the inner SELECT/CTE via DuckDB's planner; walk the optimized plan for each Firebird `LogicalGet`; copy its `FirebirdBindData` + `column_ids` + `TableFilterSet` (+ extra_predicates/gated reasons/limit/offset) and call the same `FirebirdQueryBuilder::Build()` capture-only. PK-range eligibility comes from a widened `PrimaryKeyDescriptor` cached on the catalog entry at ATTACH (zero I/O). One output row per scan, `scan_ordinal` 1-based.

**Tech Stack:** C++ DuckDB out-of-tree extension; `FirebirdQueryBuilder`; DuckDB planner (`ClientContext::ExtractPlan` or equivalent — confirmed by the Task 1 spike); DuckDB sqllogic `.test`; PowerShell build matrix.

## Global Constraints

- **A-priori, plan-only:** NEVER open a cursor; NEVER send SQL to the target Firebird. Catalog metadata is already cached at ATTACH.
- **Read-only allow-list:** accept exactly one `SELECT` or `WITH … SELECT`. Reject multi-statement, DDL, DML, `COPY`, `EXPLAIN`, `PRAGMA` with `BinderException`.
- **Reject direct `firebird_scan(...)`** in the inner SQL, BEFORE binding, with the actionable message: `firebird_explain_pushdown: direct firebird_scan(...) is not supported here (it would open a Firebird connection at bind). ATTACH … (TYPE firebird) and reference alias.main.table instead.`
- **`remote_sql` carries only `?` placeholders / builder binds** — never inline literals, never the connection string.
- **Parallel lists invariant:** `len(residual_filters) == len(not_pushed_reasons)`. `gated_complex_reasons` add `complex_filter[none_gated]` to `residual_filters` with a matching reason.
- **`pk_range_reason` normalized**, exactly one of: `no primary key`, `composite PK`, `non-numeric PK`, `single numeric PK`. `pk_range_eligible=true` / `scan_strategy='pk-range-partitionable'` ONLY for `single numeric PK`; else `false` / `serial`.
- **No firebird scan in plan → zero rows** (not an error).
- **No action on `duckdb/community-extensions`.**
- **Build (incremental):** `scripts/build_windows_local.bat`. **Run a test:** from repo root, `build\release\test\unittest.exe test/sql/<name>.test`. **Env (PowerShell):** `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:FIREBIRD_DECFLOAT_DB="C:\fbtest\decfloat.fdb"; $env:FIREBIRD_NONE_DB="C:\fbtest\none.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`.
- Cross-version gate: `scripts/build_matrix.ps1` (v1.5.2/v1.5.3/v1.5.4).

## Known signatures (from the codebase)

`FirebirdQueryBuilder::Build` (`src/include/firebird_query.hpp`):
```cpp
struct Result {
    std::string sql;                              // remote SELECT, '?' placeholders
    std::vector<idx_t> residual_filter_indices;
    std::vector<std::string> residual_filter_reasons; // parallel to indices
    std::vector<Value> params;
    std::vector<std::string> pushed_filter_sql;
};
static Result Build(const std::string &table_name,
                    const duckdb::vector<std::string> &column_names,
                    const duckdb::vector<LogicalType> &column_types,
                    const duckdb::vector<column_t> &column_ids,
                    optional_ptr<TableFilterSet> filters,
                    optional_idx limit_override,
                    const std::string &extra_where,       // combined WHERE; "" for plan-only serial
                    const duckdb::vector<FirebirdColumnDesc> *column_descs,
                    NoneEncoding none_encoding,
                    optional_idx offset_override);
```
`FirebirdBindData` (`src/include/firebird_scanner.hpp`): `table_name`, `column_names`, `column_types`, `column_descs`, `pk` (`unique_ptr<PrimaryKeyInfo>`), `limit_override`, `offset_override`, `extra_predicates` (`{sql, params}`), `gated_complex_reasons`, `none_encoding`.

---

## Task 1: SPIKE — planner plan-extraction feasibility (GATE)

**This task gates the whole feature.** It is exploratory (throwaway prototype + written verdict), not shippable TDD. If it fails, STOP and escalate — do NOT build an alternative planner under this API.

**Files:**
- Scratch prototype only (a temporary branch/file you delete); write the verdict to `docs/superpowers/notes/explain-pushdown-spike.md`.

**Goal:** Confirm DuckDB (submodule v1.5.3) stably exposes the OPTIMIZED logical plan for an inner SELECT from inside an extension, with each Firebird `LogicalGet`'s `bind_data` (FirebirdBindData), column ids, and `TableFilterSet` reachable — WITHOUT executing the scan.

- [ ] **Step 1: Identify the extraction API**

In the duckdb submodule headers, confirm the API to get an optimized plan from a SQL string within the current database. Check `ClientContext::ExtractPlan(const string &query)` (returns `unique_ptr<LogicalOperator>`), and `Connection`/`ClientContext` reentrancy. Note the exact method, header, and whether it runs the optimizer (filter/projection pushdown) before returning. Record findings.

- [ ] **Step 2: Prototype plan walk + bind_data access**

Prototype (in a scratch table function or a unit harness): given `SELECT EMP_ID FROM fb.EMPLOYEE WHERE EMP_ID = 4` against an ATTACHed `fb`, extract the optimized plan and walk it for `LogicalGet`. For the firebird Get, confirm you can read: the bound `TableFunction` (to identify it as the firebird scan — e.g. by `function.name` or by `dynamic_cast`/`Cast` of `bind_data` to `FirebirdBindData`), `GetColumnIds()` (or `column_ids`), and `table_filters` (`TableFilterSet`). Confirm `extra_predicates`/`gated_complex_reasons` are populated on the bind_data after optimization (the PushdownComplexFilter callback ran).

- [ ] **Step 3: Confirm NO remote data SQL fired**

Verify that extracting the plan for an ATTACHed-table SELECT does NOT open a data cursor on EMPLOYEE (only catalog metadata, already cached). Confirm a direct `firebird_scan('...','EMPLOYEE')` WOULD connect at bind (justifying the pre-bind rejection). Method: instrument/log `OpenCursor`, or observe connection counts.

- [ ] **Step 4: Write the verdict**

Write `docs/superpowers/notes/explain-pushdown-spike.md`: the exact API (method + header + signature), how to identify the firebird LogicalGet, how to read column_ids/table_filters/bind_data, traversal order (preorder), and a GO / NO-GO. If NO-GO (plan not stably exposed), state precisely why and STOP — escalate to the human; the fallback "table+predicate" is a different function, out of this plan.

- [ ] **Step 5: Commit the verdict**

```bash
git add docs/superpowers/notes/explain-pushdown-spike.md
git commit -m "spike(explain): planner plan-extraction feasibility verdict"
```

(No prototype code is committed — it is throwaway. Subsequent tasks use the verdict's confirmed API.)

---

## Task 2: PrimaryKeyDescriptor cache + pure classifier

Widen the catalog's cached PK info so the 4 normalized reasons are derivable offline, and add a pure, unit-testable classifier.

**Files:**
- Modify: `src/include/firebird_scanner.hpp` (add `PrimaryKeyDescriptor` + `PkRangeClassification` + `ClassifyPkRange` decl)
- Modify: `src/firebird_storage.cpp` (populate descriptor on `FirebirdTableEntry` at ATTACH from already-loaded constraints + column types; expose a getter)
- Create: `test/sql/firebird_explain_pushdown.test` (first block exercises the classifier indirectly via explain in later tasks; here add a C++-free check is not possible, so this task's test is the classifier unit — see Step 1)
- Modify: `CMakeLists.txt` only if a new test cpp is added (we keep the classifier in `firebird_explain_pushdown.cpp`, so no separate unit binary — instead the classifier is covered by the SQL tests in Tasks 5). To get a RED/GREEN here, put the classifier in the new explain source and assert via a tiny `demo()` is not how this project tests. Therefore: **this task's verification is a compile + a deterministic SQL test added in Task 5**; mark Task 2 deliverable as "descriptor populated + classifier compiles", verified by Task 5's reason tests.

**Interfaces:**
- Produces:
```cpp
struct PrimaryKeyDescriptor {
    bool                        has_pk = false;
    duckdb::vector<std::string> columns;        // ordinal order
    bool                        single_numeric = false; // 1 col AND numeric (INT/BIGINT/...)
};
enum class PkRangeStrategy { SERIAL, PK_RANGE_PARTITIONABLE };
struct PkRangeClassification {
    bool            eligible;
    std::string     column;   // "" when not single-col
    std::string     reason;   // normalized: no primary key|composite PK|non-numeric PK|single numeric PK
    PkRangeStrategy strategy;
};
PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d);
// On FirebirdTableEntry: const PrimaryKeyDescriptor &GetPkDescriptor() const;
```

- [ ] **Step 1: Declare the types + classifier in the header**

Add to `src/include/firebird_scanner.hpp` (near `PrimaryKeyInfo`) the `PrimaryKeyDescriptor`, `PkRangeStrategy`, `PkRangeClassification`, and `ClassifyPkRange` declaration shown above.

- [ ] **Step 2: Implement `ClassifyPkRange` (pure) in the new explain source**

Create `src/firebird_explain_pushdown.cpp` (namespace `duckdb`) and implement:
```cpp
PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d) {
    if (!d.has_pk)            return {false, "",            "no primary key",   PkRangeStrategy::SERIAL};
    if (d.columns.size() > 1) return {false, "",            "composite PK",     PkRangeStrategy::SERIAL};
    if (!d.single_numeric)    return {false, d.columns[0],  "non-numeric PK",   PkRangeStrategy::SERIAL};
    return {true, d.columns[0], "single numeric PK", PkRangeStrategy::PK_RANGE_PARTITIONABLE};
}
```
Add `src/firebird_explain_pushdown.cpp` to `CMakeLists.txt` `EXTENSION_SOURCES`.

- [ ] **Step 3: Populate the descriptor on `FirebirdTableEntry`**

In `src/firebird_storage.cpp`, the catalog already loads PK/UNIQUE constraints (`LoadUniqueConstraints`) and has column types. At entry construction (where `unique_keys`/columns are available in `EnsureTablesLoaded`/`add_entry`), build a `PrimaryKeyDescriptor`: `has_pk` = a PRIMARY KEY constraint exists for the table; `columns` = its segment columns in ordinal order; `single_numeric` = (`columns.size()==1` AND the column's `LogicalType` is integer-family — `INTEGER`/`BIGINT`/`SMALLINT`/`HUGEINT`). Store it on `FirebirdTableEntry` and add `const PrimaryKeyDescriptor &GetPkDescriptor() const`. Zero new I/O (reuse the already-loaded constraint map + column types).

- [ ] **Step 4: Build to verify it compiles**

Run `scripts/build_windows_local.bat`. Expected: builds clean, extension produced. (Behavioral verification lands in Task 5's reason tests.)

- [ ] **Step 5: Commit**

```bash
git add src/include/firebird_scanner.hpp src/firebird_storage.cpp src/firebird_explain_pushdown.cpp CMakeLists.txt
git commit -m "feat(explain): PrimaryKeyDescriptor cache + ClassifyPkRange (4 normalized reasons)"
```

---

## Task 3: Function skeleton — parse guard, schema, empty result

Register `firebird_explain_pushdown(sql)`; enforce the read-only allow-list and the direct-`firebird_scan` rejection; return the full schema with zero rows (plan walk lands in Task 4).

**Files:**
- Create: `src/include/firebird_explain_pushdown.hpp` (`TableFunction GetFirebirdExplainPushdownFunction();`)
- Modify: `src/firebird_explain_pushdown.cpp` (bind/global/function + parse guard)
- Modify: `src/firebird_extension.cpp` (`#include` + `loader.RegisterFunction(GetFirebirdExplainPushdownFunction());`)
- Modify: `test/sql/firebird_explain_pushdown.test`

**Interfaces:**
- Produces: `GetFirebirdExplainPushdownFunction()`. Output schema (15 columns), in order:
  `scan_ordinal BIGINT, table_name VARCHAR, remote_sql VARCHAR, projected_columns LIST(VARCHAR), pushed_filters LIST(VARCHAR), residual_filters LIST(VARCHAR), not_pushed_reasons LIST(VARCHAR), limit_pushed BIGINT, offset_pushed BIGINT, rows_clause VARCHAR, pk_range_eligible BOOLEAN, pk_range_column VARCHAR, pk_range_reason VARCHAR, scan_strategy VARCHAR`. (14 names listed; `scan_ordinal` is #1 → 14 columns total. Count them exactly when wiring `names`/`return_types`.)

- [ ] **Step 1: Write the failing test (guards + empty)**

Create `test/sql/firebird_explain_pushdown.test`:
```
# name: test/sql/firebird_explain_pushdown.test
# description: a-priori plan-only pushdown explainer
# group: [firebird]

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

# no firebird scan in plan -> zero rows
query I
SELECT COUNT(*) FROM firebird_explain_pushdown('SELECT 1');
----
0

# read-only guard: DML rejected
statement error
SELECT * FROM firebird_explain_pushdown('UPDATE fb.main.EMPLOYEE SET EMP_NAME = ''x''');
----

# direct firebird_scan rejected (would open a connection at bind)
statement error
SELECT * FROM firebird_explain_pushdown('SELECT * FROM firebird_scan(''${FIREBIRD_TEST_DB}'', ''EMPLOYEE'')');
----
```

- [ ] **Step 2: Run, verify it fails**

Run: `build\release\test\unittest.exe test/sql/firebird_explain_pushdown.test`
Expected: FAIL — `firebird_explain_pushdown` does not exist.

- [ ] **Step 3: Implement the header + skeleton**

`src/include/firebird_explain_pushdown.hpp`:
```cpp
#pragma once
#include "duckdb/function/table_function.hpp"
namespace duckdb { TableFunction GetFirebirdExplainPushdownFunction(); }
```
In `src/firebird_explain_pushdown.cpp` add bind/global/function. Bind: require one VARCHAR arg; set the 14-column schema. Validate the SQL string:
1. Parse with DuckDB's `Parser` (`Parser p; p.ParseQuery(sql);`). Require exactly one statement and `statements[0]->type == StatementType::SELECT_STATEMENT` → else `throw BinderException("firebird_explain_pushdown: only a single read-only SELECT/WITH...SELECT is allowed");`.
2. Reject direct `firebird_scan`: walk the parsed `SelectStatement` for any table function reference named `firebird_scan` (case-insensitive) — a `TableFunctionRef` whose function is a `FunctionExpression` with `function_name == "firebird_scan"`. If found → `throw BinderException` with the Global-Constraints message. (Do this BEFORE any bind/optimize so the firebird_scan bind never runs.)
GlobalState holds `vector<ExplainRow> rows; idx_t cursor=0; MaxThreads()=1`. Function emits chunked rows (Task 4 fills `rows`; here `rows` stays empty so valid SELECTs return 0 rows too — acceptable for this skeleton task, the guards are what's tested).

- [ ] **Step 4: Register + build + run, verify pass**

Add the include + `loader.RegisterFunction(GetFirebirdExplainPushdownFunction());` to `src/firebird_extension.cpp`. Build. Run the test. Expected: PASS (SELECT 1 → 0 rows; UPDATE → error; firebird_scan → error). The `statement error` blocks just need an error raised; tighten the matcher to the message substring if the harness supports it.

- [ ] **Step 5: Commit**

```bash
git add src/include/firebird_explain_pushdown.hpp src/firebird_explain_pushdown.cpp src/firebird_extension.cpp test/sql/firebird_explain_pushdown.test
git commit -m "feat(explain): firebird_explain_pushdown skeleton — parse guards + schema"
```

---

## Task 4: Plan walk + capture-only Build → emit rows

Bind+optimize the validated SELECT, walk the plan for each Firebird `LogicalGet`, copy its bind data and run `FirebirdQueryBuilder::Build()` capture-only, emit one row per scan with `scan_ordinal`.

**Files:**
- Modify: `src/firebird_explain_pushdown.cpp`
- Modify: `test/sql/firebird_explain_pushdown.test`

**Interfaces:**
- Consumes: spike verdict (`docs/superpowers/notes/explain-pushdown-spike.md`) for the exact extraction API; `FirebirdQueryBuilder::Build`; `FirebirdBindData`.
- Produces: populated `ExplainRow` per scan: `{scan_ordinal, table_name, remote_sql, projected_columns[], pushed_filters[], residual_filters[], not_pushed_reasons[], limit_pushed, offset_pushed, rows_clause}` (PK-range fields filled in Task 5).

- [ ] **Step 1: Write the failing test (simple WHERE pushed + projection + LIMIT)**

Append:
```
# simple equality pushed; remote_sql uses '?' not the literal 4
query I
SELECT scan_ordinal FROM firebird_explain_pushdown(
  'SELECT EMP_ID FROM fb.main.EMPLOYEE WHERE EMP_ID = 4');
----
1

query I
SELECT len(pushed_filters) > 0 FROM firebird_explain_pushdown(
  'SELECT EMP_ID FROM fb.main.EMPLOYEE WHERE EMP_ID = 4');
----
true

# remote_sql contains a placeholder and NOT the literal value or a path
query I
SELECT remote_sql LIKE '%?%'
   AND remote_sql NOT LIKE '%fbtest%'
   AND remote_sql NOT LIKE '%= 4%'
  FROM firebird_explain_pushdown(
  'SELECT EMP_ID FROM fb.main.EMPLOYEE WHERE EMP_ID = 4');
----
true

# projection prune
query I
SELECT projected_columns
  FROM firebird_explain_pushdown('SELECT EMP_ID FROM fb.main.EMPLOYEE');
----
[EMP_ID]

# SQL LIMIT is NOT pushed; the Firebird scanner only emits ROWS for the
# row_limit= named param (absent in ATTACH-path SQL), so these columns are NULL.
query I
SELECT limit_pushed IS NULL FROM firebird_explain_pushdown(
  'SELECT * FROM fb.main.EMPLOYEE LIMIT 100');
----
true
```

- [ ] **Step 2: Run, verify it fails**

Run the test. Expected: FAIL — explain returns 0 rows (walk not implemented).

- [ ] **Step 3: Implement plan walk + capture**

**Mechanism per the spike verdict (`docs/superpowers/notes/explain-pushdown-spike.md`) — MANDATORY:** `ExtractPlan` takes a NON-recursive `context_lock`. Calling it on the outer `ClientContext` (the one running this function) deadlocks. Therefore do the extraction in the **execute phase** (InitGlobal), on a **fresh `Connection`** over the same database, which has its own ClientContext+lock and shares the ATTACHed catalogs:
```cpp
Connection con(*context.db);
con.context->RunFunctionInTransaction([&](){ /* if needed */ });
auto plan = con.context->ExtractPlan(sql);   // optimized plan
```
Guard the optimizer: if `con.context->config.enable_optimizer == false` (or DBConfig), the plan would be un-optimized (pushdown not applied) → either temporarily enable it on `con` or throw a clear error. Confirm the exact `Connection`/`ExtractPlan` calling form against the spike doc.

The SELECT/CTE allow-list + direct-`firebird_scan` rejection (Task 3) stay in **bind** (parse-only, no ExtractPlan) — they must run before this execute-phase extraction.

Recursively walk the plan in **preorder** over `op.children`, assigning `scan_ordinal` starting at 1 to each `LogicalGet` identified as Firebird (`get.function.name == "firebird_scan"`; then `get.bind_data->Cast<FirebirdBindData>()`). For each:
- `column_ids` from the Get (`GetColumnIds()`); `table_filters` (`TableFilterSet`); `bind_data` → `FirebirdBindData`.
- Call `FirebirdQueryBuilder::Build(bd.table_name, bd.column_names, bd.column_types, column_ids, filters, bd.limit_override, /*extra_where=*/"", &bd.column_descs, bd.none_encoding, bd.offset_override)` — capture-only, serial WHERE (no PK bounds), no cursor.
- `remote_sql = result.sql`; `pushed_filters = result.pushed_filter_sql` + each `bd.extra_predicates[i].sql`; `residual_filters` = `"filter[" + idx + "]"` per `result.residual_filter_indices`; `not_pushed_reasons = result.residual_filter_reasons`.
- `projected_columns`: map `column_ids` → `bd.column_names` (`COLUMN_IDENTIFIER_ROW_ID` → `"<rowid>"`).
- `limit_pushed`/`offset_pushed` from `bd.limit_override`/`bd.offset_override` (`optional_idx` → BIGINT or NULL).
- `rows_clause`: if limit set, `"ROWS " + (offset?offset+1:1) + " TO " + (offset?offset:0)+limit` else NULL. (Match the scanner's ROWS form; copy its formatting.)
Store rows on the bind data (or recompute in InitGlobal). Emit chunked in the function via `output.data[c].SetValue(row, value)`; LIST columns via `Value::LIST(LogicalType::VARCHAR, children)`.

Do the walk **outside any scan callback** (we walk an already-optimized plan). Never call `OpenCursor`.

- [ ] **Step 4: Build + run, verify pass**

Build; run the test. Set `remote_sql`/`projected_columns` expecteds to the ACTUAL emitted strings if they differ in spacing (deterministic — adjust once, keep exact).

- [ ] **Step 5: Commit**

```bash
git add src/firebird_explain_pushdown.cpp test/sql/firebird_explain_pushdown.test
git commit -m "feat(explain): plan walk + capture-only Build, one row per scan with scan_ordinal"
```

---

## Task 5: PK-range fields + scan_strategy (the 4 reasons)

Fill `pk_range_eligible`/`pk_range_column`/`pk_range_reason`/`scan_strategy` from the catalog entry's `PrimaryKeyDescriptor` via `ClassifyPkRange`.

**Files:**
- Modify: `src/firebird_explain_pushdown.cpp`
- Modify: `test/sql/firebird_explain_pushdown.test`

**Interfaces:**
- Consumes: `FirebirdTableEntry::GetPkDescriptor()`, `ClassifyPkRange` (Task 2).

- [ ] **Step 1: Write the failing test (4 normalized reasons)**

Append (fixtures: EMPLOYEE EMP_ID INTEGER PK; DEPT DEPT_NO VARCHAR PK; TPK_COMPOSITE composite PK; TCHILD no PK):
```
query III
SELECT pk_range_reason, pk_range_eligible, scan_strategy
  FROM firebird_explain_pushdown('SELECT * FROM fb.main.EMPLOYEE');
----
single numeric PK	true	pk-range-partitionable

query III
SELECT pk_range_reason, pk_range_eligible, scan_strategy
  FROM firebird_explain_pushdown('SELECT * FROM fb.main.DEPT');
----
non-numeric PK	false	serial

query III
SELECT pk_range_reason, pk_range_eligible, scan_strategy
  FROM firebird_explain_pushdown('SELECT * FROM fb.main.TPK_COMPOSITE');
----
composite PK	false	serial

query III
SELECT pk_range_reason, pk_range_eligible, scan_strategy
  FROM firebird_explain_pushdown('SELECT * FROM fb.main.TCHILD');
----
no primary key	false	serial
```

- [ ] **Step 2: Run, verify it fails** (fields empty/default).

- [ ] **Step 3: Implement**

When walking the plan, for each Firebird `LogicalGet`, resolve its `FirebirdTableEntry` (from the bound table / catalog) and call `GetPkDescriptor()` → `ClassifyPkRange(desc)`. Fill: `pk_range_eligible = c.eligible`; `pk_range_column = c.column.empty() ? NULL : c.column`; `pk_range_reason = c.reason`; `scan_strategy = c.strategy == PK_RANGE_PARTITIONABLE ? "pk-range-partitionable" : "serial"`. If the entry can't be resolved from the Get (shouldn't happen for an ATTACHed table), fall back to deriving the descriptor from `bd` PK info — but prefer the catalog entry. (If only `FirebirdBindData` is reachable, not the entry, populate the descriptor at bind on the bind data instead — decide per the spike's reachability finding; either way the 4 reasons must come from cached metadata, zero I/O.)

- [ ] **Step 4: Build + run, verify pass.**

- [ ] **Step 5: Commit**

```bash
git add src/firebird_explain_pushdown.cpp test/sql/firebird_explain_pushdown.test
git commit -m "feat(explain): PK-range eligibility + scan_strategy from cached descriptor"
```

---

## Task 6: Residual+reason pairing incl. NONE-gated; cardinality invariant

Ensure `gated_complex_reasons` surface as `complex_filter[none_gated]` and the two lists stay equal length; cover CHARACTER SET NONE / NOT IN.

**Files:**
- Modify: `src/firebird_explain_pushdown.cpp`
- Modify: `test/sql/firebird_explain_pushdown.test`

- [ ] **Step 1: Write the failing test**

Append (uses the NONE fixture column or a NOT IN that gates). Use `none.fdb` attached as a second catalog, OR a NOT IN on EMPLOYEE that produces a residual. Concretely, assert the invariant always holds and the NONE path appears:
```
# residual/reason lists are always equal length
query I
SELECT len(residual_filters) = len(not_pushed_reasons)
  FROM firebird_explain_pushdown(
  'SELECT * FROM fb.main.EMPLOYEE WHERE EMP_ID NOT IN (1,2,3)');
----
true
```
For the NONE-gated path, attach `none.fdb` and explain a `NOT IN` / `LIKE` on the NONE text column; assert `residual_filters` contains `complex_filter[none_gated]` and the parallel reason is `NONE_CHARSET`. (Set the exact predicate to one the scanner gates — mirror `firebird_bind_params`/`firebird_none_charset` patterns.)

- [ ] **Step 2: Run, verify it fails** (gated entries missing or lengths unequal).

- [ ] **Step 3: Implement**

After building `residual_filters`/`not_pushed_reasons` from `Build()`, append the gated entries exactly as the scanner does (`src/firebird_scanner.cpp` ~L763): for each `bd.gated_complex_reasons` entry, push `"complex_filter[none_gated]"` to `residual_filters` and the reason to `not_pushed_reasons`. Assert (in code, `D_ASSERT`) the two lists are equal length before emitting.

- [ ] **Step 4: Build + run, verify pass.**

- [ ] **Step 5: Commit**

```bash
git add src/firebird_explain_pushdown.cpp test/sql/firebird_explain_pushdown.test
git commit -m "feat(explain): NONE-gated residual entries + cardinality invariant"
```

---

## Task 7: Self-join ordinal, valid WITH, no-cursor proof

Cover multi-scan ordering, CTE, and the no-remote-SQL guarantee.

**Files:**
- Modify: `test/sql/firebird_explain_pushdown.test`
- Modify: `src/firebird_explain_pushdown.cpp` (only if a fix is needed)

- [ ] **Step 1: Write the tests**

```
# self-join: two rows, same table_name, distinct scan_ordinal 1 and 2
query II
SELECT scan_ordinal, table_name FROM firebird_explain_pushdown(
  'SELECT a.EMP_ID FROM fb.main.EMPLOYEE a JOIN fb.main.EMPLOYEE b ON a.DEPT_NO=b.DEPT_NO')
ORDER BY scan_ordinal;
----
1	EMPLOYEE
2	EMPLOYEE

# valid WITH ... SELECT explains the scan inside the CTE
query I
SELECT table_name FROM firebird_explain_pushdown(
  'WITH x AS (SELECT EMP_ID FROM fb.main.EMPLOYEE) SELECT * FROM x');
----
EMPLOYEE

# WITH ... DELETE rejected
statement error
SELECT * FROM firebird_explain_pushdown(
  'WITH x AS (SELECT 1) DELETE FROM fb.main.EMPLOYEE');
----
```

- [ ] **Step 2: Run.** If the self-join collapses to one Get (DuckDB may dedupe), confirm the preorder walk visits both Gets; if the optimizer truly produces two `LogicalGet`s, both appear. If DuckDB reuses one Get for a self-join, document the actual count and adjust the expected to the real plan (set-based note). The `WITH...DELETE` must error at the parse guard (statement type ≠ SELECT).

- [ ] **Step 3: Fix only if a test fails** (e.g. ordinal assignment not preorder). Otherwise no code change.

- [ ] **Step 4: Commit**

```bash
git add test/sql/firebird_explain_pushdown.test src/firebird_explain_pushdown.cpp
git commit -m "test(explain): self-join scan_ordinal, valid WITH, WITH...DELETE rejected"
```

---

## Task 8: No-cursor verification, docs, cross-version matrix

**Files:**
- Modify: `src/firebird_explain_pushdown.cpp` (only if instrumentation needed)
- Modify: `docs/pt/function_manual.md`, `docs/en/function_manual.md`
- Modify: `scripts/build_matrix.ps1`
- Modify: `test/sql/firebird_explain_pushdown.test` (no-cursor assertion if stable)

- [ ] **Step 1: No-cursor proof**

Prefer an assertion that explain never touched Firebird: after `firebird_explain_pushdown('SELECT * FROM fb.main.EMPLOYEE')`, `firebird_last_query()` for the same ClientContext must show NO new remote_sql attributable to explain (explain doesn't go through the scan telemetry path at all). Add:
```
statement ok
SELECT * FROM firebird_explain_pushdown('SELECT * FROM fb.main.EMPLOYEE');
# explain must not have recorded a remote scan (it never opened a cursor)
query I
SELECT COUNT(*) FROM firebird_last_query() WHERE remote_sql LIKE '%EMPLOYEE%';
----
0
```
If `firebird_last_query` state from earlier blocks makes this non-deterministic, instead add a fresh ATTACH at the top of a dedicated section, or document the guarantee as covered by code inspection in review. Pick the stable option.

- [ ] **Step 2: Document in both manuals**

Add a `firebird_explain_pushdown` subsection to `docs/pt/function_manual.md` and `docs/en/function_manual.md` (Level 4 — Diagnostics, near `firebird_last_query`): purpose (a-priori vs last_query post-hoc), the 14 output columns, the SELECT/CTE-only + reject-direct-firebird_scan rules, the placeholder-only `remote_sql` guarantee, parallel-list invariant, the 4 `pk_range_reason` values, and a usage example. Mirror PT/EN per `docs/DOCS_PARITY.md`.

- [ ] **Step 3: Add to the matrix**

In `scripts/build_matrix.ps1`, add `'firebird_explain_pushdown.test' = 'FIREBIRD_TEST_DB'` to the `$testFixtureVar` hashtable.

- [ ] **Step 4: Run the cross-version matrix**

Run (env set): `& scripts/build_matrix.ps1`. Expected: v1.5.2/v1.5.3/v1.5.4 all PASS including `firebird_explain_pushdown.test`; submodule restored to pin; identical assertion counts (no API drift).

- [ ] **Step 5: Commit**

```bash
git add src/firebird_explain_pushdown.cpp docs/pt/function_manual.md docs/en/function_manual.md scripts/build_matrix.ps1 test/sql/firebird_explain_pushdown.test
git commit -m "docs+test(explain): no-cursor proof, function manual, matrix coverage"
```

---

## Self-Review

**Spec coverage:**
- Spike gate → Task 1. PK descriptor + 4 reasons → Task 2 + Task 5. Parse allow-list + reject direct firebird_scan → Task 3. Plan walk + capture-only Build + placeholder-only remote_sql → Task 4. Residual+reason + NONE-gated + cardinality invariant → Task 6. scan_ordinal + self-join + WITH + zero-rows → Tasks 3/4/7. No-cursor proof + docs + matrix → Task 8. All spec sections mapped. ✓
- `pk_range_reason` normalized values, eligibility only for single numeric → Task 2 classifier + Task 5 tests. ✓
- Zero rows when no firebird scan → Task 3 test. ✓

**Placeholder scan:** Task 1 is a spike (verdict doc), explicitly not TDD — its "throwaway prototype" is intentional, not a placeholder. Task 4/5 note "set expected to actual" for deterministic strings (normal TDD discovery) and one genuine contingency (self-join Get count, Task 7 Step 2) flagged with a concrete fallback. Task 2's verification deferred to Task 5 is stated explicitly (no separate C++ unit harness in this project). No bare TBDs.

**Type consistency:** `PrimaryKeyDescriptor`/`PkRangeClassification`/`ClassifyPkRange` defined Task 2, used Task 5. `ExplainRow` fields consistent Tasks 3/4/5/6. `Build()` signature matches the codebase. 14-column schema consistent across Tasks 3/4/5.

**Key risk:** Task 1 spike gates everything; if plan extraction isn't stably exposed, STOP — do not ship an alternative planner under this API.
