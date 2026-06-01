# v0.6.1 plan — Improve remote Firebird UX and ATTACH performance

Investigation log for PM/HUMAN review. **Benchmark first, code second.** Code
is only written if the benchmark confirms a clear win from a low-risk change.

Status: **validated for release** on branch `perf/remote-firebird-v0.6.1`.

## Motivation

Real-world usage against a remote Firebird server (LAN/WAN) feels far slower
than a local file database. Reported symptom: the first query after
`ATTACH ... (TYPE firebird)` against a large database stalls for seconds.

Observed in the field: RTT to the remote host was ~144 ms (user's own network
infra, not the extension). Even after that infra is fixed to a healthy LAN
RTT (~10–20 ms), we want the first-touch experience to stay snappy.

## Hypothesis (initial)

`ATTACH` of a large remote database is expensive due to eager
catalog/metadata discovery. Investigate lazy catalog loading, caching, or a
reduction in metadata queries.

## Findings (code reading — already conclusive on the mechanism)

`ATTACH` itself is cheap and lazy. Catalog discovery is deferred until the
first `ScanSchemas` / `Scan` / `LookupEntry` call (the first query that
references the attached catalog, `SHOW TABLES`, or a GUI introspection).

The cost lives in `FirebirdSchemaEntry::EnsureTablesLoaded()`
(`src/firebird_storage.cpp:232`):

1. One query lists all user relation names (`RDB$RELATIONS`) — **1 round-trip**.
2. A loop then calls `LoadTableSchema()` **once per table** to read its
   columns from `RDB$RELATION_FIELDS JOIN RDB$FIELDS`
   (`src/firebird_scanner.cpp:149`) — **N round-trips for N tables**.

Two problems:

- **N round-trips.** One metadata query per table. The per-table query is
  acknowledged in the code as a "follow-up optimisation" (batch-fetch)
  — `src/firebird_storage.cpp:262`.
- **Eager all-tables.** Columns are materialised for *every* user table at
  first catalog touch, even tables the session never queries.

Modelled cost (N tables × RTT) — matches the field symptom:

| Tables | local (~0.1 ms) | LAN (20 ms) | reported infra (144 ms) |
| --- | --- | --- | --- |
| 100 | ~10 ms | 2 s | 14 s |
| 500 | ~50 ms | 10 s | 72 s |

This is why local feels instant (N × ~0.1 ms is negligible) and remote
stalls. The bottleneck is **round-trip count on first catalog load**, not
data volume and not the `ATTACH` handshake.

## Benchmark plan (do this first — quantify before coding)

Reproduce against a remote database with a realistic table count (≥100 user
relations) and capture wall-clock for each step. Compare local vs remote LAN.

Measure:

1. `firebird_tables('<dsn>')` — standalone discovery function.
2. `ATTACH ... (TYPE firebird)` — handshake only (expect cheap).
3. First query after `ATTACH` (triggers `EnsureTablesLoaded` — expect the
   spike).
4. Second query after `ATTACH` (catalog warm — expect fast).
5. `firebird_scan()` on a small / medium / large table.
6. `SELECT *` vs explicit column projection.
7. With vs without a `WHERE` that pushes down.

Instrumentation: the observability layer already records per-query timing,
partition count, and pushdown flags (`firebird_query_log()` /
`src/firebird_observability.cpp`). Use it to attribute time to
discovery vs scan vs fetch, and to confirm parallel partitioning kicked in.

Bottleneck attribution checklist:

- [ ] catalog discovery (`EnsureTablesLoaded`) — expected primary suspect
- [ ] per-table column/PK/metadata reads
- [ ] row fetch
- [ ] round-trips vs data volume
- [ ] parallel scan engaged (PK present) vs single-partition fallback

## Measured results — Phase 1 (catalog hypothesis) — CONFIRMED

Environment: DuckDB CLI + released `firebird.duckdb_extension` v0.6.0
(baseline). Remote database on a healthy LAN host; measured RTT
**~3 ms** (1–4 ms). Schema: **2789 user tables, 46030 columns** (legacy ERP).
Driver script: `bench/v0.6.1_phase1_catalog.sql`.

| Step | Run time | Verdict |
| --- | --- | --- |
| `firebird_tables(dsn)` (1 query) | 1.62 s | single discovery query, 2789 rows |
| `ATTACH ... (TYPE firebird)` | 0.000 s | lazy/cheap — **not** the cost |
| First query (cold → `EnsureTablesLoaded`) | **25.26 s** | **the bottleneck** |
| Second query (catalog warm/cached) | 0.115 s | 220× faster — one-time cost |
| `duckdb_columns()` (warm) | 0.122 s | — |

