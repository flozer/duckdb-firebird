# firebird_health Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `firebird_health('<alias>')`, a single-row read-only database/server health diagnostic with factual metrics plus a structured `warnings` list.

**Architecture:** A bespoke single-row table function (same shape as `firebird_profile_table`): bind validates the ATTACH alias, the scan acquires a catalog lease, runs one always-readable system query (`RDB$DATABASE` + `rdb$get_context`) and one privilege-filtered monitoring query (`MON$DATABASE` + `MON$ATTACHMENTS`) wrapped in a try/catch for graceful degradation, computes derived gaps and warnings in C++, and emits exactly one row.

**Tech Stack:** DuckDB out-of-tree C++ extension, libfbclient via `FirebirdConnection`/`FirebirdStatement`, DuckDB sqllogictest `.test` files.

## Global Constraints

- Read-only only: `SELECT` against system/monitoring tables. No DDL/DML, no user SQL, no business-data row values emitted.
- DuckDB version-locked to v1.5.3 submodule pin `14eca11bd9d4a0de2ea0f078be588a9c1c5b279c` — do NOT change the pin.
- Column-type accessor discipline: SMALLINT → `GetShort`; INTEGER → `GetLong`; BIGINT → `GetInt64`; text → `GetText`. Using the wrong width returns garbage.
- Gap threshold is the literal `1000000`, documented as a conservative default signal, NOT a universal limit.
- `mon_unavailable` warning is emitted ONLY when the monitoring query throws — never for limited/partial visibility (a partial `attachments` count is a faithful, non-failing result).
- No duckdb/community-extensions / upstream action.
- PT/EN docs parity (`docs/pt/` and `docs/en/` per `docs/DOCS_PARITY.md`).

---

## File Structure

- `src/include/firebird_health.hpp` (new) — declares `TableFunction GetFirebirdHealthFunction();`
- `src/firebird_health.cpp` (new) — the entire function: bind, init, scan, queries, warnings.
- `src/firebird_extension.cpp` (modify) — `#include` + `RegisterFunction(GetFirebirdHealthFunction())`.
- `test/sql/firebird_health.test` (new) — happy-path shape/type/range + warnings assertions against `FIREBIRD_TEST_DB`.
- `test/sql/firebird_none_charset.test` (modify) — append the deterministic `charset_none` assertion (file already gated on `FIREBIRD_NONE_DB`).
- `scripts/build_matrix.ps1` (modify) — add `firebird_health.test` → `FIREBIRD_TEST_DB`.
- `.github/workflows/build-linux-fb-matrix.yml` (modify) — run `firebird_health.test` in the FB matrix.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` (modify) — public docs.

---

### Task 1: firebird_health function (core)

**Files:**
- Create: `src/include/firebird_health.hpp`
- Create: `src/firebird_health.cpp`
- Modify: `src/firebird_extension.cpp` (add include near line 8; add `RegisterFunction` near line 36)
- Test: `test/sql/firebird_health.test`

**Interfaces:**
- Consumes: `AcquireFirebirdCatalogLease(ClientContext&, const std::string&)` and `ValidateFirebirdAttachAlias(ClientContext&, const std::string&)` from `firebird_dbt_sources.hpp`; the lease exposes `lease.conn` (a `FirebirdConnection*`-like with `->OpenCursor(sql)` returning `unique_ptr<FirebirdStatement>`). `FirebirdStatement`: `bool Fetch()`, `bool IsNull(idx_t)`, `int16_t GetShort(idx_t)`, `int32_t GetLong(idx_t)`, `int64_t GetInt64(idx_t)`, `std::string GetText(idx_t)`.
- Produces: `TableFunction GetFirebirdHealthFunction();` — registered as `firebird_health(VARCHAR)`, returns 1 row, 15 columns named/typed exactly as in Step 3 below.

- [ ] **Step 1: Write the failing test**

Create `test/sql/firebird_health.test`:

```
# name: test/sql/firebird_health.test
# description: firebird_health - single-row DB/server health diagnostic:
#              factual metrics (version/ODS/dialect/charset/page/durability/
#              sweep/transaction counters/attachments) + structured warnings.
# group: [firebird]

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement error
SELECT * FROM firebird_health(NULL);
----
firebird_health(alias VARCHAR): alias is required

statement error
SELECT * FROM firebird_health('nope_does_not_exist_xyz');
----
no attached catalog named 'nope_does_not_exist_xyz'

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

