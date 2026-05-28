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
   `partitions=`.
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

XSQLDA buffers are owned by `FirebirdStatement` and reused across every
`isc_dsql_fetch`. BLOBs go through a separate `isc_open_blob2` /
`isc_get_segment` loop and are materialized into a single DuckDB
`string_t` / blob value.

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

## Deferred work (as of v0.5.1)

Items shipped since the original v0.2 cut (Native `ATTACH`, projection
and predicate pushdown, prepared statements, PK-range partitioning,
HUGEINT / DECIMAL(38) / TIMESTAMP_TZ types, `CHARACTER SET NONE`
handling, `information_schema`) are tracked in the
[Current Status](../../README.md#current-status) section of the README.

The items below remain open:

| Item                                       | Notes |
|---|---|
| Connection pool                            | Each LocalState opens its own connection. With native `ATTACH` shipped, short-lived connections happen via catalog metadata calls — a pool is now worth measuring. |
| Automatic `LIMIT` pushdown into Firebird SQL | The query builder accepts a limit; needs wiring through [DuckDB](https://github.com/duckdb/duckdb)'s `TableFunction` limit pushdown hook. Modest win — DuckDB already stops calling the scan early once the limit is hit. Manual `row_limit=` / `row_offset=` already works. |
| Broader `LIKE` / regex pushdown            | Selective `LIKE 'prefix%'` is shipped (v0.5). Broader patterns (`%word%`, regex) still sit in `EXPRESSION_FILTER` and remain residual in DuckDB. |
| Stable C extension ABI                     | The Stable C ABI (`duckdb_extension.h`) does **not** support `StorageExtension` — only scalar / aggregate / table / replacement functions. Migrating today would mean losing native `ATTACH ... AS fb (TYPE firebird)`. Tracked upstream in [duckdb/duckdb](https://github.com/duckdb/duckdb); once the C ABI gains storage-extension support, we revisit. |
| Scanner-native Arrow `RecordBatch` output  | The scanner produces DuckDB `Vector` / `DataChunk` columns; Arrow conversion happens at the DuckDB/GizmoSQL boundary. Native Arrow output is a v1.x candidate. |

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
