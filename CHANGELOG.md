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
- Once the shared snapshot is established, a partition worker that cannot
  join it fails the scan with an error; no partition is read outside the
  snapshot.

### Added

- `snapshot_mode` column (`'serial'` / `'shared'` / `'independent'`) in
  `firebird_last_query()` and `firebird_query_log()`, appended after
  `bytes_read_estimate` (20 columns).
- `scripts/check_shared_snapshot.sh`: consistency check under a concurrent
  writer, run as its own step on every Firebird 3/4/5 leg of the Linux
  matrix.

### Fixed

- Scans of a table with a single-column integer primary key no longer skip
  rows whose key lies outside the `MIN`/`MAX` probed before the scan (#93).
  On the ATTACH path the bounds are probed the first time a query references
  a table and then cached, so every row inserted afterwards with a key above
  the cached `MAX` or below the cached `MIN` was silently missing from every
  later scan of that table, partitioned or not, including filtered ones,
  until the database was re-attached. The first partition now has no lower
  bound and the last no upper bound; a single-partition scan sends no range
  predicate at all.
- Read `INT128`, `NUMERIC(19+)`, `DECFLOAT`, `TIMESTAMP`/`TIME WITH TIME ZONE`
  and `BOOLEAN` correctly from servers with `DataTypeCompatibility = 3.0`
  or `2.5`. Before, `INT128` and `BOOLEAN` raised a DuckDB `INTERNAL Error`,
  which invalidates the database instance, and the time zone types came
  back shifted or garbled without an error. The extension now asks for the
  native types on every attachment (`isc_dpb_set_bind`).
- Windows: loading the client library from a path
  (`DUCKDB_FIREBIRD_CLIENT_LIBRARY`) now resolves its dependent DLLs from
  the library's own folder first (`LOAD_WITH_ALTERED_SEARCH_PATH`). A
  Firebird 3 `fbclient.dll` (which ships `msvcr100.dll`, `zlib1.dll` and
  ICU next to it) failed with "The specified module could not be found"
  unless its folder was also on `PATH`.

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
