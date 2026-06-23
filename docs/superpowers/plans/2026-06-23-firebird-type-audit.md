# firebird_type_audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `firebird_type_audit('fb')` — a read-only, findings-only audit of type/charset conversion fidelity between Firebird native types and the DuckDB projection.

**Architecture:** A typed catalog table function (same scaffold family as `firebird_domains` in `src/firebird_metadata_functions.cpp`). One `RDB$RELATION_FIELDS ⋈ RDB$FIELDS` query whose `WHERE` filters to exactly the 6 finding conditions (so every fetched row is a finding); the row-mapper classifies each by raw `RDB$FIELD_TYPE` code. No business-data reads, never runs the user's SQL, only Firebird system tables via a pooled lease.

**Tech Stack:** C++ DuckDB out-of-tree extension; the existing `MetadataFn`/`MakeMetadataFunction` scaffold + `TextOrNull`/`ShortOrNull` helpers; Firebird `RDB$` system tables; DuckDB sqllogic `.test`; PowerShell build matrix.

## Global Constraints

- **Read-only.** Reads no business data, never executes the user's SQL, queries only Firebird system tables (`RDB$`) via a pooled lease (like the other metadata functions). No writes to Firebird.
- **No action on `duckdb/community-extensions` / upstream.**
- **Findings-only:** emit one row ONLY for columns matching a finding; trivial columns (INTEGER, VARCHAR UTF8, DATE, lossless NUMERIC/DECIMAL ≤38) produce no row.
- **6 normalized `finding` codes, exactly:** `none_charset`, `decfloat_as_varchar`, `int128`, `blob_text`, `time_tz`, `timestamp_tz`. No `numeric_precision`, no `blob_binary`.
- **`none_charset` detail must NOT claim an effective `none_encoding`** — only state decoding depends on `none_encoding` (documented default `win1252`); the function does not read the scan's setting.
- **`int128`** = `RDB$FIELD_TYPE = 26` with scale 0 (projects to `HUGEINT`): native INT128 and scale-0 NUMERIC/DECIMAL(19–38). Scaled (scale≠0) `RDB$FIELD_TYPE = 26` → `DECIMAL(38,s)` is lossless → NOT a finding.
- **Schema is the literal `'main'`**; RDB$ identifiers TRIMmed; every query `ORDER BY`; test expected = real output, never weakened.
- **FB4+ findings** (`decfloat_as_varchar`, `int128`, `time_tz`, `timestamp_tz`) tested only in a separate conditional FB4+ test; the FB3-compatible canonical fixture is not touched.
- **Build (incremental):** `scripts/build_windows_local.bat`. **Run a test (local build registers by ABSOLUTE path):** `build\release\test\unittest.exe "D:/Dados/duckdb-firebird/test/sql/<name>.test"`. **Env (PowerShell):** `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:FIREBIRD_DECFLOAT_DB="C:\fbtest\decfloat.fdb"; $env:FIREBIRD_NONE_DB="C:\fbtest\none.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`.
- Cross-version gate: `scripts/build_matrix.ps1` (v1.5.2/v1.5.3/v1.5.4).

## Authoritative `RDB$FIELD_TYPE` codes (from `src/firebird_scanner.cpp` mapping)

`7`=SMALLINT, `8`=INTEGER, `16`=BIGINT, `14`=CHAR, `37`=VARCHAR, `261`=BLOB, `10`=FLOAT, `27`=DOUBLE, `12`=DATE, `13`=TIME, `35`=TIMESTAMP, `23`=BOOLEAN, **`24`=DECFLOAT(16)**, **`25`=DECFLOAT(34)**, **`26`=INT128 / NUMERIC|DECIMAL(p>18)**, **`28`=TIME WITH TIME ZONE**, **`29`=TIMESTAMP WITH TIME ZONE**. All RDB$ numeric metadata columns are SMALLINT → read with `GetShort` (NOT `GetLong`). `RDB$CHARACTER_SET_ID = 0` means CHARACTER SET NONE.

---

## Task 1: firebird_type_audit function + FB-agnostic tests

**Files:**
- Modify: `src/firebird_metadata_functions.cpp` (add `GetFirebirdTypeAuditFunction()`)
- Modify: `src/include/firebird_metadata_functions.hpp` (declare it)
- Modify: `src/firebird_extension.cpp` (register it)
- Create: `test/sql/firebird_type_audit.test`

