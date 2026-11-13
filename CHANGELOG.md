# Changelog

## [Unreleased]

### Changed

- Partitioned scans (`partitions > 1`, or automatic partitioning on the
  ATTACH path) now read **one Firebird snapshot**. A coordinating
  connection starts a read-only SNAPSHOT transaction and every partition
  worker joins it with `isc_tpb_at_snapshot_number` (Firebird 4+). Before,
  each partition ran its own read-committed transaction, so partitions
  could see different committed states of the table: a row moved or a
  value transferred between partitions during the scan could be counted
  twice or missed. Serial scans are unchanged.
- On Firebird 3 (no snapshot numbers), or if the shared snapshot cannot be
  established, partitioned scans keep the previous behavior exactly
  (independent read-committed transactions).

### Added

- `snapshot_mode` column (`'serial'` / `'shared'` / `'independent'`) in
  `firebird_last_query()` and `firebird_query_log()`, appended after
  `bytes_read_estimate` (20 columns).
- `scripts/check_shared_snapshot.sh`: consistency check under a concurrent
  writer, run as its own step on every Firebird 3/4/5 leg of the Linux
  matrix.

## [v1.3.1] - 2026-10-26

Patch release completing DuckDB v2.0 compatibility. No SQL functions,
parameters, or default behavior were added or changed.

### Fixed

- Recognize DuckDB v2.0's normalized `NOT IN` expression
  (`OPERATOR_NOT` around `COMPARE_IN`) in the complex-filter pushdown hook.
- Restore remote `NOT IN` pushdown, NONE-column residual telemetry, and
  `firebird_unpushed_mode` guards on DuckDB v2.0.
- Update lambda syntax in five test assertions so the same suite runs on
  DuckDB v1.5.6 and v2.0.
- Require all four release-asset builds; DuckDB v2 artifacts can no longer
  fail without failing the release workflow.

### Validated

- DuckDB v1.5.6: 26/26 test files, 1,082 assertions on Windows with
  Firebird 5.0.3.
- DuckDB v2.0.0: 26/26 test files, 1,082 assertions on Windows with
  Firebird 5.0.3.
- EMPLOYEE smoke test passed on both DuckDB lines: native ATTACH, scan,
  metadata discovery, and telemetry.

### Downloads

Linux x64 and Windows x64 packages are provided separately for DuckDB
v1.5.6+ and DuckDB v2.0+ (`-duckdb2` suffix).

[v1.3.1]: https://github.com/flozer/duckdb-firebird/releases/tag/v1.3.1