Conclusions:

- The `ATTACH` handshake is effectively free (0.000 s). The hypothesis is
  refined: cost is the **eager per-table column load on first catalog
  touch**, not `ATTACH` itself.
- First catalog touch = **25.26 s** for 2789 tables ≈ **9 ms/table** at 3 ms
  RTT (≈3 round-trips per table: parse + execute + fetch of the per-table
  `RDB$RELATION_FIELDS` query).
- `firebird_tables()` resolves the *same* 2789 tables in **1.62 s** because it
  issues **one** discovery query instead of a per-table loop — direct,
  same-database proof that batching is ~15× faster and that Fix A is viable.
- Projected on the originally reported 144 ms infra:
  2789 × ~3 round-trips × 144 ms ≈ **~20 minutes** of first-query freeze —
  explains why the remote experience was unusable.

Decision: bottleneck confirmed, **Fix A** is low-risk and already proven
viable in-tree → implement for v0.6.1. Projected first-query: 25 s → ~1–2 s.

## Measured results — Phase 1b/1c (high-latency WAN probe) — projection validated

Environment: same CLI + v0.6.0 extension, high-latency remote database;
measured RTT **~144 ms**. Schema: **2871 user tables**.
Driver scripts: `bench/v0.6.1_phase1b_high_latency_probe.sql`,
`bench/v0.6.1_phase1c_rtt_calib.sql`.

Light probe (1b):

| Step | Run time | Verdict |
| --- | --- | --- |
| `firebird_tables(dsn)` (1 query) | 2.45 s | 2871 tables; single query, batched fetch — *not* 48× the LAN figure |
| `ATTACH ... (TYPE firebird)` | 0.000 s | lazy/cheap again |

Round-trip calibration (1c) — did **not** run the full cold load:

| Op | Run time | Reading |
| --- | --- | --- |
| `firebird_scan(RDB$DATABASE)` ×5 (1 row, reconnect each) | ~3.86 s steady | connect + PK probe (3 RDB$ round-trips) + schema + scan |
| `firebird_scan(RDB$RELATIONS)` (2927 rows) | 31.2 s | ⚠ BLOB columns → per-blob `open_blob`/`get_segment` round-trips |

Observations:

- Each Firebird round-trip costs ~144 ms here, and ops chain several
  round-trips — a single standalone scan is ~3.86 s.
- ⚠ **BLOB-heavy fetches are catastrophic at high RTT** (31 s for ~2900 rows
  of `RDB$RELATIONS`). Separate future optimisation candidate (inline BLOB /
  batched segment reads); not part of the catalog fix.
- Per-table cold-load query (`RDB$RELATION_FIELDS`, warm pooled connection,
  scalar columns) ≈ 3 round-trips. LAN: 9 ms/table at 3 ms RTT → at 144 ms
  ≈ 330–430 ms/table.

Refined cold-load projection for the high-latency database:
**2871 × ~330–430 ms ≈ ~16–21 minutes** of first-query freeze.
Calibration confirms the order of magnitude without burning ~20 min on the
full run.

Decision unchanged: **Fix A**. Batch replaces 2871 per-table queries with ~1;
projected first-query at 144 ms drops from ~16–21 min to a few seconds
(`firebird_tables` territory + column fetch). Empirical before/after on this
WAN DB is deferred to **after** Fix A lands — prove the fix, not the problem.

## Candidate fixes (rank after benchmark confirms)

**Fix A — batch the column load (low risk, primary v0.6.1 candidate).**
Replace the per-table `LoadTableSchema` loop with a single query over
`RDB$RELATION_FIELDS JOIN RDB$FIELDS` for all user relations
(`ORDER BY RDB$RELATION_NAME, RDB$FIELD_POSITION`), then partition rows per
relation client-side. Collapses N round-trips → ~1. Same data shape, same
filtering, no behavioural change beyond speed. Already self-identified in the
code as the intended follow-up.

**Fix B — truly lazy per-table column load (larger change, v0.7 candidate).**
Load only relation *names* at first catalog touch; defer a table's column
read to the `LookupEntry` / `GetScanFunction` for that specific table. Makes
the common case (attach + query a handful of tables) pay only for tables
actually referenced. More invasive: changes when the binder sees columns and
must keep `SHOW TABLES` / `information_schema` full scans correct.

These compose: A helps full introspection (GUI / `SHOW TABLES`), B helps
targeted queries. A is the safe incremental win.