**Interfaces:**
- Consumes: the scaffold (`MetadataFn`, `MakeMetadataFunction`, `TextOrNull`) already in `firebird_metadata_functions.cpp`; `FirebirdStatement` cursor (`GetShort`/`GetText`/`IsNull`).
- Produces: `firebird_type_audit('fb')` → columns `table_schema, table_name, column_name, firebird_type, duckdb_type, finding, detail` (all VARCHAR).

- [ ] **Step 1: Write the failing test (FB-agnostic findings)**

Create `test/sql/firebird_type_audit.test`:
```
# name: test/sql/firebird_type_audit.test
# description: type/charset fidelity audit — findings-only (FB-version-agnostic findings)
# group: [firebird]

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

# EMPLOYEE.NOTE is BLOB SUB_TYPE 1 (text, non-NONE) -> blob_text
query IIII
SELECT table_name, column_name, duckdb_type, finding
  FROM firebird_type_audit('fb')
 WHERE table_name = 'EMPLOYEE' AND column_name = 'NOTE';
----
EMPLOYEE	NOTE	VARCHAR	blob_text

# FILE_STORAGE.PAYLOAD is BLOB SUB_TYPE 0 (binary) -> NOT a finding (lossless BLOB)
query I
SELECT COUNT(*) FROM firebird_type_audit('fb')
 WHERE table_name = 'FILE_STORAGE' AND column_name = 'PAYLOAD';
----
0

# trivial columns never appear
query I
SELECT COUNT(*) FROM firebird_type_audit('fb')
 WHERE table_name = 'EMPLOYEE' AND column_name IN ('EMP_ID','HIRE_DATE','ACTIVE');
----
0

statement ok
ATTACH '${FIREBIRD_NONE_DB}' AS fbn (TYPE firebird);

# none.fdb TXT.LABEL is VARCHAR CHARACTER SET NONE -> none_charset
query III
SELECT column_name, duckdb_type, finding
  FROM firebird_type_audit('fbn')
 WHERE table_name = 'TXT' AND column_name = 'LABEL';
----
LABEL	VARCHAR	none_charset

# none.fdb TXT.NOTE is BLOB SUB_TYPE 1 CHARACTER SET NONE -> none_charset wins over blob_text
query II
SELECT column_name, finding
  FROM firebird_type_audit('fbn')
 WHERE table_name = 'TXT' AND column_name = 'NOTE';
----
NOTE	none_charset
```

- [ ] **Step 2: Run, verify it fails**

Run: `build\release\test\unittest.exe "D:/Dados/duckdb-firebird/test/sql/firebird_type_audit.test"`
Expected: FAIL — `firebird_type_audit` does not exist.

- [ ] **Step 3: Declare in the header**

In `src/include/firebird_metadata_functions.hpp`, add to the namespace:
```cpp
TableFunction GetFirebirdTypeAuditFunction();
```

- [ ] **Step 4: Implement the function**