# Exactly one row.
query I
SELECT COUNT(*) FROM firebird_health('fb');
----
1

# Factual shape/range checks (exact transaction numbers vary per session).
query IIIIIII
SELECT
  CASE WHEN engine_version <> '' THEN 1 ELSE 0 END,
  CASE WHEN regexp_matches(ods_version, '^[0-9]+\.[0-9]+$') THEN 1 ELSE 0 END,
  CASE WHEN sql_dialect = 3 THEN 1 ELSE 0 END,
  CASE WHEN page_size > 0 THEN 1 ELSE 0 END,
  CASE WHEN next_transaction > 0 THEN 1 ELSE 0 END,
  CASE WHEN oit_gap >= 0 AND oat_gap >= 0 THEN 1 ELSE 0 END,
  CASE WHEN attachments >= 1 THEN 1 ELSE 0 END
FROM firebird_health('fb');
----
1 1 1 1 1 1 1

# Default charset of the UTF8 fixture, and no charset_none warning for it.
query TT
SELECT default_charset, list_contains(warnings, 'charset_none')
FROM firebird_health('fb');
----
UTF8	false

# warnings is a LIST(VARCHAR) (type-stable even when empty/non-empty).
query T
SELECT typeof(warnings) FROM firebird_health('fb');
----
VARCHAR[]
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build\release\test\unittest.exe "$(pwd -W)/test/sql/firebird_health.test"`
Expected: FAIL — `firebird_health` is not a registered function (Catalog Error / function does not exist).

- [ ] **Step 3: Create the header**

Create `src/include/firebird_health.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// firebird_health(alias VARCHAR)
//
// Single-row, read-only health diagnostic for an attached Firebird
// catalog. `alias` is the ATTACH alias (e.g. 'fb'). Emits one row of
// factual server/database metrics plus a structured `warnings` list.
//
// Columns: engine_version, ods_version, sql_dialect, default_charset,
// page_size, forced_writes, sweep_interval, oldest_transaction,
// oldest_active, oldest_snapshot, next_transaction, oit_gap, oat_gap,
// attachments, warnings.
//
// Factual diagnostic, not an opaque advisor: every warning maps to an
// explicit documented condition, and the driving counters are columns.
// Monitoring-table reads are privilege-filtered, not gated: a partial
// attachments count is a faithful result. The `mon_unavailable` warning
// is emitted only on a real monitoring-query failure.
TableFunction GetFirebirdHealthFunction();

} // namespace duckdb
```

- [ ] **Step 4: Create the implementation**

Create `src/firebird_health.cpp`:

```cpp
#include "firebird_health.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_dbt_sources.hpp" // AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias

#include <string>
#include <vector>

namespace duckdb {

// Conservative default gap signal (NOT a universal limit) — documented in
// the function manual. Changing this constant changes both gap warnings.
static const int64_t FB_HEALTH_GAP_THRESHOLD = 1000000;

struct HealthInfo {
    // Always readable by any attachment.
    std::string engine_version;
    std::string default_charset;

    // MON$-sourced; valid only when mon_ok.
    bool mon_ok = false;
    int16_t ods_major = 0;
    int16_t ods_minor = 0;
    int16_t sql_dialect = 0;
    int32_t page_size = 0;
    bool forced_writes = false;
    int32_t sweep_interval = 0;
    int64_t oit = 0;
    int64_t oat = 0;
    int64_t ost = 0;
    int64_t next_tx = 0;
    int32_t attachments = 0;

