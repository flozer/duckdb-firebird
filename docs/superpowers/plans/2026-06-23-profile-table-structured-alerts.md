# firebird_profile_table Structured Alerts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an additive `alerts LIST(STRUCT(code, severity, message))` column to `firebird_profile_table`, derived from a single source so it stays 1:1 with the existing `warnings` column.

**Architecture:** Introduce an `Alert{code, severity, message}` struct and an `AddAlert` helper in `firebird_profile_table.cpp`. Every existing `warnings.push_back("…")` site becomes an `AddAlert(...)` call with a stable code and a fixed severity, keeping the prose verbatim. At emit time, `alerts` is the struct list and `warnings` is derived as `alerts[i].message`, guaranteeing the lists never diverge.

**Tech Stack:** DuckDB out-of-tree C++ extension, DuckDB `Value::STRUCT`/`Value::LIST`, sqllogictest `.test` files.

## Global Constraints

- Purely additive: `warnings LIST(VARCHAR)` keeps its exact current shape, content, and order. Do NOT remove or reorder it.
- `alerts` is appended AFTER `warnings` (column index 10; function goes 10→11 columns).
- `severity` ∈ {`LOW`, `MEDIUM`, `HIGH`} only — reuses the `full_scan_risk` vocabulary.
- Codes are a stable public contract: never reuse a code for a different condition, never change a code's meaning.
- Tested invariant: `len(alerts) == len(warnings)` AND `alerts[i].message == warnings[i]` for every i.
- Message strings stay byte-for-byte identical to the current prose (so existing `warnings` assertions stay green).
- DuckDB version-locked; submodule pin must NOT change. No duckdb/community-extensions/upstream action.

## Code + severity catalog (the 14 existing warnings)

| code | severity | current prose (the `message`, verbatim) |
|---|---|---|
| `view_no_scan_lever` | HIGH | "Object is a VIEW: no primary key, indexes, or partition lever. A scan reads the full view definition. Consider materializing through DuckDB/dbt/Parquet for repeated analytics." |
| `view_definition_not_inspected` | MEDIUM | "View definition not inspected (RDB$VIEW_SOURCE unavailable or unreadable): join/aggregation/filter shape is unknown. Treat as potentially heavy." |
| `view_contains_join` | HIGH | "View contains a JOIN: a scan may materialize a join server-side on every read. Prefer materializing through DuckDB/dbt/Parquet for repeated analytics." |
| `view_contains_aggregation` | HIGH | "View contains aggregation (GROUP BY or aggregate functions): each scan recomputes the aggregate server-side. Materialize the result if queried repeatedly." |
| `view_no_filter` | MEDIUM | "View has no WHERE filter in its definition: a scan reads the full underlying data set. Push a selective filter or materialize." |
| `partition_advisory` | LOW | "Recommended partitions=" + N + " is advisory and derived from the PK MIN/MAX range width, not the row count. The PK range may be sparse (gaps, deletes), so partitions can be uneven. Validate against the live server before scanning a production database in parallel." |
| `server_parallelism_caveat` | LOW | "If Firebird server-side parallelism is already enabled/configured (e.g. Firebird 5 ParallelWorkers), prefer starting with partitions=1 or benchmark before combining server-side and client-side parallelism." |
| `pk_range_small_serial` | LOW | "Primary key range is small (PK MIN/MAX span < 10000): serial scan recommended; partitioning would add overhead without meaningful parallelism." |
| `no_primary_key` | HIGH | "No primary key detected: a scan is a full table scan and cannot be range-partitioned. Add a selective WHERE on an indexed column, or materialize the table." |
| `composite_pk_serial` | LOW | "Primary key is composite: the parallel-scan lever needs a single-column numeric PK. Scans run serially." |
| `numeric_pk_no_range_serial` | LOW | "Primary key is single-column numeric but has no usable MIN/MAX range (empty or near-empty table): no partition lever, scans run serially." |
| `non_numeric_pk_serial` | LOW | "Primary key is single-column but non-numeric: the parallel-scan lever needs a numeric PK. Scans run serially." |
| `no_indexed_filter_columns` | MEDIUM | "No cheap indexed filter columns detected: WHERE clauses may not use an index and could force a full scan." |
| `none_charset_text_columns` | MEDIUM | "Relation has CHARACTER SET NONE text columns: text-filter pushdown is disabled for those columns (UTF-8 literals may not round-trip against raw NONE bytes)." |

