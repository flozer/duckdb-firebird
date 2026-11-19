# duckdb-firebird — Architecture

This document explains how the extension is wired together, the design
decisions that shaped each milestone, and the path forward.

## Goal

Make Firebird tables addressable from DuckDB SQL with native pushdown so
queries like

```sql
SELECT  e.dept_no,
        COUNT(*),
        AVG(e.salary)
  FROM  firebird_scan('firebird://sysdba:masterkey@db/srv:/data/prod.fdb',
                      'EMPLOYEE') e
  JOIN  read_parquet('s3://lake/departments/*.parquet') d
   ON   e.dept_no = d.dept_no
 WHERE  e.hire_date > DATE '2020-01-01'
 GROUP  BY e.dept_no;
```

run as a single federated plan: the date filter is pushed into Firebird,
only the projected columns travel over the wire, and DuckDB's vectorized
executor handles the join + aggregate.

## Layout

```
src/
├── firebird_extension.{hpp,cpp}   Extension entry point (register functions)
├── firebird_client.{hpp,cpp}      Thin C++ wrapper over libfbclient (ibase.h)
├── firebird_types.{hpp,cpp}       Firebird XSQLDA  ⇄  DuckDB LogicalType
├── firebird_query.{hpp,cpp}       SQL builder w/ projection & filter pushdown
└── firebird_scanner.{hpp,cpp}     TableFunction: bind / init / scan + firebird_tables
test/sql/firebird_scan.test        sqllogictest fixtures (93 assertions)
scripts/setup_test_firebird.sh     CI fixture bootstrap (EMPLOYEE schema)
.github/workflows/
├── build-linux.yml                Build + Firebird-3 fixture + run tests
└── build-windows.yml              Build on windows-latest with Firebird SDK ZIP
```

## Data flow

```
                        ┌────────────────────────────────────────┐
                        │  DuckDB planner                        │
                        │  - parses firebird_scan('…', 'TBL')    │
                        │  - calls FirebirdScanBind  ────────────┼──► RDB$ probe
                        │                                        │   (schema + PK)
                        │  - decides projection / filter pushdown│
                        │  - InitGlobal — slices PK range into   │
                        │    N PartitionSpec entries             │
                        └────────────────────────────────────────┘
                                       │
              ┌────────────────────────┼────────────────────────┐
              ▼                        ▼                        ▼
        worker 1 / FirebirdLocalState — owns FirebirdConnection
            scan loop: pop PartitionSpec  →  OpenCursor
                       fetch ⨯ STANDARD_VECTOR_SIZE  →  FirebirdAppendValue
                       cursor exhausted? → pop next, else done
```

## Pushdown

Three pushdown axes are advertised on the `TableFunction`:

| Axis        | Status                              |
|---|---|
| Projection  | ✅ — `column_ids` materialised in the produced `SELECT` list |
| Filter      | ✅ — supported subset translated to native Firebird SQL |
| LIMIT       | 🟡 — builder accepts a limit argument; planner wiring is open |

### Filter translation

The scanner walks the `TableFilterSet` and emits Firebird-side WHERE
fragments for the supported subset. Anything outside the subset stays in
DuckDB and is re-applied above the scan (we set `filter_prune = false`).

| `TableFilterType`            | Translated to Firebird SQL?           |
|---|---|
| `IS_NULL` / `IS_NOT_NULL`    | ✅ `col IS [NOT] NULL`                |
| `CONSTANT_COMPARISON`        | ✅ `=, <>, <, >, <=, >=` vs literal  |
| `CONJUNCTION_AND/OR`         | ✅ recursive translation              |
| `IN_FILTER`                  | ✅ `col IN (…)`                       |
| `OPTIONAL_FILTER`            | ✅ unwrap → child translation         |
| `STRUCT_EXTRACT`, `EXPRESSION_FILTER`, `DYNAMIC_FILTER` | ⛔ residual (DuckDB applies) |

Literal formatting covers TINYINT…HUGEINT, FLOAT/DOUBLE, DECIMAL,
VARCHAR (single-quoted with embedded-quote escape), DATE, TIME,
TIMESTAMP, BOOLEAN, and NULL.

## Parallel scan

Single-column INTEGER / BIGINT primary keys are partitioned and scanned
in parallel:

