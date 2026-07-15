# Type Lossless Hardening Implementation Plan (#33)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix issue #33 — a view's computed/aggregate column (e.g. `SUM(NUMERIC(10,2))`) whose catalog-frozen type disagrees with Firebird's live runtime type crashes the scanner with `Expected vector of type INT64, but found vector of type INT128`.

**Architecture:** Add a prepare-only (no-execute) constructor to `FirebirdStatement` so `FirebirdScanBind` can cheaply describe a view's ACTUAL live column layout via XSQLDA. When the scan target is a view, reconcile both `column_types` (feeds DuckDB's `return_types`/Vector allocation) and `column_descs` (feeds `FirebirdAppendValue`'s fetch-time accessor width) from that live description — never from the view's static, `CREATE VIEW`-time-frozen catalog metadata alone.

**Tech Stack:** DuckDB out-of-tree C++ extension, `FirebirdStatement`/libfbclient XSQLDA describe, DuckDB sqllogictest `.test` files.

## Global Constraints

- The reconciliation must correct BOTH `column_types` (DuckDB `LogicalType`, drives Vector allocation) AND `column_descs` (`FirebirdColumnDesc.sqltype/sqlsubtype/sqlscale/sqllen`, drives the fetch-time accessor width in `FirebirdAppendValue`). Fixing only one and not the other reproduces the same class of crash or a silent wrong-width read.
- `character_set_id` for text/BLOB columns is NOT overwritten from the live XSQLDA (XSQLDA cannot supply it for BLOBs — the blob subtype occupies that slot instead) — it stays sourced from the catalog (`RDB$FIELDS`), exactly as `LoadTableSchema` already does today.
- The extra prepare-only round trip runs ONLY when the scan target is a view. A base table's storage width cannot legitimately disagree with its own catalog metadata between prepare and now, so base-table scan binds pay zero added cost from this fix.
- No change to the scanner's parallelism/charset-gating/pagination decisions (Smart Scan Planning Report's existing logic) — this fix only touches type/descriptor derivation at bind time.
- `ReadBlob`'s multi-segment truncation bug (issue #35) and `TIME WITH TIME ZONE`'s discarded offset are explicitly OUT of scope — do not touch `ReadBlob` or the TZ fetch paths.
- No new fixture — `V_DEPT_HEADCOUNT` (existing) already reproduces #33 deterministically.
- DuckDB version-locked; submodule pin must NOT change. No duckdb/community-extensions/upstream action.
- PT/EN docs parity per `docs/DOCS_PARITY.md`.

---

## File Structure

- `src/include/firebird_client.hpp` (modify) — declare a `PrepareOnlyTag` struct + a new `FirebirdStatement` constructor overload.
- `src/firebird_client.cpp` (modify) — implement the new constructor (reuses the existing private `Prepare(sql)` method, stops before `isc_dsql_execute`).
- `src/firebird_scanner.cpp` (modify) — `FirebirdScanBind`: compute `bind->is_view` unconditionally (was gated on `!bind->pk && paging`); add the new type/descriptor reconciliation block, gated on `bind->is_view` alone.
- `test/sql/firebird_scan.test` (modify) — regression case proving #33 no longer crashes AND that the value + DuckDB type are correct.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` (modify) — brief note on the reconciliation behavior.

---

### Task 1: Prepare-only constructor on `FirebirdStatement`

**Files:**
- Modify: `src/include/firebird_client.hpp`
- Modify: `src/firebird_client.cpp`

**Interfaces:**
- Consumes: the existing private `FirebirdStatement::Prepare(const std::string &sql)` method (already does `isc_dsql_allocate_statement` + `isc_dsql_prepare` + `isc_dsql_describe` (if needed) + `AllocateBuffers()` — everything except `isc_dsql_execute`).
- Produces: `struct PrepareOnlyTag {};` and a new public constructor `FirebirdStatement(FirebirdConnection &conn, const std::string &sql, PrepareOnlyTag)` that calls `Prepare(sql)` and returns WITHOUT executing. Callers read `.columns()` for the described `FirebirdColumnDesc` vector and let the object go out of scope — its destructor (`isc_dsql_free_statement(..., DSQL_drop)`) is safe to call on a prepared-but-never-executed statement.