In `src/firebird_metadata_functions.cpp`, before the closing namespace, add (uses `<string>` already included):
```cpp
TableFunction GetFirebirdTypeAuditFunction() {
    static const MetadataFn desc{
        "firebird_type_audit",
        {"table_schema","table_name","column_name","firebird_type",
         "duckdb_type","finding","detail"},
        {LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,
         LogicalType::VARCHAR},
        // Findings-only: the WHERE matches exactly the 6 finding conditions,
        // so every fetched row is a finding. RDB$FIELD_TYPE codes per the
        // scanner's blr->SQL map: 24/25 DECFLOAT, 26 INT128, 28/29 TZ,
        // 14/37 CHAR/VARCHAR, 261 BLOB. CHARACTER_SET_ID 0 = NONE.
        "SELECT TRIM(rf.RDB$RELATION_NAME), TRIM(rf.RDB$FIELD_NAME), "
        "       f.RDB$FIELD_TYPE, COALESCE(f.RDB$FIELD_SUB_TYPE,0), "
        "       COALESCE(f.RDB$FIELD_SCALE,0), COALESCE(f.RDB$FIELD_PRECISION,0), "
        "       COALESCE(f.RDB$FIELD_LENGTH,0), COALESCE(f.RDB$CHARACTER_SET_ID,-1) "
        "  FROM RDB$RELATION_FIELDS rf "
        "  JOIN RDB$FIELDS f ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE "
        " WHERE COALESCE(rf.RDB$SYSTEM_FLAG,0) = 0 AND ( "
        "       f.RDB$FIELD_TYPE IN (24,25,28,29) "
        "    OR (f.RDB$FIELD_TYPE = 26 AND COALESCE(f.RDB$FIELD_SCALE,0) = 0) "
        "    OR (f.RDB$FIELD_TYPE IN (14,37) AND f.RDB$CHARACTER_SET_ID = 0) "
        "    OR (f.RDB$FIELD_TYPE = 261 AND f.RDB$FIELD_SUB_TYPE = 1) ) "
        " ORDER BY rf.RDB$RELATION_NAME, rf.RDB$FIELD_NAME",
        [](FirebirdStatement &c) -> duckdb::vector<Value> {
            int ft   = c.GetShort(2);
            int st   = c.GetShort(3);
            int prec = c.GetShort(5);
            int len  = c.GetShort(6);
            int cs   = c.IsNull(7) ? -1 : c.GetShort(7);
            const bool none = (cs == 0);
            std::string fbtype, ddtype, finding, detail;
            switch (ft) {
            case 24:
            case 25:
                fbtype  = (ft == 24) ? "DECFLOAT(16)" : "DECFLOAT(34)";
                ddtype  = "VARCHAR";
                finding = "decfloat_as_varchar";
                detail  = "DECFLOAT projected as VARCHAR via server-side CAST "
                          "(no native decimal-float type); textual semantics on filters.";
                break;
            case 26: // scale 0 only reaches here (WHERE guard) -> HUGEINT
                fbtype  = (st == 1) ? ("NUMERIC(" + std::to_string(prec) + ",0)")
                        : (st == 2) ? ("DECIMAL(" + std::to_string(prec) + ",0)")
                                    : "INT128";
                ddtype  = "HUGEINT";
                finding = "int128";
                detail  = "128-bit integer projected as HUGEINT (lossless); "
                          "BI tools without int128 support may need care.";
                break;
            case 28:
                fbtype = "TIME WITH TIME ZONE"; ddtype = "TIME WITH TIME ZONE";
                finding = "time_tz";
                detail = "TIME WITH TIME ZONE; session offset / zone-id handling caveat.";
                break;
            case 29:
                fbtype = "TIMESTAMP WITH TIME ZONE"; ddtype = "TIMESTAMP WITH TIME ZONE";
                finding = "timestamp_tz";
                detail = "TIMESTAMP WITH TIME ZONE; session offset / zone-id handling caveat.";
                break;
            case 14:
            case 37:
                fbtype  = ((ft == 14) ? "CHAR(" : "VARCHAR(") + std::to_string(len) +
                          ") CHARACTER SET NONE";
                ddtype  = "VARCHAR";
                finding = "none_charset";
                detail  = "CHARACTER SET NONE: decoding depends on the scan's "
                          "none_encoding (documented default win1252; not read here); "
                          "strict mode may reject, round-trip not guaranteed.";
                break;
            case 261: // text BLOB (sub_type 1 per WHERE)
                if (none) {
                    fbtype  = "BLOB SUB_TYPE 1 CHARACTER SET NONE";
                    ddtype  = "VARCHAR";
                    finding = "none_charset";
                    detail  = "NONE-charset text BLOB: decoding depends on the scan's "
                              "none_encoding (documented default win1252; not read here).";
                } else {
                    fbtype  = "BLOB SUB_TYPE 1";
                    ddtype  = "VARCHAR";
                    finding = "blob_text";
                    detail  = "Text BLOB projected as VARCHAR; charset + size caveat.";
                }
                break;
            default:
                fbtype = "TYPE_" + std::to_string(ft); ddtype = ""; finding = "UNKNOWN";
                detail = "unexpected field type reached the audit WHERE";
                break;
            }
            return {Value("main"), TextOrNull(c, 0), TextOrNull(c, 1),
                    Value(fbtype), Value(ddtype), Value(finding), Value(detail)};
        }};
    return MakeMetadataFunction(desc);
}
```
(The `default` branch is an unreachable guard — the WHERE only admits the handled codes — but it avoids an empty/garbage row if the WHERE and switch ever drift.)

- [ ] **Step 5: Register**

In `src/firebird_extension.cpp` add: `loader.RegisterFunction(GetFirebirdTypeAuditFunction());`

- [ ] **Step 6: Build + run, verify pass**

Run `scripts/build_windows_local.bat`, then the test. If `firebird_type` strings differ from expected (the test only asserts `duckdb_type`/`finding`, not `firebird_type`, to stay robust), adjust only if a finding/duckdb_type mismatch — set to actual deterministic output. Expected: PASS.

- [ ] **Step 7: Regression**

Re-run the full firebird suite per file (absolute paths). The audit is additive; nothing else changes. Expected: no regressions.

