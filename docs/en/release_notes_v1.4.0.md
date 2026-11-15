# Release notes — v1.4.0

This release was contributed by **[@fdcastel](https://github.com/fdcastel)
(F.D.Castel)**. All four fixes below are his work, arising from the
cooperation he proposed in [#89](https://github.com/flozer/duckdb-firebird/issues/89).
Our thanks — this release exists because of that contribution.

No new SQL functions or parameters. One new observability column
(`snapshot_mode`) and one behavioral change to partitioned scans
(a single shared snapshot), described below.

## Changes

### Partitioned scans read one Firebird snapshot (#94)

A partitioned scan (`partitions > 1`, or automatic partitioning on the
ATTACH path) previously ran each partition in its own read-committed
transaction, so partitions could observe different committed states of the
table — a row moved or a value transferred between partitions during the
scan could be counted twice or missed.

Now a coordinating connection starts a read-only SNAPSHOT transaction and
every partition worker joins it with `isc_tpb_at_snapshot_number`
(Firebird 4+), so all partitions read one consistent state. On Firebird 3
(no snapshot numbers), or if the shared snapshot cannot be established, the
previous behavior is kept exactly. Once the shared snapshot is established,
a worker that cannot join it fails the scan with a clear error rather than
reading its partitions outside the snapshot; its pooled connection is
discarded, and the error is surfaced in `firebird_last_query()`. Serial
scans are unchanged.

A new `snapshot_mode` column (`'serial'` / `'shared'` / `'independent'`)
is appended to `firebird_last_query()` and `firebird_query_log()` after
`bytes_read_estimate` (now 20 columns).

### Rows outside the cached key range are no longer skipped (#95)

On the ATTACH path the `MIN`/`MAX` primary-key bounds are probed the first
time a query references a table and then cached. Rows inserted afterwards
with a key above the cached `MAX` or below the cached `MIN` were silently
missing from every later scan of that table — partitioned or not, including
filtered ones — until the database was re-attached. The first partition now
has no lower bound and the last no upper bound; a single-partition scan
sends no range predicate at all.

### Native data types from servers with `DataTypeCompatibility` (#98)

Against a server configured with `DataTypeCompatibility = 3.0` or `2.5`,
`INT128` and `BOOLEAN` raised a DuckDB `INTERNAL Error` (which invalidates
the database instance), and `TIMESTAMP`/`TIME WITH TIME ZONE` came back
shifted or garbled without an error. The extension now asks for the native
types on every attachment (`isc_dpb_set_bind`), so `INT128`,
`NUMERIC(19+)`, `DECFLOAT`, the time-zone types, and `BOOLEAN` read
correctly.

### Windows client-library loading from a path (#96)

When the client library is given by path
(`DUCKDB_FIREBIRD_CLIENT_LIBRARY`), its dependent DLLs are now resolved
from the library's own folder first (`LOAD_WITH_ALTERED_SEARCH_PATH`). A
Firebird 3 `fbclient.dll` — which ships `msvcr100.dll`, `zlib1.dll` and ICU
next to it — previously failed with "The specified module could not be
found" unless its folder was also on `PATH`.

## Validation

- Full Firebird 3/4/5 CI matrix green on DuckDB v1.5.6 and v2.0.0 for all
  four changes.
- The shared-snapshot guarantee is checked under a concurrent writer
  (`scripts/check_shared_snapshot.sh`) on every Firebird 3/4/5 leg, and the
  join-failure path is checked (`scripts/check_snapshot_join_failure.sh`)
  on Firebird 4+.

## Downloads and compatibility

Use the package built for your DuckDB line:

- `duckdb-firebird-1.4.0-linux-x64.tar.gz` and
  `duckdb-firebird-1.4.0-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.4.0-linux-x64-duckdb2.tar.gz` and
  `duckdb-firebird-1.4.0-windows-x64-duckdb2.zip`: DuckDB v2.0+.

The community catalog remains independent of this repository release.
