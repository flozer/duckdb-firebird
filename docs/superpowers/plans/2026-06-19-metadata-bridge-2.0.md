# Metadata Bridge 2.0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface Firebird PKs/UNIQUE/FKs through standard `information_schema`, plus typed FB-specific table functions for indexes, foreign-key rules, generators, domains, computed columns, dependencies and comments — all read-only.

**Architecture:** Hybrid. Constraints are attached to each `FirebirdTableEntry` at catalog-load time (`src/firebird_storage.cpp`) so DuckDB derives `information_schema.table_constraints` / `key_column_usage` / `referential_constraints` for free, exactly as the existing `NotNullConstraint` already drives `information_schema.columns.is_nullable`. Everything without a faithful DuckDB equivalent ships as a typed table function in a new `src/firebird_metadata_functions.cpp`, modelled on `firebird_tables` (multi-row) + `firebird_generate_dbt_sources` (catalog-alias lease).

**Tech Stack:** C++ (DuckDB out-of-tree extension), Firebird `RDB$` system tables via the existing `FirebirdConnection`/cursor API, DuckDB sqllogic `.test` files, Firebird `isql` fixtures, PowerShell build matrix.

## Global Constraints

- **Read-only.** No writes to Firebird. CREATE/ALTER/DROP stay `Unsupported`. Copied verbatim from spec.
- **No community/upstream action** (`duckdb/community-extensions`) without Fernando's explicit authorization. Compatibility passing does not authorize it.
- **Schema is the literal constant `"main"`** for every `*_schema` column (Firebird has no schemas; catalog exposes `fb.main.*`). Never derived from `RDB$`.
- **All `RDB$` identifiers are space-padded `CHAR`** — apply `TRIM`/`RTRIM` (server-side in SQL) to every identifier before returning.
- **Unknown type codes never drop a row:** label column becomes `'UNKNOWN'` and the raw numeric code stays in its own `*_type_code` column.
- **Determinism:** every function query ends with an explicit `ORDER BY`.
- **Test environment (Windows):** Firebird 5 running; `C:\fbtest\test.fdb` provisioned; env `FIREBIRD_TEST_DB=C:\fbtest\test.fdb`, `FIREBIRD_DECFLOAT_DB`, `FIREBIRD_NONE_DB`, `ISC_USER=SYSDBA`, `ISC_PASSWORD=masterkey` set before running `unittest.exe`.
- **Build (incremental):** `scripts/build_windows_local.bat` (first build slow; ninja incremental after). Cross-version gate: `scripts/build_matrix.ps1`.
- **Run a single test:** from repo root, `build\release\test\unittest.exe test/sql/<name>.test` (env vars set; `fbclient.dll` already copied next to the binary by the build script).

---

## Cursor API reference (existing, used throughout)

From `firebird_dbt_sources.cpp` / `firebird_scanner.cpp`:

- `auto cur = conn.OpenCursor(sql);` then `while (cur->Fetch()) { ... }`
- `cur->GetText(i)` → `std::string`; `cur->GetShort(i)` → int16; `cur->GetLong(i)` → int32; `cur->IsNull(i)` → bool
- Catalog-alias lease (operate on an ATTACHed `fb`): `auto lease = AcquireFirebirdCatalogLease(context, catalog_name); FirebirdConnection &conn = *lease.conn;` — releases on scope exit.
- Alias validation at bind time: `ValidateFirebirdAttachAlias(context, catalog_name);`
- Emit any typed value (incl. NULL) into output: `output.data[c].SetValue(row, value);` where `value` is a `Value` (`Value()` = SQL NULL).
- Register in `src/firebird_extension.cpp`: `loader.RegisterFunction(GetFirebird<X>Function());`

---

## Task 1: Fixture expansion + cascade updates

Add FK / composite-FK / domain / generator / comments / computed-column coverage to the canonical fixture, in BOTH fixture scripts, idempotently, and update the two tests whose counts/ordering shift.

**Files:**
- Modify: `scripts/setup_test_firebird.sh` (Linux/CI heredoc) — add objects before the final `EOF`
- Modify: `scripts/smoke_fixture.sql` (Windows) — mirror identically
- Modify: `test/sql/firebird_metadata.test` (table count 9→? , column count 27→?)
- Modify: `test/sql/firebird_dbt_sources.test` (object ordering)
- Provision: `C:\fbtest\test.fdb` (re-run fixture via `isql`)

**Interfaces:**
- Produces (fixture objects later tasks rely on): table `DEPT(DEPT_NO VARCHAR(3) PK, DEPT_NAME VARCHAR(40))`; FK `FK_EMP_DEPT` on `EMPLOYEE(DEPT_NO) → DEPT(DEPT_NO)` `ON UPDATE CASCADE ON DELETE SET NULL`; table `TCHILD(PARENT_A INT, PARENT_B INT, NOTE VARCHAR(20))` with composite FK `FK_TCHILD_TPK` on `(PARENT_A, PARENT_B) → TPK_COMPOSITE(A,B)` `ON UPDATE NO ACTION ON DELETE CASCADE`; `DOMAIN D_SALARY AS NUMERIC(10,2) CHECK (VALUE > 0)`; `GENERATOR GEN_EMP_ID` initial value 100; `COMMENT ON TABLE EMPLOYEE`; `COMMENT ON COLUMN EMPLOYEE.EMP_NAME`; computed column `EMPLOYEE.NAME_LEN COMPUTED BY (CHAR_LENGTH(EMP_NAME))`.

- [ ] **Step 1: Add the new objects to `scripts/setup_test_firebird.sh`**