- [ ] **Step 8: Commit**

```bash
git add src/firebird_metadata_functions.cpp src/include/firebird_metadata_functions.hpp src/firebird_extension.cpp test/sql/firebird_type_audit.test
git commit -m "feat(type-audit): firebird_type_audit findings-only function + FB-agnostic tests"
```

---

## Task 2: FB4+ fixture expansion + conditional FB4+ test

**Files:**
- Modify: `scripts/fixture_biz4.sql` (add DECFLOAT + ensure INT128 scale-0 + TZ columns on the FB4+ fixture table)
- Create: `test/sql/firebird_type_audit_fb4.test`

**Interfaces:**
- Consumes: `firebird_type_audit('fb')` from Task 1; the FB4+ fixture DB (env `FIREBIRD_BIZ4_DB`, or the matrix's `biz4.fdb`).

- [ ] **Step 1: Inspect the existing FB4+ fixture**

Read `scripts/fixture_biz4.sql`. It creates `FB4_TYPES` with FB4-only types (the fb-matrix FB4+ smoke selects `BIG_NUM` INT128, `BIG_DEC` DECIMAL(38), `TS_TZ` TIMESTAMP_TZ, `T_TZ` TIME_TZ from it). Note exact column names/types present.

- [ ] **Step 2: Add the missing audit-relevant columns**

Ensure `FB4_TYPES` has one column per FB4+ finding. Add what's missing (DECFLOAT is the likely gap), e.g. append columns:
```sql
ALTER TABLE FB4_TYPES ADD DF34 DECFLOAT(34);
COMMIT;
```
Keep the existing INT128 (`BIG_NUM`, scale 0 → `int128`), TZ columns (`T_TZ`/`TS_TZ`). If `BIG_NUM` is not a scale-0 INT128, add `ADD INT_BIG INT128;`. (Do NOT change the FB3-compatible canonical fixtures: `setup_test_firebird.sh`, the inline fixture in `build-linux-fb-matrix.yml`, `smoke_fixture.sql`.) Re-provision the local biz4 DB if used locally.

- [ ] **Step 3: Write the FB4+ test**

Create `test/sql/firebird_type_audit_fb4.test`. Gate it on the FB4+ fixture env so it skips on FB3:
```
# name: test/sql/firebird_type_audit_fb4.test
# description: type audit — FB4+ findings (DECFLOAT/INT128/TIME-TZ/TIMESTAMP-TZ)
# group: [firebird]

require firebird

require-env FIREBIRD_BIZ4_DB

require-env ISC_PASSWORD

statement ok
ATTACH '${FIREBIRD_BIZ4_DB}' AS b (TYPE firebird);

# all four FB4+ findings present on FB4_TYPES, by finding code
query II
SELECT finding, COUNT(*)
  FROM firebird_type_audit('b')
 WHERE table_name = 'FB4_TYPES'
   AND finding IN ('decfloat_as_varchar','int128','time_tz','timestamp_tz')
 GROUP BY finding
 ORDER BY finding;
----
decfloat_as_varchar	1
int128	1
time_tz	1
timestamp_tz	1

# spot-check the duckdb_type projections
query II
SELECT finding, duckdb_type
  FROM firebird_type_audit('b')
 WHERE table_name = 'FB4_TYPES' AND finding = 'int128';
----
int128	HUGEINT
```
(Adjust the `COUNT(*)` per finding to the actual number of matching columns you put in `FB4_TYPES` — keep deterministic. If `BIG_DEC` is `DECIMAL(38,2)` it is scaled → NOT an `int128` finding; ensure exactly one scale-0 INT128 column so `int128` count is 1.)

- [ ] **Step 4: Provision + run locally (FB5 has FB4+ types)**

Provision the expanded `biz4.fdb` locally (isql), set `$env:FIREBIRD_BIZ4_DB="localhost/3050:C:\fbtest\biz4.fdb"` (remote form — FB4+ types need a real FB4/5 server), build, run:
`build\release\test\unittest.exe "D:/Dados/duckdb-firebird/test/sql/firebird_type_audit_fb4.test"`
Expected: PASS. Set the per-finding counts to the actual fixture if they differ.

- [ ] **Step 5: Wire the FB4+ test into the matrix workflow (FB4+ only)**

In `.github/workflows/build-linux-fb-matrix.yml`, in the existing **"FB4+ live type smoke"** step (`if: matrix.fb_major != '3'`), after provisioning biz4, export `FIREBIRD_BIZ4_DB` and run the FB4+ audit test:
```yaml
          echo "FIREBIRD_BIZ4_DB=firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8" >> $GITHUB_ENV
          ./build/release/test/unittest test/sql/firebird_type_audit_fb4.test
```
(The biz4 fixture is already created for FB4+ in that workflow at `scripts/fixture_biz4.sql`.)

- [ ] **Step 6: Commit**

```bash
git add scripts/fixture_biz4.sql test/sql/firebird_type_audit_fb4.test .github/workflows/build-linux-fb-matrix.yml
git commit -m "test(type-audit): FB4+ fixture (DECFLOAT/INT128/TZ) + conditional FB4+ audit test"
```

---

## Task 3: Docs (PT/EN) + matrix coverage + cross-version validation

**Files:**
- Modify: `docs/pt/function_manual.md`, `docs/en/function_manual.md`
- Modify: `scripts/build_matrix.ps1`

- [ ] **Step 1: Document in both manuals**

Add a `firebird_type_audit(catalog_name)` subsection to Level 4 (Diagnostics) in both manuals (PT in the PT file, EN in the EN file). Cover: findings-only audit of type/charset conversion fidelity; read-only (no business data, no user SQL, only `RDB$` via lease); the 7-column output table; the 6 `finding` codes with one-line meanings; the `none_charset` note (depends on `none_encoding`, default win1252, not read by the function); the `int128`-is-scale-0 note; and a usage example `SELECT * FROM firebird_type_audit('fb');`. Mirror PT/EN per `docs/DOCS_PARITY.md`.

- [ ] **Step 2: Add to the matrix**

In `scripts/build_matrix.ps1`, add to the `$testFixtureVar` hashtable:
```
    'firebird_type_audit.test'         = 'FIREBIRD_TEST_DB'
```
(The FB4+ test runs via the fb-matrix workflow, not the local Windows matrix — it needs a live FB4+ server. Do NOT add `firebird_type_audit_fb4.test` to `build_matrix.ps1` unless `FIREBIRD_BIZ4_DB` is provisioned locally; if you provisioned it in Task 2 Step 4, add it with `'firebird_type_audit_fb4.test' = 'FIREBIRD_BIZ4_DB'` and ensure the matrix script reads that env var — otherwise leave it out so it's skipped, not failed.)

- [ ] **Step 3: Run the cross-version matrix**

Run (env set): `& scripts/build_matrix.ps1`. Expected: v1.5.2/v1.5.3/v1.5.4 all PASS including `firebird_type_audit.test`; submodule restored to pin; identical assertion counts (no API drift).

- [ ] **Step 4: Commit**

```bash
git add docs/pt/function_manual.md docs/en/function_manual.md scripts/build_matrix.ps1
git commit -m "docs(type-audit): document firebird_type_audit + matrix coverage"
```

---

## Self-Review

**Spec coverage:**
- Findings-only function + 6 codes → Task 1 (SQL WHERE + classifier). ✓
- `none_charset` no-effective-encoding claim → Task 1 detail strings + Task 3 docs. ✓
- `int128` = scale-0 (HUGEINT), scaled excluded → Task 1 WHERE (`scale=0`) + comment. ✓
- `blob_binary`/`numeric_precision` excluded → not in WHERE; Task 1 test asserts FILE_STORAGE.PAYLOAD (binary blob) absent. ✓
- FB3-agnostic findings (none_charset, blob_text) → Task 1 test; FB4+ findings → Task 2 conditional test. ✓
- Expand existing FB4+ fixture, don't touch FB3 canonical → Task 2. ✓
- Read-only / RDB$-via-lease / no user SQL → Global Constraints + scaffold (lease-based) reuse. ✓
- Docs PT/EN + matrix → Task 3. ✓

**Placeholder scan:** "set counts/strings to actual" steps are deterministic-output discovery with concrete starting values (the test asserts `finding`/`duckdb_type`, which are fixed by the classifier, not by the DB) — not open TODOs. No bare TBDs.

**Type consistency:** `GetFirebirdTypeAuditFunction` declared (Task 1 Step 3), defined (Step 4), registered (Step 5). 7-column schema consistent across function + tests + docs. `finding` codes identical everywhere. `GetShort` used for all SMALLINT RDB$ columns (per the accessor rule learned in Metadata Bridge).

**Known verify-at-implementation:** exact `FB4_TYPES` column inventory (Task 2 Step 1 reads it first); whether `BIG_DEC` is scaled (→ not int128) — Task 2 ensures exactly one scale-0 INT128 column so the `int128` count is deterministic.
