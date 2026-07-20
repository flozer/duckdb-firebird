# ReconcileViewColumnTypes Fallback Hardening (#36) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Change `ReconcileViewColumnTypes` so that any failure to *positively confirm* an object is not a view (a `LookupObjectType` exception, or a plain `false` return) falls through to the existing defensive live-describe probe, instead of skipping reconciliation — closing issue #36 without reintroducing the #33 crash risk under uncertainty.

**Architecture:** Single, narrowly-scoped change to one function's control flow in `src/firebird_view_analysis.cpp`. No signature changes, no new files, no changes to the probe block or to `LookupObjectType` itself.

**Tech Stack:** C++17, DuckDB extension C API, Firebird ISC client API (via the existing `FirebirdConnection`/`FirebirdStatement` wrappers).

## Global Constraints

- Only skip reconciliation when the object is **confirmed non-view** (`LookupObjectType` returns `true` and `object_type != "VIEW"`). Every other outcome — an exception, or a `false` return (object not found) — must fall through to the existing live-describe probe, never an early skip. This is the one rule the whole fix exists to enforce; do not weaken it for any reason, including performance.
- No test-only fault-injection hook added to `LookupObjectType`, `ReconcileViewColumnTypes`, or any call site. Validation is via full existing suite + cross-version DuckDB matrix + CI, not a simulated fault.
- No change to `LookupObjectType`'s signature or the probe block's existing `catch (...)` (`firebird_view_analysis.cpp:230-233`).
- No changes to `duckdb/community-extensions` or any upstream repo.
- This is not a v1.0.0 blocker in itself (already shipped) — closing it is a prerequisite Fernando set before considering a Community Update, and a candidate for a v1.0.1 patch.
- Strong adversarial review required specifically on: the try/catch scope (confirm it wraps only the `LookupObjectType` call), and the `object_type_confirmed` three-way logic (confirmed-view / confirmed-non-view / unknown-so-defensive-probe).

---

### Task 1: Harden the object-type check in ReconcileViewColumnTypes

**Files:**
- Modify: `src/firebird_view_analysis.cpp:198-205`

**Interfaces:**
- Consumes: `LookupObjectType(FirebirdConnection &conn, const std::string &upper_table, std::string &out_type)` (unchanged signature, `src/firebird_view_analysis.cpp:31-45`) — returns `bool` (found a matching `RDB$RELATIONS` row or not), throws on a genuine connection-level failure.
- Produces: nothing consumed by a later task — this is the only task in this plan. `ReconcileViewColumnTypes`'s own signature and both call sites (`firebird_scanner.cpp:585`, `firebird_storage.cpp:391-394`) are unchanged.

- [ ] **Step 1: Confirm the current file content matches this plan's assumptions**

```bash
sed -n '188,236p' src/firebird_view_analysis.cpp
```

Expected (confirmed while writing this plan, reading the file directly):

```cpp
bool ReconcileViewColumnTypes(FirebirdConnection &conn,
                              const std::string &table_name,
                              const duckdb::vector<std::string> &column_names,
                              duckdb::vector<LogicalType> &column_types,
                              duckdb::vector<FirebirdColumnDesc> &column_descs) {
    std::string upper = table_name;
    for (auto &c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    std::string object_type;
    if (!LookupObjectType(conn, upper, object_type)) {
        return false;
    }
    const bool is_view = (object_type == "VIEW");
    if (!is_view) {
        return false;
    }

    try {
        std::ostringstream probe_sql;
        probe_sql << "SELECT ";
        for (idx_t i = 0; i < column_names.size(); ++i) {
            if (i) probe_sql << ", ";
            probe_sql << QuoteIdent(column_names[i]);
        }
        probe_sql << " FROM " << QuoteIdent(table_name);

        FirebirdStatement probe(conn, probe_sql.str(),
                                FirebirdStatement::PrepareOnlyTag{});
        const auto &live_cols = probe.columns();
        if (live_cols.size() == column_descs.size()) {
            for (idx_t i = 0; i < live_cols.size(); ++i) {
                // Preserve the catalog-sourced character_set_id — XSQLDA
                // cannot supply it for BLOB columns (that slot carries
                // the blob subtype instead).
                FirebirdColumnDesc reconciled = live_cols[i];
                reconciled.character_set_id = column_descs[i].character_set_id;
                column_descs[i] = reconciled;
                column_types[i] = FirebirdToDuckDBType(reconciled);
            }
        }
    } catch (...) {
        // Live describe failed — fall back to the catalog-derived types
        // already in column_types/column_descs.
    }

    return true;
}
```

