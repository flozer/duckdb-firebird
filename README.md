# duckdb-firebird

A DuckDB extension that lets you query Firebird databases directly from
DuckDB SQL — with projection and filter pushdown — so you can run
federated queries over Firebird, Parquet, CSV, S3 and the rest of the
DuckDB ecosystem from a single planner.

> **Status:** prototype. Phase 1 is in place: `firebird_scan(...)` table
> function with projection + filter pushdown, single-threaded scan. PK-range
> parallelism, `ATTACH`-based catalogs and a connection pool are designed
> and queued — see [`docs/architecture.md`](docs/architecture.md).

## Why

If you already use Firebird for OLTP and have been exporting Parquet to do
analytics on top, this extension removes the export step: DuckDB talks to
Firebird through `libfbclient` and the optimiser pushes filters/projections
to the source. Big aggregations still happen inside DuckDB's vectorized
engine — you just stop maintaining a parallel ETL pipeline.

The design borrows directly from
[`firebird_peregrine_falcon`](../firebird_peregrine_falcon): same read-only
transaction profile, same `RDB$RELATION_FIELDS` schema probe, same
plan-for-PK-range parallelism (deferred to v0.2).

## Quick start

```sql
LOAD firebird;

-- 1. Live scan
SELECT * FROM firebird_scan(
    'firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb?charset=UTF8',
    'EMPLOYEE'
) LIMIT 10;

-- 2. Projection + filter pushdown — only EMP_NO, FIRST_NAME and the
-- WHERE clause are sent to Firebird:
SELECT EMP_NO, FIRST_NAME
  FROM firebird_scan('firebird://…', 'EMPLOYEE')
 WHERE DEPT_NO = '600'
   AND HIRE_DATE > DATE '2020-01-01';

-- 3. Federated join with a Parquet file
SELECT  e.dept_no, COUNT(*), AVG(e.salary)
  FROM  firebird_scan('firebird://…', 'EMPLOYEE') e
  JOIN  read_parquet('s3://lake/departments/*.parquet') d
   ON   e.dept_no = d.dept_no
 GROUP  BY e.dept_no;

-- 4. Materialize a snapshot
CREATE TABLE local_emp AS
  SELECT * FROM firebird_scan('firebird://…', 'EMPLOYEE');
```

### Connection strings

Two forms accepted:

```text
firebird://USER:PASS@HOST:PORT/DB_PATH?charset=UTF8&dialect=3&role=…
user=SYSDBA;password=masterkey;database=server:/data/db.fdb;charset=UTF8
```

A bare path (e.g. `/var/lib/firebird/test.fdb`) is also accepted for local
databases; defaults apply (`SYSDBA` / `masterkey`, UTF-8, dialect 3).

### Named parameter overrides

```sql
SELECT * FROM firebird_scan('/data/prod.fdb', 'EMPLOYEE',
                            user='analytics',
                            password='secret',
                            charset='WIN1252',
                            role='READER',
                            dialect=3,
                            partitions=4);
```

- `partitions` — force the PK-range parallelism level. `0` (default) auto-
  picks (conservative: only kicks in above ~4 M rows). `1` forces serial
  and skips the PK probe entirely (best for interactive queries on small
  tables). `N>=2` partitions the PK range explicitly — useful on remote
  Firebird or Classic / SuperClassic deployments where parallelism is
  cheap; counterproductive on local SuperServer where the server
  scheduler serializes queries.

### Discover the schema

```sql
-- List user tables, their column counts and primary-key columns:
SELECT * FROM firebird_tables('firebird://…');
-- table_name | column_count | has_pk | pk_column
-- EMPLOYEE   |            7 | true   | EMP_ID
-- ORDERS     |           12 | true   | ORDER_ID
-- DEPT       |            3 | false | NULL
```

### Native ATTACH

```sql
ATTACH 'firebird://SYSDBA:masterkey@host/path/db.fdb' AS fb (TYPE firebird);

-- Every Firebird user table is now reachable through DuckDB's catalog:
SELECT * FROM fb.main.EMPLOYEE WHERE DEPT_NO = '600';
SELECT COUNT(*) FROM fb.main.employee;          -- case-insensitive
DESCRIBE fb.main.EMPLOYEE;                       -- DuckDB sees real columns

-- Federated:
SELECT e.EMP_NAME, d.label
  FROM fb.main.EMPLOYEE e
  JOIN read_parquet('s3://lake/dept.parquet') d ON e.DEPT_NO = d.dept_no;

DETACH fb;
```

The catalog is read-only: `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE` and
`ALTER` against `fb.*` all raise `NotImplemented`. Connection options
(`user`, `password`, `charset`, `role`, `dialect`) can be supplied via the
URL or via attach options:

