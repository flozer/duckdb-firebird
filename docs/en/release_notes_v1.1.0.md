# Release notes — v1.1.0

Feature release closing the post-v1.0.2 debt queue (F1–F5). Released from
`main`; the DuckDB Community catalog update is a separate, explicitly
authorized step.

## Highlights

### `firebird_profile_table`: row estimate + structured required-filter alerts

- New columns `estimated_rows` / `row_estimate_method`: a cheap PK-range
  upper bound (`MAX - MIN + 1`, assumes a dense key) by default; `NULL`
  for tables without a usable single-column numeric PK.
- New named parameter `exact_row_count=true`: opt-in server-side
  `COUNT(*)` (method `exact_count`), observable via the new
  `exact_count_executed` LOW alert. The estimate never runs `COUNT(*)`
  on its own.
- Structured required-filter recommendations for HIGH-risk base tables:
  `filter_before_scan` (MEDIUM, names the filterable columns) and
  `materialize_before_scan` (HIGH, no filter/watermark candidate
  exists). Views keep their own guidance.

### `firebird_pool_stats`: active leases + last error

- New columns `active_connections` (leases handed out and not yet
  returned) and `last_error` (sanitized message of the most recent
  failed connection creation; password redacted, length capped; only
  post-`ATTACH` failures are recorded). Output grows 8 → 10 columns.

### CI/test debt closed

- `firebird_decfloat.test` promoted into the main suite step (runs on
  FB4/FB5 legs; FB3 skips explicitly); `setup_test_firebird.sh`
  provisions a dedicated `decfloat.fdb` and exports the env on Firebird
  4+ servers.
- New `firebird_partitions_scan.test` (all three Firebird legs) with a
  dedicated fixture: a sparse ~12M PK span recommends real partitioning
  and a real `partitions=4` scan returns every row exactly once; also
  covers the `materialize_before_scan` profile case.

### Compatibility

- **DuckDB v1.5.6 validated** (released 2026-09-28): builds clean, full
  suite 21/21 test files (903 assertions), identical to the v1.5.3
  baseline. No API drift in the surface this extension uses. Build pin
  intentionally stays v1.5.3 until the DuckDB Community Extensions
  catalog moves its own target (currently v1.5.5). See
  [docs/pt/duckdb_1_5_compatibility_plan.md](../pt/duckdb_1_5_compatibility_plan.md).

## Validation

- Full sqllogictest suite green in CI (Linux x64, Windows x64, FB 3/4/5
  matrix) and locally via `scripts/build_matrix.ps1` on v1.5.3 and
  v1.5.6.
- Read-only maturity battery on a real ~66GB Firebird 5.0.3 ERP restore
  (metadata and aggregate counts only): 3,174 tables discovered,
  17,866 `none_charset` findings from `firebird_type_audit`, 291 FKs,
  9,842 indexes; `firebird_profile_table` on a ~20M-span PK table
  recommended `partitions=10` with both partition alerts, and the
  documented sparse-PK overcount was reproduced at scale (PK-range
  estimate 20,107,392 vs exact `COUNT(*)` 967,387);
  `firebird_pool_stats` stayed coherent (idle parked, active 0,
  `last_error` NULL) throughout.

## Compatibility notes

- `firebird_pool_stats` consumers selecting `*` see two new columns;
  nothing removed or renamed.
- `firebird_profile_table` consumers selecting `*` see two new columns
  plus one new named parameter; nothing removed or renamed.
- Three new stable alert codes: `filter_before_scan`,
  `materialize_before_scan`, `exact_count_executed`.