If the file differs (different line numbers, different logic already present), stop and re-read the full function before editing — don't assume this plan's line numbers are still exact.

- [ ] **Step 2: Replace the object-type check (lines 198-205 only)**

Change:

```cpp
    std::string object_type;
    if (!LookupObjectType(conn, upper, object_type)) {
        return false;
    }
    const bool is_view = (object_type == "VIEW");
    if (!is_view) {
        return false;
    }
```

to:

```cpp
    std::string object_type;
    bool object_type_confirmed = false;
    try {
        object_type_confirmed = LookupObjectType(conn, upper, object_type);
    } catch (...) {
        // Connection-level failure classifying the object -- do NOT assume
        // "not a view" and skip reconciliation. That's exactly the crash
        // ReconcileViewColumnTypes exists to prevent (see the aggregate-view
        // INT128/NUMERIC comment at this function's call site in
        // firebird_scanner.cpp). Fall through to the live-describe probe
        // below defensively instead; object_type_confirmed stays false so
        // the check below doesn't treat this as a confirmed non-view.
    }

    // Only skip reconciliation when the object is CONFIRMED non-view --
    // never on mere uncertainty (a LookupObjectType exception, or a
    // `false` return meaning no matching RDB$RELATIONS row was found).
    // Both uncertain cases fall through to the probe below defensively.
    if (object_type_confirmed && object_type != "VIEW") {
        return false;
    }
```

The rest of the function (the `try { ... probe ... } catch (...) { ... }` block and the final `return true;`) is unchanged — do not touch it.

- [ ] **Step 3: Confirm the diff is exactly this and nothing else**

```bash
git diff src/firebird_view_analysis.cpp
```

Expected: only the lines shown in Step 2 changed (7 lines removed, 13 lines added, net +6). No other function, no other file touched.

- [ ] **Step 4: Build**

```bash
scripts\build_windows_local.bat
```

Expected: build succeeds with no new warnings from `firebird_view_analysis.cpp` (the file compiles the same translation unit as before; only control flow changed, no new includes needed for `try`/`catch`, already used elsewhere in this same file).

- [ ] **Step 5: Run the full local test suite and confirm zero regressions**

```bash
set ISC_USER=SYSDBA
set ISC_PASSWORD=masterkey
set FIREBIRD_TEST_DB=C:\fbtest\test.fdb
set FIREBIRD_DECFLOAT_DB=C:\fbtest\decfloat.fdb
set FIREBIRD_NONE_DB=C:\fbtest\none.fdb
for %f in (test\sql\firebird_*.test) do build\release\test\unittest.exe "%f"
```

