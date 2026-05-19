# duckdb-firebird — Architecture

This document explains how the extension is wired together, the design
decisions that shaped the prototype, and the path forward.

## Goal

Make Firebird tables addressable from DuckDB SQL with native pushdown, so a
query like

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

runs as a single federated plan: the date filter is pushed into Firebird,
only the projected columns travel over the wire, and DuckDB's vectorized
executor handles the join + aggregate.

## Layout

```
src/
├── firebird_extension.{hpp,cpp}   Extension entry point (register functions)
├── firebird_client.{hpp,cpp}      Thin C++ wrapper over libfbclient (ibase.h)
├── firebird_types.{hpp,cpp}       Firebird XSQLDA  ⇄  DuckDB LogicalType
├── firebird_query.{hpp,cpp}       SQL builder w/ projection & filter pushdown
└── firebird_scanner.{hpp,cpp}     TableFunction: bind / init / scan
test/sql/firebird_scan.test        sqllogictest fixtures
```

## Data flow

```
                        ┌────────────────────────────────────────┐
                        │  DuckDB planner                        │
                        │  - parses firebird_scan('…', 'TBL')    │
                        │  - calls FirebirdScanBind  ────────────┼──► RDB$ probe
                        │  - decides projection / filter pushdown│   (one round-trip)
                        └────────────────────────────────────────┘
                                       │
                                       ▼
                        ┌────────────────────────────────────────┐
                        │  FirebirdLocalState (per scan thread)  │
                        │  - FirebirdConnection                  │
                        │  - FirebirdStatement (open cursor)     │
                        │  - cursor SQL built from               │
                        │    FirebirdQueryBuilder::Build()       │
                        └────────────────────────────────────────┘
                                       │ isc_dsql_fetch loop
                                       ▼
                        ┌────────────────────────────────────────┐
                        │  FirebirdScanFunction(output: DataChunk)│
                        │  - For each col: FirebirdAppendValue   │
                        │    writes into FlatVector slot          │
                        │  - output.SetCardinality(rows)         │
                        └────────────────────────────────────────┘
```

## Pushdown

`firebird_scanner.cpp` advertises `projection_pushdown = true` and
`filter_pushdown = true`. DuckDB then hands the scanner a `column_ids` list
plus a `TableFilterSet` keyed by **projected** column index. The query
builder:

- Emits exactly the projected columns in the `SELECT` list (always
  double-quoted to preserve catalog casing).
- Walks the `TableFilterSet` and translates the supported subset to native
  Firebird SQL:
  - `IS NULL` / `IS NOT NULL`
  - `= / <> / < / > / <= / >=` against numeric / string / date / time /
    timestamp / boolean literals
  - `AND` / `OR` conjunctions of the above
- Records any filter it cannot translate as a *residual*, which DuckDB
  re-evaluates above the scan. We set `filter_prune = false` for that
  reason — flipping it to `true` once pushdown becomes lossless is a
  follow-up.

Unsupported today (intentional, kept for follow-ups):

- `IN` / `NOT IN`
- `LIKE` / `BETWEEN` (BETWEEN is rewritten as two comparisons by DuckDB, so
  it works in practice)
- HUGEINT / DECIMAL(38, …) literals
- Composite-key / row-comparison filters

## Type mapping

| Firebird (sqltype, subtype, scale) | DuckDB LogicalType                       |
|---|---|
| `SQL_TEXT` / `SQL_VARYING`           | `VARCHAR`                                |
| `SQL_SHORT/LONG/INT64`, scale = 0    | `SMALLINT` / `INTEGER` / `BIGINT`        |
| `SQL_SHORT/LONG/INT64`, scale < 0    | `DECIMAL(width, -scale)` (widths 4/9/18) |
| `SQL_FLOAT`                          | `FLOAT`                                  |
| `SQL_DOUBLE` / `SQL_D_FLOAT`         | `DOUBLE`                                 |
| `SQL_TIMESTAMP`                      | `TIMESTAMP`                              |
| `SQL_TYPE_DATE`                      | `DATE`                                   |
| `SQL_TYPE_TIME`                      | `TIME`                                   |
| `SQL_BOOLEAN`                        | `BOOLEAN`                                |
| `SQL_BLOB`, subtype = 1              | `VARCHAR` (whole blob materialized)      |
| `SQL_BLOB`, otherwise                | `BLOB`                                   |

