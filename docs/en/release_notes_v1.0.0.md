# duckdb-firebird v1.0.0 - Release notes

Theme: **Production Bridge**. This release consolidates `duckdb-firebird` as
a stable, read-only runtime bridge from DuckDB to Firebird 3/4/5 -- metadata
and diagnostics depth, a scan-planning report, lossless type/BLOB handling,
and a closed Runtime/ABI Compatibility gate backed by a real cross-version
matrix and a maturity battery run against a real ~90GB database.

This is the `duckdb-firebird` repository's own v1.0.0. The DuckDB Community
Extensions catalog still installs the previously-submitted **v0.6.1**
(`community-extensions/description.yml` is unchanged by this release) --
nothing in `duckdb/community-extensions` is touched here. A community
catalog update is a separate, later step that requires its own explicit
authorization.

## Highlights

### Metadata & Explain (v0.7)

- **Metadata Bridge 2.0** - `information_schema` constraints (PK/UNIQUE/FK
  with referential rules) plus seven Firebird catalog functions:
  `firebird_foreign_keys`, `firebird_indexes`, `firebird_generators`,
  `firebird_domains`, `firebird_computed_columns`, `firebird_dependencies`,
  `firebird_comments`.
- **`firebird_explain_pushdown(sql)`** - a-priori, plan-only report of what
  SQL is sent to Firebird: filters pushed vs. not pushed (with reason),
  projected columns, applied `ROWS`, and PK-range partition usage.
- **`firebird_type_audit('fb')`** - findings-only audit of type/charset
  conversion fidelity (`decfloat_as_varchar`, `int128`, `time_tz`,
  `timestamp_tz`, `none_charset`, `blob_text`); columns with no caveat are
  omitted.

### Diagnostics Bridge (v0.8)

- **`firebird_health('fb')`** - single-row database/server diagnostic:
  engine/ODS version, dialect, charset, page size, forced writes, sweep
  interval, OIT/OAT/next-transaction gaps, attachment count, and structured
  warning codes (`oit_gap_high`, `sweep_disabled`, `charset_none`, etc.).
- **Structured `firebird_profile_table` alerts** - a
  `LIST(STRUCT(code, severity, message))` column flags tables/views with no
  PK, heavy view shapes (JOIN/aggregation/no-filter), and other BI-relevant
  risks, instead of free-text notes.
- **`firebird_index_profile('fb.main.T')`** - per-index diagnostic:
  candidate indexes, selectivity where available, dependencies, and
  cheap-filter recommendations.

### Production Bridge (v1.0)

- **Smart Scan Planning Report** - consolidates signals already produced by
  `firebird_profile_table`, `firebird_health`, `firebird_index_profile`,
  `firebird_type_audit`, and `firebird_explain_pushdown` into one scan-safety
  decision (serial vs. PK-range, heavy-view warnings, charset-driven pushdown
  disablement, `ROWS`-without-`ORDER BY` warnings), plus a `ROWS`
  determinism fix for that planning path.
- **Type-lossless hardening (#33)** - fixed a crash fetching a Firebird
  view's computed/aggregate column whose live type differs from its
  catalog-frozen type (e.g. `NUMERIC(38,2)`/INT128-backed `SUM()`),
  covering both the direct `firebird_scan(...)` path and ATTACH-resolved
  `fb.main.VIEW` references.
- **BLOB-lossless hardening (#35)** - fixed `ReadBlob` silently dropping
  every segment after the first for multi-segment text/binary BLOBs (the
  loop incorrectly treated `rc == 0` as end-of-blob; only `isc_segstr_eof`
  is a real end-of-blob signal).
- **`DECFLOAT(16/34)` lossless fallback** - surfaces as VARCHAR via a
  server-side `CAST`, replacing the previous silent-NULL behavior.

### Runtime/ABI Compatibility Gate

- **Cross-version DuckDB matrix** - `v1.5.2`, `v1.5.3` (pin), `v1.5.4` all
  build and pass the full local suite (19 files, 854 assertions), zero API
  drift.
- **Firebird 3/4/5 CI matrix, fully green and unified** - one canonical
  fixture source (`scripts/fixture_common.sql` /
  `scripts/fixture_biz4.sql` / `scripts/fixture_none_charset.sql` /
  `scripts/fixture_decfloat.sql`) shared by local and CI runs, a
  version-matched client library extracted per matrix leg (closing a real
  client/server wire-protocol mismatch), and a permanent anti-drift guard
  (`scripts/check_no_inline_fixture_drift.sh`) preventing inline fixture SQL
  from silently reappearing in the workflow.
- **CI coverage closed** - the Linux matrix now runs 20 `test/sql/*.test`
  files (was 4), including the previously CI-invisible NONE-charset,
  DECFLOAT, and BLOB-lossless suites.
- **Real maturity battery** - a new read-only, env-var-driven harness
  (`scripts/maturity_battery.ps1`) ran against a real ~90GB test database
  (2926 relations, 15367 `NONE`-charset columns): zero product errors,
  self-consistent BLOB/text fetches (same row fetched twice, checksums
  compared, never the value itself), `sweep_disabled`/`charset_none`
  correctly flagged as expected characteristics of that legacy-profile
  database rather than defects.

## Validation

- CI green on Windows x64 (MSVC), Linux x64, and the Firebird 3/4/5 Linux
  matrix (including the anti-drift guard step).
- Cross-version DuckDB matrix (v1.5.2/v1.5.3/v1.5.4): 19/19 test files,
  854/854 assertions, zero `env-missing`, zero API drift.
- Read-only maturity battery against a real ~90GB database: zero errors,
  9/9 sampled BLOB/text columns self-consistent, no row values or content
  ever logged.
- Issue hygiene: #33 and #35 (both fixed in this cycle) confirmed closed;
  #36 (a narrow, non-blocking `ReconcileViewColumnTypes` error-scoping
  nuance) and #47/#48 residual notes tracked as non-blocking follow-ups, not
  release blockers.

## Upgrade Notes

- No SQL changes are required for existing `firebird_scan(...)`,
  `firebird_tables(...)`, and `ATTACH ... (TYPE firebird)` calls.
- `firebird_pool_stats`, `firebird_health`, `firebird_type_audit`,
  `firebird_index_profile`, and `firebird_explain_pushdown` are additive --
  no existing function's signature or return shape changed.
- `DECFLOAT(16/34)` columns now return VARCHAR instead of always-NULL
  DOUBLE; queries that assumed the old silent-NULL behavior should be
  reviewed.
- The DuckDB Community catalog is unaffected -- it still serves v0.6.1 until
  a separate, explicitly authorized community update.

## Non-blocking follow-ups (tracked, not release blockers)

- #36 - a connection-level failure inside `ReconcileViewColumnTypes`'s
  `LookupObjectType` call demotes the whole ATTACH batch to the slower
  per-table fallback instead of isolating just the affected view.
- #47 residual - the anti-drift guard's keyword coverage and a
  missing-workflow-file edge case have known, documented limits (see the
  guard's own header comment); not a defect in the fixture-drift fix it
  ships with.
- Windows CI builds and uploads the artifact but does not run
  `test/sql/firebird_*.test` in CI -- that coverage is currently manual
  (`scripts/build_matrix.ps1`), an automation gap tracked for future work.
- `guide_linux.md` / `guide_windows.md` mention `v1.5.3` as the build
  version without calling out the full `v1.5.2`-`v1.5.4` compatibility
  range documented in `docs/pt/duckdb_1_5_compatibility_plan.md`.

## Release Assets

Release packages are generated by GitHub Actions for:

- `linux-x64`
- `windows-x64`