```sql
ATTACH 'C:/data/prod.fdb' AS fb (
    TYPE firebird,
    user 'analytics',
    password 'secret',
    charset 'WIN1252');
```

Only the `main` schema is exposed (Firebird's flat-table model). Table
names round-trip case-insensitively — `fb.main.EMPLOYEE`, `fb.main.employee`
and `fb.main."EMPLOYEE"` all resolve to the same table.

### Lightweight "attach" via DDL emission

`firebird_attach_sql` emits the DDL needed to create a CREATE SCHEMA + one
CREATE OR REPLACE VIEW per Firebird table. After running the DDL, every
Firebird table is reachable through the local DuckDB catalog — no
`firebird_scan(...)` boilerplate per query:

```sql
-- 1. Generate the DDL.
COPY (SELECT sql FROM firebird_attach_sql('firebird://…', 'fb'))
   TO 'fb_attach.sql' (FORMAT 'csv', HEADER false, QUOTE '');
-- 2. Run it.
.read fb_attach.sql
-- 3. Query as if everything were local. Projection + filter pushdown
--    still reach the firebird_scan call wrapped by the view.
SELECT * FROM fb."EMPLOYEE" WHERE DEPT_NO = '600';
```

A real `ATTACH 'fb://…' AS fb (TYPE firebird)` (full `StorageExtension`) is
on the roadmap; the view-based recipe above covers the same federated-
read use case in the meantime.

## Build

This follows the standard DuckDB out-of-tree extension layout.

```bash
# One-time:
git submodule add https://github.com/duckdb/duckdb.git
git submodule add https://github.com/duckdb/extension-ci-tools.git
git submodule update --init --recursive

# Dependencies:
#   - libfbclient (Firebird client library)
#       Debian/Ubuntu:  apt install firebird-dev
#       macOS:          brew install firebird   (or use the vcpkg manifest)
#       Windows/CI:     vcpkg install libfbclient

# Build:
GEN=ninja make debug
GEN=ninja make release

# Run SQL tests (requires a reachable Firebird server — see test/sql/):
make test_debug

# Built artifact:
ls build/release/extension/firebird/firebird.duckdb_extension
```

## What's implemented (v0.2)

| Feature                                                 | Status |
|---|---|
| `firebird_scan(conn, table)`                            | ✅ |
| `firebird_tables(conn)` — list user tables + PK info    | ✅ |
| `firebird_attach_sql(conn[, schema])` — DDL for view-based attach | ✅ |
| **`ATTACH 'firebird://…' AS fb (TYPE firebird)`** — native catalog | ✅ |
| Named parameter overrides (user / password / charset / role / dialect / **partitions**) | ✅ |
| Projection pushdown                                     | ✅ |
| Filter pushdown — `=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `IS NULL`, `BETWEEN`, **`IN(…)`**, optional-filter unwrap | ✅ |
| PK-range parallel scan (opt-in via `partitions=N`)      | ✅ |
| Type mapping — INTEGER / VARCHAR / DECIMAL(scale) / DATE / TIMESTAMP / TIME / BOOLEAN / BLOB SUB_TYPE 1 → VARCHAR / BLOB | ✅ |
| End-to-end test fixture in CI (Linux + real Firebird 3) | ✅ |
| Windows x64 build via GitHub Actions                    | ✅ |
| LIMIT pushdown                                          | 🟡 partial (builder accepts limit; planner hook tbd) |
| `ATTACH 'fb://…' AS fb (TYPE firebird)`                 | ⏳ next |
| Stable C extension ABI                                  | ⏳ next |

## Repository layout

```
src/
├── firebird_extension.cpp      Extension entry point
├── firebird_client.cpp         libfbclient wrapper
├── firebird_types.cpp          Firebird ⇄ DuckDB type mapping
├── firebird_query.cpp          SQL builder with pushdown
├── firebird_scanner.cpp        firebird_scan table function
└── include/
test/sql/                       sqllogictest fixtures
docs/architecture.md            Design notes + deferred-work plan
CMakeLists.txt + Makefile       Standard DuckDB extension build
vcpkg.json                      libfbclient dependency
```

## License

MIT — see [`LICENSE`](LICENSE).

## Credits

Design and type-mapping decisions trace directly to
[`firebird_peregrine_falcon`](https://github.com/flozer/firebird_peregrine_falcon)
(the 1 M rows/s Firebird → Parquet extractor); look at its
`src/extractor.rs` and `PERFORMANCE_OPTIMIZATION_REPORT.md` for the
upstream prior art.
