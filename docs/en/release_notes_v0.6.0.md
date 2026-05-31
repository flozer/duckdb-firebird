# duckdb-firebird v0.6.0 — Release notes

Theme: **Firebird-native diagnostics** (Phase 4). The connector core is
unchanged; this release adds factual, conservative diagnostic surfaces and
fixes a Firebird 4+ type gap. Materialization stays out of scope (DuckDB's
strength), per the roadmap.

## New

- **`firebird_profile_table('catalog.schema.table')`** — single-row factual
  diagnostic for one Firebird relation: object type (TABLE/VIEW), primary
  key + columns, indexes, watermark candidates, filter candidates,
  full-scan risk, advisory `recommended_partitions`, and warnings. Reads
  only `RDB$` system tables plus a best-effort PK MIN/MAX probe; never reads
  business rows, never returns the connection string.
- **Heavy-view diagnostics** — for `object_type = VIEW`, a shallow
  conservative inspection of the stored view SELECT (`RDB$VIEW_SOURCE`)
  surfaces `JOIN`, aggregation (`GROUP BY` / `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/
  `LIST`), and missing-`WHERE` warnings. Token-level (not a SQL parser);
  string literals (incl. escaped `''`), comments, and split whitespace are
  handled; the view text is never returned.
- **`firebird_pool_stats('fb')`** — connection-pool introspection for one
  attached Firebird catalog by explicit alias: `pool_enabled`,
  `max_idle_size`, `idle_timeout_ms`, `idle_connections`, `total_created`,
  `total_reused`, `total_discarded`. Reads only counters the pool already
  tracks; never leases a connection; does not enumerate catalogs.

## Changed

- **Pushdown explainability telemetry** — `firebird_last_query()` /
  `firebird_query_log()` schema grew from **15 to 18 columns**:
  `limit_pushed`, `offset_pushed` (paging actually pushed, NULL when none),
  and `not_pushed_reasons` (coarse reason per residual filter: `NONE_CHARSET`
  / `UNSUPPORTED_OP` / `ROWID_OR_INVALID_COLUMN` /
  `UNSUPPORTED_PROJECTION_MAPPING`). Records the NONE-charset gate on
  complex filters that was previously dropped silently.
- **Adaptive `recommended_partitions`** — refined in
  `firebird_profile_table()` (10-column schema unchanged):
  partitions derived from the single-column numeric PK MIN/MAX range width
  (no row count, no `COUNT(*)`), capped at 8; a small range now recommends
  `partitions=1` with an explicit note; a server-side parallelism caveat
  (Firebird 5 `ParallelWorkers`) is emitted when `partitions > 1`.
  **Diagnostic/recommendation only — `firebird_scan` is unchanged, nothing
  is parallelized automatically, no performance gain promised.**
- **`DECFLOAT(16)` / `DECFLOAT(34)`** now surface as **VARCHAR** (was
  `DOUBLE` in the schema). See Fixed.

## Fixed

- **DECFLOAT silent NULL** — Firebird 4+ `DECFLOAT(16/34)` (IEEE
  Decimal64/Decimal128) were typed `DOUBLE` but always fetched `NULL`. Now
  projected server-side as `CAST(col AS VARCHAR(64))` and surfaced as
  lossless VARCHAR (ordinary/exponent values, `NaN`, `±Infinity`, real NULL
  preserved). Pushed filters (simple + `IN`/`NOT IN`/`LIKE`) compare against
  the same casted expression, so text semantics match. No local
  decimal-float decoder, no `DOUBLE` default.
- **GCC/Linux build break** — `FirebirdPoolStatsRow` was not forward-declared
  at namespace scope, so the `ReadFirebirdPoolStats` friend failed to name a
  type under GCC (the community CI toolchain), making the private `pool_`
  access fail. MSVC had masked it. Forward-declared at namespace scope;
  pure portability fix, no behavior change. (`b662edc`)

## Deferred

- **Conservative aggregate pushdown** — DuckDB v1.5.3 has no
  extension-facing aggregate-pushdown hook. The clean `COUNT(*)` path is
  `get_partition_stats`, but `GetPartitionStatsInput` does not expose
  `table_filters` to the callback, so a bare `COUNT(*)` cannot be
  distinguished from a filtered one without risking a wasted remote
  `COUNT(*)` on filtered queries. Deferred (decision recorded in roadmap);
  no code shipped.
- **DECFLOAT lossy numeric fast path** (`decfloat='double'`) — possible
  future opt-in; not in this release.
- **`recommended_partitions > 1` CI fixture** — a wide-range numeric-PK
  table would force a new relation into the main fixture and cascade
  metadata/dbt test updates; the heuristic is covered by code, the `= 1`
  paths and warnings by the test.

## Tests

- Local SQL test suite green on the Windows-native build (FB5 fixture):
  full firebird group **13/13**, including new `firebird_profile_table.test`
  (46 assertions), `firebird_observability.test` (50), `firebird_pool_stats.test`
  (27), `firebird_decfloat.test` (33, dedicated `FIREBIRD_DECFLOAT_DB` fixture
  that skips in CI).
- **Local Docker community/GitHub-path simulation green** (gcc, Ubuntu 24.04,
  fresh recursive clone, DuckDB v1.5.3): extension compiles (546/546),
  artifact is ELF, `libfbclient` is **not** linked (runtime-load contract),
  runtime client resolvable.
- **Docker smoke end-to-end green**: `LOAD` + `firebird_scan` + `ATTACH` +
  `firebird_profile_table` against a disposable synthetic fixture inside the
  container.

## Release Assets

- `duckdb-firebird-0.6.0-linux-x64.tar.gz`
- `duckdb-firebird-0.6.0-windows-x64.zip`

## Docs

- `function_manual` (PT + EN): new `firebird_profile_table`,
  `firebird_pool_stats`, DECFLOAT type-mapping note, `recommended_partitions`
  section, pushdown-explainability columns.
- `observability.md` (EN): 18-column schema, `not_pushed_reasons` vocabulary,
  pool-introspection cross-reference.
- `roadmap` (PT + EN): Phase 4 items marked released in v0.6.0, aggregate
  pushdown deferred with rationale.
- `README`: v0.6 diagnostics section.
- `test_report.md` (EN): DECFLOAT status flipped from DOUBLE/NULL to VARCHAR.