1. **Bind** probes `RDB$INDICES ⋈ RDB$RELATION_CONSTRAINTS ⋈
   RDB$INDEX_SEGMENTS` (three round-trips) for a single-column PK, then
   runs one `MIN/MAX(pk)` query which Firebird answers from the PK index.
2. **InitGlobal** slices the `[min, max]` range into N partitions (by
   equal numeric width — coarse but cheap). N defaults to a conservative
   pick (one partition for ranges under ~4 M rows, then up to
   `hardware_concurrency`), and is overridable via the named parameter
   `partitions=`. The outer partitions are open-ended (the first has no
   lower bound, the last no upper bound), so rows outside the probed
   `[min, max]` (committed after the probe, or after an ATTACH cached it)
   are still read; stale bounds only affect how evenly rows spread.
3. **Each worker** has its own `FirebirdConnection`, pulls one
   `PartitionSpec` at a time off a mutex-protected queue, opens a cursor
   scoped to that PK window, drains it, then pops the next.

The query builder splices the partition predicate into the SQL with
proper `WHERE` / `AND` glue, so projection + filter pushdown + partition
window all coexist.

### Why the conservative default?

Local benchmarks against **Firebird 3.0 SuperServer** showed naive
partitioning is *slower* than serial: 4 partitions over 1 M rows took
2.7 s versus the 2.2 s single-thread baseline. The server scheduler
serializes queries against one database file, so extra cursors only add
overhead. The behaviour reverses on remote / Classic / SuperClassic
deployments where parallelism is cheap — hence `partitions=` is an
explicit opt-in for those setups.

| Setting          | Behaviour                                                    |
|---|---|
| `partitions=0` (default) | Auto-pick. Skip parallelism unless PK range > 4 M rows. |
| `partitions=1`           | Force serial **and** skip the PK probe (3 RDB$ round-trips saved — best for interactive small-table queries). |
| `partitions=N` (N ≥ 2)   | Force exactly N partitions, capped by the actual row range. |

## Type mapping

| Firebird (sqltype, subtype, scale) | DuckDB LogicalType                       |
|---|---|
| `SQL_TEXT` / `SQL_VARYING`           | `VARCHAR`                                |
| `SQL_SHORT/LONG/INT64`, scale = 0    | `SMALLINT` / `INTEGER` / `BIGINT`        |
| `SQL_SHORT/LONG/INT64`, scale < 0    | `DECIMAL(width, -scale)` (widths 4/9/18) |
| `SQL_FLOAT`                          | `FLOAT`                                  |
| `SQL_DOUBLE` / `SQL_D_FLOAT`         | `DOUBLE`                                 |
| `SQL_TIMESTAMP`                      | `TIMESTAMP` (MJD epoch offset)           |
| `SQL_TYPE_DATE`                      | `DATE` (MJD epoch offset)                |
| `SQL_TYPE_TIME`                      | `TIME`                                   |
| `SQL_BOOLEAN`                        | `BOOLEAN`                                |
| `SQL_BLOB`, subtype = 1              | `VARCHAR` (whole blob materialized)      |
| `SQL_BLOB`, otherwise                | `BLOB`                                   |
| Anything else                        | `VARCHAR` (degraded — TODO: HUGEINT, DECIMAL(38), TIMESTAMP_TZ) |

Schema discovery (`LoadTableSchema`) reads `RDB$RELATION_FIELDS` joined
with `RDB$FIELDS` — one round-trip per table — and translates Firebird's
internal `RDB$FIELD_TYPE` codes (7, 8, 14, 35, …) into the SQL-level
XSQLDA codes the runtime uses.

## Discovery — `firebird_tables()`

```sql
SELECT * FROM firebird_tables('firebird://…');
```

returns

```
 table_name | column_count | has_pk | pk_column
------------+--------------+--------+-----------
 EMPLOYEE   |            7 | true   | EMP_ID
 ORDERS     |           12 | true   | ORDER_ID
 …
```

Implementation: one query against `RDB$RELATIONS` with correlated
sub-selects for column count and PK column. Filters out system tables
(`RDB$SYSTEM_FLAG = 0`) and non-persistent relations (views, GTTs).

## libfbclient interaction

