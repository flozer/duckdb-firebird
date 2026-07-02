# Smart Scan Planning Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `firebird_explain_pushdown` with view-heaviness, charset-block, and real-planned-partition-count signals; eliminate the divergence between `firebird_profile_table`'s partition advisory and the scanner's real heuristic; fix the `ROWS`-without-`ORDER BY` pagination-determinism bug.

**Architecture:** Two small extractions (`PickPartitionCount` un-`static`'d from the scanner; `LookupObjectType`/`AnalyzeViewSource` moved to a new shared file) feed both `firebird_explain_pushdown`'s new columns and a scanner-side bind-time view-shape check. The `ROWS` fix is a static, bind-time decision (never a runtime probe) with a local-slicing fallback in the scanner's fetch loop for the one case where server-side pagination is provably unsafe.

**Tech Stack:** DuckDB out-of-tree C++ extension, existing `FirebirdConnection`/`FirebirdStatement` API, DuckDB sqllogictest `.test` files.

## Global Constraints

- No existing column of `firebird_explain_pushdown` changes name, type, or meaning — the 4 new columns are strictly additive (14 → 18).
- No change to the scanner's parallelism heuristic or charset-pushdown-gating *decisions* themselves — both are already correct today. Only the partition-count *formula* is unified (one source of truth), not the decision logic around it.
- `RDB$DB_KEY` is a physical pagination stabilizer for the lifetime of one scan/transaction only — never a business ordering contract, never used for range partitioning, incremental/watermark logic, or as a recommended key anywhere else.
- The `ROWS`-without-`ORDER BY` fix must never be detected by a runtime probe-and-catch: `SELECT RDB$DB_KEY FROM <aggregate/join view>` returns SQL `NULL` silently (verified empirically), not an error, so the safe/unsafe decision must be made statically at bind time from the view's shape.
- **Mandatory invariant, tested explicitly:** `firebird_scan(..., row_limit=N, row_offset=M)` must never return more than the requested slice, even when server-side `ROWS` pushdown is disabled for safety. When `ROWS` can't be pushed, the scanner applies the same slice locally — it never silently reverts to "return everything."
- DuckDB version-locked; submodule pin must NOT change. No duckdb/community-extensions/upstream action.

---

## File Structure

- `src/include/firebird_scanner.hpp` (modify) — declare `PickPartitionCount`; `FirebirdBindData` gains 3 new fields.
- `src/firebird_scanner.cpp` (modify) — un-`static` `PickPartitionCount`; `FirebirdScanBind` gains view-shape lookup; `OpenNextPartitionCursor` gains the pagination decision order; `FirebirdScanFunction`'s fetch loop gains local-slice skip/cap; `FirebirdLocalState` gains 2 counters.
- `src/firebird_view_analysis.hpp` / `.cpp` (new) — extracted `LookupObjectType` + `ViewAnalysis`/`AnalyzeViewSource`.
- `src/firebird_profile_table.cpp` (modify) — delete local `LookupObjectType`/`ViewAnalysis`/`AnalyzeViewSource`/partition ladder; call the shared versions; reword `pk_range_small_serial`'s message.
- `src/include/firebird_query.hpp` / `src/firebird_query.cpp` (modify) — `FirebirdQueryBuilder::Build` gains a `pagination_order_by` parameter; `ROWS` emission now pairs with `ORDER BY`.
- `src/firebird_explain_pushdown.cpp` / `.hpp` (modify) — 4 new columns.
- `test/sql/firebird_profile_table.test` (verify unchanged — no edits expected).
- `test/sql/firebird_paging.test` (modify) — new PK-less, simple-view, heavy-view, and critical named-param cases.
- `test/sql/firebird_explain_pushdown.test` (modify) — new column assertions.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` (modify) — `firebird_explain_pushdown` section (4 new columns + `RDB$DB_KEY` semantics notes); `firebird_profile_table` section (shared-formula note).

---

### Task 1: Single source of truth for partition count

**Files:**
- Modify: `src/include/firebird_scanner.hpp`
- Modify: `src/firebird_scanner.cpp:584-592`
- Modify: `src/firebird_profile_table.cpp:598-645`
- Test: `test/sql/firebird_profile_table.test` (verify — no edits expected)

**Interfaces:**
- Consumes: nothing new.
- Produces: `idx_t PickPartitionCount(int64_t min_v, int64_t max_v)` — declared in `firebird_scanner.hpp`, defined in `firebird_scanner.cpp`, callable from any translation unit that includes that header (both `firebird_explain_pushdown.cpp` and `firebird_profile_table.cpp` already do, transitively or directly).

- [ ] **Step 1: Declare `PickPartitionCount` in the header**

In `src/include/firebird_scanner.hpp`, add this declaration right after the `ClassifyPkRange` declaration (after line 41, `PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d);`):

```cpp
// Single source of truth for scan-parallelism sizing: given a PK's
// [min_v, max_v] range, returns how many partitions the scanner will
// actually use. Pure/numeric — no Firebird I/O, safe to call from
// anywhere (firebird_profile_table's advisory recommendation and
// firebird_explain_pushdown's planned_partitions column both call this
// so they can never diverge from the scanner's real behavior).
idx_t PickPartitionCount(int64_t min_v, int64_t max_v);
```

- [ ] **Step 2: Un-`static` the definition**

In `src/firebird_scanner.cpp`, find:

```cpp
static idx_t PickPartitionCount(int64_t min_v, int64_t max_v) {
```

Replace with:

```cpp
idx_t PickPartitionCount(int64_t min_v, int64_t max_v) {
```

(No other change to the function body — it stays exactly as-is: `firebird_scanner.cpp:584-592`.)

- [ ] **Step 3: Build to confirm the header change compiles**

Run: `scripts\build_windows_local.bat`
Expected: build succeeds (this step only removes a `static` keyword and adds a declaration; no behavior change yet).

- [ ] **Step 4: Replace `firebird_profile_table.cpp`'s duplicate ladder**

In `src/firebird_profile_table.cpp`, find this block (currently lines 598-645):

```cpp
    } else if (pk) {
        // Single-column numeric PK with a usable MIN/MAX range — the only
        // safe parallelism lever the scanner supports. The recommendation
        // is advisory and conservative: it scales partition count by the PK
        // *range width* (not row count, which we deliberately do not probe —
        // no COUNT(*) / full scan in the analyzer) and caps at a small
        // ceiling so we never suggest runaway parallelism against an OLTP
        // server. Ceiling stays at 8 (matches the scanner's own auto path).
        const int64_t span = pk->max_value - pk->min_value;
        int32_t parts = 1;
        if (span >= 1000000) {
            parts = 8;
        } else if (span >= 100000) {
            parts = 4;
        } else if (span >= 10000) {
            parts = 2;
        }
        p.recommended_partitions = parts;
        p.full_scan_risk = (parts > 1) ? "MEDIUM" : "LOW";
        if (parts > 1) {
            AddAlert(p, "partition_advisory", "LOW",
                "Recommended partitions=" + std::to_string(parts) +
                " is advisory and derived from the PK MIN/MAX range width, "
                "not the row count. The PK range may be sparse (gaps, "
                "deletes), so partitions can be uneven. Validate against the "
                "live server before scanning a production database in "
                "parallel.");
            // Server-side parallelism caveat: Firebird 5 can parallelize a
            // single query server-side, and combining that with client-side
            // PK-range partitions can oversubscribe the server. We have no
            // cheap, reliable probe for the server's ParallelWorkers setting
            // from the catalog, so this is surfaced as a generic caveat
            // rather than a detected condition.
            AddAlert(p, "server_parallelism_caveat", "LOW",
                "If Firebird server-side parallelism is already "
                "enabled/configured (e.g. Firebird 5 ParallelWorkers), prefer "
                "starting with partitions=1 or benchmark before combining "
                "server-side and client-side parallelism.");
        } else {
            // PK is numeric with a real but narrow range (span < 10000):
            // ProbePrimaryKey accepted it, but partitioning would not pay
            // off. Be explicit so the caller does not wonder why a numeric
            // PK still recommends serial.
            AddAlert(p, "pk_range_small_serial", "LOW",
                "Primary key range is small (PK MIN/MAX span < 10000): "
                "serial scan recommended; partitioning would add overhead "
                "without meaningful parallelism.");
        }
    } else {
```

Replace with:

```cpp
    } else if (pk) {
        // Single-column numeric PK with a usable MIN/MAX range — the only
        // safe parallelism lever the scanner supports. recommended_partitions
        // is now computed by the SAME PickPartitionCount the scanner itself
        // uses at scan time (firebird_scanner.hpp), so this recommendation
        // can never diverge from what a real scan would actually do. It
        // remains advisory in spirit (row count is never probed — no
        // COUNT(*) / full scan in the analyzer), but the number itself is
        // now the scanner's real answer, not a separate estimate.
        const idx_t parts = PickPartitionCount(pk->min_value, pk->max_value);
        p.recommended_partitions = static_cast<int32_t>(parts);
        p.full_scan_risk = (parts > 1) ? "MEDIUM" : "LOW";
        if (parts > 1) {
            AddAlert(p, "partition_advisory", "LOW",
                "Recommended partitions=" + std::to_string(parts) +
                " is advisory and derived from the PK MIN/MAX range width, "
                "not the row count. The PK range may be sparse (gaps, "
                "deletes), so partitions can be uneven. Validate against the "
                "live server before scanning a production database in "
                "parallel.");
            // Server-side parallelism caveat: Firebird 5 can parallelize a
            // single query server-side, and combining that with client-side
            // PK-range partitions can oversubscribe the server. We have no
            // cheap, reliable probe for the server's ParallelWorkers setting
            // from the catalog, so this is surfaced as a generic caveat
            // rather than a detected condition.
            AddAlert(p, "server_parallelism_caveat", "LOW",
                "If Firebird server-side parallelism is already "
                "enabled/configured (e.g. Firebird 5 ParallelWorkers), prefer "
                "starting with partitions=1 or benchmark before combining "
                "server-side and client-side parallelism.");
        } else {
            // PK is numeric with a real range, but below the scanner's own
            // partitioning threshold (PickPartitionCount's minimum-rows-per-
            // partition floor) — the same function that decides this at
            // scan time returned 1. Be explicit so the caller does not
            // wonder why a numeric PK still recommends serial.
            AddAlert(p, "pk_range_small_serial", "LOW",
                "Primary key range is small relative to the scanner's "
                "partitioning threshold: serial scan recommended; "
                "partitioning would add overhead without meaningful "
                "parallelism.");
        }
    } else {
```

- [ ] **Step 5: Build and run the affected test**

Run: `scripts\build_windows_local.bat`

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_profile_table.test"`

Expected: `All tests passed (N assertions)`. The `EMPLOYEE` fixture's PK span is tiny (5 rows, `EMP_ID` 1–5) — both the old ladder and `PickPartitionCount` return `parts=1` for it, and the existing test asserts `warnings ... LIKE '%range is small%'` (a substring, not the exact old "span < 10000" number), so no test edit is needed. If this fails, the reworded message likely dropped the "range is small" substring — check the exact wording against Step 4's replacement text.

- [ ] **Step 6: Commit**

```bash
git add src/include/firebird_scanner.hpp src/firebird_scanner.cpp src/firebird_profile_table.cpp
git commit -m "refactor(scan-planner): single source of truth for partition count

firebird_profile_table.recommended_partitions now calls the scanner's
own PickPartitionCount instead of a separate, differently-calibrated
ladder. The two could recommend different partition counts than what
a real scan would use; now they cannot diverge."
```

---

### Task 2: Extract shared view-shape analysis

**Files:**
- Create: `src/firebird_view_analysis.hpp`
- Create: `src/firebird_view_analysis.cpp`
- Modify: `src/firebird_profile_table.cpp:146-341` (delete local copies, call shared versions)
- Modify: `CMakeLists.txt` (add the new source file)
- Test: `test/sql/firebird_profile_table.test` (verify — no edits expected, this is a behavior-preserving refactor)

**Interfaces:**
- Consumes: `firebird_client.hpp`'s `FirebirdConnection`.
- Produces: `bool LookupObjectType(FirebirdConnection&, const std::string &upper_table, std::string &out_type)` and `struct ViewAnalysis { bool inspected, has_join, has_group_by, has_aggregate, has_where; }` + `ViewAnalysis AnalyzeViewSource(FirebirdConnection&, const std::string &upper_table)` — both declared in `firebird_view_analysis.hpp`, callable by `firebird_profile_table.cpp` (Task 2), `firebird_explain_pushdown.cpp` (Task 3), and `firebird_scanner.cpp` (Task 4).

- [ ] **Step 1: Create the header**

Create `src/firebird_view_analysis.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// RDB$RELATIONS.RDB$RELATION_TYPE: 0/NULL persistent table, 1 view, 2
// external, 3 MON$ snapshot, 4/5 GTT. RDB$VIEW_BLR being non-NULL is the
// canonical "this is a view" signal across Firebird versions, so we lean
// on it as the primary discriminator and fall back to RELATION_TYPE.
// Returns false (with no rows) when the relation does not exist.
bool LookupObjectType(FirebirdConnection &conn,
                     const std::string &upper_table,
                     std::string &out_type);

// Heavy-view analysis result. All best-effort: when the view source can't
// be read safely, `inspected` stays false and the caller emits an explicit
// "definition not inspected" signal instead of guessing.
struct ViewAnalysis {
    bool inspected = false;
    bool has_join = false;
    bool has_group_by = false;
    bool has_aggregate = false;
    bool has_where = false;
};

// Reads RDB$RELATIONS.RDB$VIEW_SOURCE (BLOB SUB_TYPE 1, the original view
// SELECT text) and runs cheap, conservative pattern detection over it. We
// never expose the source text itself — only boolean shape flags drive
// downstream diagnostics. Detection is intentionally shallow (token search
// on an upper-cased copy with string/comment noise stripped), NOT a SQL
// parser: the goal is a factual "this view joins / aggregates / has no
// filter" signal, not plan-level analysis.
//
// Any failure (no source blob, read error, empty text) returns
// inspected=false so the caller can say so explicitly rather than imply a
// clean simple view.
ViewAnalysis AnalyzeViewSource(FirebirdConnection &conn,
                               const std::string &upper_table);

} // namespace duckdb
```

- [ ] **Step 2: Create the implementation**

Create `src/firebird_view_analysis.cpp`:

```cpp
#include "firebird_view_analysis.hpp"

#include <string>

namespace duckdb {

// SQL-quote a single-quoted string literal (doubles embedded quotes). Local
// copy — this codebase's established convention is small per-file helper
// duplication over cross-file coupling for this exact helper (see
// firebird_scanner.cpp, firebird_profile_table.cpp, firebird_index_profile.cpp,
// each with their own copy).
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

bool LookupObjectType(FirebirdConnection &conn,
                     const std::string &upper_table,
                     std::string &out_type) {
    auto cur = conn.OpenCursor(
        "SELECT CASE WHEN r.RDB$VIEW_BLR IS NOT NULL "
        "            OR r.RDB$RELATION_TYPE = 1 THEN 'VIEW' "
        "            ELSE 'TABLE' END "
        "  FROM RDB$RELATIONS r "
        " WHERE r.RDB$RELATION_NAME = " + SqlLiteral(upper_table));
    if (!cur->Fetch()) {
        return false;
    }
    out_type = cur->GetText(0);
    return true;
}

ViewAnalysis AnalyzeViewSource(FirebirdConnection &conn,
                               const std::string &upper_table) {
    ViewAnalysis va;
    std::string src;
    try {
        auto cur = conn.OpenCursor(
            "SELECT RDB$VIEW_SOURCE FROM RDB$RELATIONS "
            " WHERE RDB$RELATION_NAME = " + SqlLiteral(upper_table));
        if (!cur->Fetch() || cur->IsNull(0)) {
            return va; // inspected = false
        }
        src = cur->ReadBlob(0);
    } catch (...) {
        return va; // inspected = false
    }
    if (src.empty()) {
        return va;
    }

    // Normalize: upper-case, and blank out anything that could carry
    // keyword-shaped noise — single-quoted string literals, line comments
    // (-- ...), and block comments (/* ... */) — so a literal like
    // 'fake JOIN and WHERE' or a commented-out clause can't trip the token
    // search. We don't need a real tokenizer for this — replacing those
    // bytes with spaces is enough to keep keyword matching honest.
    //
    // SQL escapes a single quote inside a literal by doubling it ('') —
    // that pair is NOT a close-then-reopen, it's one embedded quote. We
    // must consume both characters while staying in-string, otherwise a
    // literal like 'O''Brien JOIN' would be seen as closing after "O",
    // exposing "Brien JOIN" to the scanner. Same care for the comment
    // forms (only honored outside a string literal).
    std::string norm;
    norm.reserve(src.size());
    enum { CODE, STR, LINE_COMMENT, BLOCK_COMMENT } state = CODE;
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (state) {
        case CODE:
            if (c == '\'') {
                state = STR;
                norm.push_back(' ');
            } else if (c == '-' && n == '-') {
                state = LINE_COMMENT;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume second '-'
            } else if (c == '/' && n == '*') {
                state = BLOCK_COMMENT;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume '*'
            } else {
                norm.push_back(static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c))));
            }
            break;
        case STR:
            if (c == '\'' && n == '\'') {
                // Escaped quote: stay in string, blank both bytes.
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume the second quote
            } else if (c == '\'') {
                state = CODE; // real closing quote
                norm.push_back(' ');
            } else {
                norm.push_back(' '); // string body — blanked
            }
            break;
        case LINE_COMMENT:
            if (c == '\n') {
                state = CODE;
                norm.push_back('\n');
            } else {
                norm.push_back(' ');
            }
            break;
        case BLOCK_COMMENT:
            if (c == '*' && n == '/') {
                state = CODE;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume '/'
            } else {
                norm.push_back(' ');
            }
            break;
        }
    }

    // Collapse every run of whitespace (space, tab, newline, CR, form feed)
    // into a single space. Stored RDB$VIEW_SOURCE keeps the author's
    // original formatting, so keywords can be split by newlines/tabs
    // ("GROUP\nBY", "WHERE\t", "INNER\n  JOIN"). Without this, the
    // single-space token search below would miss them. After collapsing,
    // every keyword boundary is exactly one space.
    std::string flat;
    flat.reserve(norm.size());
    bool prev_ws = false;
    for (char c : norm) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                         c == '\f' || c == '\v');
        if (ws) {
            if (!prev_ws) {
                flat.push_back(' ');
            }
            prev_ws = true;
        } else {
            flat.push_back(c);
            prev_ws = false;
        }
    }

    // Pad with spaces so word-boundary checks at the ends are uniform.
    std::string hay = " " + flat + " ";

    auto contains = [&](const std::string &needle) {
        return hay.find(needle) != std::string::npos;
    };

    va.inspected = true;
    // JOIN in any spelling (INNER/LEFT/RIGHT/FULL/CROSS JOIN all contain
    // "JOIN"; a comma-join is not detected here — conservative, we only
    // flag the explicit keyword).
    va.has_join = contains(" JOIN ");
    va.has_group_by = contains(" GROUP BY ");
    va.has_where = contains(" WHERE ");
    // Common aggregate calls. The "(" guards against matching a column
    // named e.g. SUMMARY or MAXVALUE. Both "FUNC(" and "FUNC (" spellings
    // are checked since whitespace between the name and "(" is legal SQL.
    va.has_aggregate = contains("COUNT(") || contains("SUM(") ||
                       contains("AVG(") || contains("MIN(") ||
                       contains("MAX(") || contains("LIST(") ||
                       contains("COUNT (") || contains("SUM (") ||
                       contains("AVG (") || contains("MIN (") ||
                       contains("MAX (") || contains("LIST (");
    return va;
}

} // namespace duckdb
```

- [ ] **Step 3: Add the new source file to CMakeLists.txt**

In `CMakeLists.txt`'s `EXTENSION_SOURCES` list, add `src/firebird_view_analysis.cpp` right after `src/firebird_index_profile.cpp` (or wherever the diagnostic-function sources are grouped in the current file — the implementer should place it adjacent to `firebird_profile_table.cpp`'s entry for consistency with the other extracted-helper files in this codebase).

- [ ] **Step 4: Delete the local copies in `firebird_profile_table.cpp` and call the shared versions**

In `src/firebird_profile_table.cpp`, add the include near the top (after the existing `#include "firebird_scanner.hpp"` line):

```cpp
#include "firebird_view_analysis.hpp" // LookupObjectType, ViewAnalysis, AnalyzeViewSource
```

Delete the local `LookupObjectType` function (currently lines 158-178, the block starting `// RDB$RELATIONS.RDB$RELATION_TYPE: ...` through the closing `}` of `LookupObjectType`) and the local `ViewAnalysis` struct + `AnalyzeViewSource` function (currently lines 180-341, from `// Heavy-view analysis result...` through the closing `}` of `AnalyzeViewSource`). Do NOT delete `LoadIndexes`/`FormatIndex`/anything after — only these two extracted pieces.

No other line changes: the call sites `LookupObjectType(conn, upper, p.object_type)` (line 492) and `AnalyzeViewSource(conn, upper)` (line 571) stay textually identical — they now resolve to the shared header's declarations instead of the file-local `static` ones.

- [ ] **Step 5: Build and run the affected test**

Run: `scripts\build_windows_local.bat`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_profile_table.test"`

Expected: `All tests passed (N assertions)`, identical assertion count to before Task 1+2 (this is a pure behavior-preserving extraction — every existing assertion about `object_type`, view alerts, `indexes`, etc. must be untouched).

- [ ] **Step 6: Commit**

```bash
git add src/firebird_view_analysis.hpp src/firebird_view_analysis.cpp src/firebird_profile_table.cpp CMakeLists.txt
git commit -m "refactor(scan-planner): extract view-shape analysis to a shared file

LookupObjectType + ViewAnalysis/AnalyzeViewSource move from
firebird_profile_table.cpp to firebird_view_analysis.hpp/.cpp so
firebird_explain_pushdown and the scanner can reuse the same
~140-line view-shape logic instead of duplicating it."
```

---

### Task 3: Extend `firebird_explain_pushdown` with 4 new columns

**Files:**
- Modify: `src/firebird_explain_pushdown.cpp`
- Test: `test/sql/firebird_explain_pushdown.test`

**Interfaces:**
- Consumes: `PickPartitionCount` (Task 1), `LookupObjectType`/`AnalyzeViewSource` (Task 2), the existing `ExplainRow` struct and `WalkPlan` function in this same file.
- Produces: `firebird_explain_pushdown` now returns 18 columns; the 4 new ones (`view_heavy`, `view_heavy_reasons`, `charset_pushdown_blocked`, `planned_partitions`) are consumed by nothing downstream yet (read-only reporting surface).

- [ ] **Step 1: Write the failing tests**

Read `test/sql/firebird_explain_pushdown.test` first to see its existing `ATTACH` alias and fixture usage, then append these new cases at the end of the file (adjust the alias to match whatever the file already uses — do not introduce a second `ATTACH`):

```
# ---------------------------------------------------------------------------
#  Smart Scan Planning Report additions: view_heavy, view_heavy_reasons,
#  charset_pushdown_blocked, planned_partitions.
# ---------------------------------------------------------------------------

# Base table target: view_heavy is NULL (not a view), charset_pushdown_blocked
# is false (EMPLOYEE has no CHARACTER SET NONE columns), planned_partitions
# reflects the real PickPartitionCount for EMPLOYEE's tiny PK range (= 1).

query TTT
SELECT view_heavy, charset_pushdown_blocked, planned_partitions
  FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.EMPLOYEE WHERE EMP_ID IS NOT NULL');
----
NULL	false	1

# Simple view target (no JOIN/GROUP BY/aggregate): view_heavy=false,
# view_heavy_reasons is empty.

query TT
SELECT view_heavy, len(view_heavy_reasons)
  FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.V_ACTIVE_EMP WHERE EMP_ID IS NOT NULL');
----
false	0

# Heavy view target (self-JOIN + GROUP BY): view_heavy=true with reasons.

query T
SELECT view_heavy FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.V_DEPT_HEADCOUNT');
----
true

query T
SELECT len(view_heavy_reasons) > 0 FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.V_DEPT_HEADCOUNT');
----
true
```

If the fixture DB already has a `CHARACTER SET NONE` text-column table reachable through this test's `ATTACH` alias, add one more case asserting `charset_pushdown_blocked=true` for a filter on that column; if not, note in the implementer report that this specific sub-case is unverifiable with the current fixture and skip it (do not invent a new fixture for this task — that is out of scope).

- [ ] **Step 2: Run the tests to verify they fail**

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_explain_pushdown.test"`

Expected: FAIL — `view_heavy`/`charset_pushdown_blocked`/`planned_partitions`/`view_heavy_reasons` are not yet columns of `firebird_explain_pushdown`.

- [ ] **Step 3: Add the includes**

In `src/firebird_explain_pushdown.cpp`, add after the existing `#include "firebird_query.hpp"` line:

```cpp
#include "firebird_view_analysis.hpp" // LookupObjectType, ViewAnalysis, AnalyzeViewSource
```

- [ ] **Step 4: Extend `ExplainRow`**

Find the `ExplainRow` struct (currently lines 65-85) and add these 4 fields right before the closing `};`:

```cpp
    bool         view_heavy_valid = false;
    bool         view_heavy       = false;
    std::vector<std::string> view_heavy_reasons;
    bool         charset_pushdown_blocked = false;
    bool         planned_partitions_valid = false;
    int64_t      planned_partitions       = 0;
```

- [ ] **Step 5: Extend the bind schema**

Find the `names`/`return_types` assignment in `ExplainPushdownBind` (currently lines 211-242) and add these 4 entries at the end of each vector (after `"scan_strategy"` / the last `LogicalType::VARCHAR`):

```cpp
        "view_heavy",
        "view_heavy_reasons",
        "charset_pushdown_blocked",
        "planned_partitions",
```

and:

```cpp
        LogicalType::BOOLEAN,
        LogicalType::LIST(LogicalType::VARCHAR),
        LogicalType::BOOLEAN,
        LogicalType::BIGINT,
```

- [ ] **Step 6: Compute the new signals in `WalkPlan`**

In `src/firebird_explain_pushdown.cpp`'s `WalkPlan`, find the closing of the PK-range-eligibility block (currently ending around line 386, right before `rows.push_back(std::move(r));`):

```cpp
            {
                auto c = ClassifyPkRange(bd.pk_descriptor);
                r.pk_range_eligible = c.eligible;
                r.pk_range_column   = c.column;
                r.pk_range_reason   = c.reason;
                r.scan_strategy     =
                    (c.strategy == PkRangeStrategy::PK_RANGE_PARTITIONABLE)
                        ? "pk-range-partitionable"
                        : "serial";
            }

            rows.push_back(std::move(r));
```

Replace with:

```cpp
            {
                auto c = ClassifyPkRange(bd.pk_descriptor);
                r.pk_range_eligible = c.eligible;
                r.pk_range_column   = c.column;
                r.pk_range_reason   = c.reason;
                r.scan_strategy     =
                    (c.strategy == PkRangeStrategy::PK_RANGE_PARTITIONABLE)
                        ? "pk-range-partitionable"
                        : "serial";
            }

            // planned_partitions: the REAL count PickPartitionCount would
            // pick for this table's PK range, zero additional I/O — bd.pk
            // is already populated (lazily, memoized) by
            // FirebirdTableEntry::GetScanFunction at ATTACH-bind time, the
            // same round trip a real scan already pays.
            if (bd.pk) {
                r.planned_partitions_valid = true;
                r.planned_partitions = static_cast<int64_t>(
                    PickPartitionCount(bd.pk->min_value, bd.pk->max_value));
            }

            // charset_pushdown_blocked: pure synthesis of the already-
            // computed not_pushed_reasons — no new detection logic.
            r.charset_pushdown_blocked =
                std::find(r.not_pushed_reasons.begin(),
                          r.not_pushed_reasons.end(),
                          "NONE_CHARSET") != r.not_pushed_reasons.end();

            // view_heavy / view_heavy_reasons: this is the one genuinely
            // new I/O in this function — reading RDB$RELATIONS /
            // RDB$VIEW_SOURCE requires a live connection, which capture-
            // only pushdown telemetry never needed before. Opened
            // directly from the cached conn_info (same pattern
            // FirebirdScanBind itself uses), scoped to this one lookup.
            {
                FirebirdConnection conn(bd.conn_info);
                std::string upper = bd.table_name;
                for (auto &c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                std::string object_type;
                if (LookupObjectType(conn, upper, object_type)) {
                    r.view_heavy_valid = true;
                    r.view_heavy = (object_type == "VIEW");
                    if (r.view_heavy) {
                        ViewAnalysis va = AnalyzeViewSource(conn, upper);
                        if (!va.inspected) {
                            r.view_heavy_reasons.push_back(
                                "view definition not inspected "
                                "(RDB$VIEW_SOURCE unavailable or unreadable)");
                        } else {
                            if (va.has_join) {
                                r.view_heavy_reasons.push_back(
                                    "view contains a JOIN");
                            }
                            if (va.has_group_by || va.has_aggregate) {
                                r.view_heavy_reasons.push_back(
                                    "view contains aggregation "
                                    "(GROUP BY or aggregate functions)");
                            }
                            if (!va.has_where) {
                                r.view_heavy_reasons.push_back(
                                    "view has no WHERE filter in its "
                                    "definition");
                            }
                        }
                    }
                }
            }

            rows.push_back(std::move(r));
```

- [ ] **Step 7: Emit the new columns**

In `ExplainPushdownFunction`, find the last existing column write (currently `output.data[13].SetValue(row, Value(r.scan_strategy));`) and add these 4 lines right after it, before `++row;`:

```cpp
        output.data[14].SetValue(row, r.view_heavy_valid
            ? Value::BOOLEAN(r.view_heavy)
            : Value(LogicalType::BOOLEAN));
        output.data[15].SetValue(row, make_varchar_list(r.view_heavy_reasons));
        output.data[16].SetValue(row, Value::BOOLEAN(r.charset_pushdown_blocked));
        output.data[17].SetValue(row, r.planned_partitions_valid
            ? Value::BIGINT(r.planned_partitions)
            : Value(LogicalType::BIGINT));
```

- [ ] **Step 8: Build and run the test**

Run: `scripts\build_windows_local.bat`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_explain_pushdown.test"`

Expected: `All tests passed (N assertions)`, including the new cases from Step 1.

- [ ] **Step 9: Commit**

```bash
git add src/firebird_explain_pushdown.cpp test/sql/firebird_explain_pushdown.test
git commit -m "feat(scan-planner): view_heavy, charset_pushdown_blocked, planned_partitions

Additive columns on firebird_explain_pushdown (14 -> 18). view_heavy
reuses the shared AnalyzeViewSource; charset_pushdown_blocked
synthesizes the already-computed not_pushed_reasons; planned_partitions
uses the scanner's real PickPartitionCount against the already-cached
PrimaryKeyInfo -- zero additional I/O beyond what ATTACH-bind already
pays."
```

---

### Task 4: Fix `ROWS`-without-`ORDER BY` (correctness)

**Files:**
- Modify: `src/include/firebird_query.hpp`
- Modify: `src/firebird_query.cpp`
- Modify: `src/include/firebird_scanner.hpp`
- Modify: `src/firebird_scanner.cpp`
- Modify: `src/firebird_explain_pushdown.cpp` (its own `Build()` call site needs the new parameter)
- Test: `test/sql/firebird_paging.test`

**Interfaces:**
- Consumes: `LookupObjectType`/`AnalyzeViewSource` (Task 2). `FirebirdBindData` (Task 1's `PickPartitionCount` is unrelated to this task).
- Produces: `FirebirdQueryBuilder::Build` gains a `pagination_order_by` parameter (required whenever `limit` is valid); `FirebirdBindData` gains `is_view`, `is_view_simple_for_pagination`, and no new bind field is needed for the local-slice flag — it is derived at scan time in `OpenNextPartitionCursor`, not stored on bind data (see Step 6).

- [ ] **Step 1: Write the failing tests**

Read `test/sql/firebird_paging.test` first to confirm its existing `ATTACH` alias, then append these cases (adjust the alias to match):

```
# ---------------------------------------------------------------------------
#  Smart Scan Planning Report: ROWS pagination must always pair with a
#  server-side ORDER BY, using a decision order:
#    1. single-column numeric PK -> ORDER BY <pk> ASC
#    2. base table, no such PK -> ORDER BY RDB$DB_KEY
#    3. simple view (no join/group-by/aggregate), no such PK -> ORDER BY RDB$DB_KEY
#    4. heavy view / uninspectable source -> do NOT push ROWS; slice locally
# ---------------------------------------------------------------------------

# Case 2: PK-less base table (TNO_INDEX, from the firebird_index_profile
# item — zero index, zero PK). Two ROWS windows must not overlap: the same
# row must not appear on both pages (proves RDB$DB_KEY ordering is stable
# across pages within one scan).

statement ok
CREATE OR REPLACE TABLE t_page1 AS
SELECT * FROM firebird_scan('${FIREBIRD_TEST_DB}', 'TNO_INDEX',
                            row_limit=1, row_offset=0);

statement ok
CREATE OR REPLACE TABLE t_page2 AS
SELECT * FROM firebird_scan('${FIREBIRD_TEST_DB}', 'TNO_INDEX',
                            row_limit=1, row_offset=1);

query I
SELECT COUNT(*) FROM t_page1 INTERSECT SELECT * FROM t_page2;
----
0

# Case 3: simple view (V_ACTIVE_EMP), no PK exposed through the view scan
# path -> ORDER BY RDB$DB_KEY must appear via firebird_explain_pushdown.

query T
SELECT remote_sql LIKE '%ORDER BY RDB$DB_KEY%'
   AND remote_sql LIKE '%ROWS%'
  FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.V_ACTIVE_EMP'
  ) LIMIT 1;
----
NULL

# Case 4a: heavy view (V_DEPT_HEADCOUNT) — no ROWS clause should ever
# appear in remote_sql for this target, regardless of any limit context.

query T
SELECT remote_sql NOT LIKE '%ROWS%' FROM firebird_explain_pushdown(
    'SELECT * FROM fb.main.V_DEPT_HEADCOUNT') LIMIT 1;
----
true

# Case 4b (CRITICAL correctness): row_limit/row_offset against the heavy
# view via the actual scanner (not explain_pushdown) — the local-slice
# fallback must bound the result to exactly N rows, never "everything".
# V_DEPT_HEADCOUNT groups EMPLOYEE by DEPT_NO; the fixture has 3 distinct
# DEPT_NO values (600, 700, 900) among active+inactive rows combined in
# the view's own definition, so row_limit=2 must return exactly 2 rows,
# not all of them.

statement ok
CREATE OR REPLACE TABLE t_heavy_sliced AS
SELECT * FROM firebird_scan('${FIREBIRD_TEST_DB}', 'V_DEPT_HEADCOUNT',
                            row_limit=2);

query I
SELECT COUNT(*) FROM t_heavy_sliced;
----
2
```

Before finalizing this step, the implementer MUST verify `V_DEPT_HEADCOUNT`'s actual row count in the fixture (via a plain `SELECT COUNT(*) FROM fb.main.V_DEPT_HEADCOUNT` or equivalent) — if it does not have at least 3 distinct groups, adjust `row_limit` to a number strictly less than the view's real row count so the assertion is a genuine slice, not a coincidental full-return. Do not weaken this test to `<=` — it must prove the count is bounded to exactly the requested number.

Also verify `V_ACTIVE_EMP` case 3's exact expected value once `firebird_explain_pushdown` actually supports inspecting a bare (no named-param limit) query's hypothetical pagination — if `firebird_explain_pushdown` only reports `remote_sql` for the query AS WRITTEN (no `row_limit=`/`row_offset=` in the SQL text, so no `ROWS` at all appears for a plain `SELECT * FROM fb.main.V_ACTIVE_EMP`), replace Case 3 with a query that DOES carry a named-parameter equivalent reachable through the ATTACH path, or drop the `explain_pushdown`-based Case 3 assertion and instead prove case 3 via a direct `firebird_scan(..., row_limit=1)` against `V_ACTIVE_EMP` returning the expected 1 row with no error — whichever the existing test file's established idiom supports. Flag this adjustment explicitly in the implementer report; it depends on exact runtime behavior not fully knowable from static reading alone.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_paging.test"`

Expected: FAIL — no `ORDER BY` is emitted today, `TNO_INDEX`'s two pages are not guaranteed disjoint, and `V_DEPT_HEADCOUNT` with `row_limit=2` returns every row (the exact regression this task fixes).

- [ ] **Step 3: `FirebirdQueryBuilder::Build` gains `pagination_order_by`**

In `src/include/firebird_query.hpp`, change:

```cpp
    static Result Build(const std::string &table_name,
                        const std::vector<std::string> &all_column_names,
                        const std::vector<LogicalType>  &all_column_types,
                        const std::vector<column_t>     &column_ids,
                        optional_ptr<TableFilterSet>     filters,
                        optional_idx                     limit,
                        const std::string               &extra_predicate = {},
                        const std::vector<FirebirdColumnDesc> *column_descs = nullptr,
                        NoneEncoding                     none_encoding = NoneEncoding::WIN1252,
                        optional_idx                     offset = optional_idx());
```

to:

```cpp
    static Result Build(const std::string &table_name,
                        const std::vector<std::string> &all_column_names,
                        const std::vector<LogicalType>  &all_column_types,
                        const std::vector<column_t>     &column_ids,
                        optional_ptr<TableFilterSet>     filters,
                        optional_idx                     limit,
                        const std::string               &extra_predicate = {},
                        const std::vector<FirebirdColumnDesc> *column_descs = nullptr,
                        NoneEncoding                     none_encoding = NoneEncoding::WIN1252,
                        optional_idx                     offset = optional_idx(),
                        // Server-side ORDER BY column emitted alongside a
                        // ROWS clause, required whenever `limit` is valid.
                        // A ROWS clause without ORDER BY is non-deterministic
                        // across pages. Callers that determined no safe
                        // order exists must pass an invalid `limit` instead
                        // of an empty string here (see firebird_scanner.cpp's
                        // pagination decision order).
                        const std::string               &pagination_order_by = {});
```

In `src/firebird_query.cpp`, update the definition's parameter list to match (add the same new parameter at the end), and replace the `--- LIMIT / OFFSET pushdown ---` block:

```cpp
FirebirdQueryBuilder::Result FirebirdQueryBuilder::Build(
    const std::string &table_name,
    const std::vector<std::string> &all_column_names,
    const std::vector<LogicalType>  &/*all_column_types*/,
    const std::vector<column_t>     &column_ids,
    optional_ptr<TableFilterSet>     filters,
    optional_idx                     limit,
    const std::string               &extra_predicate,
    const std::vector<FirebirdColumnDesc> *column_descs,
    NoneEncoding                     none_encoding,
    optional_idx                     offset) {
```

to:

```cpp
FirebirdQueryBuilder::Result FirebirdQueryBuilder::Build(
    const std::string &table_name,
    const std::vector<std::string> &all_column_names,
    const std::vector<LogicalType>  &/*all_column_types*/,
    const std::vector<column_t>     &column_ids,
    optional_ptr<TableFilterSet>     filters,
    optional_idx                     limit,
    const std::string               &extra_predicate,
    const std::vector<FirebirdColumnDesc> *column_descs,
    NoneEncoding                     none_encoding,
    optional_idx                     offset,
    const std::string               &pagination_order_by) {
```

and replace:

```cpp
    // --- LIMIT / OFFSET pushdown --------------------------------------------
    // Firebird >= 1.5 uses `ROWS N` for top-N and `ROWS m TO n` (1-based,
    // inclusive) for offset+limit. We never emit offset without limit —
    // FirebirdScanBind rejects that combination at bind time so the caller
    // sees an actionable error instead of an over-fetch.
    if (limit.IsValid()) {
        if (offset.IsValid()) {
            const idx_t start = offset.GetIndex() + 1;       // 1-based
            const idx_t end   = offset.GetIndex() + limit.GetIndex();
            sql << " ROWS " << start << " TO " << end;
        } else {
            sql << " ROWS " << limit.GetIndex();
        }
    }
```

with:

```cpp
    // --- LIMIT / OFFSET pushdown --------------------------------------------
    // Firebird >= 1.5 uses `ROWS N` for top-N and `ROWS m TO n` (1-based,
    // inclusive) for offset+limit. We never emit offset without limit —
    // FirebirdScanBind rejects that combination at bind time so the caller
    // sees an actionable error instead of an over-fetch.
    //
    // A ROWS clause is only deterministic across pages when paired with a
    // server-side ORDER BY. The caller (firebird_scanner.cpp) is
    // responsible for determining a safe order column BEFORE calling
    // Build with a valid `limit` — pagination_order_by must be non-empty
    // whenever `limit` is valid. If no safe order exists, the caller must
    // not set `limit` at all here (it applies the slice locally instead).
    if (limit.IsValid()) {
        D_ASSERT(!pagination_order_by.empty());
        sql << " ORDER BY " << pagination_order_by;
        if (offset.IsValid()) {
            const idx_t start = offset.GetIndex() + 1;       // 1-based
            const idx_t end   = offset.GetIndex() + limit.GetIndex();
            sql << " ROWS " << start << " TO " << end;
        } else {
            sql << " ROWS " << limit.GetIndex();
        }
    }
```

- [ ] **Step 4: `FirebirdBindData` gains view-shape fields**

In `src/include/firebird_scanner.hpp`, add these 2 fields to `FirebirdBindData` right after the `bool db_charset_none = false;` line (the last field before the closing `};`):

```cpp
    // View-shape signals for the ROWS-pagination safety decision (see
    // firebird_scanner.cpp's OpenNextPartitionCursor). Populated at bind
    // time only when the target has no usable single-column numeric PK —
    // computing this for every scan would be wasted I/O when the PK path
    // already gives a safe, cheap ORDER BY column.
    bool is_view = false;
    // Meaningful only when is_view is true: no JOIN / GROUP BY / aggregate
    // detected AND the source was inspectable. A simple pass-through view
    // safely inherits its base table's RDB$DB_KEY (verified empirically);
    // a heavy or uninspectable view does not (RDB$DB_KEY silently returns
    // SQL NULL for a self-JOIN + GROUP BY view — not an error).
    bool is_view_simple_for_pagination = false;
```

- [ ] **Step 5: `FirebirdScanBind` probes the PK for paging too, and computes the view shape when needed**

In `src/firebird_scanner.cpp`, add the include after the existing `#include "firebird_query.hpp"` line:

```cpp
#include "firebird_view_analysis.hpp" // LookupObjectType, ViewAnalysis, AnalyzeViewSource
```

**Critical prerequisite fix, easy to miss:** `paging` (the local `bool` computed a few lines earlier in this same function, from `bind->limit_override.IsValid() || bind->offset_override.IsValid()`) unconditionally forces `bind->partitions_override = 1`. The EXISTING PK-probe guard below is `if (bind->partitions_override != 1)` — which means **the PK probe is currently skipped for every paginated scan today, even when the table has a perfectly good single-column numeric PK** (e.g. `EMPLOYEE`). Case 1 of the pagination decision order (PK-based `ORDER BY`) can never fire unless the probe also runs when paging is active. This is a deliberate, small, one-time cost: a paginated request is a one-off/interactive query, not a hot loop, so paying the extra round trip here is acceptable and necessary to honor the PK-first decision order.

Find this block in `FirebirdScanBind` (currently lines 534-546):

```cpp
    FirebirdConnection conn(bind->conn_info);
    bind->db_charset_none = DatabaseCharsetIsNone(conn);
    LoadTableSchema(conn, bind->table_name,
                    bind->column_names, bind->column_types, bind->column_descs,
                    bind->none_encoding);

    // PK probe is only worth its three RDB$ round-trips if we might actually
    // parallelize. If the caller forced partitions=1 we skip it entirely;
    // also keeps interactive small-table queries fast on remote servers.
    if (bind->partitions_override != 1) {
        bind->pk = ProbePrimaryKey(conn, bind->table_name,
                                   bind->column_names, bind->column_types);
    }

    return_types = bind->column_types;
    names        = bind->column_names;
    return std::move(bind);
```

Replace with:

```cpp
    FirebirdConnection conn(bind->conn_info);
    bind->db_charset_none = DatabaseCharsetIsNone(conn);
    LoadTableSchema(conn, bind->table_name,
                    bind->column_names, bind->column_types, bind->column_descs,
                    bind->none_encoding);

    // PK probe is only worth its three RDB$ round-trips if we might actually
    // parallelize, OR if we're paging and need a safe, cheap ORDER BY
    // column for ROWS pushdown (Smart Scan Planning Report's pagination
    // decision order prefers the PK over RDB$DB_KEY when one exists). A
    // paginated request is typically a one-off/interactive query, not a
    // hot loop, so the extra round trip is a deliberate, acceptable cost
    // here — `paging` is the same local computed above from
    // limit_override/offset_override.
    if (bind->partitions_override != 1 || paging) {
        bind->pk = ProbePrimaryKey(conn, bind->table_name,
                                   bind->column_names, bind->column_types);
    }

    // View-shape lookup for the ROWS-pagination safety decision. Only
    // worth the extra round trip when there is no single-column numeric
    // PK to fall back on for ordering — that path is already safe and
    // cheap, so we don't pay this cost for the common case.
    if (!bind->pk) {
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

- [ ] **Step 6: `OpenNextPartitionCursor` computes the pagination decision and the local-slice flag**

In `src/firebird_scanner.cpp`, find the `Build` call in `OpenNextPartitionCursor` (currently lines 696-706):

```cpp
    auto query = FirebirdQueryBuilder::Build(
        bind.table_name,
        bind.column_names,
        bind.column_types,
        gstate.column_ids,
        gstate.filters,
        bind.limit_override,
        combined,
        &bind.column_descs,
        bind.none_encoding,
        bind.offset_override);
```

Replace with:

```cpp
    // Pagination decision order (Smart Scan Planning Report): a ROWS
    // clause is only pushed when a safe, deterministic server-side
    // ORDER BY is available. `local.needs_local_slice` tells the fetch
    // loop below whether it must enforce row_limit/row_offset itself
    // because Build() below will NOT push ROWS for this partition.
    std::string pagination_order_by;
    optional_idx limit_to_push    = bind.limit_override;
    optional_idx offset_to_push   = bind.offset_override;
    local.needs_local_slice = false;
    if (bind.limit_override.IsValid()) {
        if (bind.pk) {
            // Case 1: single-column numeric PK.
            pagination_order_by = QuoteIdent(bind.pk->column) + " ASC";
        } else if (!bind.is_view) {
            // Case 2: base table, no such PK.
            pagination_order_by = "RDB$DB_KEY";
        } else if (bind.is_view_simple_for_pagination) {
            // Case 3: simple view, no such PK.
            pagination_order_by = "RDB$DB_KEY";
        } else {
            // Case 4: heavy view or uninspectable source — RDB$DB_KEY is
            // not safe here (a self-JOIN + GROUP BY view returns it as
            // SQL NULL, silently, not an error). Do not push ROWS; the
            // fetch loop enforces the slice locally instead.
            limit_to_push  = optional_idx();
            offset_to_push = optional_idx();
            local.needs_local_slice = true;
            local.slice_limit  = bind.limit_override;
            local.slice_offset = bind.offset_override.IsValid()
                ? bind.offset_override
                : optional_idx(static_cast<idx_t>(0));
        }
    }

    auto query = FirebirdQueryBuilder::Build(
        bind.table_name,
        bind.column_names,
        bind.column_types,
        gstate.column_ids,
        gstate.filters,
        limit_to_push,
        combined,
        &bind.column_descs,
        bind.none_encoding,
        offset_to_push,
        pagination_order_by);
```

- [ ] **Step 7: `FirebirdLocalState` gains the local-slice counters**

In `src/firebird_scanner.cpp`, find `struct FirebirdLocalState` (currently starting at line 78) and add these fields right after `bool connection_reused = false;`:

```cpp
    // Local-slice fallback (Smart Scan Planning Report, case 4): set by
    // OpenNextPartitionCursor when no safe ORDER BY exists for a ROWS
    // push, so this local state must enforce row_limit/row_offset itself
    // by skipping/capping rows as they're fetched. Inert (both false/
    // unset) for every other case — zero behavior change to the existing
    // fast path.
    bool needs_local_slice = false;
    optional_idx slice_limit;
    optional_idx slice_offset;
    idx_t rows_skipped = 0;
    idx_t rows_emitted  = 0;
```

- [ ] **Step 8: `FirebirdScanFunction`'s fetch loop enforces the local slice**

In `src/firebird_scanner.cpp`, find the fetch loop in `FirebirdScanFunction` (currently lines 876-895):

```cpp
        while (row < target) {
            if (!local.cursor) {
                if (!OpenNextPartitionCursor(ctx, data, local)) break;
            }
            auto t0 = std::chrono::steady_clock::now();
            const bool got = local.cursor->Fetch();
            auto t1 = std::chrono::steady_clock::now();
            fetch_us += std::chrono::duration_cast<std::chrono::microseconds>(
                            t1 - t0).count();
            if (!got) {
                local.cursor.reset();
                continue;
            }
            for (idx_t c = 0; c < n_fetch_cols; ++c) {
                FirebirdAppendValue(*local.cursor, c,
                                    local.fetch_chunk.data[c], row,
                                    bind.none_encoding);
            }
            ++row;
        }
```

Replace with:

```cpp
        while (row < target) {
            if (!local.cursor) {
                if (!OpenNextPartitionCursor(ctx, data, local)) break;
            }
            auto t0 = std::chrono::steady_clock::now();
            const bool got = local.cursor->Fetch();
            auto t1 = std::chrono::steady_clock::now();
            fetch_us += std::chrono::duration_cast<std::chrono::microseconds>(
                            t1 - t0).count();
            if (!got) {
                local.cursor.reset();
                continue;
            }
            // Local-slice fallback (case 4 of the pagination decision
            // order): no ROWS clause was pushed for this scan, so
            // row_limit/row_offset must be enforced here instead of
            // silently returning every row.
            if (local.needs_local_slice) {
                if (local.slice_offset.IsValid() &&
                    local.rows_skipped < local.slice_offset.GetIndex()) {
                    ++local.rows_skipped;
                    continue;
                }
                if (local.slice_limit.IsValid() &&
                    local.rows_emitted >= local.slice_limit.GetIndex()) {
                    local.cursor.reset();
                    break;
                }
            }
            for (idx_t c = 0; c < n_fetch_cols; ++c) {
                FirebirdAppendValue(*local.cursor, c,
                                    local.fetch_chunk.data[c], row,
                                    bind.none_encoding);
            }
            ++row;
            if (local.needs_local_slice) {
                ++local.rows_emitted;
            }
        }
```

- [ ] **Step 9: Fix `firebird_explain_pushdown.cpp`'s own `Build()` call site**

In `src/firebird_explain_pushdown.cpp`, `WalkPlan`'s capture-only `Build` call (currently around lines 292-302) does not push any real ROWS clause without a query-supplied `row_limit=`/`row_offset=`, but the signature now has a new trailing parameter — the call site compiles as-is (the new parameter defaults to `{}` in the header when omitted), so **no code change is required here**. Confirm this by building (Step 10) rather than editing — if the build fails on this call site, add `, /*pagination_order_by=*/""` as its last argument (it is always empty here since this file's `Build` call never sets a real `limit` that would trigger the `D_ASSERT` — `real_limit`/`real_offset` reflect only `bd.limit_override`/`bd.offset_override`, and when those ARE valid, the assertion would fire if this call site doesn't also pass an order column; if that happens, defer to Task 3's already-emitted `remote_sql` semantics and pass `"RDB$DB_KEY"` here as a display-only approximation, noting in the report that `explain_pushdown`'s own Build call is capture-only and never executes against Firebird, so an assertion failure here — if it occurs — must be fixed by threading the same decision-order logic into `WalkPlan`, not by suppressing the assert).

- [ ] **Step 10: Build and run the tests**

Run: `scripts\build_windows_local.bat`

Set env (PowerShell): `$env:FIREBIRD_TEST_DB="C:\fbtest\test.fdb"; $env:ISC_USER="SYSDBA"; $env:ISC_PASSWORD="masterkey"`

Run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_paging.test"`

Expected: `All tests passed (N assertions)`, including the CRITICAL Case 4b assertion (`row_limit=2` against the heavy view returns exactly 2 rows).

Also run: `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_explain_pushdown.test"` and `build\release\test\unittest.exe "C:/tmp/fbwt-ssp/test/sql/firebird_scan.test"` (regression check — this task touches the core scan/fetch loop; confirm no existing scan behavior broke).

- [ ] **Step 11: Commit**

```bash
git add src/include/firebird_query.hpp src/firebird_query.cpp src/include/firebird_scanner.hpp src/firebird_scanner.cpp src/firebird_explain_pushdown.cpp test/sql/firebird_paging.test
git commit -m "fix(scan-planner): ROWS pagination always pairs with a safe ORDER BY

Firebird's ROWS/ROWS-m-TO-n pushdown was never paired with a
server-side ORDER BY, making multi-page reads of the same query
non-deterministic. Decision order: single-column numeric PK -> that
column; base table or simple view with no such PK -> RDB\$DB_KEY
(verified empirically stable within one scan); heavy/uninspectable
view -> no ROWS pushed, sliced locally instead.

RDB\$DB_KEY returns silent SQL NULL (not an error) on a self-JOIN +
GROUP BY view, ruling out a runtime probe -- the decision is static,
made at bind time from the view's shape.

The scanner's OWN row_limit/row_offset named parameters are the only
thing enforcing that slice when ROWS can't be pushed (a plain SQL
LIMIT is unaffected -- DuckDB already trims it client-side regardless).
Local slicing in the fetch loop preserves that contract exactly:
firebird_scan(..., row_limit=N) against a heavy view now returns
exactly N rows instead of silently returning everything."
```

---

### Task 5: Documentation (PT/EN)

**Files:**
- Modify: `docs/pt/function_manual.md`
- Modify: `docs/en/function_manual.md`

**Interfaces:**
- Consumes: the 4 new `firebird_explain_pushdown` columns (Task 3), the shared `PickPartitionCount` (Task 1).
- Produces: nothing (docs only).

- [ ] **Step 1: Document in the EN manual**

In `docs/en/function_manual.md`, find the `firebird_explain_pushdown` section. Add the 4 new columns to its column table (`view_heavy` BOOLEAN nullable, `view_heavy_reasons` LIST(VARCHAR), `charset_pushdown_blocked` BOOLEAN, `planned_partitions` BIGINT nullable), then add these notes:

- `planned_partitions` is the exact value `firebird_scan` would use for this table's PK range — computed by the same `PickPartitionCount` function, not a separate estimate.
- `charset_pushdown_blocked` is a boolean synthesis of `not_pushed_reasons` containing `'NONE_CHARSET'`.
- `view_heavy`/`view_heavy_reasons`: when the scan target is a view with a JOIN, `GROUP BY`, aggregate function, or an unreadable definition, `view_heavy=true` and `view_heavy_reasons` explains why. This is also why `remote_sql` may show no `ROWS` clause even when `row_limit`/`row_offset` were requested against that view — see the `RDB$DB_KEY` note below.
- **`RDB$DB_KEY` semantics**: when `firebird_scan` paginates a table/view with no usable single-column numeric primary key, it orders by `RDB$DB_KEY` — a physical row-pagination stabilizer valid only for the duration of one scan/transaction. It is NOT a business ordering contract, NOT stable across transactions or after row updates, and is NEVER used for range partitioning, incremental/watermark logic, or as a recommended key elsewhere in this extension. For a heavy view (JOIN/aggregate) or a view whose definition could not be read, `RDB$DB_KEY` is unsafe (Firebird silently returns it as `NULL` rather than erroring) — `firebird_scan` does not push `ROWS` in that case and instead applies `row_limit`/`row_offset` locally, so the requested slice is still honored correctly, just without the server-side pushdown optimization.

In the `firebird_profile_table` section, add a short note that `recommended_partitions` is computed by the same `PickPartitionCount` function the scanner itself uses at scan time — the two can no longer disagree.

- [ ] **Step 2: Document in the PT manual (parity)**

In `docs/pt/function_manual.md`, mirror Step 1 in both the `firebird_explain_pushdown` and `firebird_profile_table` sections — the same additions, translated, following that file's existing heading layout (PT uses H4 sub-headings, per `docs/DOCS_PARITY.md`).

- [ ] **Step 3: Commit**

```bash
git add docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(scan-planner): document explain_pushdown's new columns + RDB\$DB_KEY semantics"
```

---

## Self-Review

**Spec coverage:**
- Single source of truth for `PickPartitionCount` → Task 1. ✔
- Extracted shared view-shape analysis → Task 2. ✔
- 4 new `firebird_explain_pushdown` columns, additive → Task 3. ✔
- `ROWS`-without-`ORDER BY` fix with the 4-case decision order → Task 4 Steps 3-8. ✔
- `RDB$DB_KEY` semantics (physical stabilizer only, never partitioning/incremental/watermark) → Task 4 code comments + Task 5 docs. ✔
- No runtime probe-and-catch for the heavy-view case (static bind-time decision) → Task 4 Step 5 (`FirebirdScanBind`), reusing the shared `AnalyzeViewSource` signal. ✔
- **Mandatory invariant** (`row_limit`/`row_offset` never over-returns even when `ROWS` is disabled) → Task 4 Steps 6-8 (local-slice fallback) + Step 1's Case 4b critical test. ✔
- Plain SQL `LIMIT` stays DuckDB's own responsibility, unaffected → confirmed via research (no `limit_pushdown` registered), no code change needed for that path, documented in Task 4's commit message and Task 5 docs.
- Reworded `pk_range_small_serial` message while preserving the `firebird_profile_table.test` substring assertion → Task 1 Step 4/5. ✔

**Placeholder scan:** No TBD/TODO. Task 4 Step 9 and Step 1's fixture-verification notes are deliberate "verify against the live build, adjust if needed" instructions — not vague hand-waving, but explicit fallback guidance for the one place this plan depends on runtime behavior (exact row counts, exact `explain_pushdown` output shape for a bare query) that static reading of the source cannot fully determine. This is called out explicitly rather than guessed.

**Type consistency:** `PickPartitionCount(int64_t, int64_t) -> idx_t` used identically in Task 1 (profile_table) and Task 3 (explain_pushdown). `LookupObjectType`/`ViewAnalysis`/`AnalyzeViewSource` signatures identical across Task 2's extraction, Task 3's consumer, and Task 4's consumer. `FirebirdQueryBuilder::Build`'s new `pagination_order_by` parameter is used consistently at both call sites (Task 4 Step 6 for the scanner, Task 4 Step 9 for explain_pushdown's capture-only path). `FirebirdBindData.is_view`/`is_view_simple_for_pagination` set in Task 4 Step 5, read in Task 4 Step 6. `FirebirdLocalState.needs_local_slice`/`slice_limit`/`slice_offset`/`rows_skipped`/`rows_emitted` set in Task 4 Step 6, read in Task 4 Step 8.

**Note for implementer (Task 4):** this is the highest-risk task in this plan — it changes the core scan/fetch loop. Read `test/sql/firebird_scan.test` and confirm it still passes unchanged (Step 10 already calls this out). If `OpenNextPartitionCursor`'s or `FirebirdScanFunction`'s current line numbers have drifted from what this plan cites (e.g. due to unrelated changes since this plan was written), locate the exact functions by name/signature rather than assuming the line numbers are still accurate — the code blocks given here are the ground truth for content, not the line numbers.
