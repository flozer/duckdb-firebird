# Changelog

## [Unreleased]

### Changed

- The `firebird_profile_table()` alerts for tables without a usable primary
  key (`no_primary_key`, `composite_pk_serial`, `non_numeric_pk_serial`) now
  point to `partition_by = 'dbkey'` on Firebird 4/5.

### Added

- `scripts/check_catalog_load_failure.sh`: kills a pooled attachment before
  the first table query of an ATTACH and checks that the failed catalog load
  leaves no connection leased; run as its own step on every Firebird 3/4/5
  leg of the Linux matrix.
- `partition_by = 'pk' | 'dbkey'` for `firebird_scan()`. `'dbkey'` (explicit
  `partitions >= 2`, base tables, Firebird 4 and 5) splits the table into
  `RDB$DB_KEY` ranges over its physical record space (`RDB$PAGES`,
  `MAKE_DBKEY`), read inside the shared snapshot: no `MIN`/`MAX` probe, a
  split that does not depend on how the keys are distributed (balanced to
  within one pointer page), and tables without a usable primary key can be
  split. The default `'pk'` keeps the previous behavior;
  anything `'dbkey'` can't handle (including a scan whose shared snapshot
  could not be started) falls back to it. With a pushed filter that can
  use an index, every DB_KEY partition repeats the index scan, so `'pk'` is
  the better choice there.
- `partition_method` column (`'none'` / `'pk'` / `'dbkey'`) in
  `firebird_last_query()` and `firebird_query_log()`, appended after
  `snapshot_mode` (21 columns).
- `scripts/check_shared_snapshot.sh` also runs `partition_by = 'dbkey'`
  scans, checks `partition_method` on every scan, and on Firebird 4/5
  checks that every DB_KEY range of a scan holds rows.
- `scripts/check_dead_pooled_connection.sh`: kills a pooled attachment and
  checks that `firebird_health()` and scans work again from the call after
  the failed one, and that an auto-partitioned table is partitioned again;
  its own step on every Firebird 3/4/5 leg of the Linux matrix.

### Fixed

- ATTACH: a pooled connection whose attachment was gone (killed on the
  server, network failure) went back to the pool after a failed query, so
  every later query on the ATTACH failed the same way until it was
  re-attached. This happened after a failed `firebird_health()` or other
  catalog function, a failed scan, and a failed PK probe, which also
  remembered "no usable primary key" for the table, so it was never
  partitioned again. A connection now marks itself broken when a call fails
  with a lost-connection error, and the pool discards it instead of parking
  it; the PK probe doesn't remember a result it got on a broken connection,
  and a table list loaded on one isn't cached. The first query that gets
  such a connection still fails (#105).
- ATTACH: when loading the table list failed (for example because the
  pooled connection's attachment had been killed on the server), the
  connection was dropped without telling the pool, so each failed load left
  `firebird_pool_stats().active_connections` one too high for the rest of
  the ATTACH. The connection is now counted as discarded.
- The functions that take an ATTACH alias (`firebird_health()`,
  `firebird_pool_stats()`, `firebird_profile_table()`,
  `firebird_index_profile()`, `firebird_generators()` and the other catalog
  metadata functions such as `firebird_indexes()`) reported an unknown or
  non-Firebird alias under the name `firebird_generate_dbt_sources`. The
  error now names the function that was called.

## [v1.4.0] - 2026-11-15

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
  snapshot, and the worker's pooled connection is discarded. The error is
  also set in `firebird_last_query()` when another worker of the scan opens
  a cursor.

### Added

- `snapshot_mode` column (`'serial'` / `'shared'` / `'independent'`) in
  `firebird_last_query()` and `firebird_query_log()`, appended after
  `bytes_read_estimate` (20 columns).
- `scripts/check_shared_snapshot.sh`: consistency check under a concurrent
  writer, run as its own step on every Firebird 3/4/5 leg of the Linux
  matrix.
- `scripts/check_snapshot_join_failure.sh`: kills one, then all, of the
  pooled attachments of an ATTACH and checks that the next partitioned scan
  fails with the "could not join" error, that the dead connections are
  discarded rather than pooled, what `firebird_last_query()` shows, and that
  a later scan succeeds (Firebird 4+; its own step in the Linux matrix,
  skipped on Firebird 3).

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