The client wrapper (`firebird_client.cpp`) uses the legacy ISC API
(`isc_attach_database`, `isc_dsql_prepare`, `isc_dsql_fetch`) and owns
one read-only `isc_tr_handle` per connection. The transaction is
configured with `isc_tpb_read | isc_tpb_read_committed |
isc_tpb_rec_version | isc_tpb_nowait | isc_tpb_no_auto_undo`, matching
the long-running scan profile used by the peregrine extractor
(Strategies 3 and 4 in `PERFORMANCE_OPTIMIZATION_REPORT.md`).

Cancellation: DuckDB checks its interrupt flag between chunks, but one
Firebird call (`isc_dsql_execute` of a sorting query, `isc_dsql_fetch` of
an aggregate) can block in the client library for minutes. While a scan
chunk, a `firebird_query` call or a scan's setup talks to Firebird, a
`FirebirdCancelGuard` registers its connection with one shared watcher
thread (`firebird_cancel.cpp`), which checks the query's interrupt flag
every 100 ms and, once it is set, calls
`fb_cancel_operation(fb_cancel_raise)` on that attachment. The blocked call
fails with "operation was cancelled". `FirebirdConnection::CheckStatus`
throws that error as DuckDB's `InterruptException` only after a cancel the
watcher sent; any other error, or the same error from a DBA's
`DELETE FROM MON$STATEMENTS`, keeps its own message. The connection is
marked broken, so the pool drops it. A raise can go unanswered (a call
that sends one short request after another was seen waiting on the
server's reply for good, on Firebird 3, 4 and 5), so a guard still
registered 1 s after the raise gets `fb_cancel_abort`: the client closes
the attachment's network connection, the blocked call fails whatever the
server does, and the server rolls the transaction back.

Each cancel runs on a short-lived thread of its own, so one that blocks
holds up neither the watcher nor another connection's cancel. In the
client library a raise sends one small packet under the port's write
lock, so it blocks only when the network send does; an abort closes the
socket. The library runs the cancels of one attachment one at a time, so
a raise that blocks also delays that attachment's abort. Registering
costs one mutex round trip per chunk, not per row.

Guarded: a scan's chunks, `firebird_query`'s open and fetches, and a
scan's setup (the catalog round trips of `firebird_scan()`'s bind, the
shared-snapshot coordinator, the DB_KEY geometry, the fallback PK probe,
a worker joining the snapshot, the start of the transaction snapshot).
Not guarded, so an interrupt waits for the call to return: opening a
connection (an attach has no handle to cancel yet), loading an ATTACH's
table metadata, and the round trips that keep a transaction snapshot
alive. For tests, `DUCKDB_FIREBIRD_TEST_CANCEL_ABORT=1` in the environment
makes the first cancel the abort (`scripts/check_cancel.py`).

XSQLDA buffers are owned by `FirebirdStatement` and reused across every
`isc_dsql_fetch`. BLOBs go through a separate `isc_open_blob2` /
`isc_get_segment` loop and are materialized into a single DuckDB
`string_t` / blob value.

## Writes

Writes are opt-in per ATTACH, with DuckDB's own `READ_WRITE` option
(`FirebirdCatalog::write_enabled`); without it nothing below happens. The
read side is unchanged: scans and `firebird_query` keep the read pool and
its read-only transaction.

- **A second pool per ATTACH** (`FirebirdCatalog::WritePool()`). A
  connection that held a write transaction never serves a read; it goes
  back to its pool on a fresh read-only transaction.
- **Session reset.** A connection that ran caller SQL (a write
  transaction, a `firebird_query`) is marked
  (`FirebirdConnection::MarkSessionChanged`, when `firebird_pool_session_reset`
  is true); `FirebirdConnectionPool::Release` runs `ALTER SESSION RESET` on
  a marked connection before parking it (Firebird 4+; a failure discards
  it). The reset runs in the parked read-only transaction, which the
  server restarts; it would roll back a write transaction's uncommitted
  work, so it only ever runs after the commit or rollback.
- **The write transaction** (`FirebirdConnection::StartWriteTransaction`):
  `isc_tpb_write | isc_tpb_read_committed | isc_tpb_rec_version |
  isc_tpb_wait` with `isc_tpb_lock_timeout` from `firebird_write_lock_timeout`
  (`isc_tpb_nowait` for 0).