- [ ] **Step 1: Declare the tag + constructor**

In `src/include/firebird_client.hpp`, inside `class FirebirdStatement`, add right after the existing two-constructor block (after the `params`-taking constructor declaration, currently ending at line 105, before `~FirebirdStatement();`):

```cpp
    // Prepares and describes `sql` WITHOUT executing it — no cursor is
    // opened, no rows are ever fetchable from an instance constructed
    // this way. Used to cheaply learn the ACTUAL runtime column layout
    // (via XSQLDA) for a query whose static catalog metadata might be
    // stale (e.g. a view's computed/aggregate column, whose type was
    // frozen at CREATE VIEW time and can legitimately disagree with
    // what Firebird's live DSQL compiler produces for the identical
    // expression today). Call .columns() to read the described layout.
    struct PrepareOnlyTag {};
    FirebirdStatement(FirebirdConnection &conn, const std::string &sql,
                      PrepareOnlyTag);
```

- [ ] **Step 2: Implement it**

In `src/firebird_client.cpp`, add right after the existing params-taking constructor (which currently ends at line 624, right before `FirebirdStatement::~FirebirdStatement()`):

```cpp
FirebirdStatement::FirebirdStatement(FirebirdConnection &conn,
                                     const std::string &sql,
                                     PrepareOnlyTag)
    : conn_(conn) {
    Prepare(sql);
    // Deliberately no isc_dsql_execute call — describe-only.
}
```

- [ ] **Step 3: Build**

Run: `scripts\build_windows_local.bat`
Expected: build succeeds (this step only adds a constructor overload — no call site uses it yet, so no behavior change).

- [ ] **Step 4: Commit**

```bash
git add src/include/firebird_client.hpp src/firebird_client.cpp
git commit -m "feat(type-lossless): add prepare-only (no-execute) FirebirdStatement constructor"
```

---

### Task 2: Reconcile view column types/descriptors from live XSQLDA

**Files:**
- Modify: `src/firebird_scanner.cpp`
- Test: `test/sql/firebird_scan.test`

**Interfaces:**
- Consumes: Task 1's `FirebirdStatement(conn, sql, PrepareOnlyTag{})` constructor + `.columns()`; the existing `LookupObjectType` (from `firebird_view_analysis.hpp`, already included in this file); the existing `FirebirdToDuckDBType(const FirebirdColumnDesc&)` and `QuoteIdent(const std::string&)` free functions (already used elsewhere in this same file, no new include needed).
- Produces: `FirebirdScanBind`'s `bind->column_types`/`bind->column_descs` are correct for a view target even when its catalog metadata is stale for a computed/aggregate column. `bind->is_view` is now computed unconditionally (previously only computed when `!bind->pk && paging`).

- [ ] **Step 1: Write the failing test**