---

## File Structure

- `src/firebird_profile_table.cpp` (modify) — `Alert` struct, `AddAlert` helper, convert 14 push sites, `AlertList`/`WarningsFromAlerts` emit helpers, bind column, emit.
- `src/include/firebird_profile_table.hpp` (modify) — document the `alerts` column + stable-code contract in the header comment.
- `test/sql/firebird_profile_table.test` (modify) — keep existing `warnings` assertions verbatim; add invariant + code/severity assertions.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` (modify) — document `alerts` + catalog + contract.

---

### Task 1: Structured alerts in firebird_profile_table

**Files:**
- Modify: `src/firebird_profile_table.cpp`
- Modify: `src/include/firebird_profile_table.hpp`
- Test: `test/sql/firebird_profile_table.test`

**Interfaces:**
- Consumes: existing `TableProfile`, `ProfileTableBind`, `ProfileTableFunction`, `VarcharList` in `firebird_profile_table.cpp`; `firebird_profile_table(VARCHAR)` returns columns ending in `warnings` (currently index 9).
- Produces: an 11th column `alerts LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR))` at output index 10; `warnings` (index 9) derived from `alerts[*].message`.

- [ ] **Step 1: Write the failing tests**

Append to `test/sql/firebird_profile_table.test` (after the existing happy-path assertions; do NOT modify any existing `warnings` assertion). Use the fixtures the file already ATTACHes as `fb` (EMPLOYEE has a single numeric PK; V_ACTIVE_EMP is a view; TPK_COMPOSITE has a composite PK):

```
# ---------------------------------------------------------------------------
#  Structured alerts (v0.8): alerts is 1:1 with warnings, carries a stable
#  code + LOW/MEDIUM/HIGH severity, message identical to the warnings text.
# ---------------------------------------------------------------------------

# alerts column exists and is LIST(STRUCT(code,severity,message)).
query T
SELECT typeof(alerts) FROM firebird_profile_table('fb.main.V_ACTIVE_EMP');
----
STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)[]

# Invariant: same length as warnings.
query T
SELECT len(alerts) = len(warnings) FROM firebird_profile_table('fb.main.V_ACTIVE_EMP');
----
true

# Invariant: alerts[i].message == warnings[i], element-by-element (zipped UNNEST).
query I
SELECT COUNT(*) FROM (
  SELECT UNNEST(warnings) AS w,
         UNNEST(list_transform(alerts, a -> a.message)) AS m
  FROM firebird_profile_table('fb.main.V_ACTIVE_EMP')
) t
WHERE w IS DISTINCT FROM m;
----
0

# Severity domain: every alert severity is LOW/MEDIUM/HIGH.
query I
SELECT COUNT(*) FROM (
  SELECT UNNEST(alerts) AS a FROM firebird_profile_table('fb.main.V_ACTIVE_EMP')
) t
WHERE a.severity NOT IN ('LOW', 'MEDIUM', 'HIGH');
----
0

# Deterministic HIGH alert: a view raises view_no_scan_lever / HIGH.
query TT
SELECT a.code, a.severity FROM (
  SELECT UNNEST(alerts) AS a FROM firebird_profile_table('fb.main.V_ACTIVE_EMP')
) t
WHERE a.code = 'view_no_scan_lever';
----
view_no_scan_lever	HIGH