Insert immediately before the `CREATE TABLE TPK_COMPOSITE` block (so DEPT exists before EMPLOYEE's FK; but EMPLOYEE is already created earlier — add DEPT + FK via ALTER after EMPLOYEE, and place TCHILD after TPK_COMPOSITE). Concretely, after the existing `COMMIT;` that follows the `V_MULTILINE_KW` view, add:

```sql
-- Parent table for a single-column FK from EMPLOYEE.DEPT_NO.
CREATE TABLE DEPT (
    DEPT_NO   VARCHAR(3) NOT NULL PRIMARY KEY,
    DEPT_NAME VARCHAR(40)
);
INSERT INTO DEPT VALUES ('600', 'Engineering');
INSERT INTO DEPT VALUES ('700', 'Sales');
INSERT INTO DEPT VALUES ('900', 'Support');
COMMIT;

-- Single-column FK with explicit referential actions.
ALTER TABLE EMPLOYEE
    ADD CONSTRAINT FK_EMP_DEPT FOREIGN KEY (DEPT_NO)
    REFERENCES DEPT (DEPT_NO) ON UPDATE CASCADE ON DELETE SET NULL;
COMMIT;

-- Domain (named field) for firebird_domains coverage.
CREATE DOMAIN D_SALARY AS NUMERIC(10,2) CHECK (VALUE > 0);
COMMIT;

-- Generator with a known initial value for firebird_generators.
CREATE SEQUENCE GEN_EMP_ID START WITH 100;
COMMIT;

-- Computed column for firebird_computed_columns.
ALTER TABLE EMPLOYEE ADD NAME_LEN COMPUTED BY (CHAR_LENGTH(EMP_NAME));
COMMIT;

-- Comments for firebird_comments (TABLE + COLUMN).
COMMENT ON TABLE EMPLOYEE IS 'Staff records fixture';
COMMENT ON COLUMN EMPLOYEE.EMP_NAME IS 'Full name';
COMMIT;
```

And after the `TPK_COMPOSITE` block add the composite-FK child:

```sql
-- Composite FK child: exercises multi-column FK, ordinal position,
-- referenced composite PK, and ON UPDATE/ON DELETE rules.
CREATE TABLE TCHILD (
    PARENT_A INTEGER NOT NULL,
    PARENT_B INTEGER NOT NULL,
    NOTE     VARCHAR(20),
    CONSTRAINT FK_TCHILD_TPK FOREIGN KEY (PARENT_A, PARENT_B)
        REFERENCES TPK_COMPOSITE (A, B) ON UPDATE NO ACTION ON DELETE CASCADE
);
COMMIT;
```

- [ ] **Step 2: Mirror the same blocks into `scripts/smoke_fixture.sql`**

`smoke_fixture.sql` already creates `TQUOTES` + `TPK_COMPOSITE`. Add the identical `DEPT`/FK/`D_SALARY`/`GEN_EMP_ID`/computed-column/comments/`TCHILD` statements (same names, same actions, same `START WITH 100`) so the Windows fixture matches the CI fixture. Keep the trailing `QUIT;`.

- [ ] **Step 3: Re-provision the local test DB**

Run:
```bash
ISQL="/c/Program Files/Firebird/Firebird_5_0/isql.exe"
"$ISQL" -u SYSDBA -p masterkey "C:\\fbtest\\test.fdb" -i scripts/smoke_fixture.sql
```
Expected: no `Statement failed`. Verify: `DEPT`=3 rows, `TCHILD` exists, `GEN_EMP_ID` exists.
(`smoke_fixture.sql` does not create the EMPLOYEE base fixture; if `test.fdb` lacks EMPLOYEE, recreate it first from the EMPLOYEE block — the DB provisioned in the compatibility front already has EMPLOYEE + TQUOTES + TPK_COMPOSITE, so only the new objects are appended. If an object already exists, drop+recreate or recreate the DB from scratch for a clean idempotent state.)

- [ ] **Step 4: Update `firebird_metadata.test` counts**

The table list (`SHOW TABLES FROM fb` and the `information_schema.tables` query, both `ORDER BY`) now includes `DEPT` and `TCHILD`. Recompute and edit:
- Table-count assertion: was 9 → now 11 (adds `DEPT`, `TCHILD`).
- `information_schema.tables` expected block: insert `DEPT` (after `BOB`-less alpha order: `DEPT` sorts after `... ` before `EMPLOYEE`) and `TCHILD` (after `TPK_COMPOSITE`/`TQUOTES`), all `BASE TABLE`. Keep alphabetical.
- `information_schema.columns` total count: was 27 → add DEPT(2) + TCHILD(3) + EMPLOYEE.NAME_LEN(1) = 33. Edit the `COUNT(*)` expected value to 33.

Run to discover exact ordering rather than guessing:
```bash
build\release\test\unittest.exe test/sql/firebird_metadata.test
```
Read the actual-vs-expected diff and set the expected blocks to the actual catalog output (deterministic because every query is `ORDER BY`).

- [ ] **Step 5: Update `firebird_dbt_sources.test` ordering**

The ordering chain assertion (currently `EMPLOYEE < FILE_STORAGE < TPK_COMPOSITE < TQUOTES < V_ACTIVE_EMP`) must include `DEPT` and `TCHILD`. New alphabetical chain: `DEPT < EMPLOYEE < FILE_STORAGE < TCHILD < TPK_COMPOSITE < TQUOTES < V_ACTIVE_EMP`. Edit the `instr()` chain accordingly. Composite-PK note (line ~132) still holds for `TPK_COMPOSITE`; `TCHILD` has no PK so emits no `tests:` block — add an assertion only if the file already pattern-asserts per table (otherwise leave).

- [ ] **Step 6: Run both updated tests, confirm green**

Run:
```bash
build\release\test\unittest.exe test/sql/firebird_metadata.test
build\release\test\unittest.exe test/sql/firebird_dbt_sources.test
```
Expected: `All tests passed` for both.

- [ ] **Step 7: Commit**

```bash
git add scripts/setup_test_firebird.sh scripts/smoke_fixture.sql test/sql/firebird_metadata.test test/sql/firebird_dbt_sources.test
git commit -m "test(fixture): add FK/composite-FK/domain/generator/comment/computed coverage"
```

---

## Task 2: Shared scaffold for FB metadata table functions

A single small runner so each metadata function is just (name, columns, sql, row-mapper). DRY; keeps each later task tiny with complete code.

**Files:**
- Create: `src/include/firebird_metadata_functions.hpp`
- Create: `src/firebird_metadata_functions.cpp`
- Modify: `src/firebird_extension.cpp` (include header; registrations added per later task)
- Modify: `CMakeLists.txt` (add `src/firebird_metadata_functions.cpp` to the sources list)

**Interfaces:**
- Produces: `MetadataFn` descriptor + `MakeMetadataFunction(...)`; per-function value helpers `TextOrNull`, `IntOrNull`, `BoolFromFlag`; and the `Get*Function()` declarations (bodies land in later tasks).

- [ ] **Step 1: Write the header**

Create `src/include/firebird_metadata_functions.hpp`:

```cpp
#pragma once
#include "duckdb/function/table_function.hpp"

namespace duckdb {
TableFunction GetFirebirdIndexesFunction();
TableFunction GetFirebirdForeignKeysFunction();
TableFunction GetFirebirdGeneratorsFunction();
TableFunction GetFirebirdDomainsFunction();
TableFunction GetFirebirdComputedColumnsFunction();
TableFunction GetFirebirdDependenciesFunction();
TableFunction GetFirebirdCommentsFunction();
} // namespace duckdb
```

- [ ] **Step 2: Write the scaffold in the .cpp**

Create `src/firebird_metadata_functions.cpp`:

```cpp
#include "firebird_metadata_functions.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_scanner.hpp"   // AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias, FirebirdConnection

#include <functional>
#include <string>
#include <vector>

namespace duckdb {

// Value helpers: RDB$ NULL -> SQL NULL, never a silent default.
static Value TextOrNull(FirebirdCursor &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::VARCHAR) : Value(c.GetText(i));
}
static Value IntOrNull(FirebirdCursor &c, idx_t i) {
    return c.IsNull(i) ? Value(LogicalType::INTEGER)
                       : Value::INTEGER(c.GetLong(i));
}
static Value BoolFromFlag(FirebirdCursor &c, idx_t i) { // RDB$ smallint flag
    return Value::BOOLEAN(!c.IsNull(i) && c.GetShort(i) != 0);
}

// One descriptor drives the whole function.
struct MetadataFn {
    std::string                 name;
    std::vector<std::string>    col_names;
    std::vector<LogicalType>    col_types;
    std::string                 sql;
    // Map the current cursor row to one Value per output column.
    std::function<std::vector<Value>(FirebirdCursor &)> map_row;
};

struct MetaBindData : public TableFunctionData {
    std::string catalog_name;
    const MetadataFn *desc = nullptr;
};
struct MetaGlobalState : public GlobalTableFunctionState {
    std::vector<std::vector<Value>> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
MetaBind(ClientContext &context, TableFunctionBindInput &input,
         vector<LogicalType> &return_types, vector<string> &names,
         const MetadataFn &desc) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException("%s(catalog_name VARCHAR): catalog_name is "
                              "required (the alias from ATTACH ... (TYPE firebird)).",
                              desc.name);
    }
    auto bind = make_uniq<MetaBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    bind->desc = &desc;
    ValidateFirebirdAttachAlias(context, bind->catalog_name);
    names = desc.col_names;
    return_types = desc.col_types;
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
MetaInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<MetaBindData>();
    auto g = make_uniq<MetaGlobalState>();
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    auto cur = lease.conn->OpenCursor(bind.desc->sql);
    while (cur->Fetch()) {
        g->rows.push_back(bind.desc->map_row(*cur));
    }
    return std::move(g);
}

static void MetaFunction(ClientContext &, TableFunctionInput &input,
                         DataChunk &output) {
    auto &g = input.global_state->Cast<MetaGlobalState>();
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

// Bind a concrete descriptor (static storage) into a TableFunction.
static TableFunction MakeMetadataFunction(const MetadataFn &desc) {
    TableFunction fn(desc.name, {LogicalType::VARCHAR}, MetaFunction,
                     [&desc](ClientContext &ctx, TableFunctionBindInput &in,
                             vector<LogicalType> &rt, vector<string> &nm) {
                         return MetaBind(ctx, in, rt, nm, desc);
                     },
                     MetaInitGlobal);
    return fn;
}

} // namespace duckdb
```

NOTE on the bind lambda capturing `desc`: store each `MetadataFn` in a function-local `static` (defined in the per-function `Get*Function()` body in later tasks) so the reference outlives bind. Confirm `FirebirdCursor` is the actual cursor type name in `firebird_scanner.hpp`; if the header names it differently (e.g. the type returned by `OpenCursor`), adjust the helper signatures to that type. Confirm `GetLong`/`GetShort`/`GetText`/`IsNull` exist on it (they are used in `firebird_scanner.cpp`).

- [ ] **Step 3: Add the source to CMake**

In `CMakeLists.txt`, find the `set(EXTENSION_SOURCES ...)` (or equivalent list including `src/firebird_dbt_sources.cpp`) and add `src/firebird_metadata_functions.cpp`.

- [ ] **Step 4: Include the header in the extension entry**

In `src/firebird_extension.cpp`, add `#include "firebird_metadata_functions.hpp"` near the other includes. (Registrations are added per later task.)

- [ ] **Step 5: Build to verify the scaffold compiles**

Run: `scripts/build_windows_local.bat`
Expected: build succeeds, `firebird.duckdb_extension` produced. (No function registered yet — nothing to test functionally.)

- [ ] **Step 6: Commit**

```bash
git add src/include/firebird_metadata_functions.hpp src/firebird_metadata_functions.cpp src/firebird_extension.cpp CMakeLists.txt
git commit -m "feat(metadata): scaffold for FB-specific metadata table functions"
```

---

## Task 3: PK/UNIQUE constraints into information_schema

Attach `UniqueConstraint` objects at catalog load so DuckDB derives `table_constraints` + `key_column_usage` for PRIMARY KEY and UNIQUE.

**Files:**
- Modify: `src/firebird_storage.cpp` (the `add_entry` lambda, ~248-269, and add a constraint loader)
- Create: `test/sql/firebird_metadata_bridge.test`

**Interfaces:**
- Consumes: fixture from Task 1 (PK on EMPLOYEE/DEPT/TQUOTES/FILE_STORAGE; composite PK `TPK_COMPOSITE(A,B)`).
- Produces: populated `information_schema.table_constraints` (`PRIMARY KEY`, `UNIQUE`) and `key_column_usage` rows for the attached `fb` catalog.

- [ ] **Step 1: Write the failing test (information_schema block)**

Create `test/sql/firebird_metadata_bridge.test`:

```
# name: test/sql/firebird_metadata_bridge.test
# description: Metadata Bridge 2.0 — information_schema constraints + FB metadata functions
# group: [firebird]

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

# --- table_constraints: PRIMARY KEY + UNIQUE ------------------------
query III
SELECT table_name, constraint_type, COUNT(*)
  FROM information_schema.table_constraints
 WHERE table_catalog = 'fb' AND constraint_type = 'PRIMARY KEY'
 GROUP BY table_name, constraint_type
 ORDER BY table_name;
----
DEPT	PRIMARY KEY	1
EMPLOYEE	PRIMARY KEY	1
FILE_STORAGE	PRIMARY KEY	1
TPK_COMPOSITE	PRIMARY KEY	1
TQUOTES	PRIMARY KEY	1

# --- key_column_usage: composite PK ordinal -------------------------
query II
SELECT column_name, ordinal_position
  FROM information_schema.key_column_usage
 WHERE table_catalog = 'fb' AND table_name = 'TPK_COMPOSITE'
 ORDER BY ordinal_position;
----
A	1
B	2
```

- [ ] **Step 2: Run, verify it fails**

Run: `build\release\test\unittest.exe test/sql/firebird_metadata_bridge.test`
Expected: FAIL — `table_constraints` returns 0 PRIMARY KEY rows (constraints not attached yet).

- [ ] **Step 3: Add a constraint loader in `firebird_storage.cpp`**

Above the `EnsureTablesLoaded` method, add a helper that loads PK/UNIQUE columns per table (one batched query, grouped by relation):

```cpp
struct UniqueKey {
    std::string constraint_name;
    bool        is_primary = false;
    duckdb::vector<std::string> columns; // ordered by segment position
};

// relation -> its PK/UNIQUE keys. One round-trip.
static std::unordered_map<std::string, std::vector<UniqueKey>>
LoadUniqueConstraints(FirebirdConnection &conn) {
    const std::string sql =
        "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
        "       rc.RDB$CONSTRAINT_TYPE, TRIM(seg.RDB$FIELD_NAME), "
        "       seg.RDB$FIELD_POSITION "
        "  FROM RDB$RELATION_CONSTRAINTS rc "
        "  JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        " WHERE rc.RDB$CONSTRAINT_TYPE IN ('PRIMARY KEY','UNIQUE') "
        " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, seg.RDB$FIELD_POSITION";
    std::unordered_map<std::string, std::vector<UniqueKey>> out;
    auto cur = conn.OpenCursor(sql);
    std::string cur_rel, cur_con;
    UniqueKey *active = nullptr;
    while (cur->Fetch()) {
        std::string rel = cur->GetText(0);
        std::string con = cur->GetText(1);
        std::string ctype = cur->GetText(2); // 'PRIMARY KEY' / 'UNIQUE'
        if (!active || rel != cur_rel || con != cur_con) {
            out[rel].push_back(UniqueKey{con, ctype == "PRIMARY KEY", {}});
            active = &out[rel].back();
            cur_rel = rel; cur_con = con;
        }
        active->columns.push_back(cur->GetText(3));
    }
    return out;
}
```

Add `#include <unordered_map>` if not present.

- [ ] **Step 4: Attach the constraints in `add_entry`**

`EnsureTablesLoaded` already holds an open `conn`. Before the `add_entry` lambda, load the map once:

```cpp
std::unordered_map<std::string, std::vector<UniqueKey>> unique_keys;
try { unique_keys = LoadUniqueConstraints(conn); } catch (std::exception &) { /* leave empty */ }
```

Inside `add_entry`, after the NOT NULL loop and before constructing the entry, add:

```cpp
auto uk_it = unique_keys.find(table_name);
if (uk_it != unique_keys.end()) {
    for (auto &key : uk_it->second) {
        info.constraints.push_back(
            make_uniq<UniqueConstraint>(key.columns, key.is_primary));
    }
}
```

`UniqueConstraint(vector<string> columns, bool is_primary_key)` is the multi-column constructor (header `duckdb/parser/constraints/unique_constraint.hpp` — add the include if the file doesn't already pull it via the catalog headers).

- [ ] **Step 5: Build + run, verify pass**

Run: `scripts/build_windows_local.bat` then
`build\release\test\unittest.exe test/sql/firebird_metadata_bridge.test`
Expected: PASS (both blocks). If `key_column_usage` ordinal is 0-based, adjust the expected `1,2` to match actual — set expected to the real output (deterministic).

- [ ] **Step 6: Re-run the full firebird suite (regression)**

Run each `test/sql/firebird_*.test`. Expected: all pass (constraints are additive; `firebird_metadata.test` already tolerant — if its column/constraint expectations shift, update to actual).

- [ ] **Step 7: Commit**

```bash
git add src/firebird_storage.cpp test/sql/firebird_metadata_bridge.test
git commit -m "feat(metadata): PK/UNIQUE constraints into information_schema"
```

---

## Task 4: FK constraints + referential_constraints verification

Attach `ForeignKeyConstraint`; empirically determine whether DuckDB surfaces FK rows and ON UPDATE/DELETE rules; record the finding.

**Files:**
- Modify: `src/firebird_storage.cpp` (FK loader + attach)
- Modify: `test/sql/firebird_metadata_bridge.test` (FK block)
- Modify: `docs/superpowers/specs/2026-06-19-metadata-bridge-2.0-design.md` (record verification outcome)

**Interfaces:**
- Consumes: `FK_EMP_DEPT` (single col), `FK_TCHILD_TPK` (composite) from Task 1.
- Produces: `information_schema.referential_constraints` / `table_constraints` FK rows (extent TBD by verification — drives Task 5 scope).

- [ ] **Step 1: Add an FK block to the test (table_constraints FK presence — always valid)**

Append to `firebird_metadata_bridge.test`:

```
# --- table_constraints: FOREIGN KEY presence ------------------------
query II
SELECT table_name, COUNT(*)
  FROM information_schema.table_constraints
 WHERE table_catalog = 'fb' AND constraint_type = 'FOREIGN KEY'
 GROUP BY table_name
 ORDER BY table_name;
----
EMPLOYEE	1
TCHILD	1
```

- [ ] **Step 2: Run, verify it fails**

Run: `build\release\test\unittest.exe test/sql/firebird_metadata_bridge.test`
Expected: FAIL — 0 FOREIGN KEY rows.

- [ ] **Step 3: Add the FK loader in `firebird_storage.cpp`**

```cpp
struct ForeignKey {
    std::string constraint_name;
    std::string pk_table;
    duckdb::vector<std::string> fk_columns; // ordered
    duckdb::vector<std::string> pk_columns; // ordered
};

static std::unordered_map<std::string, std::vector<ForeignKey>>
LoadForeignKeys(FirebirdConnection &conn) {
    const std::string sql =
        "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
        "       TRIM(uq.RDB$RELATION_NAME), TRIM(fkseg.RDB$FIELD_NAME), "
        "       TRIM(uqseg.RDB$FIELD_NAME), fkseg.RDB$FIELD_POSITION "
        "  FROM RDB$RELATION_CONSTRAINTS rc "
        "  JOIN RDB$REF_CONSTRAINTS ref ON ref.RDB$CONSTRAINT_NAME = rc.RDB$CONSTRAINT_NAME "
        "  JOIN RDB$RELATION_CONSTRAINTS uq ON uq.RDB$CONSTRAINT_NAME = ref.RDB$CONST_NAME_UQ "
        "  JOIN RDB$INDEX_SEGMENTS fkseg ON fkseg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        "  JOIN RDB$INDEX_SEGMENTS uqseg ON uqseg.RDB$INDEX_NAME = uq.RDB$INDEX_NAME "
        "       AND uqseg.RDB$FIELD_POSITION = fkseg.RDB$FIELD_POSITION "
        " WHERE rc.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
        " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, fkseg.RDB$FIELD_POSITION";
    std::unordered_map<std::string, std::vector<ForeignKey>> out;
    auto cur = conn.OpenCursor(sql);
    std::string cur_rel, cur_con; ForeignKey *active = nullptr;
    while (cur->Fetch()) {
        std::string rel = cur->GetText(0), con = cur->GetText(1);
        if (!active || rel != cur_rel || con != cur_con) {
            out[rel].push_back(ForeignKey{con, cur->GetText(2), {}, {}});
            active = &out[rel].back(); cur_rel = rel; cur_con = con;
        }
        active->fk_columns.push_back(cur->GetText(3));
        active->pk_columns.push_back(cur->GetText(4));
    }
    return out;
}
```

- [ ] **Step 4: Attach FK constraints in `add_entry`**

Load once next to `unique_keys`:
```cpp
std::unordered_map<std::string, std::vector<ForeignKey>> fkeys;
try { fkeys = LoadForeignKeys(conn); } catch (std::exception &) {}
```
Inside `add_entry`, after the unique-constraint loop:
```cpp
auto fk_it = fkeys.find(table_name);
if (fk_it != fkeys.end()) {
    for (auto &fk : fk_it->second) {
        ForeignKeyInfo fk_info;
        fk_info.type = ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE;
        fk_info.table = fk.pk_table;
        info.constraints.push_back(make_uniq<ForeignKeyConstraint>(
            fk.pk_columns, fk.fk_columns, std::move(fk_info)));
    }
}
```
Add include `duckdb/parser/constraints/foreign_key_constraint.hpp`. NOTE: `ForeignKeyInfo` may require `pk_keys`/`fk_keys` index vectors to be populated for DuckDB to accept it; if the build or runtime rejects it, leave those empty first and rely on the parser-level columns. This is the empirical step — see Step 6.

- [ ] **Step 5: Build + run the FK presence block**

Run: `scripts/build_windows_local.bat` then the test.
Expected: PASS for the table_constraints FK presence block. If DuckDB rejects the FK constraint (referenced table is another catalog entry), and the build/attach errors, fall back to verifying via a direct query and move FK surfacing entirely to Task 5 — document that PK/UNIQUE stay in info_schema and FK lives in `firebird_foreign_keys`.

- [ ] **Step 6: Probe referential_constraints for rules (verification, no assertion yet)**

Run manually:
```bash
build\release\duckdb.exe -c "ATTACH 'C:\fbtest\test.fdb' AS fb (TYPE firebird); SELECT * FROM information_schema.referential_constraints WHERE constraint_catalog='fb';"
```
Record in the spec's "Risco a verificar" section: whether FK rows appear, and whether `update_rule`/`delete_rule` are populated (DuckDB likely emits `NO ACTION` placeholders regardless). This determines Task 5: if rules are absent/wrong, `firebird_foreign_keys` is the authoritative rule surface.

- [ ] **Step 7: Commit**

```bash
git add src/firebird_storage.cpp test/sql/firebird_metadata_bridge.test docs/superpowers/specs/2026-06-19-metadata-bridge-2.0-design.md
git commit -m "feat(metadata): FK constraints into catalog + verify referential_constraints"
```

---

## Task 5: firebird_foreign_keys('fb')

Authoritative FK surface carrying columns, ordinal, referenced constraint, and ON UPDATE/ON DELETE rules — the fidelity that DuckDB's catalog drops. Does NOT replace information_schema (PK/UNIQUE/FK presence stay there).

**Files:**
- Modify: `src/firebird_metadata_functions.cpp` (function body + descriptor)
- Modify: `src/firebird_extension.cpp` (register)
- Modify: `test/sql/firebird_metadata_bridge.test` (FK rules block)

**Interfaces:**
- Consumes: scaffold from Task 2; FK fixtures from Task 1.
- Produces: `firebird_foreign_keys('fb')` → columns `fk_schema, fk_table, fk_constraint, ordinal_position, fk_column, pk_table, pk_constraint, update_rule, delete_rule`.

- [ ] **Step 1: Write the failing test block**

Append:

```
# --- firebird_foreign_keys: composite FK, ordinal, rules ------------
query IIIIII
SELECT fk_table, fk_constraint, ordinal_position, fk_column, pk_table, pk_constraint
  FROM firebird_foreign_keys('fb')
 WHERE fk_constraint = 'FK_TCHILD_TPK'
 ORDER BY ordinal_position;
----
TCHILD	FK_TCHILD_TPK	1	PARENT_A	TPK_COMPOSITE	PK_TPK_COMPOSITE
TCHILD	FK_TCHILD_TPK	2	PARENT_B	TPK_COMPOSITE	PK_TPK_COMPOSITE

# --- FK referential rules (the fidelity DuckDB drops) ---------------
query III
SELECT fk_constraint, update_rule, delete_rule
  FROM firebird_foreign_keys('fb')
 WHERE fk_table IN ('EMPLOYEE','TCHILD') AND ordinal_position = 1
 ORDER BY fk_constraint;
----
FK_EMP_DEPT	CASCADE	SET NULL
FK_TCHILD_TPK	NO ACTION	CASCADE
```

(Set expected `pk_constraint` to the actual referenced PK constraint name if Firebird auto-named it; `TPK_COMPOSITE`'s PK is `PK_TPK_COMPOSITE` per fixture. `DEPT`/`EMPLOYEE` PKs are auto-named — adjust `FK_EMP_DEPT`'s `pk_constraint` expectation to actual after first run.)

- [ ] **Step 2: Run, verify it fails**

Expected: FAIL — `firebird_foreign_keys` not registered (`Catalog Error: Table Function ... does not exist`).

- [ ] **Step 3: Implement the function body**

In `firebird_metadata_functions.cpp`, before the closing namespace:

```cpp
TableFunction GetFirebirdForeignKeysFunction() {
    static const MetadataFn desc{
        "firebird_foreign_keys",
        {"fk_schema","fk_table","fk_constraint","ordinal_position",
         "fk_column","pk_table","pk_constraint","update_rule","delete_rule"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::INTEGER,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},
        "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
        "       fkseg.RDB$FIELD_POSITION, TRIM(fkseg.RDB$FIELD_NAME), "
        "       TRIM(uq.RDB$RELATION_NAME), TRIM(ref.RDB$CONST_NAME_UQ), "
        "       TRIM(ref.RDB$UPDATE_RULE), TRIM(ref.RDB$DELETE_RULE) "
        "  FROM RDB$RELATION_CONSTRAINTS rc "
        "  JOIN RDB$REF_CONSTRAINTS ref ON ref.RDB$CONSTRAINT_NAME = rc.RDB$CONSTRAINT_NAME "
        "  JOIN RDB$RELATION_CONSTRAINTS uq ON uq.RDB$CONSTRAINT_NAME = ref.RDB$CONST_NAME_UQ "
        "  JOIN RDB$INDEX_SEGMENTS fkseg ON fkseg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
        " WHERE rc.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
        " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, fkseg.RDB$FIELD_POSITION",
        [](FirebirdCursor &c) -> std::vector<Value> {
            return {Value("main"), TextOrNull(c,0), TextOrNull(c,1),
                    IntOrNull(c,2), TextOrNull(c,3), TextOrNull(c,4),
                    TextOrNull(c,5), TextOrNull(c,6), TextOrNull(c,7)};
        }};
    return MakeMetadataFunction(desc);
}
```

(Output column order: `fk_schema` first, so the mapper prepends `Value("main")`; query selects the remaining 8.)

- [ ] **Step 4: Register**

In `src/firebird_extension.cpp` add: `loader.RegisterFunction(GetFirebirdForeignKeysFunction());`

- [ ] **Step 5: Build + run, verify pass**

Run: `scripts/build_windows_local.bat` then the test. Adjust `pk_constraint` expectations to actual auto-named PK constraints on first run, re-run, confirm PASS.

- [ ] **Step 6: Commit**

```bash
git add src/firebird_metadata_functions.cpp src/firebird_extension.cpp test/sql/firebird_metadata_bridge.test
git commit -m "feat(metadata): firebird_foreign_keys with referential rules"
```

---

## Task 6: firebird_indexes('fb')

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`.

**Interfaces:** Produces `firebird_indexes('fb')` → `table_schema, table_name, index_name, is_unique, is_active, segment_position, column_name, expression_source`. `is_unique` reflects the index only — constraint existence is NOT inferred here (it lives in information_schema, Task 3).

- [ ] **Step 1: Failing test block**

```
# --- firebird_indexes: computed index has expression, no column -----
query IIII
SELECT index_name, is_unique, column_name, expression_source IS NOT NULL
  FROM firebird_indexes('fb')
 WHERE index_name = 'EMP_UPPER_NAME_IDX';
----
EMP_UPPER_NAME_IDX	false	NULL	true
```

- [ ] **Step 2: Run, verify fail** (`firebird_indexes does not exist`).

- [ ] **Step 3: Implement**

```cpp
TableFunction GetFirebirdIndexesFunction() {
    static const MetadataFn desc{
        "firebird_indexes",
        {"table_schema","table_name","index_name","is_unique","is_active",
         "segment_position","column_name","expression_source"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::BOOLEAN,LogicalType::BOOLEAN,LogicalType::INTEGER,
         LogicalType::VARCHAR,LogicalType::VARCHAR},
        "SELECT TRIM(i.RDB$RELATION_NAME), TRIM(i.RDB$INDEX_NAME), "
        "       i.RDB$UNIQUE_FLAG, i.RDB$INDEX_INACTIVE, "
        "       seg.RDB$FIELD_POSITION, TRIM(seg.RDB$FIELD_NAME), "
        "       i.RDB$EXPRESSION_SOURCE "
        "  FROM RDB$INDICES i "
        "  LEFT JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = i.RDB$INDEX_NAME "
        " WHERE i.RDB$SYSTEM_FLAG = 0 "
        " ORDER BY i.RDB$RELATION_NAME, i.RDB$INDEX_NAME, seg.RDB$FIELD_POSITION",
        [](FirebirdCursor &c) -> std::vector<Value> {
            // is_active = NOT inactive (RDB$INDEX_INACTIVE: 1 = inactive)
            Value active = Value::BOOLEAN(c.IsNull(3) || c.GetShort(3) == 0);
            return {Value("main"), TextOrNull(c,0), TextOrNull(c,1),
                    BoolFromFlag(c,2), active, IntOrNull(c,4),
                    TextOrNull(c,5), TextOrNull(c,6)};
        }};
    return MakeMetadataFunction(desc);
}
```

`RDB$EXPRESSION_SOURCE` is a text BLOB — confirm `GetText` reads it (the same BLOB-text path `firebird_profile_table.cpp` uses for `RDB$VIEW_SOURCE`). If `OpenCursor` can't fetch BLOB in this position, wrap it `CAST(i.RDB$EXPRESSION_SOURCE AS VARCHAR(8192))` in the SQL.

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdIndexesFunction());`
- [ ] **Step 5: Build + run, verify pass.**
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_indexes"`

---

## Task 7: firebird_generators('fb')

Bespoke (not the single-SQL scaffold): `current_value` via per-generator `GEN_ID("name",0)` with safe quoting and per-row isolation.

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`.

**Interfaces:** Produces `firebird_generators('fb')` → `generator_name, initial_value (BIGINT, NULL if unavailable), current_value (BIGINT, NULL if unreadable)`.

- [ ] **Step 1: Failing test block**

```
# --- firebird_generators: initial value known, current readable -----
query II
SELECT generator_name, initial_value
  FROM firebird_generators('fb')
 WHERE generator_name = 'GEN_EMP_ID';
----
GEN_EMP_ID	100

query I
SELECT current_value >= 100 FROM firebird_generators('fb')
 WHERE generator_name = 'GEN_EMP_ID';
----
true
```

(If `RDB$INITIAL_VALUE` is NULL on this Firebird for a `CREATE SEQUENCE START WITH 100`, change the first expected to the actual; `current_value` from `GEN_ID(...,0)` should be 100 right after creation.)

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement bespoke function**

```cpp
// Firebird identifier quoting: wrap in double quotes, double internal quotes.
static std::string QuoteFbIdent(const std::string &id) {
    std::string out = "\"";
    for (char ch : id) { if (ch == '"') out += "\"\""; else out += ch; }
    out += "\"";
    return out;
}

struct GenRow { std::string name; Value initial; Value current; };

struct GenBindData : public TableFunctionData { std::string catalog_name; };
struct GenGlobalState : public GlobalTableFunctionState {
    std::vector<GenRow> rows; idx_t cursor = 0;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
GenBind(ClientContext &context, TableFunctionBindInput &input,
        vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull())
        throw BinderException("firebird_generators(catalog_name VARCHAR): catalog_name is required.");
    auto bind = make_uniq<GenBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    ValidateFirebirdAttachAlias(context, bind->catalog_name);
    names = {"generator_name","initial_value","current_value"};
    return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
GenInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<GenBindData>();
    auto g = make_uniq<GenGlobalState>();
    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    FirebirdConnection &conn = *lease.conn;
    // List generators + initial value.
    auto cur = conn.OpenCursor(
        "SELECT TRIM(RDB$GENERATOR_NAME), RDB$INITIAL_VALUE "
        "  FROM RDB$GENERATORS WHERE RDB$SYSTEM_FLAG = 0 "
        " ORDER BY RDB$GENERATOR_NAME");
    while (cur->Fetch()) {
        GenRow r;
        r.name = cur->GetText(0);
        r.initial = cur->IsNull(1) ? Value(LogicalType::BIGINT)
                                   : Value::BIGINT(cur->GetLong(1));
        r.current = Value(LogicalType::BIGINT); // default NULL
        g->rows.push_back(std::move(r));
    }
    // Per-generator current value, isolated: one generator's failure NULLs
    // only its own current_value.
    for (auto &r : g->rows) {
        try {
            auto c2 = conn.OpenCursor(
                "SELECT GEN_ID(" + QuoteFbIdent(r.name) + ", 0) FROM RDB$DATABASE");
            if (c2->Fetch() && !c2->IsNull(0)) {
                r.current = Value::BIGINT(c2->GetLong(0));
            }
        } catch (std::exception &) { /* leave current = NULL */ }
    }
    return std::move(g);
}

static void GenFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &g = input.global_state->Cast<GenGlobalState>();
    idx_t row = 0;
    while (row < (idx_t)STANDARD_VECTOR_SIZE && g.cursor < g.rows.size()) {
        auto &r = g.rows[g.cursor++];
        output.data[0].SetValue(row, Value(r.name));
        output.data[1].SetValue(row, r.initial);
        output.data[2].SetValue(row, r.current);
        ++row;
    }
    output.SetCardinality(row);
}

TableFunction GetFirebirdGeneratorsFunction() {
    return TableFunction("firebird_generators", {LogicalType::VARCHAR},
                         GenFunction, GenBind, GenInitGlobal);
}
```

`RDB$GENERATOR_INCREMENT` is intentionally NOT selected (no fixed increment in Firebird).

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdGeneratorsFunction());`
- [ ] **Step 5: Build + run, verify pass.** If `GetLong` truncates 64-bit generator values, use the cursor's 64-bit accessor instead (check `firebird_scanner.hpp` for a `GetBigInt`/`GetInt64`; `GEN_ID` returns BIGINT).
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_generators with isolated current_value"`

---

## Task 8: firebird_domains('fb')

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`. Add a Firebird-type formatter.

**Interfaces:** Produces `firebird_domains('fb')` → `domain_name, base_type, length, scale, is_nullable, charset_name, check_source, default_source`.

- [ ] **Step 1: Failing test block**

```
# --- firebird_domains: named domain with CHECK ----------------------
query III
SELECT domain_name, base_type, check_source IS NOT NULL
  FROM firebird_domains('fb')
 WHERE domain_name = 'D_SALARY';
----
D_SALARY	NUMERIC(10,2)	true
```

(If `base_type` formatting differs, set expected to actual after first run — the formatter output is the contract.)

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Add the type formatter + function**

```cpp
// Map RDB$FIELD_TYPE (+ sub_type/length/scale/precision) to a faithful
// Firebird type string. Covers the types reachable by user domains/columns.
static std::string FormatFbType(int ftype, int sub_type, int length,
                                int scale, int precision) {
    int s = scale < 0 ? -scale : scale;
    switch (ftype) {
    case 7:  // SMALLINT / NUMERIC(<=4)
    case 8:  // INTEGER / NUMERIC
    case 16: // BIGINT / INT128 / NUMERIC
        if (sub_type == 1) return "NUMERIC(" + std::to_string(precision) + "," + std::to_string(s) + ")";
        if (sub_type == 2) return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(s) + ")";
        if (ftype == 7) return "SMALLINT";
        if (ftype == 8) return "INTEGER";
        return "BIGINT";
    case 10: return "FLOAT";
    case 27: return "DOUBLE PRECISION";
    case 12: return "DATE";
    case 13: return "TIME";
    case 35: return "TIMESTAMP";
    case 14: return "CHAR(" + std::to_string(length) + ")";
    case 37: return "VARCHAR(" + std::to_string(length) + ")";
    case 261: return sub_type == 1 ? "BLOB SUB_TYPE TEXT" : "BLOB";
    default: return "TYPE_" + std::to_string(ftype); // unknown -> raw, never NULL
    }
}

TableFunction GetFirebirdDomainsFunction() {
    static const MetadataFn desc{
        "firebird_domains",
        {"domain_name","base_type","length","scale","is_nullable",
         "charset_name","check_source","default_source"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::INTEGER,
         LogicalType::INTEGER,LogicalType::BOOLEAN,LogicalType::VARCHAR,
         LogicalType::VARCHAR,LogicalType::VARCHAR},
        "SELECT TRIM(f.RDB$FIELD_NAME), f.RDB$FIELD_TYPE, "
        "       COALESCE(f.RDB$FIELD_SUB_TYPE,0), f.RDB$FIELD_LENGTH, "
        "       COALESCE(f.RDB$FIELD_SCALE,0), COALESCE(f.RDB$FIELD_PRECISION,0), "
        "       COALESCE(f.RDB$NULL_FLAG,0), TRIM(cs.RDB$CHARACTER_SET_NAME), "
        "       CAST(f.RDB$VALIDATION_SOURCE AS VARCHAR(8192)), "
        "       CAST(f.RDB$DEFAULT_SOURCE AS VARCHAR(8192)) "
        "  FROM RDB$FIELDS f "
        "  LEFT JOIN RDB$CHARACTER_SETS cs ON cs.RDB$CHARACTER_SET_ID = f.RDB$CHARACTER_SET_ID "
        " WHERE COALESCE(f.RDB$SYSTEM_FLAG,0) = 0 "
        "   AND f.RDB$FIELD_NAME NOT STARTING WITH 'RDB$' "
        "   AND f.RDB$FIELD_NAME NOT STARTING WITH 'MON$' "
        " ORDER BY f.RDB$FIELD_NAME",
        [](FirebirdCursor &c) -> std::vector<Value> {
            std::string bt = FormatFbType(c.GetShort(1), c.GetShort(2),
                                          c.GetLong(3), c.GetShort(4), c.GetShort(5));
            Value nullable = Value::BOOLEAN(c.IsNull(6) || c.GetShort(6) == 0);
            return {TextOrNull(c,0), Value(bt), IntOrNull(c,3), IntOrNull(c,4),
                    nullable, TextOrNull(c,7), TextOrNull(c,8), TextOrNull(c,9)};
        }};
    return MakeMetadataFunction(desc);
}
```

NOTE: `RDB$FIELDS` includes the implicit per-column system field names (`RDB$<n>`) — the `NOT STARTING WITH 'RDB$'` filter keeps only user-named domains. `scale` is stored negative in Firebird; expose its absolute value via the formatter, but the `scale` column returns the raw `RDB$FIELD_SCALE` (negative) — if the contract wants absolute, negate in the mapper. Pick absolute (`-scale`) for readability and set the test accordingly.

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdDomainsFunction());`
- [ ] **Step 5: Build + run, verify pass** (set `base_type` expected to actual formatter output).
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_domains + FB type formatter"`

---

## Task 9: firebird_computed_columns('fb')

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`.

**Interfaces:** Produces `firebird_computed_columns('fb')` → `table_schema, table_name, column_name, expression_source`.

- [ ] **Step 1: Failing test block**

```
# --- firebird_computed_columns --------------------------------------
query II
SELECT table_name, column_name
  FROM firebird_computed_columns('fb')
 WHERE table_name = 'EMPLOYEE';
----
EMPLOYEE	NAME_LEN
```

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement**

```cpp
TableFunction GetFirebirdComputedColumnsFunction() {
    static const MetadataFn desc{
        "firebird_computed_columns",
        {"table_schema","table_name","column_name","expression_source"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},
        "SELECT TRIM(rf.RDB$RELATION_NAME), TRIM(rf.RDB$FIELD_NAME), "
        "       CAST(f.RDB$COMPUTED_SOURCE AS VARCHAR(8192)) "
        "  FROM RDB$RELATION_FIELDS rf "
        "  JOIN RDB$FIELDS f ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE "
        " WHERE f.RDB$COMPUTED_SOURCE IS NOT NULL "
        "   AND COALESCE(rf.RDB$SYSTEM_FLAG,0) = 0 "
        " ORDER BY rf.RDB$RELATION_NAME, rf.RDB$FIELD_POSITION",
        [](FirebirdCursor &c) -> std::vector<Value> {
            return {Value("main"), TextOrNull(c,0), TextOrNull(c,1), TextOrNull(c,2)};
        }};
    return MakeMetadataFunction(desc);
}
```

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdComputedColumnsFunction());`
- [ ] **Step 5: Build + run, verify pass.**
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_computed_columns"`

---

## Task 10: firebird_dependencies('fb')

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`. Add a dependency-type labeller.

**Interfaces:** Produces `firebird_dependencies('fb')` → `object_name, object_type, object_type_code, depends_on_name, depends_on_type, depends_on_type_code, field_name`.

- [ ] **Step 1: Failing test block**

```
# --- firebird_dependencies: view depends on its base table ----------
query III
SELECT object_name, depends_on_name, depends_on_type
  FROM firebird_dependencies('fb')
 WHERE object_name = 'V_ACTIVE_EMP' AND depends_on_name = 'EMPLOYEE'
 ORDER BY field_name
 LIMIT 1;
----
V_ACTIVE_EMP	EMPLOYEE	TABLE
```

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement with type labeller**

```cpp
// RDB$DEPENDENT_TYPE / RDB$DEPENDED_ON_TYPE codes.
static std::string DepTypeLabel(int code) {
    switch (code) {
    case 0: return "TABLE";
    case 1: return "VIEW";
    case 2: return "TRIGGER";
    case 3: return "COMPUTED_FIELD";
    case 4: return "VALIDATION";
    case 5: return "PROCEDURE";
    case 6: return "EXPRESSION_INDEX";
    case 7: return "EXCEPTION";
    case 8: return "USER";
    case 9: return "FIELD";
    case 10: return "INDEX";
    case 14: return "GENERATOR";
    case 15: return "UDF";
    case 17: return "COLLATION";
    case 18: return "PACKAGE";
    case 19: return "PACKAGE_BODY";
    default: return "UNKNOWN"; // code preserved in *_type_code column
    }
}

TableFunction GetFirebirdDependenciesFunction() {
    static const MetadataFn desc{
        "firebird_dependencies",
        {"object_name","object_type","object_type_code","depends_on_name",
         "depends_on_type","depends_on_type_code","field_name"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::INTEGER,
         LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::INTEGER,
         LogicalType::VARCHAR},
        "SELECT TRIM(RDB$DEPENDENT_NAME), RDB$DEPENDENT_TYPE, "
        "       TRIM(RDB$DEPENDED_ON_NAME), RDB$DEPENDED_ON_TYPE, "
        "       TRIM(RDB$FIELD_NAME) "
        "  FROM RDB$DEPENDENCIES "
        " ORDER BY RDB$DEPENDENT_NAME, RDB$DEPENDED_ON_NAME, RDB$FIELD_NAME",
        [](FirebirdCursor &c) -> std::vector<Value> {
            Value otc = IntOrNull(c,1), dtc = IntOrNull(c,3);
            std::string ol = c.IsNull(1) ? "UNKNOWN" : DepTypeLabel(c.GetShort(1));
            std::string dl = c.IsNull(3) ? "UNKNOWN" : DepTypeLabel(c.GetShort(3));
            return {TextOrNull(c,0), Value(ol), otc, TextOrNull(c,2),
                    Value(dl), dtc, TextOrNull(c,4)};
        }};
    return MakeMetadataFunction(desc);
}
```

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdDependenciesFunction());`
- [ ] **Step 5: Build + run, verify pass** (adjust expected to actual if Firebird emits more dependency rows; the `LIMIT 1` + filter keeps it stable).
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_dependencies with FB type codes"`

---

## Task 11: firebird_comments('fb')

**Files:** Modify `src/firebird_metadata_functions.cpp`, `src/firebird_extension.cpp`, `test/sql/firebird_metadata_bridge.test`.

**Interfaces:** Produces `firebird_comments('fb')` → `object_schema, object_name, object_type, column_name, comment`. `column_name` NULL only for TABLE/VIEW rows. Increment-1 object types: TABLE, VIEW, COLUMN.

- [ ] **Step 1: Failing test block**

```
# --- firebird_comments: TABLE + COLUMN ------------------------------
query IIII
SELECT object_name, object_type, column_name, comment
  FROM firebird_comments('fb')
 WHERE object_name = 'EMPLOYEE'
 ORDER BY object_type, column_name;
----
EMPLOYEE	COLUMN	EMP_NAME	Full name
EMPLOYEE	TABLE	NULL	Staff records fixture
```

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement (UNION ALL of relation + column descriptions)**

```cpp
TableFunction GetFirebirdCommentsFunction() {
    static const MetadataFn desc{
        "firebird_comments",
        {"object_schema","object_name","object_type","column_name","comment"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::VARCHAR,LogicalType::VARCHAR},
        "SELECT TRIM(r.RDB$RELATION_NAME), "
        "       CASE WHEN r.RDB$VIEW_BLR IS NULL THEN 'TABLE' ELSE 'VIEW' END, "
        "       CAST(NULL AS VARCHAR(63)), "
        "       CAST(r.RDB$DESCRIPTION AS VARCHAR(8192)) "
        "  FROM RDB$RELATIONS r "
        " WHERE COALESCE(r.RDB$SYSTEM_FLAG,0) = 0 AND r.RDB$DESCRIPTION IS NOT NULL "
        "UNION ALL "
        "SELECT TRIM(rf.RDB$RELATION_NAME), 'COLUMN', TRIM(rf.RDB$FIELD_NAME), "
        "       CAST(rf.RDB$DESCRIPTION AS VARCHAR(8192)) "
        "  FROM RDB$RELATION_FIELDS rf "
        " WHERE COALESCE(rf.RDB$SYSTEM_FLAG,0) = 0 AND rf.RDB$DESCRIPTION IS NOT NULL "
        " ORDER BY 1, 2, 3",
        [](FirebirdCursor &c) -> std::vector<Value> {
            return {Value("main"), TextOrNull(c,0), TextOrNull(c,1),
                    TextOrNull(c,2), TextOrNull(c,3)};
        }};
    return MakeMetadataFunction(desc);
}
```

(The first SELECT yields `column_name = NULL`; the `CAST(NULL AS VARCHAR(63))` keeps the UNION column typed. `ORDER BY 1,2,3` is determinism across the union.)

- [ ] **Step 4: Register** `loader.RegisterFunction(GetFirebirdCommentsFunction());`
- [ ] **Step 5: Build + run, verify pass.**
- [ ] **Step 6: Commit** `git commit -m "feat(metadata): firebird_comments (TABLE/VIEW/COLUMN)"`

---

## Task 12: Cross-version validation + docs

**Files:** Modify `docs/pt/function_manual.md` (+ `docs/en` parity if `DOCS_PARITY.md` requires), run `scripts/build_matrix.ps1`.

- [ ] **Step 1: Document the new surface**

Add to `docs/pt/function_manual.md`: a "Metadata Bridge 2.0" section listing the populated `information_schema` views and each new function with its columns and a one-line example (`SELECT * FROM firebird_indexes('fb');`). Mirror in `docs/en/` if `docs/DOCS_PARITY.md` mandates parity.

- [ ] **Step 2: Run the full matrix**

Run (env vars set):
```powershell
$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:FIREBIRD_DECFLOAT_DB="C:\fbtest\decfloat.fdb"; $env:FIREBIRD_NONE_DB="C:\fbtest\none.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"
& scripts/build_matrix.ps1
```
Expected: v1.5.2 / v1.5.3 / v1.5.4 all PASS, including the new `firebird_metadata_bridge.test`; submodule restored to the pin. Identical assertion counts across the three (no API drift).

- [ ] **Step 3: Commit**

```bash
git add docs/pt/function_manual.md docs/en
git commit -m "docs(metadata): document Metadata Bridge 2.0 surface"
```

---

## Self-Review

**Spec coverage:**
- information_schema PK/UNIQUE → Task 3; FK + rule verification → Task 4; FK fidelity (`firebird_foreign_keys`) → Task 5. ✓
- FB functions indexes/generators/domains/computed/dependencies/comments → Tasks 6–11. ✓
- Generators: no `increment`, `initial_value` + isolated `current_value` via quoted `GEN_ID(,0)` → Task 7. ✓
- Indexes: unique-index vs UNIQUE-constraint distinction (constraint via info_schema, not inferred) → Task 6 + note. ✓
- Comments: enumerated TABLE/VIEW/COLUMN, `column_name` NULL only without column → Task 11. ✓
- Dependencies: FB codes + readable labels in own columns → Task 10. ✓
- Name/schema conventions (`main`, TRIM, UNKNOWN+raw code) → Global Constraints + per-task. ✓
- Fixture expansion idempotent across scripts + cascade → Task 1. ✓
- Read-only, no community/upstream → Global Constraints. ✓
- FK fallback does not replace info_schema → Task 5 intro + Task 4 keeps presence in info_schema. ✓

**Placeholder scan:** Empirical/"adjust to actual" steps are deliberate (deterministic-output discovery), each with a concrete first-run value to set — not open TODOs. No bare TBDs.

**Type consistency:** `MetadataFn` / `MetaBindData` / `MetaGlobalState` / `MakeMetadataFunction` used consistently Tasks 5,6,8,9,10,11; Task 7 (generators) is intentionally bespoke with its own `GenBindData`/`GenGlobalState`. `FirebirdCursor` is flagged in Task 2 to confirm against `firebird_scanner.hpp`. Value helpers `TextOrNull`/`IntOrNull`/`BoolFromFlag` defined in Task 2, used everywhere.

**Known verify-at-implementation points** (called out inline, not hidden): cursor type name + 64-bit accessor; BLOB-text fetch vs `CAST(... AS VARCHAR)`; `ForeignKeyInfo` field requirements; whether DuckDB surfaces FK rules (drives Task 5 as primary rule surface regardless).
