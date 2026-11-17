# Release notes — v1.4.1

This release was contributed by **[@fdcastel](https://github.com/fdcastel)
(F.D.Castel)**. All six changes below are his work, continuing the
cooperation he proposed in
[#89](https://github.com/flozer/duckdb-firebird/issues/89). Our thanks —
this release exists because of that contribution.

Three opt-in additions (`partition_by = 'dbkey'`, inline-blob tuning, a
transaction snapshot) and three ATTACH-path fixes, all read-only and
backward compatible. Existing queries are unchanged unless they set the new
options.

## Added

### `partition_by = 'pk' | 'dbkey'` (#101)

`firebird_scan()` takes a new `partition_by` parameter. The default `'pk'`
keeps the previous behavior (ranges over the primary key). `'dbkey'`
(explicit `partitions >= 2`, base tables, Firebird 4 and 5) splits the
table into `RDB$DB_KEY` ranges over its physical record space (`RDB$PAGES`,
`MAKE_DBKEY`), read inside the shared snapshot: no `MIN`/`MAX` probe, a
split that does not depend on how the keys are distributed (balanced to
within one pointer page), and tables **without a usable primary key can be
partitioned**. Anything `'dbkey'` cannot handle — including a scan whose
shared snapshot could not be started — falls back to `'pk'`. With a pushed
filter that can use an index, every DB_KEY partition repeats the index
scan, so `'pk'` is the better choice there.

A new `partition_method` column (`'none'` / `'pk'` / `'dbkey'`) is appended
to `firebird_last_query()` and `firebird_query_log()` after `snapshot_mode`
(21 columns).

### Firebird 5.0.3+ inline-blob tuning and reporting (#107)

`SET firebird_max_inline_blob_size = BYTES` tunes or disables Firebird
5.0.3+ inline blobs (`isc_dpb_max_inline_blob_size`, `-1` = client default,
`0` = off, max 65535), read at `firebird_scan()` bind and at `ATTACH` like
`firebird_dummy_packet_interval`. Inlining is already on by default for TCP
attachments when the client library and the server are both 5.0.3+ (the
server does not inline blobs over XNET or embedded).

`firebird_health()` gains three columns — `client_version`,
`inline_blob_size` and `network_protocol` (18 columns) — and an
`inline_blobs_unavailable` warning for a 5.0.3+ server reached over TCP
through an older client library.

### Transaction snapshot on the ATTACH path (opt-in, off by default) (#109)

`SET firebird_transaction_snapshot = true`, or the ATTACH option
`transaction_snapshot`, makes every scan of the ATTACH in one DuckDB
transaction — serial scans included — read one Firebird snapshot
(Firebird 4+): one per statement in autocommit, one per `BEGIN … COMMIT`. A
coordinator from the ATTACH pool holds it until commit or rollback; scans
report `snapshot_mode = 'transaction'`. Firebird 3 keeps the per-scan
behavior.

`SET firebird_transaction_snapshot_idle_timeout = SECONDS` (default 60,
`0` = none) has the server release the snapshot (`SET SESSION IDLE
TIMEOUT`) when no scan of the ATTACH follows the end of a statement in
time; the next scan then fails with a message naming the setting. A joining
connection verifies the coordinator still holds the snapshot number before
trusting it, so a reused number from a reopened database is never read in
another session's snapshot.

## Fixed

### A dead pooled connection is never re-parked (#106, #103)

A pooled connection whose attachment was gone (killed on the server or by
its idle timeout, or a network failure) went back to the pool after a
failed query, so every later query on the ATTACH failed the same way until
it was re-attached. This happened after a failed `firebird_health()` or
other catalog function, a failed scan, and a failed PK probe — which also
remembered "no usable primary key", so the table was never partitioned
again. A connection now marks itself broken on a lost-connection error and
the pool discards it instead of parking it; the PK probe doesn't remember a
result from a broken connection, and a table list loaded on one isn't
cached. A failed catalog load is also now counted as discarded, so it no
longer leaves `firebird_pool_stats().active_connections` one too high.

### ATTACH-alias errors name the called function (#102)

The functions that take an ATTACH alias (`firebird_health()`,
`firebird_pool_stats()`, `firebird_profile_table()`,
`firebird_index_profile()`, `firebird_generators()` and the other catalog
metadata functions) reported an unknown or non-Firebird alias under the
name `firebird_generate_dbt_sources`. The error now names the function that
was actually called.

## Known limitation

The open Firebird issues
[#9227](https://github.com/FirebirdSQL/firebird/issues/9227) and
[#9301](https://github.com/FirebirdSQL/firebird/issues/9301) can still let a
partitioned scan — and the transaction snapshot — mix two committed states
on Firebird 4/5 under a narrow race. The behavior is read-only; nothing is
written to the source. This note will be updated when the upstream fixes
are released.

## Validation

- Full Firebird 3/4/5 CI matrix green on DuckDB v1.5.6 and v2.0.0, with a
  dedicated DuckDB v2.0.0 × Firebird 5 leg so the Firebird 4+ tests and
  scripts also run on the DuckDB v2.0 build.
- The shared-snapshot and transaction-snapshot guarantees are checked under
  a concurrent writer (`scripts/check_shared_snapshot.sh`) on every
  Firebird 3/4/5 leg, with an option-off control that must tear on
  Firebird 4/5; `scripts/check_dead_pooled_connection.sh` kills the
  coordinator and the parked connections and has another session reuse a
  lost snapshot number.

## Downloads and compatibility

Use the package built for your DuckDB line:

- `duckdb-firebird-1.4.1-linux-x64.tar.gz` and
  `duckdb-firebird-1.4.1-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.4.1-linux-x64-duckdb2.tar.gz` and
  `duckdb-firebird-1.4.1-windows-x64-duckdb2.zip`: DuckDB v2.0+.

The community catalog remains independent of this repository release.
