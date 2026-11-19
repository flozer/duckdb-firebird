# Changelog

## [Unreleased]

### Added

- `firebird_query(alias, sql)`: runs one Firebird `SELECT` on an ATTACH
  exactly as written (selectable procedures, CTEs, `FIRST`/`SKIP`, Firebird
  functions) and returns its rows, typed as a scan types the same Firebird
  types. Non-SELECT statements fail at bind; it runs in a read-only
  transaction (or the DuckDB transaction's snapshot with
  `firebird_transaction_snapshot`), leaves a `firebird_last_query()` /
  `firebird_query_log()` record, and appears in
  `firebird_explain_pushdown()` as `scan_strategy = 'passthrough'`.

### Changed

- CI and the release assets build DuckDB v2.0.1 instead of v2.0.0.
  DuckDB loads a C++ extension only into the exact version it was built
  for, so the v2.0.0 asset does not load in v2.0.1, and v2.0.1 fixes
  v2.0.0 bugs (wrong results, lost rows after a restart, deadlocks).

### Fixed

- Connection strings in the space-separated key=value form used by the
  function examples (`'database=... user=... password=...'`) were not
  parsed: with no `;` the whole string was taken as a bare database path,
  so the attach failed and its error message showed the password. They
  now parse like the `;` form when every token is a known key. Attach
  errors, and the database path shown by `duckdb_databases()`, are now
  redacted: `password=` values, `scheme://user:pass@` user info, and any
  literal occurrence of the password.
- CI: the Linux workflows never saved their ccache cache. They cached
  `~/.ccache`, but ccache 4.x keeps its files in `~/.cache/ccache` unless
  `~/.ccache` already exists, so every run ended with "Path Validation
  Error" and rebuilt DuckDB from scratch. `CCACHE_DIR` now points ccache at
  `~/.ccache`, and the build steps print `ccache --show-stats`.

## [v1.4.1] - 2026-11-17

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
  on Firebird 4/5 also for a connection killed by the server's idle timeout.
  Its own step on every Firebird 3/4/5 leg of the Linux matrix.
- `SET firebird_max_inline_blob_size = BYTES`: tunes or disables Firebird
  5.0.3+ inline blobs (`isc_dpb_max_inline_blob_size`, `-1` = client
  default, `0` = off, max 65535), read at `firebird_scan()` bind and at
  `ATTACH` like `firebird_dummy_packet_interval`. Inlining is already on by
  default for TCP attachments when the client library and the server are
  both 5.0.3+ (the server does not inline blobs over XNET or embedded).
- `firebird_health()`: `client_version`, `inline_blob_size` and
  `network_protocol` columns (18 columns), and the
  `inline_blobs_unavailable` warning for a 5.0.3+ server reached over TCP
  through an older client library.
- Transaction snapshot on the ATTACH path (opt-in, off by default):
  `SET firebird_transaction_snapshot = true`, or the ATTACH option
  `transaction_snapshot`, makes every scan of the ATTACH in one DuckDB
  transaction, serial scans included, read one Firebird snapshot
  (Firebird 4+): one per statement in autocommit, one per
  `BEGIN … COMMIT`. A coordinator from the ATTACH pool holds it until
  commit or rollback; scans report `snapshot_mode = 'transaction'`.
  Firebird 3 keeps the per-scan behavior. `SET
  firebird_transaction_snapshot_idle_timeout = SECONDS` (default 60,
  `0` = none) has the server release the snapshot (`SET SESSION IDLE
  TIMEOUT`) when no scan of the ATTACH follows the end of a statement in
  time; the next scan then fails with a message naming the setting.
  `scripts/check_shared_snapshot.sh` checks it under the concurrent writer,
  `scripts/check_dead_pooled_connection.sh` kills its coordinator and the
  parked connections and has another session reuse a lost snapshot number,
  and `test/sql/firebird_transaction_snapshot.test` covers the settings and
  modes.
- CI: a DuckDB v2.0.0 × Firebird 5 leg in the Linux Firebird matrix, so the
  FB4+-only tests and scripts also run on the DuckDB v2.0 build.
- CI: the DuckDB v1.5.6 × Firebird 5 leg of the Linux matrix also reaches
  its Firebird 5 server through a Firebird 4 `libfbclient`: it checks that
  `firebird_health()` reports `inline_blob_size` NULL and the
  `inline_blobs_unavailable` warning for that pairing, and reruns
  `firebird_health.test` (#108).
- Docs: a note on the open Firebird issues #9227 and #9301, which can still
  let a partitioned scan (and the transaction snapshot) mix two committed
  states on Firebird 4/5.

### Fixed

- ATTACH: a pooled connection whose attachment was gone (killed on the
  server or by its idle timeout, network failure) went back to the pool
  after a failed query, so
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