    std::vector<std::string> warnings;
};

static HealthInfo BuildHealth(FirebirdConnection &conn) {
    HealthInfo h;

    // 1) Always-readable fields (any user can read these). A failure here
    //    means a broken attach — let it propagate as a normal scan error.
    {
        auto cur = conn.OpenCursor(
            "SELECT rdb$get_context('SYSTEM','ENGINE_VERSION'), "
            "       TRIM(RDB$CHARACTER_SET_NAME) "
            "  FROM RDB$DATABASE");
        if (cur->Fetch()) {
            h.engine_version = cur->IsNull(0) ? std::string() : cur->GetText(0);
            h.default_charset = cur->IsNull(1) ? std::string() : cur->GetText(1);
        }
    }

    // 2) Privilege-filtered monitoring metrics. The query returns one row
    //    for every attachment (the database row is universal); a partial
    //    attachments count under limited privilege is still a faithful
    //    result. Only a real query failure (throw) degrades to NULLs +
    //    mon_unavailable.
    try {
        auto cur = conn.OpenCursor(
            "SELECT m.MON$ODS_MAJOR, m.MON$ODS_MINOR, m.MON$SQL_DIALECT, "
            "       m.MON$PAGE_SIZE, m.MON$FORCED_WRITES, m.MON$SWEEP_INTERVAL, "
            "       m.MON$OLDEST_TRANSACTION, m.MON$OLDEST_ACTIVE, "
            "       m.MON$OLDEST_SNAPSHOT, m.MON$NEXT_TRANSACTION, "
            "       (SELECT COUNT(*) FROM MON$ATTACHMENTS) "
            "  FROM MON$DATABASE m");
        if (cur->Fetch()) {
            h.ods_major = cur->GetShort(0);
            h.ods_minor = cur->GetShort(1);
            h.sql_dialect = cur->GetShort(2);
            h.page_size = cur->GetLong(3);
            h.forced_writes = (cur->GetShort(4) != 0);
            h.sweep_interval = cur->GetLong(5);
            h.oit = cur->GetInt64(6);
            h.oat = cur->GetInt64(7);
            h.ost = cur->GetInt64(8);
            h.next_tx = cur->GetInt64(9);
            // COUNT(*) is BIGINT in dialect 3; attachment counts are small,
            // so narrowing to INTEGER is safe.
            h.attachments = static_cast<int32_t>(cur->GetInt64(10));
            h.mon_ok = true;
        }
    } catch (...) {
        h.mon_ok = false;
    }

    // 3) Warnings — explicit, documented criteria, deterministic order.
    if (h.mon_ok) {
        if ((h.next_tx - h.oit) > FB_HEALTH_GAP_THRESHOLD) {
            h.warnings.push_back("oit_gap_high");
        }
        if ((h.next_tx - h.oat) > FB_HEALTH_GAP_THRESHOLD) {
            h.warnings.push_back("oat_gap_high");
        }
        if (h.sweep_interval == 0) {
            h.warnings.push_back("sweep_disabled");
        }
        if (!h.forced_writes) {
            h.warnings.push_back("forced_writes_off");
        }
    }
    if (h.default_charset == "NONE") {
        h.warnings.push_back("charset_none");
    }
    if (!h.mon_ok) {
        h.warnings.push_back("mon_unavailable");
    }

    return h;
}

struct HealthBindData : public TableFunctionData {
    std::string catalog_name;
};

struct HealthGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
HealthBind(ClientContext &context, TableFunctionBindInput &input,
           vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_health(alias VARCHAR): alias is required (the ATTACH "
            "alias, e.g. 'fb').");
    }
    auto bind = make_uniq<HealthBindData>();
    bind->catalog_name = input.inputs[0].ToString();

    // Fail a bad alias at bind time, not mid-scan.
    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {
        "engine_version", "ods_version", "sql_dialect", "default_charset",
        "page_size", "forced_writes", "sweep_interval", "oldest_transaction",
        "oldest_active", "oldest_snapshot", "next_transaction", "oit_gap",
        "oat_gap", "attachments", "warnings",
    };
    return_types = {
        LogicalType::VARCHAR,                  // engine_version
        LogicalType::VARCHAR,                  // ods_version
        LogicalType::INTEGER,                  // sql_dialect
        LogicalType::VARCHAR,                  // default_charset
        LogicalType::INTEGER,                  // page_size
        LogicalType::BOOLEAN,                  // forced_writes
        LogicalType::INTEGER,                  // sweep_interval
        LogicalType::BIGINT,                   // oldest_transaction
        LogicalType::BIGINT,                   // oldest_active
        LogicalType::BIGINT,                   // oldest_snapshot
        LogicalType::BIGINT,                   // next_transaction
        LogicalType::BIGINT,                   // oit_gap
        LogicalType::BIGINT,                   // oat_gap
        LogicalType::INTEGER,                  // attachments
        LogicalType::LIST(LogicalType::VARCHAR), // warnings
    };
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
HealthInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<HealthGlobalState>();
}

static Value VarcharList(const std::vector<std::string> &xs) {
    vector<Value> vals;
    vals.reserve(xs.size());
    for (const auto &s : xs) {
        vals.emplace_back(Value(s));
    }
    return Value::LIST(LogicalType::VARCHAR, std::move(vals));
}