(Or the equivalent loop in whatever shell you're using — the important part is running every `test/sql/firebird_*.test` file, not a subset.)

Expected: every file reports `All tests passed (N assertions)`, with **the exact same assertion counts as before this change** for every file — in particular:
- `firebird_scan.test` and `firebird_attach.test` (these exercise the `V_DEPT_HEADCOUNT` view — the confirmed-view path through `ReconcileViewColumnTypes`, both via `firebird_scan(...)` directly and via `ATTACH`-resolved `fb.main.V_DEPT_HEADCOUNT` — must still reconcile correctly and show no crash, matching the #33 regression coverage).
- Every other file (confirmed-table paths) must show unchanged counts too.

If any count differs, STOP — do not adjust the test or the fixture to make counts match; something about the control-flow change altered a code path this plan says should be untouched. Report BLOCKED with the exact diff in counts.

- [ ] **Step 6: Cross-version DuckDB matrix**

```powershell
# From a copy of scripts/build_matrix.ps1 with $root repointed at this
# worktree (C:/tmp/fbwt-36), never the dirty principal worktree -- same
# pattern used throughout this session (see the sed-copy approach from
# the Runtime/ABI Compatibility Gate front).
pwsh -File <repointed-copy>/build_matrix.ps1
```

Expected: `v1.5.2`, `v1.5.3`, `v1.5.4` all `PASS`, same assertion count as Step 5 for each (854 as of the last full-suite run this session, but re-derive from Step 5's actual output rather than assuming this number is still current).

- [ ] **Step 7: Commit**

```bash
git add src/firebird_view_analysis.cpp
git commit -m "$(cat <<'EOF'
fix(view-reconcile): #36 -- never skip reconciliation on mere uncertainty

ReconcileViewColumnTypes called LookupObjectType with no try/catch of
its own; a connection-level failure there propagated straight up,
which the batch ATTACH path's own outer catch (firebird_storage.cpp)
couldn't distinguish from a genuine full-batch failure -- both
demoted the whole ATTACH to the slower per-table fallback instead of
isolating just the one problematic view.

The issue's own suggested fix (treat a LookupObjectType failure as
"not reconciled", return false) was unsafe: column_types/column_descs
are exactly the fields that prevent the #33 crash (a view's frozen
catalog metadata claiming a wider type than what SUM()/aggregates
actually produce at runtime). Skipping reconciliation on a mere
connection hiccup would silently reintroduce that crash, gated behind
an even rarer trigger.

Corrected rule: only skip reconciliation when LookupObjectType
POSITIVELY CONFIRMS the object is not a view (succeeds, returns a
non-VIEW type). Any other outcome -- an exception, or a `false` return
(no matching RDB$RELATIONS row) -- now falls through to the existing
live-describe probe defensively, the same as a confirmed view. The
probe is execute-free (PrepareOnlyTag) and safe to run against a
plain table too; its own existing catch(...) already handles a
genuinely-dropped table gracefully.

No test-only fault-injection hook added (rejected as unnecessary
invasiveness for this narrow case) -- validated via the full existing
suite (zero assertion-count regressions, including the #33
V_DEPT_HEADCOUNT regression coverage) and the cross-version DuckDB
matrix (v1.5.2/v1.5.3/v1.5.4).
EOF
)"
```

- [ ] **Step 8: Push, open PR, and watch real CI**

```bash
git push -u origin fix/36-reconcile-view-column-types-fallback
gh pr create --repo flozer/duckdb-firebird --draft --base main --head fix/36-reconcile-view-column-types-fallback \
  --title "fix(view-reconcile): #36 -- never skip reconciliation on mere uncertainty" \
  --body "Closes #36. See docs/superpowers/specs/2026-07-20-reconcile-view-fallback-design.md."
gh run list --repo flozer/duckdb-firebird --branch fix/36-reconcile-view-column-types-fallback --limit 5
```

Wait for all 3 workflows (`Build + Test Linux x64`, `Build + Test Linux x64 (FB 3/4/5 matrix)`, `Build Windows x64`) via `gh run watch <id> --repo flozer/duckdb-firebird --exit-status` for each. All 3 must be green — this branch touches a code path exercised on every leg (view reconciliation happens on any ATTACH/scan against a view, present in the shared `fixture_common.sql`'s `V_DEPT_HEADCOUNT`).

---

## After Task 1

Single-task plan — no further tasks. Given the strong-review requirement Fernando set, the task review for this one task must specifically verify:
1. The try/catch wraps *only* the `LookupObjectType` call (not the whole function, not extending into the probe block).
2. `object_type_confirmed`'s three states are handled correctly: confirmed-view (proceeds to probe, unchanged), confirmed-non-view (returns `false`, unchanged), unconfirmed/uncertain — whether by exception or by `false` return — (proceeds to probe, the actual behavior change).
3. No change to the probe block, `LookupObjectType`, or either call site.
4. The commit message and code comment both state the rule ("only skip reconciliation when confirmed non-view") clearly enough that a future reader can't reintroduce the issue's original unsafe suggestion without seeing why it was rejected.

This is small enough that a single strong review (not a multi-task SDD cycle) is sufficient before merge -- but the review itself must be adversarial per Fernando's explicit instruction, not a rubber stamp.