- **It follows the DuckDB transaction.** `FirebirdTransaction` starts it on
  the first write (`GetFirebirdWriteConnection`) and commits or rolls it
  back in `FirebirdTransactionManager::CommitTransaction` /
  `RollbackTransaction`. Autocommit gives one Firebird transaction per
  statement; `BEGIN ... COMMIT` gives one for all its writes.
- **`firebird_execute`** (`firebird_execute.cpp`) prepares the statement
  on the write connection, refuses transaction control, runs it (reading
  and discarding the rows of a statement that returns rows) and reads the
  record counts with `isc_dsql_sql_info` (`isc_info_sql_records`). With
  `source`, it runs the DuckDB query on its own `Connection`, reads it as
  a stream (`SendQuery` on DuckDB 1.5, `Submit` and `QueryResultStream`
  on 2.0) with that connection's streaming buffer set to 1 KB, so DuckDB
  computes a chunk or two ahead of the writes, and executes the prepared
  statement once per row.
- **Parameters** (`FirebirdParamEncoder`, `firebird_param_encoder.cpp`):
  `isc_dsql_describe_bind` gives the parameters; each DuckDB value is put
  in the input XSQLDA in a type that represents it exactly (integers as
  `SQL_INT64`, `DECIMAL(p,s)` as `SQL_INT64` with `sqlscale = -s`,
  `SQL_INT128` on Firebird 4+, text as `SQL_VARYING` in the parameter's
  character set, BLOBs created with `isc_create_blob2`), and the server
  converts it to the declared type. Every execution resets type, length,
  data pointer and NULL indicator of every parameter. The scan's
  pushed-down filter values go through the same encoder.
- **`INSERT INTO fb.main.T`** (`firebird_insert.cpp`):
  `FirebirdCatalog::PlanInsert` builds a sink (`FIREBIRD_INSERT`) that
  prepares `INSERT INTO "T" ("C1", ...) VALUES (?, ...)` once, with the
  columns the INSERT names, and runs it once per row through the
  parameter encoder. DuckDB 1.5 gives the column map in
  `LogicalInsert::column_index_map`; DuckDB 2.0 passes every column, and
  the named ones are those whose `bound_defaults` entry the binder left
  in place.
- **A result read halfway keeps the write.** DuckDB commits a streamed
  result only once it is fully read. On DuckDB 1.5 the bind sets
  `QueryResultOutputType::FORCE_MATERIALIZED`; DuckDB 2.0 has no such
  property, so there, in autocommit, `firebird_execute` commits its write
  transaction before it returns its row (`CommitFirebirdWriteNow`). Both
  register the write at bind (`RegisterDBModify`).
- **DDL** (`isc_info_sql_stmt_ddl`) first closes the ATTACH's idle
  connections (their metadata cache would make Firebird refuse DROP /
  ALTER of tables they used: "object ... is in use"), then marks the
  transaction (`NoteFirebirdDdl`); when it commits, the catalog clears
  its table cache, so `fb.main` loads the tables again.
  `firebird_clear_cache(alias)` does the same on demand.

## Build

The build follows the standard DuckDB out-of-tree extension layout:

```bash
# One-time:
git submodule update --init --recursive

# Linux build + test (Firebird 3.0 server running):
GEN=ninja make release
FIREBIRD_TEST_DB=/tmp/fbdata/test.fdb ISC_PASSWORD=… \
    ./build/release/test/unittest test/sql/firebird_scan.test

# Result:
build/release/extension/firebird/firebird.duckdb_extension
```

Linkage: on Linux `pkg_check_modules(fbclient)` or a bare `-lfbclient`
fallback. On Windows / macOS the workflow downloads the Firebird SDK
ZIP, exposes its location through `FB_SDK_ROOT`, and `CMakeLists.txt`
picks up `ibase.h` + `fbclient_ms.lib` from there.

## "ATTACH"-style UX via `firebird_attach_sql`

A full `StorageExtension` (so `ATTACH 'fb://…' AS fb (TYPE firebird)`
works and resolves `fb.SCHEMA.TABLE` natively) is the next milestone.
Until then the extension ships `firebird_attach_sql`, which emits the
exact DDL needed to wire up a CREATE SCHEMA + one CREATE OR REPLACE
VIEW per table:

```sql
COPY (SELECT sql FROM firebird_attach_sql('firebird://…', 'fb'))
   TO 'fb_attach.sql' (FORMAT 'csv', HEADER false, QUOTE '');
.read fb_attach.sql
-- Now everything is queryable as fb.<TABLE>, with pushdown intact:
SELECT * FROM fb."EMPLOYEE" WHERE DEPT_NO IN ('600', '700');
```

We tried emitting the DDL *and* executing it from inside the table
function (calling `ClientContext::Query()` during `InitGlobal`), but
that path deadlocks the client lock — the outer query is already
holding it. Returning the DDL as data, plus a CLI-side `.read`, is the
robust workaround until the real catalog plumbing lands.

The legacy view of this trick (verified by hand, sql shown for
context):

```sql
CREATE SCHEMA IF NOT EXISTS fb;

WITH t AS (
    SELECT table_name
      FROM firebird_tables('firebird://SYSDBA:masterkey@db/data/prod.fdb')
)
SELECT printf(
   'CREATE OR REPLACE VIEW fb.%I AS '
   'SELECT * FROM firebird_scan(''firebird://SYSDBA:masterkey@db/data/prod.fdb'', ''%s'');',
   table_name, table_name
) FROM t;
-- Then copy-paste the resulting CREATE VIEW … into the prompt, OR pipe
-- the result through duckdb -c "$(...)" to apply in one shot.

-- After running the generated DDL:
SELECT * FROM fb.EMPLOYEE WHERE DEPT_NO = '600';
```

This sidesteps the catalog plumbing for now; users get federated
queries against Firebird through familiar `schema.table` syntax, and
the views inherit every pushdown the scanner supports.

## Deferred work (as of v1.1.0)

Items shipped since the original v0.2 cut (Native `ATTACH`, projection
and predicate pushdown, prepared statements, PK-range partitioning,
HUGEINT / DECIMAL(38) / TIMESTAMP_TZ types, `CHARACTER SET NONE`
handling, `information_schema`, the Metadata Bridge and diagnostics
surface, the connection pool, row estimates, and in-band
`duckdb_functions()` documentation) are tracked in the
[Current Status](../../README.md#current-status) section of the README.

The items below remain open:

| Item                                       | Notes |
|---|---|
| Automatic `LIMIT` pushdown into Firebird SQL | Needs a `TableFunction` limit-pushdown hook that DuckDB v1.5.x does not expose to storage extensions. Manual `row_limit=` / `row_offset=` already works, with deterministic `RDB$DB_KEY`/PK ordering. |
| Aggregate pushdown (`COUNT/MIN/MAX`, no joins) | Blocked by the same v1.5.x table-filters API gap. DuckDB applies aggregates above the scan. |
| Broader `LIKE` / regex pushdown            | Selective `LIKE 'prefix%'` is shipped (v0.5). Broader patterns (`%word%`, regex) still sit in `EXPRESSION_FILTER` and remain residual in DuckDB. |
| Stable C extension ABI                     | The Stable C ABI (`duckdb_extension.h`) does **not** support `StorageExtension` — only scalar / aggregate / table / replacement functions. Migrating today would mean losing native `ATTACH ... AS fb (TYPE firebird)`. Tracked upstream in [duckdb/duckdb](https://github.com/duckdb/duckdb); once the C ABI gains storage-extension support, we revisit. |
| Scanner-native Arrow `RecordBatch` output  | The scanner produces DuckDB `Vector` / `DataChunk` columns; Arrow conversion happens at the DuckDB/GizmoSQL boundary. Only worth building with a consumer that measures a real win. |

## References

- `firebird_peregrine_falcon/src/extractor.rs` — Rust prior art: PK
  partitioning, type mapping, connection pool. The PK probe queries,
  read-only TPB shape, and RDB$ schema-discovery layout in this
  extension trace directly to it.
- `firebird_peregrine_falcon/PERFORMANCE_OPTIMIZATION_REPORT.md` — the
  23-strategy report motivating the parallel-scan work and the
  conservative-by-default heuristic.
- `firebird/src/include/firebird/impl/sqlda_pub.h` — canonical XSQLDA
  constants used in `firebird_types.cpp`.
- DuckDB
  [`postgres_scanner`](https://github.com/duckdb/postgres_scanner) and
  [`sqlite_scanner`](https://github.com/duckdb/sqlite_scanner) — same
  `TableFunction` + `StorageExtension` shape we are targeting for the
  ATTACH milestone.