static void HealthFunction(ClientContext &context, TableFunctionInput &input,
                           DataChunk &output) {
    auto &g = input.global_state->Cast<HealthGlobalState>();
    if (g.emitted) {
        output.SetCardinality(0);
        return;
    }
    auto &bind = input.bind_data->Cast<HealthBindData>();

    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    HealthInfo h = BuildHealth(*lease.conn);

    // MON$-sourced columns are typed NULLs when the monitoring query failed.
    auto bigint_or_null = [&](int64_t v) {
        return h.mon_ok ? Value::BIGINT(v) : Value(LogicalType::BIGINT);
    };
    auto int_or_null = [&](int32_t v) {
        return h.mon_ok ? Value::INTEGER(v) : Value(LogicalType::INTEGER);
    };

    output.SetCardinality(1);
    output.data[0].SetValue(0, Value(h.engine_version));
    output.data[1].SetValue(
        0, h.mon_ok ? Value(std::to_string(h.ods_major) + "." +
                            std::to_string(h.ods_minor))
                    : Value(LogicalType::VARCHAR));
    output.data[2].SetValue(0, int_or_null(h.sql_dialect));
    output.data[3].SetValue(0, Value(h.default_charset));
    output.data[4].SetValue(0, int_or_null(h.page_size));
    output.data[5].SetValue(
        0, h.mon_ok ? Value::BOOLEAN(h.forced_writes)
                    : Value(LogicalType::BOOLEAN));
    output.data[6].SetValue(0, int_or_null(h.sweep_interval));
    output.data[7].SetValue(0, bigint_or_null(h.oit));
    output.data[8].SetValue(0, bigint_or_null(h.oat));
    output.data[9].SetValue(0, bigint_or_null(h.ost));
    output.data[10].SetValue(0, bigint_or_null(h.next_tx));
    output.data[11].SetValue(0, bigint_or_null(h.next_tx - h.oit));
    output.data[12].SetValue(0, bigint_or_null(h.next_tx - h.oat));
    output.data[13].SetValue(0, int_or_null(h.attachments));
    output.data[14].SetValue(0, VarcharList(h.warnings));
    g.emitted = true;
}

TableFunction GetFirebirdHealthFunction() {
    TableFunction fn("firebird_health", {LogicalType::VARCHAR},
                     HealthFunction, HealthBind, HealthInitGlobal);
    return fn;
}

} // namespace duckdb
```

- [ ] **Step 5: Register the function**

In `src/firebird_extension.cpp`, add the include alongside the other metadata-function includes (near line 8):

```cpp
#include "firebird_health.hpp"
```

And register it after the `firebird_type_audit` registration (near line 36):

```cpp
    loader.RegisterFunction(GetFirebirdHealthFunction());
```

- [ ] **Step 6: Build**

Run: `build_windows_local.bat`
Expected: build succeeds; `build\release\extension\firebird\firebird.duckdb_extension` produced.

- [ ] **Step 7: Run the test to verify it passes**

Run: `build\release\test\unittest.exe "$(pwd -W)/test/sql/firebird_health.test"`
Expected: `All tests passed (N assertions)`.

(Set env first: `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`.)

- [ ] **Step 8: Commit**

```bash
git add src/include/firebird_health.hpp src/firebird_health.cpp src/firebird_extension.cpp test/sql/firebird_health.test
git commit -m "feat(health): firebird_health single-row DB/server diagnostic"
```

---

### Task 2: Deterministic charset_none assertion (none.fdb)

**Files:**
- Modify: `test/sql/firebird_none_charset.test` (append; file already `require-env FIREBIRD_NONE_DB`)

**Interfaces:**
- Consumes: `firebird_health(VARCHAR)` from Task 1.
- Produces: nothing (test only).

- [ ] **Step 1: Append the failing assertion**

At the end of `test/sql/firebird_none_charset.test`, append (the file already ATTACHes the NONE fixture — reuse its alias; if it uses a different alias than `fbn`, match the existing one):

```
# firebird_health surfaces a deterministic charset_none warning for a
# database whose default character set is NONE.
statement ok
ATTACH '${FIREBIRD_NONE_DB}' AS fbnh (TYPE firebird);