# Deterministic LOW alert: a composite PK raises composite_pk_serial / LOW.
query TT
SELECT a.code, a.severity FROM (
  SELECT UNNEST(alerts) AS a FROM firebird_profile_table('fb.main.TPK_COMPOSITE')
) t
WHERE a.code = 'composite_pk_serial';
----
composite_pk_serial	LOW

# A table with a single numeric PK raises no view/no-PK HIGH alerts.
query I
SELECT COUNT(*) FROM (
  SELECT UNNEST(alerts) AS a FROM firebird_profile_table('fb.main.EMPLOYEE')
) t
WHERE a.code IN ('no_primary_key', 'view_no_scan_lever');
----
0
```

- [ ] **Step 2: Run the tests to verify they fail**

Run (env first: `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`):
`build\release\test\unittest.exe "C:/tmp/fbwt-pp/test/sql/firebird_profile_table.test"`
Expected: FAIL — `alerts` column does not exist (Binder Error: referenced column "alerts" not found).

- [ ] **Step 3: Add the Alert struct and AddAlert helper**

In `src/firebird_profile_table.cpp`, add the struct near the other model structs (e.g. just before `struct TableProfile`):

```cpp
// One structured diagnostic. `code` is a stable public identifier (never
// reused or redefined once shipped); `severity` is LOW | MEDIUM | HIGH,
// reusing the full_scan_risk vocabulary; `message` is the human-readable
// prose (also surfaced verbatim in the legacy `warnings` column).
struct Alert {
    std::string code;
    std::string severity;
    std::string message;
};
```

In `struct TableProfile`, replace the line:

```cpp
    std::vector<std::string> warnings;
```

with:

```cpp
    std::vector<Alert> alerts;
```

Add the helper just after the `TableProfile` struct definition:

```cpp
// Single source for every diagnostic. The legacy `warnings` column is
// derived from these alerts' messages at emit time, so the two lists
// cannot diverge.
static void AddAlert(TableProfile &p, const char *code, const char *severity,
                     std::string message) {
    p.alerts.push_back(Alert{code, severity, std::move(message)});
}
```

- [ ] **Step 4: Convert the 14 warning sites to AddAlert**

In `BuildProfile`, replace each `p.warnings.push_back(...)` with the matching `AddAlert` call (code + severity from the catalog; prose unchanged). The replacements, in source order:

```cpp
// view base
AddAlert(p, "view_no_scan_lever", "HIGH",
    "Object is a VIEW: no primary key, indexes, or partition lever. "
    "A scan reads the full view definition. Consider materializing "
    "through DuckDB/dbt/Parquet for repeated analytics.");

// view source not inspected
AddAlert(p, "view_definition_not_inspected", "MEDIUM",
    "View definition not inspected (RDB$VIEW_SOURCE unavailable "
    "or unreadable): join/aggregation/filter shape is unknown. "
    "Treat as potentially heavy.");

// view JOIN
AddAlert(p, "view_contains_join", "HIGH",
    "View contains a JOIN: a scan may materialize a join "
    "server-side on every read. Prefer materializing through "
    "DuckDB/dbt/Parquet for repeated analytics.");

// view aggregation
AddAlert(p, "view_contains_aggregation", "HIGH",
    "View contains aggregation (GROUP BY or aggregate "
    "functions): each scan recomputes the aggregate "
    "server-side. Materialize the result if queried "
    "repeatedly.");

// view no WHERE
AddAlert(p, "view_no_filter", "MEDIUM",
    "View has no WHERE filter in its definition: a scan reads "
    "the full underlying data set. Push a selective filter or "
    "materialize.");

// partition advisory (dynamic message — keep the std::to_string concat)
AddAlert(p, "partition_advisory", "LOW",
    "Recommended partitions=" + std::to_string(parts) +
    " is advisory and derived from the PK MIN/MAX range width, "
    "not the row count. The PK range may be sparse (gaps, "
    "deletes), so partitions can be uneven. Validate against the "
    "live server before scanning a production database in "
    "parallel.");