Read `test/sql/firebird_scan.test` first to confirm its existing `ATTACH`/fixture conventions, then append this case (adjust to match the file's exact alias if it differs from `fb`):

```
# ---------------------------------------------------------------------------
#  Type Lossless Hardening (#33): a view's computed/aggregate column can
#  have a catalog-frozen type (set at CREATE VIEW time) that disagrees
#  with Firebird's live runtime type for the identical expression today.
#  V_DEPT_HEADCOUNT's TOTAL_SALARY (SUM(EMPLOYEE.SALARY), SALARY is
#  NUMERIC(10,2)) is catalogued as INT128-backed DECIMAL(38,2), but
#  Firebird's live SUM() promotion produces BIGINT-backed NUMERIC(18,2).
#  Before the fix: "INTERNAL Error: Expected vector of type INT64, but
#  found vector of type INT128". This must not just avoid crashing — the
#  value AND the reconciled DuckDB type must both be correct.
# ---------------------------------------------------------------------------

statement ok
SELECT * FROM fb.main.V_DEPT_HEADCOUNT;

query I
SELECT typeof(TOTAL_SALARY) = 'DECIMAL(18,2)' FROM fb.main.V_DEPT_HEADCOUNT LIMIT 1;
----
true

query T
SELECT SUM(TOTAL_SALARY) = (SELECT SUM(SALARY) FROM fb.main.EMPLOYEE)
  FROM fb.main.V_DEPT_HEADCOUNT;
----
true
```

Before finalizing, the implementer MUST verify the exact reconciled type string DuckDB reports for `typeof(...)` on this build/DuckDB version (it may render as `DECIMAL(18,2)` exactly, or with different internal spacing) — run it live and adjust the expected string to match verified reality, the same standard already established in this codebase (never guess a `typeof()` string). The second query cross-checks the reconciled value against an independent aggregate computed directly over `EMPLOYEE` — this is what actually proves correctness, not just "didn't crash."

- [ ] **Step 2: Run the test to verify it fails**

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-tlh/test/sql/firebird_scan.test"`
Expected: FAIL — the first `statement ok` block throws `INTERNAL Error: Expected vector of type INT64, but found vector of type INT128` (or an equivalent internal error on this exact build — confirm the failure is the crash, not an unrelated pre-existing failure).

- [ ] **Step 3: Restructure the view-shape block to compute `is_view` unconditionally**

In `src/firebird_scanner.cpp`, find this block in `FirebirdScanBind` (the exact code currently there, following the PK probe):

```cpp
    // View-shape lookup for the ROWS-pagination safety decision. Only
    // worth the extra round trip when there is no single-column numeric
    // PK to fall back on for ordering — that path is already safe and
    // cheap, so we don't pay this cost for the common case. Also gated on
    // `paging`: is_view/is_view_simple_for_pagination are consumed ONLY by
    // OpenNextPartitionCursor's pagination decision below, so a plain
    // (non-paginated) scan of a PK-less table/view must not pay this extra
    // round trip for a value it will never use.
    if (!bind->pk && paging) {
        std::string upper = bind->table_name;
        for (auto &c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        std::string object_type;
        if (LookupObjectType(conn, upper, object_type)) {
            bind->is_view = (object_type == "VIEW");
            if (bind->is_view) {
                ViewAnalysis va = AnalyzeViewSource(conn, upper);
                bind->is_view_simple_for_pagination =
                    va.inspected && !va.has_join && !va.has_group_by &&
                    !va.has_aggregate;
            }
        }
    }

    return_types = bind->column_types;
    names        = bind->column_names;
    return std::move(bind);
```

Replace with:

```cpp
    // View-type check: cheap, single indexed RDB$RELATIONS lookup — now
    // run UNCONDITIONALLY (not gated on paging) because it also drives
    // the type/descriptor reconciliation below (#33), which matters for
    // ANY view scan, not just paginated ones. The (much more expensive)
    // AnalyzeViewSource text-parse below stays gated to the pagination
    // case only — it is not needed for the reconciliation fix.
    std::string upper_for_view_check = bind->table_name;
    for (auto &c : upper_for_view_check) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    {
        std::string object_type;
        if (LookupObjectType(conn, upper_for_view_check, object_type)) {
            bind->is_view = (object_type == "VIEW");
        }
    }

    // Pagination-safety view-shape analysis (Smart Scan Planning Report) —
    // still gated to the paginated, PK-less case only; expensive
    // (RDB$VIEW_SOURCE text parse), and its result is consumed ONLY by
    // OpenNextPartitionCursor's pagination decision.
    if (bind->is_view && !bind->pk && paging) {
        ViewAnalysis va = AnalyzeViewSource(conn, upper_for_view_check);
        bind->is_view_simple_for_pagination =
            va.inspected && !va.has_join && !va.has_group_by &&
            !va.has_aggregate;
    }

    // Type/descriptor reconciliation (#33): a view's column metadata is
    // frozen in RDB$FIELDS at CREATE VIEW time and can legitimately
    // disagree with what Firebird's live DSQL compiler produces for the
    // identical expression today (confirmed root cause: SUM() over a
    // NUMERIC(10,2) column promotes to BIGINT-backed NUMERIC(18,2) at
    // runtime, while the view's frozen catalog metadata claims
    // INT128-backed DECIMAL(38,2) — allocating the DuckDB Vector from the
    // stale wide type while the fetch writes the narrow real type crashes
    // DuckDB's own Vector::VerifyVectorType assertion). Reconcile BOTH
    // column_types (drives Vector allocation via return_types below) AND
    // column_descs (drives FirebirdAppendValue's fetch-time accessor
    // width) from a live, execute-free Prepare against the exact
    // projected column list — never from the catalog alone. Only the
    // character_set_id stays catalog-sourced (XSQLDA cannot supply it for
    // BLOB columns; LoadTableSchema already reads it from RDB$FIELDS).
    //
    // Best-effort: if the live describe fails for any reason, keep the
    // catalog-derived types (today's behavior, unchanged) rather than
    // hard-failing the bind — no worse than the pre-fix baseline.
    if (bind->is_view) {
        try {
            std::ostringstream probe_sql;
            probe_sql << "SELECT ";
            for (idx_t i = 0; i < bind->column_names.size(); ++i) {
                if (i) probe_sql << ", ";
                probe_sql << QuoteIdent(bind->column_names[i]);
            }
            probe_sql << " FROM " << QuoteIdent(bind->table_name);

            FirebirdStatement probe(conn, probe_sql.str(),
                                    FirebirdStatement::PrepareOnlyTag{});
            const auto &live_cols = probe.columns();
            if (live_cols.size() == bind->column_descs.size()) {
                for (idx_t i = 0; i < live_cols.size(); ++i) {
                    // Preserve the catalog-sourced character_set_id —
                    // XSQLDA cannot supply it for BLOB columns (that slot
                    // carries the blob subtype instead).
                    FirebirdColumnDesc reconciled = live_cols[i];
                    reconciled.character_set_id =
                        bind->column_descs[i].character_set_id;
                    bind->column_descs[i] = reconciled;
                    bind->column_types[i] = FirebirdToDuckDBType(reconciled);
                }
            }
        } catch (...) {
            // Live describe failed — fall back to the catalog-derived
            // types already in bind->column_types/column_descs.
        }
    }

    return_types = bind->column_types;
    names        = bind->column_names;
    return std::move(bind);
```

- [ ] **Step 4: Build and run the test**

Run: `scripts\build_windows_local.bat`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-tlh/test/sql/firebird_scan.test"`
Expected: `All tests passed (N assertions)` — including the new #33 regression case.

Also run the regression checks for the two other files this bind path affects:
`build\release\test\unittest.exe "C:/tmp/fbwt-tlh/test/sql/firebird_paging.test"` and
`build\release\test\unittest.exe "C:/tmp/fbwt-tlh/test/sql/firebird_explain_pushdown.test"`
(set `$env:FIREBIRD_NONE_DB="C:\fbtest\none.fdb"` for the latter) — both exercise
`bind->is_view`/pagination logic touched by Step 3's restructuring; confirm no
regression (same assertion counts as before this task).

- [ ] **Step 5: Commit**

```bash
git add src/firebird_scanner.cpp test/sql/firebird_scan.test
git commit -m "fix(type-lossless): reconcile view column types from live XSQLDA (#33)

A view's computed/aggregate column type is frozen in RDB\$FIELDS at
CREATE VIEW time and can legitimately disagree with what Firebird's
live DSQL compiler produces for the identical expression today.
Confirmed root cause for #33: SUM(NUMERIC(10,2)) promotes to
BIGINT-backed NUMERIC(18,2) at runtime, while the view's catalog
metadata claims INT128-backed DECIMAL(38,2) -- allocating the Vector
from the stale wide type while the fetch writes the narrow real type
crashes DuckDB's Vector::VerifyVectorType assertion.

For a view scan target, reconcile both column_types (drives Vector
allocation) and column_descs (drives fetch-time accessor width) from
a live, execute-free Prepare against the projected columns -- gated
to view targets only, so base-table scan binds pay zero added cost."
```

---

### Task 3: Documentation (PT/EN)

**Files:**
- Modify: `docs/pt/function_manual.md`
- Modify: `docs/en/function_manual.md`

**Interfaces:**
- Consumes: the fix from Task 2.
- Produces: nothing (docs only).

- [ ] **Step 1: Document in the EN manual**

In `docs/en/function_manual.md`, find `firebird_scan`'s existing documentation. Add a short note (a new bullet or short paragraph, near any existing notes about type handling/`firebird_type_audit`):

> When the scan target is a view, column types are reconciled against
> Firebird's live, runtime-described column layout rather than trusted
> from the view's catalog metadata alone. A view's computed/aggregate
> column type (e.g. a `SUM()` over a `NUMERIC` column) is frozen at
> `CREATE VIEW` time and can legitimately disagree with what Firebird's
> live SQL compiler produces for the identical expression today —
> without this reconciliation, that mismatch could crash the scan.
> Base-table scans are unaffected (a table's storage width cannot
> disagree with its own catalog metadata) and pay no added cost.

- [ ] **Step 2: Document in the PT manual (parity)**

In `docs/pt/function_manual.md`, mirror Step 1 in `firebird_scan`'s section — the same note, translated, following that file's existing heading/note conventions.

- [ ] **Step 3: Commit**

```bash
git add docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(type-lossless): document view column type reconciliation (#33 fix)"
```

---

## Self-Review

**Spec coverage:**
- Root cause fix (live XSQLDA reconciliation for views) → Task 2. ✔
- BOTH `column_types` AND `column_descs` corrected, not just one → Task 2 Step 3's reconciliation loop sets both from the same `reconciled` struct. ✔
- `character_set_id` stays catalog-sourced → Task 2 Step 3 explicitly preserves it before overwriting the rest of the struct. ✔
- Gated to view targets only (zero cost for base tables) → Task 2 Step 3's `if (bind->is_view)` guard; the `is_view` check itself is unconditional but cheap (single indexed catalog lookup), documented as the deliberate, small, unavoidable cost of even making the decision — the EXPENSIVE part (the prepare-only describe call) stays view-gated. ✔
- Test proves both "doesn't crash" AND "value + type correct" → Task 2 Step 1's three assertions (unconditional scan succeeds; `typeof` matches the reconciled type; value cross-checked against an independent aggregate). ✔
- `ReadBlob`/`TIME_TZ` out of scope → not touched by any task; issue #35 already opened for `ReadBlob`. ✔
- No new fixture → Task 2 reuses existing `V_DEPT_HEADCOUNT`. ✔
- `firebird_type_audit` unaffected → not modified by any task (confirmed in the spec: it reads the catalog directly for its own reporting purpose, independent of the scan bind path this fix touches).

**Placeholder scan:** No TBD/TODO; all code blocks complete; the Step 1 note about verifying the exact `typeof()` string live is explicit verification guidance, not a vague placeholder.

**Type consistency:** `FirebirdStatement::PrepareOnlyTag` used identically in its header declaration (Task 1) and its call site (Task 2). `FirebirdToDuckDBType`/`QuoteIdent` called with their existing, unchanged signatures (no new overloads needed — confirmed both are already free functions callable from `firebird_scanner.cpp` without new includes). `bind->is_view` read by both the (now unconditional) reconciliation block and the (still gated) pagination block, consistent with its single declaration in `firebird_scanner.hpp` (untouched by this plan — the field already exists from the Smart Scan Planning Report work).

**Note for implementer:** Task 2 Step 3's restructuring moves `LookupObjectType`'s call outside its previous `!bind->pk && paging` guard — read the CURRENT state of this exact block before editing (the plan's "find this block" text is the ground truth for content; if line numbers have drifted, locate by the comment text `"View-shape lookup for the ROWS-pagination safety decision"`).