query TT
SELECT default_charset, list_contains(warnings, 'charset_none')
FROM firebird_health('fbnh');
----
NONE	true
```

- [ ] **Step 2: Run the test to verify it passes**

Run: `build\release\test\unittest.exe "$(pwd -W)/test/sql/firebird_none_charset.test"`
Expected: `All tests passed (N assertions)` (with `$env:FIREBIRD_NONE_DB` set to the NONE fixture).

- [ ] **Step 3: Commit**

```bash
git add test/sql/firebird_none_charset.test
git commit -m "test(health): deterministic charset_none warning via none.fdb"
```

---

### Task 3: Matrix + documentation

**Files:**
- Modify: `scripts/build_matrix.ps1` (add to `$testFixtureVar`)
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (run the new test)
- Modify: `docs/pt/function_manual.md`, `docs/en/function_manual.md`

**Interfaces:**
- Consumes: `firebird_health(VARCHAR)` from Task 1.
- Produces: nothing (matrix wiring + docs).

- [ ] **Step 1: Add the test to the local matrix**

In `scripts/build_matrix.ps1`, add an entry to the `$testFixtureVar` hashtable next to the other `FIREBIRD_TEST_DB` tests:

```powershell
    'firebird_health.test'             = 'FIREBIRD_TEST_DB'
```

(`firebird_none_charset.test` is already mapped to `FIREBIRD_NONE_DB`, so the Task 2 assertion is already covered.)

- [ ] **Step 2: Add the test to the Linux FB matrix workflow**

In `.github/workflows/build-linux-fb-matrix.yml`, locate the step that runs the `FIREBIRD_TEST_DB`-backed tests (the EMPLOYEE-fixture step) and add `firebird_health.test` to its test list exactly as the sibling tests are invoked (match the existing `unittest ... test/sql/<name>.test` pattern in that step).

- [ ] **Step 3: Document in PT manual**

In `docs/pt/function_manual.md`, add a `firebird_health` section (follow the layout of the existing `firebird_profile_table` section): purpose, signature `firebird_health('fb')`, the 15-column table, the warnings table with explicit triggers, and these notes verbatim in spirit:

- Limiar de gap `1.000.000`: sinal conservador padrão, **não** um limite universal; ajustável apenas recompilando (constante `FB_HEALTH_GAP_THRESHOLD`).
- `attachments` é a contagem **visível ao usuário atual** (visão parcial sob privilégio limitado, total sob privilégio de monitoramento) — resultado fiel, não erro.
- `mon_unavailable` aparece somente em **falha real** da consulta de monitoramento; visibilidade limitada não dispara o warning. Nesse caso as colunas MON$ ficam NULL e `engine_version`/`default_charset` continuam preenchidos.

- [ ] **Step 4: Document in EN manual (parity)**

In `docs/en/function_manual.md`, add the equivalent `firebird_health` section with the same column table, warnings table, and the three notes translated to English (gap threshold is a conservative default not a universal limit; `attachments` is the count visible to the current user; `mon_unavailable` only on a real monitoring-query failure).

- [ ] **Step 5: Commit**

```bash
git add scripts/build_matrix.ps1 .github/workflows/build-linux-fb-matrix.yml docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(health): matrix entry + PT/EN function_manual for firebird_health"
```

---

## Self-Review

**Spec coverage:**
- 15-column contract → Task 1 Step 3/4 (names + types + SetValue). ✔
- Always-readable fields + MON$ query + graceful degradation → Task 1 `BuildHealth`. ✔
- Derived `oit_gap`/`oat_gap` computed in C++ → Task 1 (`next_tx - oit`, `next_tx - oat`). ✔
- 6 warnings with explicit triggers + deterministic order → Task 1 Step 4. ✔
- `1000000` threshold as documented conservative signal → constant + Task 3 docs. ✔
- `attachments` = count visible to current user; partial ≠ error → comment in code + Task 3 docs. ✔
- `mon_unavailable` only on real query throw → try/catch sets `mon_ok=false` only on exception → Task 1. ✔
- Deterministic `charset_none` via none.fdb → Task 2. ✔
- Range-based tests (no exact tx numbers) → Task 1 Step 1. ✔
- Matrix + PT/EN docs parity → Task 3. ✔

**Placeholder scan:** No TBD/TODO; all code blocks complete; no "add error handling" hand-waving. ✔

**Type consistency:** `GetFirebirdHealthFunction` consistent across hpp/cpp/extension; `firebird_health` name consistent in code + tests + docs; column names identical between bind `names` and the test SELECTs; `FB_HEALTH_GAP_THRESHOLD` referenced consistently. ✔

**Note for implementer:** `firebird_none_charset.test` may already ATTACH the NONE fixture under a specific alias; Task 2 uses a fresh alias `fbnh` to avoid collision, but if the file's existing ATTACH alias is reusable, prefer reusing it. Confirm the exact `unittest` invocation pattern in the Linux workflow's test step before editing (Task 3 Step 2).