Schema discovery (`LoadTableSchema` in `firebird_scanner.cpp`) reads
`RDB$RELATION_FIELDS` joined with `RDB$FIELDS` — one round-trip per table —
and translates Firebird's internal `RDB$FIELD_TYPE` codes (7, 8, 14, 35, …)
into the SQL-level XSQLDA codes the runtime uses.

## libfbclient interaction

The client wrapper (`firebird_client.cpp`) uses the legacy ISC API
(`isc_attach_database`, `isc_dsql_prepare`, `isc_dsql_fetch`) and owns one
read-only `isc_tr_handle` per connection. The transaction is configured
with `isc_tpb_read | isc_tpb_read_committed | isc_tpb_rec_version |
isc_tpb_nowait | isc_tpb_no_auto_undo`, matching the long-running scan
profile used by the peregrine extractor (Strategies 3 and 4 in
`PERFORMANCE_OPTIMIZATION_REPORT.md`).

XSQLDA buffers are owned by `FirebirdStatement` and reused across every
`isc_dsql_fetch`. BLOBs go through a separate
`isc_open_blob2`/`isc_get_segment` loop and are materialized into a single
DuckDB `string_t` / blob value.

## What's *not* in the prototype (deferred)

1. **PK-range parallelism.** Today `MaxThreads() == 1`. The next step is
   porting `extract_streaming_parallel` from `extractor.rs`: probe
   `RDB$INDICES` + `RDB$RELATION_CONSTRAINTS` for the PK, run
   `MIN/MAX(pk)`, slice the range into N partitions, and run each in its
   own thread by passing a per-thread `WHERE pk BETWEEN a AND b` clause.
   This is the change that takes scans from ~120 K rows/s (single TCP
   connection) into the 400 K–900 K rows/s range demonstrated by the
   peregrine extractor.
2. **Connection pool.** Per scan we currently open one connection. Once
   parallelism lands, swap in a `crossbeam::ArrayQueue`-style lock-free
   pool (the C++ equivalent is a `moodycamel::ConcurrentQueue` or a
   spin-lock-protected freelist).
3. **`ATTACH 'fb://…' AS fb (TYPE firebird)`.** Implement a
   `StorageExtension` so Firebird tables show up in the DuckDB catalog
   without an explicit `firebird_scan` call. Requires:
   `FirebirdCatalog : Catalog`, `FirebirdSchemaEntry : SchemaCatalogEntry`,
   `FirebirdTableEntry : TableCatalogEntry`. Each `Scan` on the table
   entry funnels back into the existing scanner.
4. **LIMIT pushdown via DuckDB hint.** The query builder already accepts a
   `limit` argument; we just need to wire DuckDB's `LimitPushdown` event
   into `FirebirdLocalState`.
5. **Stable C extension API.** The extension currently uses the C++ ABI,
   matching `postgres_scanner` and `sqlite_scanner`. Migrating to
   DuckDB's stable C API (`duckdb/duckdb_extension.h`) would let one
   binary work across DuckDB versions. Done as a v0.2 effort once the
   feature set stabilises.

## Build

The build is the standard DuckDB out-of-tree shape:

```bash
# One-time:
git submodule add https://github.com/duckdb/duckdb.git
git submodule add https://github.com/duckdb/extension-ci-tools.git
git submodule update --init --recursive

# Build:
GEN=ninja make debug          # debug
make release                  # release
make test_debug               # run SQL tests

# Result:
build/{debug,release}/extension/firebird/firebird.duckdb_extension
```

Linkage: `find_package(PkgConfig)` + `pkg_check_modules(FBCLIENT fbclient)`
locates libfbclient. On vcpkg-based builds the dependency is declared in
`vcpkg.json`.

## References

- `firebird_peregrine_falcon/src/extractor.rs` — Rust prior art: PK
  partitioning, type mapping, connection pool. Many design choices here
  (read-only / no-auto-undo TPB, RDB$ schema probe, BLOB sub_type 1 →
  text) trace directly to it.
- `firebird_peregrine_falcon/PERFORMANCE_OPTIMIZATION_REPORT.md` — the
  23-strategy report that motivates the deferred parallelism work.
- `firebird/src/include/firebird/impl/sqlda_pub.h` — canonical XSQLDA
  constants used in `firebird_types.cpp`.
- DuckDB
  [`postgres_scanner`](https://github.com/duckdb/postgres_scanner) and
  [`sqlite_scanner`](https://github.com/duckdb/sqlite_scanner) — same
  TableFunction + StorageExtension shape we are targeting.