// server-side parallelism caveat
AddAlert(p, "server_parallelism_caveat", "LOW",
    "If Firebird server-side parallelism is already "
    "enabled/configured (e.g. Firebird 5 ParallelWorkers), prefer "
    "starting with partitions=1 or benchmark before combining "
    "server-side and client-side parallelism.");

// narrow PK range -> serial
AddAlert(p, "pk_range_small_serial", "LOW",
    "Primary key range is small (PK MIN/MAX span < 10000): "
    "serial scan recommended; partitioning would add overhead "
    "without meaningful parallelism.");

// no PK
AddAlert(p, "no_primary_key", "HIGH",
    "No primary key detected: a scan is a full table scan and "
    "cannot be range-partitioned. Add a selective WHERE on an "
    "indexed column, or materialize the table.");

// composite PK
AddAlert(p, "composite_pk_serial", "LOW",
    "Primary key is composite: the parallel-scan lever needs a "
    "single-column numeric PK. Scans run serially.");

// single numeric PK, no usable range
AddAlert(p, "numeric_pk_no_range_serial", "LOW",
    "Primary key is single-column numeric but has no usable "
    "MIN/MAX range (empty or near-empty table): no partition "
    "lever, scans run serially.");

// single non-numeric PK
AddAlert(p, "non_numeric_pk_serial", "LOW",
    "Primary key is single-column but non-numeric: the "
    "parallel-scan lever needs a numeric PK. Scans run "
    "serially.");

// no cheap filter columns
AddAlert(p, "no_indexed_filter_columns", "MEDIUM",
    "No cheap indexed filter columns detected: WHERE clauses may not "
    "use an index and could force a full scan.");

// NONE charset text columns
AddAlert(p, "none_charset_text_columns", "MEDIUM",
    "Relation has CHARACTER SET NONE text columns: text-filter "
    "pushdown is disabled for those columns (UTF-8 literals may not "
    "round-trip against raw NONE bytes).");
```

Each replacement preserves the exact prose currently passed to
`p.warnings.push_back(...)` at that site — only the call wrapper changes.
Leave the empty `if (!p.has_primary_key && !is_view) { ... }` no-op block as-is
(it pushes nothing).

- [ ] **Step 5: Add the emit helpers**

Add near the existing `VarcharList` helper:

```cpp
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
```

- [ ] **Step 6: Add the bind column**

In `ProfileTableBind`, the `names` initializer currently ends with `"warnings",`. Add `"alerts",` immediately after it:

```cpp
        "warnings",
        "alerts",
    };
```

And in `return_types`, after the warnings entry `LogicalType::LIST(LogicalType::VARCHAR),` (the last one), add the alerts struct-list type:

```cpp
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::LIST(LogicalType::STRUCT({
            {"code", LogicalType::VARCHAR},
            {"severity", LogicalType::VARCHAR},
            {"message", LogicalType::VARCHAR}})),
    };
```

- [ ] **Step 7: Emit warnings (derived) + alerts**

In `ProfileTableFunction`, the last data assignment is currently:

```cpp
    output.data[9].SetValue(0, VarcharList(p.warnings));
```

Replace it with the derived warnings + the new alerts column:

```cpp
    output.data[9].SetValue(0, WarningsFromAlerts(p.alerts));
    output.data[10].SetValue(0, AlertStructList(p.alerts));
```

- [ ] **Step 8: Update the header doc**

In `src/include/firebird_profile_table.hpp`, in the function's doc comment, add the `alerts` column after the `warnings` line and a contract note:

```cpp
//   - warnings              LIST(VARCHAR)
//   - alerts                LIST(STRUCT(code, severity, message))
//
// `alerts` is the structured, machine-readable form of `warnings`: it is
// 1:1 and in the same order (alerts[i].message == warnings[i]). `code` is a
// stable public identifier (never reused or redefined once shipped) and
// `severity` is LOW | MEDIUM | HIGH (the full_scan_risk vocabulary).
```

- [ ] **Step 9: Build**

Run: `build_windows_local.bat`
Expected: build succeeds; `build\release\extension\firebird\firebird.duckdb_extension` produced.

- [ ] **Step 10: Run the tests to verify they pass**

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-pp/test/sql/firebird_profile_table.test"`
Expected: `All tests passed (N assertions)` — including the existing (unchanged) `warnings` assertions and the new alerts assertions.

