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
                            dialect=3);
```

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

## What's implemented (v0.1 prototype)

| Feature                              | Status        |
|---|---|
| `firebird_scan(conn, table)`         | ✅ Yes        |
| Named parameter overrides            | ✅ Yes        |
| Projection pushdown                  | ✅ Yes        |
| Filter pushdown (=, <>, <, >, <=, >=, AND, OR, IS NULL) | ✅ Yes |
| LIMIT pushdown                       | 🟡 Plumbed, not wired |
| Type mapping (numeric, text, date, time, timestamp, blob, decimal) | ✅ Yes |
| PK-range parallel scan               | ⏳ Designed (`docs/architecture.md`) |
| Connection pool                      | ⏳ Designed   |
| `ATTACH 'fb://…' AS fb (TYPE firebird)` | ⏳ Designed |
| Stable C extension ABI               | ⏳ v0.2       |

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
