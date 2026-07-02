# firebird_index_profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `firebird_index_profile('<alias.schema.table>')`, a per-index, read-only diagnostic table function: one row per existing index (or one synthetic row when a table has zero indexes), with selectivity, dependency flags, structured alerts, and table-level unindexed-filter-candidate recommendations.

**Architecture:** A bespoke multi-row table function (row-cursor pattern, like the existing `firebird_metadata_functions.cpp` scaffold): bind parses the qualified name and validates the ATTACH alias; init-global acquires a catalog lease, loads every index on the relation (`RDB$INDICES` + `RDB$INDEX_SEGMENTS` + two `RDB$RELATION_CONSTRAINTS` joins for PK/FK + `RDB$STATISTICS` for selectivity), computes table-level `unindexed_filter_candidates`, builds one row per index (or a synthetic row if none), and buffers all rows for streaming; the scan callback drains the buffer like `MetaFunction` does.

**Tech Stack:** DuckDB out-of-tree C++ extension, `FirebirdConnection`/`FirebirdStatement` (existing `GetDouble` accessor, not yet used anywhere in the codebase), Firebird `isql` fixture DDL, sqllogictest `.test` files.

## Global Constraints

- Read-only only: SELECT against `RDB$INDICES`, `RDB$INDEX_SEGMENTS`, `RDB$RELATION_CONSTRAINTS`, `RDB$RELATIONS`, plus the existing `LoadTableSchema` path. No DDL/DML at query time, no business-data row values.
- Grain is one row per existing index. A table with **zero indexes** emits exactly **one synthetic row** (never zero rows for an existing relation): `index_name IS NULL` marks it; every other index-scoped column is NULL on that row; `alerts` carries `{code:'no_indexes_on_table', severity:'HIGH', ...}`.
- Alert codes are a stable public contract (same discipline as `firebird_profile_table`'s `alerts`): `no_indexes_on_table` (HIGH), `index_inactive` (MEDIUM), `missing_statistics` (LOW). **No** numeric-threshold `low_selectivity` alert in this version — `selectivity` is exposed as a raw nullable DOUBLE column only.
- `missing_statistics` — not `stale_statistics` — because `RDB$STATISTICS IS NULL` proves the statistic was never computed, not that it is stale.
- `unindexed_filter_candidates` excludes any column covered by **any** index segment, including an *inactive* index's segments (that condition is signaled separately via `index_inactive`, not by omission from this list).
- Column-type accessor discipline: Firebird SMALLINT → `GetShort`; DOUBLE/FLOAT (`RDB$STATISTICS`) → `GetDouble`; text → `GetText`.
- DuckDB version-locked; submodule pin must NOT change. No duckdb/community-extensions/upstream action.
- PT/EN docs parity (`docs/pt/` and `docs/en/`) per `docs/DOCS_PARITY.md`.

---

## File Structure

- `scripts/setup_test_firebird.sh` (modify) — append the two new fixture tables (Linux CI, full-DB recreation path).
- `scripts/fixture_index_profile.sql` (new) — the same two tables, additive DDL for the existing local Windows `test.fdb` (no `CREATE DATABASE`).
- `test/sql/firebird_metadata.test` (modify) — the exact relation-list assertion needs the two new table names inserted alphabetically.
- `src/include/firebird_index_profile.hpp` (new) — declares `TableFunction GetFirebirdIndexProfileFunction();`.
- `src/firebird_index_profile.cpp` (new) — the entire function: parsing, bind, init-global (row loading), scan.
- `src/firebird_extension.cpp` (modify) — `#include` + `RegisterFunction`.
- `CMakeLists.txt` (modify) — add the new `.cpp` to `EXTENSION_SOURCES`.
- `test/sql/firebird_index_profile.test` (new).
- `scripts/build_matrix.ps1`, `.github/workflows/build-linux-fb-matrix.yml` (modify) — matrix entries.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` (modify) — public docs.

---

### Task 1: Fixtures — zero-index and inactive-index tables

**Files:**
- Modify: `scripts/setup_test_firebird.sh`
- Create: `scripts/fixture_index_profile.sql`
- Modify: `test/sql/firebird_metadata.test`

**Interfaces:**
- Consumes: nothing (pure fixture/test-data change).
- Produces: two new relations in the `FIREBIRD_TEST_DB` fixture that Task 2's test file depends on: `TNO_INDEX` (zero indexes: `CODE VARCHAR(10)`, `QTY INTEGER`, `NOTE VARCHAR(20)`) and `TIDX_INACTIVE` (`ID INTEGER NOT NULL PRIMARY KEY`, `QTY INTEGER`, `NOTE VARCHAR(20)`, plus a secondary index `IDX_TIDX_INACTIVE_QTY` on `QTY` that is `ALTER`ed `INACTIVE`).

- [ ] **Step 1: Add the fixtures to the Linux CI provisioning script**

In `scripts/setup_test_firebird.sh`, insert the following block right after the existing `TCHILD` block (after the line `COMMIT;` that follows the `TCHILD` `CREATE TABLE` + its trailing comment, and before the closing `EOF` of the heredoc):

```sql
-- Zero-index fixture: no PK, no unique constraint, no FK, no explicit
-- index. Exercises firebird_index_profile()'s synthetic "no indexes" row,
-- the no_indexes_on_table alert, and unindexed_filter_candidates.
CREATE TABLE TNO_INDEX (
    CODE  VARCHAR(10),
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TNO_INDEX VALUES ('A1', 10, 'first');
INSERT INTO TNO_INDEX VALUES ('B2', 20, 'second');
COMMIT;

-- Inactive-index fixture: a PK (so this table is NOT a zero-index case)
-- plus one secondary index later disabled. Exercises the index_inactive
-- alert, and confirms unindexed_filter_candidates excludes a column
-- covered by an index even when that index is inactive.
CREATE TABLE TIDX_INACTIVE (
    ID    INTEGER NOT NULL PRIMARY KEY,
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TIDX_INACTIVE VALUES (1, 5, 'x');
COMMIT;
CREATE INDEX IDX_TIDX_INACTIVE_QTY ON TIDX_INACTIVE (QTY);
ALTER INDEX IDX_TIDX_INACTIVE_QTY INACTIVE;
COMMIT;
```

- [ ] **Step 2: Create the local-Windows additive fixture script**

Create `scripts/fixture_index_profile.sql` (mirrors the additive-only style of `scripts/fixture_none_charset.sql` — no `CREATE DATABASE`, applied to the already-existing `test.fdb`):

```sql
/* firebird_index_profile fixtures — additive against the EXISTING
 * FIREBIRD_TEST_DB (test.fdb). Does NOT create a new database.
 *
 * Run with (Windows):
 *   "C:\Program Files\Firebird\Firebird_5_0\isql.exe" -user SYSDBA ^
 *     -password masterkey -i scripts\fixture_index_profile.sql C:\fbtest\test.fdb
 */

-- Zero-index fixture: no PK, no unique constraint, no FK, no explicit
-- index. Exercises firebird_index_profile()'s synthetic "no indexes" row,
-- the no_indexes_on_table alert, and unindexed_filter_candidates.
CREATE TABLE TNO_INDEX (
    CODE  VARCHAR(10),
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TNO_INDEX VALUES ('A1', 10, 'first');
INSERT INTO TNO_INDEX VALUES ('B2', 20, 'second');
COMMIT;

-- Inactive-index fixture: a PK (so this table is NOT a zero-index case)
-- plus one secondary index later disabled. Exercises the index_inactive
-- alert, and confirms unindexed_filter_candidates excludes a column
-- covered by an index even when that index is inactive.
CREATE TABLE TIDX_INACTIVE (
    ID    INTEGER NOT NULL PRIMARY KEY,
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TIDX_INACTIVE VALUES (1, 5, 'x');
COMMIT;
CREATE INDEX IDX_TIDX_INACTIVE_QTY ON TIDX_INACTIVE (QTY);
ALTER INDEX IDX_TIDX_INACTIVE_QTY INACTIVE;
COMMIT;
```

- [ ] **Step 3: Run the fixture script against the local test database**

Run (PowerShell, adjust the Firebird install path if different — check
`C:\Program Files\Firebird\Firebird_5_0\isql.exe` exists first):

```powershell
& "C:\Program Files\Firebird\Firebird_5_0\isql.exe" -user SYSDBA -password masterkey -i scripts\fixture_index_profile.sql C:\fbtest\test.fdb
```

Expected: no output (isql is silent on success). Verify the tables exist:

```powershell
& "C:\Program Files\Firebird\Firebird_5_0\isql.exe" -user SYSDBA -password masterkey C:\fbtest\test.fdb -a
```

then inspect the output for `CREATE TABLE "TNO_INDEX"` and `CREATE TABLE "TIDX_INACTIVE"` (the `-a` flag extracts the full DDL of the database; if your `isql` build doesn't support `-a`, instead run interactively and `SELECT RDB$RELATION_NAME FROM RDB$RELATIONS WHERE RDB$RELATION_NAME IN ('TNO_INDEX','TIDX_INACTIVE');` — expect 2 rows).

- [ ] **Step 4: Update the exact relation-list assertion**

In `test/sql/firebird_metadata.test`, the query around lines 83–97 asserts the complete, alphabetically-ordered relation list. Replace:

```
fb	main	DEPT	BASE TABLE
fb	main	EMPLOYEE	BASE TABLE
fb	main	FILE_STORAGE	BASE TABLE
fb	main	TCHILD	BASE TABLE
fb	main	TPK_COMPOSITE	BASE TABLE
fb	main	TQUOTES	BASE TABLE
fb	main	V_ACTIVE_EMP	BASE TABLE
fb	main	V_ALL_EMP	BASE TABLE
fb	main	V_DEPT_HEADCOUNT	BASE TABLE
```

with:

```
fb	main	DEPT	BASE TABLE
fb	main	EMPLOYEE	BASE TABLE
fb	main	FILE_STORAGE	BASE TABLE
fb	main	TCHILD	BASE TABLE
fb	main	TIDX_INACTIVE	BASE TABLE
fb	main	TNO_INDEX	BASE TABLE
fb	main	TPK_COMPOSITE	BASE TABLE
fb	main	TQUOTES	BASE TABLE
fb	main	V_ACTIVE_EMP	BASE TABLE
fb	main	V_ALL_EMP	BASE TABLE
fb	main	V_DEPT_HEADCOUNT	BASE TABLE
```

(inserted alphabetically: `TCHILD` < `TIDX_INACTIVE` < `TNO_INDEX` < `TPK_COMPOSITE`).

- [ ] **Step 5: Build once (first build in this worktree) and run the affected test**

Run: `build_windows_local.bat` from the worktree root (slow — first build in `C:\tmp\fbwt-idx`; full DuckDB).

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-idx/test/sql/firebird_metadata.test"`
Expected: `All tests passed (N assertions)`.

- [ ] **Step 6: Commit**

```bash
git add scripts/setup_test_firebird.sh scripts/fixture_index_profile.sql test/sql/firebird_metadata.test
git commit -m "test(index-profile): add zero-index and inactive-index fixtures"
```

---

### Task 2: firebird_index_profile function (core)

**Files:**
- Create: `src/include/firebird_index_profile.hpp`
- Create: `src/firebird_index_profile.cpp`
- Modify: `src/firebird_extension.cpp` (add `#include "firebird_index_profile.hpp"` after line 8, the `firebird_health.hpp` include; add `loader.RegisterFunction(GetFirebirdIndexProfileFunction());` after line 38, the `GetFirebirdHealthFunction()` registration)
- Modify: `CMakeLists.txt` (add `src/firebird_index_profile.cpp` to `EXTENSION_SOURCES`, right after `src/firebird_profile_table.cpp`)
- Test: `test/sql/firebird_index_profile.test`

**Interfaces:**
- Consumes: `AcquireFirebirdCatalogLease`/`ValidateFirebirdAttachAlias` from `firebird_dbt_sources.hpp`; `LoadTableSchema` + `NoneEncoding` + `FirebirdColumnDesc` from `firebird_scanner.hpp`; `FirebirdConnection`/`FirebirdStatement` accessors (`OpenCursor`, `Fetch`, `IsNull`, `GetText`, `GetShort`, `GetDouble`) from `firebird_client.hpp` (transitively included).
- Produces: `TableFunction GetFirebirdIndexProfileFunction();` registered as `firebird_index_profile(VARCHAR)`, multi-row, 9 columns named/typed exactly as in Step 3 below.

- [ ] **Step 1: Write the failing tests**

Requires Task 1's fixtures already applied to the local `test.fdb` (they are — Task 1 is complete before this task starts). Create `test/sql/firebird_index_profile.test`:

```
# name: test/sql/firebird_index_profile.test
# description: firebird_index_profile - per-index diagnostic: columns,
#              is_unique/is_active/is_primary_key/is_foreign_key,
#              selectivity, structured alerts, unindexed_filter_candidates,
#              synthetic zero-index row.
# group: [firebird]
#
# Requires the EMPLOYEE fixture (PK + FK + expression index) and the
# TNO_INDEX / TIDX_INACTIVE fixtures from scripts/setup_test_firebird.sh /
# scripts/fixture_index_profile.sql.

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement error
SELECT * FROM firebird_index_profile(NULL);
----
firebird_index_profile(qualified_name VARCHAR): qualified_name is required

statement error
SELECT * FROM firebird_index_profile('fb.main.EMPLOYEE.EXTRA');
----
firebird_index_profile(qualified_name VARCHAR): expected

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

statement error
SELECT * FROM firebird_index_profile('fb.main.NOPE_DOES_NOT_EXIST_XYZ');
----
firebird_index_profile: relation 'NOPE_DOES_NOT_EXIST_XYZ' not found

# ---------------------------------------------------------------------------
#  EMPLOYEE: PK + FK + one expression index -> more than one row.
# ---------------------------------------------------------------------------

statement ok
CREATE OR REPLACE TABLE t_idx_emp AS
SELECT * FROM firebird_index_profile('fb.main.EMPLOYEE');

query T
SELECT COUNT(*) > 1 FROM t_idx_emp;
----
true

# The PK-backing index row: is_primary_key + is_unique, index_name present
# (not the synthetic row). Name is auto-generated by Firebird, so match by
# flag, not by name.

query T
SELECT COUNT(*) = 1 FROM t_idx_emp
 WHERE is_primary_key = true AND is_unique = true AND index_name IS NOT NULL;
----
true

# The FK-backing index row: is_foreign_key true.

query T
SELECT COUNT(*) = 1 FROM t_idx_emp WHERE is_foreign_key = true;
----
true

# The expression index (EMP_UPPER_NAME_IDX) has no segment columns.

query T
SELECT columns FROM t_idx_emp WHERE index_name = 'EMP_UPPER_NAME_IDX';
----
[]

# No index in this fixture DB has ever had SET STATISTICS run, so every
# real (non-synthetic) row's selectivity is NULL, and every real row
# carries the missing_statistics/LOW alert.

query I
SELECT COUNT(*) FROM t_idx_emp WHERE index_name IS NOT NULL AND selectivity IS NOT NULL;
----
0

query I
SELECT COUNT(*) FROM t_idx_emp
 WHERE index_name IS NOT NULL
   AND NOT list_contains(list_transform(alerts, a -> a.code), 'missing_statistics');
----
0

# ---------------------------------------------------------------------------
#  TNO_INDEX: zero indexes -> exactly 1 synthetic row.
# ---------------------------------------------------------------------------

statement ok
CREATE OR REPLACE TABLE t_idx_none AS
SELECT * FROM firebird_index_profile('fb.main.TNO_INDEX');

query I
SELECT COUNT(*) FROM t_idx_none;
----
1

query T
SELECT index_name IS NULL AND columns = [] AND is_unique IS NULL
   AND is_active IS NULL AND is_primary_key IS NULL AND is_foreign_key IS NULL
   AND selectivity IS NULL
FROM t_idx_none;
----
true

query T
SELECT list_contains(list_transform(alerts, a -> a.code), 'no_indexes_on_table')
FROM t_idx_none;
----
true

query TT
SELECT a.code, a.severity FROM (
  SELECT UNNEST(alerts) AS a FROM t_idx_none
) t
WHERE a.code = 'no_indexes_on_table';
----
no_indexes_on_table	HIGH

# No column of TNO_INDEX is covered by any index, so all 3 eligible
# columns (CODE VARCHAR, QTY INTEGER, NOTE VARCHAR) are candidates.

query T
SELECT list_contains(unindexed_filter_candidates, 'CODE')
   AND list_contains(unindexed_filter_candidates, 'QTY')
   AND list_contains(unindexed_filter_candidates, 'NOTE')
FROM t_idx_none;
----
true

# ---------------------------------------------------------------------------
#  TIDX_INACTIVE: PK (active) + one secondary index later disabled.
# ---------------------------------------------------------------------------

statement ok
CREATE OR REPLACE TABLE t_idx_inactive AS
SELECT * FROM firebird_index_profile('fb.main.TIDX_INACTIVE');

query I
SELECT COUNT(*) FROM t_idx_inactive;
----
2

query TT
SELECT is_active, list_contains(list_transform(alerts, a -> a.code), 'index_inactive')
FROM t_idx_inactive WHERE index_name = 'IDX_TIDX_INACTIVE_QTY';
----
false	true

query T
SELECT is_active FROM t_idx_inactive WHERE is_primary_key = true;
----
true

# unindexed_filter_candidates excludes QTY (covered by the inactive index)
# and ID (covered by the PK) but includes NOTE (never indexed at all).

query T
SELECT list_contains(unindexed_filter_candidates, 'NOTE')
   AND NOT list_contains(unindexed_filter_candidates, 'QTY')
   AND NOT list_contains(unindexed_filter_candidates, 'ID')
FROM t_idx_inactive;
----
true
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-idx/test/sql/firebird_index_profile.test"`
Expected: FAIL — `firebird_index_profile` is not a registered function (Catalog Error / function does not exist).

- [ ] **Step 3: Create the header**

Create `src/include/firebird_index_profile.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// firebird_index_profile(qualified_name VARCHAR)
//
// Per-index, read-only diagnostic for a Firebird relation reachable
// through an attached Firebird catalog. The argument is a qualified name
// in `catalog.schema.table` form (e.g. 'fb.main.CUSTOMER'); the schema
// part is accepted only as 'main' (the Firebird ATTACH path exposes
// exactly one schema) and may be omitted ('fb.CUSTOMER'). Missing
// relation -> BinderException.
//
// Grain: ONE ROW PER EXISTING INDEX. If the table has zero indexes, emits
// exactly ONE synthetic row instead of zero rows, so the "no indexes at
// all" signal is never silently lost to the per-index grain:
//   - index_name IS NULL marks that synthetic row (never a real index)
//   - every other index-scoped column is NULL on that row
//   - alerts carries {code: 'no_indexes_on_table', severity: 'HIGH', ...}
//
// Columns:
//   - index_name                  VARCHAR, nullable (NULL = synthetic row)
//   - columns                     LIST(VARCHAR)  index segment columns,
//                                 ordered; empty for an expression index
//                                 or the synthetic row
//   - is_unique                   BOOLEAN, nullable
//   - is_active                   BOOLEAN, nullable
//   - is_primary_key              BOOLEAN, nullable
//   - is_foreign_key              BOOLEAN, nullable
//   - selectivity                 DOUBLE, nullable (RDB$STATISTICS; a
//                                 LOWER value tends to indicate a MORE
//                                 selective index; NULL means the
//                                 statistic was never computed — not
//                                 "stale". No numeric threshold-based
//                                 alert exists for this value yet.)
//   - alerts                      LIST(STRUCT(code, severity, message))
//                                 codes (stable public contract):
//                                 no_indexes_on_table (HIGH),
//                                 index_inactive (MEDIUM),
//                                 missing_statistics (LOW)
//   - unindexed_filter_candidates LIST(VARCHAR) table-level fact,
//                                 repeated on every row. Excludes any
//                                 column covered by ANY index segment,
//                                 including an inactive index's segments.
//
// This is a factual diagnostic, not a cost-based advisor: there is no
// numeric selectivity threshold in this version.
TableFunction GetFirebirdIndexProfileFunction();

} // namespace duckdb
```

- [ ] **Step 4: Create the implementation**

Create `src/firebird_index_profile.cpp`:

```cpp
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
```

- [ ] **Step 5: Register the function**

In `src/firebird_extension.cpp`, add the include after the `firebird_health.hpp` include (currently line 8):

```cpp
#include "firebird_index_profile.hpp"
```

And register it after the `GetFirebirdHealthFunction()` registration (currently line 38):

```cpp
    loader.RegisterFunction(GetFirebirdIndexProfileFunction());
```

- [ ] **Step 6: Add the source file to CMakeLists.txt**

In `CMakeLists.txt`, in the `EXTENSION_SOURCES` list, add the new file right after `src/firebird_profile_table.cpp` (currently line 30):

```cmake
    src/firebird_index_profile.cpp
```

- [ ] **Step 7: Build**

Run: `build_windows_local.bat`
Expected: build succeeds; `build\release\extension\firebird\firebird.duckdb_extension` produced.

- [ ] **Step 8: Run the test to verify it passes**

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-idx/test/sql/firebird_index_profile.test"`
Expected: `All tests passed (N assertions)`.

- [ ] **Step 9: Commit**

```bash
git add src/include/firebird_index_profile.hpp src/firebird_index_profile.cpp src/firebird_extension.cpp CMakeLists.txt test/sql/firebird_index_profile.test
git commit -m "feat(index-profile): firebird_index_profile per-index diagnostic"
```

---

### Task 3: Matrix + documentation

**Files:**
- Modify: `scripts/build_matrix.ps1` (add to `$testFixtureVar`)
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (run the new test)
- Modify: `docs/pt/function_manual.md`, `docs/en/function_manual.md`

**Interfaces:**
- Consumes: `firebird_index_profile(VARCHAR)` from Task 2.
- Produces: nothing (matrix wiring + docs).

- [ ] **Step 1: Add the test to the local matrix**

In `scripts/build_matrix.ps1`, add an entry to the `$testFixtureVar` hashtable next to the other `FIREBIRD_TEST_DB` tests:

```powershell
    'firebird_index_profile.test'      = 'FIREBIRD_TEST_DB'
```

- [ ] **Step 2: Add the test to the Linux FB matrix workflow**

In `.github/workflows/build-linux-fb-matrix.yml`, locate the step that runs the `FIREBIRD_TEST_DB`-backed tests (the EMPLOYEE-fixture step, the same one `firebird_health.test` and `firebird_profile_table.test` were added to) and add `firebird_index_profile.test` to its test list, matching the existing `unittest ... test/sql/<name>.test` invocation pattern exactly.

- [ ] **Step 3: Document in PT manual**

In `docs/pt/function_manual.md`, add a `firebird_index_profile` section (follow the layout of the existing `firebird_profile_table` section): purpose, signature `firebird_index_profile('fb.main.TABLE')`, the 9-column table, the 3-code alert table with severities, and these notes verbatim in spirit:

- Grão por índice: tabela sem nenhum índice emite **uma linha sintética** (`index_name IS NULL`), não zero linhas — o sinal "sem índice algum" nunca se perde.
- `selectivity` (RDB$STATISTICS): valor **bruto**, sem interpretação — um valor **menor** tende a indicar índice **mais seletivo**; `NULL` significa que a estatística nunca foi calculada (não significa "desatualizada"). **Sem limiar numérico** nesta versão (evita virar advisor opaco).
- `unindexed_filter_candidates` exclui colunas cobertas por **qualquer** segmento de índice, mesmo de índice **inativo** — esse sinal de inatividade já vem por `index_inactive`.

- [ ] **Step 4: Document in EN manual (parity)**

In `docs/en/function_manual.md`, add the equivalent `firebird_index_profile` section with the same column table, alert table, and the three notes translated to English (per-index grain with the synthetic zero-index row; raw nullable `selectivity` with the lower-is-more-selective direction and no numeric threshold; `unindexed_filter_candidates` excludes columns covered by any index including inactive ones).

- [ ] **Step 5: Commit**

```bash
git add scripts/build_matrix.ps1 .github/workflows/build-linux-fb-matrix.yml docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(index-profile): matrix entry + PT/EN function_manual"
```

---

## Self-Review

**Spec coverage:**
- Multi-row, per-index grain → Task 2 (`IndexProfileGlobalState`/row-cursor). ✔
- 9-column contract with exact names/types → Task 2 Steps 3–4. ✔
- Synthetic zero-index row (`index_name IS NULL`, other columns NULL, `no_indexes_on_table`/HIGH) → Task 2 `BuildIndexProfile` + tested in Task 2 Step 1. ✔
- `missing_statistics` (not `stale_statistics`) → Task 2. ✔
- `index_inactive`/MEDIUM, no numeric `low_selectivity` → Task 2 (only 3 codes total). ✔
- `unindexed_filter_candidates` excludes columns covered by any index segment including inactive → Task 2 `is_covered` built from ALL rows' `columns` regardless of `is_active`; tested via `TIDX_INACTIVE` case. ✔
- `selectivity` raw nullable DOUBLE via `GetDouble` → Task 2. ✔
- `is_foreign_key` dependency flag → Task 2 second LEFT JOIN; tested via EMPLOYEE's FK. ✔
- Zero-index + inactive-index fixtures → Task 1. ✔
- Required companion `firebird_metadata.test` relation-list update → Task 1 Step 4. ✔
- Matrix + PT/EN docs parity → Task 3. ✔

**Placeholder scan:** No TBD/TODO; all code blocks complete; no "add error handling" hand-waving; the deliberate no-catch decision in `LoadIndexRows` is explained inline, not hand-waved. ✔

**Type consistency:** `GetFirebirdIndexProfileFunction` name consistent across hpp/cpp/extension/CMakeLists; column names identical between `IndexProfileBind`'s `names` and the test file's `SELECT`s; `Alert`/`AddAlert`/`AlertStructList`/`VarcharList` names consistent within the new file (intentionally NOT shared with `firebird_profile_table.cpp`'s identically-named-but-separate copies — both are `static` and file-scoped, so no link collision). `IndexProfileRow.has_selectivity`/`.selectivity` used consistently in both `LoadIndexRows` and `IndexProfileInitGlobal`. ✔

**Note for implementer (Task 2):** Every hardcoded `"firebird_profile_table"` string that exists in the ORIGINAL `ParseQualifiedName`/`SqlLiteral` (in `firebird_profile_table.cpp`) has already been renamed to `"firebird_index_profile"` in this plan's Step 4 code — do not copy-paste from the original file, transcribe the code exactly as given in this plan.