## v0.6.1 decision criteria

- If the benchmark confirms the discovery bottleneck and **Fix A** holds up as
  low-risk, implement it for v0.6.1.
- If the right solution needs a large change (**Fix B** or wire-buffer/DPB
  tuning), document findings and open a v0.7 roadmap item.

## Measured results — Phase 2 (Fix A before/after) — VALIDATED

Built by CI (`build-windows.yml`, DuckDB v1.5.3) — all jobs green
(Windows MSVC, Linux x64, Linux FB 3/4/5 matrix). Benchmarked with the
downloaded Windows artifact against a local `duckdb.exe` v1.5.3 (same commit
`14eca11bd9`) — no local build required.

LAN (~3 ms RTT, 2789 tables / 46030 columns):

| Step | v0.6.0 | Fix A | Result |
| --- | --- | --- | --- |
| `firebird_tables()` | 1.62 s | 0.76 s | — |
| `ATTACH` | 0.000 s | 0.002 s | — |
| **first query (cold)** | **25.26 s** | **1.44 s** | **~17.6× faster** |
| second query (warm) | 0.115 s | 0.061 s | — |
| columns materialised | 46030 | 46030 | identical catalog |

WAN (~144 ms RTT, 2871 tables / 47111 columns):

| Step | v0.6.0 | Fix A | Result |
| --- | --- | --- | --- |
| `firebird_tables()` | 2.45 s | 2.58 s | — |
| `ATTACH` | 0.000 s | 0.000 s | — |
| **first query (cold)** | **~16–21 min (projected)** | **9.49 s** | **~100–130× faster** |
| second query (warm) | — | 0.057 s | — |

Conclusions:

- LAN first-query: **25.26 s → 1.44 s**, inside the ~1–3 s acceptance target.
- WAN first-query: from a projected ~16–21 min freeze to **9.49 s** —
  remote ATTACH of a large schema goes from unusable to usable.
- Catalog is identical (same table + column counts), so the speedup is not
  from dropping data.

**Fix A meets the v0.6.1 acceptance criterion and is validated.**

## Implementation status — Fix A (CI-green + benchmarked)

Committed on `perf/remote-firebird-v0.6.1`; draft PR #20. Statically reviewed,
compiled by CI on all platforms, and benchmarked before/after (above). Earlier
caution (no local build / no tool install) is resolved: validation used the
CI-built artifact, so nothing had to be installed locally.

Changes (`src/firebird_scanner.{cpp,hpp}`, `src/firebird_storage.cpp`):

- `MapFirebirdColumn` — extracted the `blr_*` → `SQL_*` normalisation +
  NONE-charset → BLOB logic into one helper, reused by both the per-table and
  batch paths (no duplicated type table).
- `LoadAllTableSchemas` — one
  `RDB$RELATION_FIELDS ⋈ RDB$FIELDS ⋈ RDB$RELATIONS` query, grouped per
  relation client-side (rows ORDER BY relation, field position). Same
  user-relation filter as catalog discovery.
- `EnsureTablesLoaded` — batch path first; on failure falls back to the
  original per-table loop.

Static-review hardening (the fallback must not mask a real fault):

- The batch `catch` captures the original error message instead of
  discarding it.
- The fallback's own discovery query is wrapped: if it also fails (genuine
  connection / credential / permission fault, not a batch-query quirk), both
  errors are re-thrown together.
- If the fallback finds relations but loads **none** of them (systemic
  per-table failure, e.g. no `RDB$RELATION_FIELDS` access), it throws with
  the batch error rather than returning a silent empty catalog.
- Added `#include <utility>` for the `std::pair` return.

Verification checklist (static): signature/ownership ✓, column order/semantics
preserved ✓, CHARACTER SET NONE handling identical ✓, per-table fallback
intact ✓, batch failure does not mask connection/permission errors ✓, no
BLOB/data-fetch path touched ✓. Compilation + runtime: **pending build**.

Acceptance: first query on a large LAN schema must drop from ~25 s to roughly
`firebird_tables()` + column-fetch (~1–3 s) to ship in v0.6.1.

## Open follow-ups (out of scope unless benchmark points here)

- Wire-buffer / prefetch tuning in the connection DPB.
- Partitioning for tables without a single-column numeric PK (e.g. via
  `RDB$DB_KEY`) so parallel scan covers more of the schema.
- ⚠ BLOB-heavy fetch over high-RTT links (Phase 1c: 31 s for ~2900
  `RDB$RELATIONS` rows) — inline BLOB / batched segment reads.
