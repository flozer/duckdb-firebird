# Smart Scan Planning Report — Design (v1.0 Production Bridge, item 1)

Date: 2026-07-02
Status: approved
Branch: `feat/v1.0-smart-scan-planner`

## Purpose

Roadmap v1.0's "Smart Scan Planner" mixes two different kinds of claims:
decision/action verbs ("decidir serial vs PK-range", "evitar paralelismo",
"bloquear pushdown por charset") that are **already real scanner behavior
today**, and reporting verbs ("alertar view pesada", "alertar ROWS sem
ORDER BY") for signals that are **currently invisible at scan time**. This
item does NOT redesign the scanner's parallelism/charset-gating decisions —
they are already correct and tested. It does three things:

1. **Consolidates existing signals** into `firebird_explain_pushdown`
   (extended, not a new function) so a single query-shaped call reports
   view-heaviness, charset-pushdown-blocking, and the *real* planned
   partition count together.
2. **Eliminates a real divergence**: `firebird_profile_table`'s
   `recommended_partitions` and the scanner's actual `PickPartitionCount`
   are two independent, differently-calibrated formulas today. After this
   change there is exactly one.
3. **Fixes a latent determinism bug**: the scanner pushes Firebird
   `ROWS n` / `ROWS m TO n` pagination without ever emitting a server-side
   `ORDER BY`, which makes multi-page reads of the same query
   non-deterministic. This is treated as a correctness fix, not an
   advisory.

## Why extend `firebird_explain_pushdown` (not a new `firebird_scan_plan`)

`explain_pushdown('SELECT ... FROM fb.main.T ...')` already takes a full
query, already resolves the real `LogicalGet` target, and already runs
`ProbePrimaryKey` for `pk_range_eligible`/`scan_strategy`. The missing
signals (view shape, charset block, real partition count) need exactly that
same context. A table-only `firebird_scan_plan('fb.main.T')` would have no
visibility into the actual query's clauses and would just duplicate
`firebird_profile_table`'s existing advisory columns with a weaker signal.
Extending is additive; no existing column changes name, type, or meaning.

## 1. Single source of truth: `PickPartitionCount`

`PickPartitionCount(min_v, max_v) -> idx_t` (`firebird_scanner.cpp:584`) is
already pure and numeric — it only touches its two `int64_t` arguments,
`std::thread::hardware_concurrency()`, and the file-local constant
`MIN_ROWS_PER_PARTITION = 2'000'000`. No `FirebirdConnection`, no cursor, no
lock. Changes:

- Remove `static`; declare it in `firebird_scanner.hpp` so it is callable
  from any translation unit that already includes that header
  (`firebird_explain_pushdown.cpp` already does).
- `firebird_profile_table.cpp:606-616`'s own ladder (`span>=1000000→8`,
  `>=100000→4`, `>=10000→2`, else `1` — a *different*, uncalibrated formula)
  is deleted. `BuildProfile` calls `PickPartitionCount(pk->min_value,
  pk->max_value)` instead. `recommended_partitions` becomes byte-for-byte
  what the scanner will actually do — not a separate estimate.
- **Documented behavior change**: the profile's advisory warnings currently
  describe the old ladder's thresholds in prose (e.g. "PK MIN/MAX span <
  10000"). Those messages must be reworded to describe the real rule
  (`MIN_ROWS_PER_PARTITION = 2,000,000`, hardware-concurrency-capped)
  instead of silently keeping stale numbers in the text while the logic
  underneath changed.

## 2. Single source of truth: view-shape analysis

`ViewAnalysis`/`AnalyzeViewSource` (`firebird_profile_table.cpp:183-219`,
~140 lines including its tokenizer) and the small `LookupObjectType`
(`firebird_profile_table.cpp:146-160`) are extracted to a new file pair:

- `src/firebird_view_analysis.hpp` / `src/firebird_view_analysis.cpp`
- Exports: `bool LookupObjectType(FirebirdConnection&, const std::string
  &upper_table, std::string &out_type)` and `ViewAnalysis
  AnalyzeViewSource(FirebirdConnection&, const std::string &upper_table)`
  (unchanged signatures, just relocated). Its `SqlLiteral` dependency moves
  with it (or is duplicated locally in the new file, following this
  codebase's small-helper-duplication convention for that one function —
  implementer's call, either is fine since `SqlLiteral` is 10 lines).
- `firebird_profile_table.cpp` calls the shared version instead of its own
  copy (deletes its local `LookupObjectType`/`AnalyzeViewSource`/
  `ViewAnalysis` definitions, keeps using the same call sites).
- `firebird_explain_pushdown.cpp` and the scanner (`firebird_scanner.cpp`,
  see §4 below) both gain the ability to call this for the first time.

This is a real single-responsibility extraction (not duplication) because
the logic is substantial; duplicating a 140-line tokenizer across three
files would be a maintenance hazard the small-helper-duplication convention
was never meant to cover.

## 3. New `firebird_explain_pushdown` columns (additive, 14 → 18)

| column | type | source |
|---|---|---|
| `view_heavy` | BOOLEAN, nullable | `true`/`false` when the scan target is a view (via the shared `LookupObjectType`); `NULL` when the target is a base table |
| `view_heavy_reasons` | LIST(VARCHAR) | the shared `AnalyzeViewSource`'s reasons (join / aggregation / no-WHERE / source-not-inspectable) when `view_heavy=true`; also carries a specific reason when ROWS pushdown was skipped for this reason (see §4) — `[]` when `view_heavy` is `false` or `NULL` |
| `charset_pushdown_blocked` | BOOLEAN | `true` when any entry in the existing `not_pushed_reasons` equals `'NONE_CHARSET'` — pure synthesis of an already-computed signal, no new detection logic |
| `planned_partitions` | INTEGER, nullable | `PickPartitionCount(pk_min, pk_max)` when `pk_range_eligible = true` (the exact value the scanner will use); `NULL` when not eligible |

No existing column (`scan_ordinal`, `table_name`, `remote_sql`,
`projected_columns`, `pushed_filters`, `residual_filters`,
`not_pushed_reasons`, `limit_pushed`, `offset_pushed`, `rows_clause`,
`pk_range_eligible`, `pk_range_column`, `pk_range_reason`,
`scan_strategy`) changes name, type, or meaning.

## 4. ROWS-without-ORDER-BY fix (correctness, not advisory)

`FirebirdQueryBuilder::Build` (`firebird_query.cpp:243-401`) emits
`ROWS n` / `ROWS m TO n` (`:389-397`) with no `ORDER BY`. Its sole caller,
`OpenNextPartitionCursor` (`firebird_scanner.cpp:696-704`), already has
`bind.pk` in scope. Decision order, evaluated once at scan time per
partition cursor:

1. **Single-column numeric PK available** (`bind.pk` is non-null — the same
   narrow case `ProbePrimaryKey` already produces for range partitioning):
   `ORDER BY <pk_col> ASC`.
2. **No such PK, target is a base table**: `ORDER BY RDB$DB_KEY`.
3. **No such PK, target is a *simple* view** (via the shared
   `AnalyzeViewSource`: `inspected=true` AND no join/group-by/aggregate):
   `ORDER BY RDB$DB_KEY` — verified empirically that a single-table
   pass-through view inherits the base table's `RDB$DB_KEY` (tested against
   `V_ACTIVE_EMP`/`V_ALL_EMP`).
4. **No such PK, target is a *heavy* view** (join/aggregate present, or the
   source could not be inspected): **do not push `ROWS` to Firebird** for
   this scan.

### Critical correctness distinction: two different limit mechanisms

`firebird_scan` does **not** register `limit_pushdown` with DuckDB
(confirmed: `firebird_scanner.cpp:1510-1527` sets `projection_pushdown` /
`filter_pushdown` / `supports_pushdown_type`, but no limit-pushdown field).
This means there are two independent mechanisms, and case 4 must treat them
differently:

- **A plain SQL `LIMIT`/`OFFSET` clause** (`SELECT * FROM
  firebird_scan(...) LIMIT 10`) is never pushed into the scanner at all
  today — DuckDB's own `LIMIT` operator trims rows *after* the table
  function returns them, unconditionally, regardless of this change. Safe,
  unaffected either way.