- [ ] **Step 11: Commit**

```bash
git add src/firebird_profile_table.cpp src/include/firebird_profile_table.hpp test/sql/firebird_profile_table.test
git commit -m "feat(profile): structured alerts column (code/severity/message)"
```

---

### Task 2: Documentation (PT/EN)

**Files:**
- Modify: `docs/pt/function_manual.md`
- Modify: `docs/en/function_manual.md`

**Interfaces:**
- Consumes: the `alerts` column + catalog from Task 1.
- Produces: nothing (docs only).

- [ ] **Step 1: Document in the EN manual**

In `docs/en/function_manual.md`, find the `firebird_profile_table` section. Add the `alerts` column to its column table (right after `warnings`), then add a subsection documenting:
- `alerts` is `LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR))`, the structured form of `warnings`, 1:1 and same order (`alerts[i].message == warnings[i]`).
- `severity` ∈ {LOW, MEDIUM, HIGH} (the full_scan_risk vocabulary).
- A **stable-code contract** note: codes are public API — never reused for a different condition, never redefined.
- The full code→severity catalog table (the 14 codes with their severity and a one-line meaning), copied from this plan's catalog.

- [ ] **Step 2: Document in the PT manual (parity)**

In `docs/pt/function_manual.md`, mirror Step 1 in the `firebird_profile_table` section: add `alerts` to the column table, the same code/severity catalog, the severity-domain note, and the stable-code contract note — translated, following the existing PT section's heading layout (per docs/DOCS_PARITY.md).

- [ ] **Step 3: Commit**

```bash
git add docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(profile): document structured alerts column + code catalog (PT/EN)"
```

---

## Self-Review

**Spec coverage:**
- Additive `alerts` column, `warnings` unchanged → Task 1 Steps 6-7. ✔
- 1:1 invariant + same order, single source → `AddAlert` + `WarningsFromAlerts`/`AlertStructList` from the same `alerts` vector (Steps 3,5,7); tested Step 1. ✔
- 14-code catalog with exact severities → Step 4. ✔ (cross-checked: HIGH = view_no_scan_lever, view_contains_join, view_contains_aggregation, no_primary_key; MEDIUM = view_definition_not_inspected, view_no_filter, no_indexed_filter_columns, none_charset_text_columns; LOW = partition_advisory, server_parallelism_caveat, pk_range_small_serial, composite_pk_serial, numeric_pk_no_range_serial, non_numeric_pk_serial)
- severity ∈ {LOW,MEDIUM,HIGH} → Step 1 domain test. ✔
- Stable-code contract documented → Step 8 (header) + Task 2 (manuals). ✔
- Message byte-identical → Step 4 preserves prose; existing warnings assertions kept (Step 1 preamble). ✔
- No matrix change needed (test already wired) → noted in spec. ✔

**Placeholder scan:** No TBD/TODO; all code shown; prose copied verbatim. ✔

**Type consistency:** `Alert{code,severity,message}` used consistently; `alerts` column type identical in bind (Step 6) and emit (Step 5 `AlertStructList`); output indices 9 (warnings, derived) and 10 (alerts) consistent with the 11-column bind order. ✔

**Note for implementer:** The fixtures `EMPLOYEE`, `V_ACTIVE_EMP`, `TPK_COMPOSITE` are already ATTACHed in the existing test under alias `fb`. If a fixture name differs in the actual test file, match the existing names. Confirm `typeof(alerts)` renders exactly as `STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)[]` on this DuckDB version (Step 1 first assertion); if the spacing/format differs, adjust that one expected string to what the build prints — the structure, not the formatting, is the requirement.