- **The scanner's own named parameters** `row_limit`/`row_offset`
  (`firebird_scanner.cpp:456-464,1504-1505`) are the ONLY way
  `bind.limit_override`/`bind.offset_override` get populated, and today
  the ONLY mechanism enforcing that slice is the `ROWS`/`ROWS m TO n`
  clause pushed to Firebird (confirmed: `paging` forces `partitions=1`,
  §4's `Build()` call is the sole consumer of `limit_override`/
  `offset_override`, `firebird_scanner.cpp:502-517,696-706`). **If case 4
  simply stops emitting `ROWS`, `firebird_scan(..., row_limit:=10)`
  against a heavy view would silently return every row instead of 10 — a
  correctness regression, not a performance one.**

**Resolution (Fernando's explicit preference): local slicing in the
scanner.** When case 4 applies AND `row_limit`/`row_offset` were requested,
`Build()` is called with `limit_override`/`offset_override` cleared (no
`ROWS` clause emitted — the full result streams back), and
`FirebirdScanFunction`'s fetch loop (`firebird_scanner.cpp:876-895`)
performs the slice itself:

- A new bind-time flag (e.g. `bind.local_slice_required`) is set only for
  this case (heavy view + no safe order + paging requested).
- Two new `FirebirdLocalState` counters (e.g. `rows_skipped`,
  `rows_emitted`), initialized to 0, persist across
  `FirebirdScanFunction` calls for the life of the scan (it is already a
  `LocalTableFunctionState`, `firebird_scanner.cpp:78-107`).
- In the fetch loop, right after `local.cursor->Fetch()` succeeds and
  before appending the row into `local.fetch_chunk`: if
  `rows_skipped < offset_override`, increment `rows_skipped` and skip this
  row (do not append, do not advance `row`); else if
  `rows_emitted >= limit_override`, stop pulling entirely (`local.cursor
  .reset(); break;` out of the fetch loop — this scan is done); else
  append normally and increment `rows_emitted`.
- When `local_slice_required` is false (every other case), this logic is
  fully inert — zero behavior change to the existing fast path.

Read-only, correct, and only less efficient than server-side `ROWS` in the
one narrow case (paging against a heavy, unordered view) where server-side
pagination was never safe to begin with.

Case 4 is a **static, bind-time** decision, not a runtime probe-and-catch:
empirically, `SELECT RDB$DB_KEY FROM V_DEPT_HEADCOUNT` (self-JOIN +
`GROUP BY`) does **not error** — it returns `RDB$DB_KEY` as SQL `NULL` for
every row. A runtime try/catch would never see a failure to react to; the
non-determinism would be silent. The decision must therefore be made ahead
of execution using the same view-shape signal §2 already extracts.

Composite or non-numeric single-column PKs fall into cases 2/3 (use
`RDB$DB_KEY`), not case 1. This is a deliberate, always-correct-but-
occasionally-suboptimal scope choice: it reuses the PK information the
scanner already computes for partitioning, rather than adding a second,
broader PK-discovery path just for ordering. Documented as a known scope
choice, not a silent gap.

**This requires the scanner to gain view-shape awareness at bind time for
the first time** (today it treats every relation as an opaque, uniform
scan target — `LookupObjectType`/`AnalyzeViewSource` are unreachable from
`FirebirdScanBind`). `FirebirdScanBind` gains a call to the shared
`LookupObjectType`, and — only when the target is a view without a usable
PK — a call to `AnalyzeViewSource`, storing `is_view` and `is_view_simple`
(or equivalent) in `FirebirdBindData` for `OpenNextPartitionCursor` to read.

### `RDB$DB_KEY` semantics — documented, not just implemented

- `RDB$DB_KEY` is a **physical pagination stabilizer for the duration of one
  scan/transaction**, not a business ordering contract and not a stable
  identifier across transactions or after row updates/vacuums.
- It is used **only** to make multi-page `ROWS` reads of the *same* query
  return a consistent row order within that scan.
- It must **never** be used for range partitioning, incremental/watermark
  logic, or as a suggested key in any other function's recommendations
  (`firebird_profile_table`'s `watermark_candidates`/`filter_candidates`
  stay untouched — this fix does not feed into those).
- The three notes above are documented in both function manuals.

### Testing

- `test/sql/firebird_paging.test`: existing `EMPLOYEE`-based cases (case 1)
  continue to pass with an `ORDER BY EMP_ID` now appearing in the remote
  SQL surfaced by `firebird_explain_pushdown`/observability (exact
  assertion mechanism is the implementer's call — whatever this test suite
  already uses to inspect pushed SQL).
- New case: a PK-less base table (reuse the existing `TNO_INDEX` fixture
  from the `firebird_index_profile` item — zero index, zero PK) paginated
  across two `ROWS` windows, asserting the same row appears in only one
  page (proves `RDB$DB_KEY` ordering is stable across pages within one
  scan).
- New case: a heavy view (`V_DEPT_HEADCOUNT`, self-JOIN + `GROUP BY`,
  already a fixture) scanned with a `LIMIT` — assert via
  `firebird_explain_pushdown` that **no `ROWS` clause appears in
  `remote_sql`** (i.e. `rows_clause IS NULL` or `remote_sql NOT LIKE
  '%ROWS%'`), and that `view_heavy=true` with `view_heavy_reasons`
  containing an entry that explains why ROWS pushdown was skipped for this
  view (e.g. a reason distinguishing "aggregate/join view: pagination not
  pushed" from the plain heavy-view reasons already produced by
  `AnalyzeViewSource`).
- New case: a simple view (`V_ACTIVE_EMP`) paginated with `ROWS`, asserting
  `remote_sql` DOES contain `ORDER BY RDB$DB_KEY` and a `ROWS` clause (case
  3 exercised).
- **Critical correctness case**: `firebird_scan('fb.main.V_DEPT_HEADCOUNT',
  row_limit := N, row_offset := M)` (the named-parameter path, NOT a plain
  SQL `LIMIT`) against the heavy view, with N/M chosen against the fixture's
  known row count. Assert the returned row count is exactly N (or the
  remainder if fewer rows exist past the offset) — proving the local-slice
  fallback in `FirebirdScanFunction` actually bounds the result, not just
  that `remote_sql` omits `ROWS`. This is the regression this fix exists to
  prevent: without local slicing, this exact call would silently return
  every row in the view instead of N.

## Files

- `src/firebird_scanner.hpp` / `.cpp` — un-`static` `PickPartitionCount`;
  `FirebirdScanBind` gains view-shape lookup (calling the new shared
  header); `FirebirdBindData` gains `is_view`/`is_view_simple`/
  `local_slice_required`-equivalent fields; `FirebirdLocalState` gains
  `rows_skipped`/`rows_emitted` counters; `FirebirdScanFunction`'s fetch
  loop gains the skip/cap logic (§4), inert when
  `local_slice_required=false`.
- `src/firebird_query.hpp` / `.cpp` — `FirebirdQueryBuilder::Build` gains an
  order-by/pagination-eligibility parameter; ROWS emission gated by the
  decision order in §4.
- `src/firebird_view_analysis.hpp` / `.cpp` (new) — extracted
  `LookupObjectType` + `ViewAnalysis`/`AnalyzeViewSource`.
- `src/firebird_profile_table.cpp` — delete local
  `LookupObjectType`/`AnalyzeViewSource`/`ViewAnalysis`/partition ladder;
  call the shared versions; reword the affected warning message(s).
- `src/firebird_explain_pushdown.cpp` / `.hpp` — 4 new columns, calls into
  the shared `PickPartitionCount` and view-analysis header.
- `test/sql/firebird_paging.test` — new PK-less and view cases.
- `test/sql/firebird_explain_pushdown.test` — new column assertions
  (view_heavy, view_heavy_reasons, charset_pushdown_blocked,
  planned_partitions), including the ROWS-not-pushed-for-heavy-view case.
- `test/sql/firebird_profile_table.test` — existing `recommended_partitions`
  assertions re-verified against the shared `PickPartitionCount` (expected
  to be unchanged in value for the existing small-span fixtures; warning
  *text* assertions updated for the reworded message).
- `scripts/build_matrix.ps1`, `.github/workflows/build-linux-fb-matrix.yml`
  — no new test file to wire (existing files extended), matrix already
  covers them.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` — update the
  `firebird_explain_pushdown` section (4 new columns) and add the
  `RDB$DB_KEY` semantics notes; note in `firebird_profile_table`'s section
  that `recommended_partitions` now shares the scanner's real formula.

## Out of scope

- Any change to the parallelism/charset-gating decisions themselves — both
  are already correct.
- A numeric `low_selectivity` threshold anywhere.
- A new `firebird_scan_plan` function.
- Using `RDB$DB_KEY` for anything beyond pagination-order stabilization
  within one scan.
- Any duckdb/community-extensions / upstream action.
