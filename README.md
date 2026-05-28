<div align="center">
  <h1>duckdb-firebird</h1>
  <p><strong>Query Firebird directly from <a href="https://github.com/duckdb/duckdb">DuckDB</a>.</strong></p>
  <p>
    Federated analytics over Firebird, Parquet, CSV, S3, and local DuckDB tables,
    with pushdown, native ATTACH, legacy charset handling, and materialization paths.
  </p>
  <p>
    <a href="LICENSE"><img alt="license MIT" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
    <a href="https://github.com/flozer/duckdb-firebird/releases/tag/v0.5.1"><img alt="release v0.5.1" src="https://img.shields.io/badge/release-v0.5.1-blue.svg"></a>
    <a href="https://github.com/flozer/duckdb-firebird/actions/workflows/build-linux-fb-matrix.yml"><img alt="linux matrix" src="https://github.com/flozer/duckdb-firebird/actions/workflows/build-linux-fb-matrix.yml/badge.svg"></a>
    <a href="https://github.com/duckdb/community-extensions/pull/1980"><img alt="community PR" src="https://img.shields.io/badge/DuckDB%20community-PR%20open-yellow.svg"></a>
  </p>
  <p>
    <a href="docs/en/usage_guide.md">Usage guide</a> |
    <a href="docs/pt/function_manual.md">Function manual</a> |
    <a href="docs/en/roadmap.md">Roadmap</a> |
    <a href="CONTRIBUTING.md">Contributing</a> |
    <a href="CODE_OF_CONDUCT.md">Code of conduct</a> |
    <a href="SECURITY.md">Security</a>
  </p>
</div>

`duckdb-firebird` is a DuckDB extension for read-only analytics on Firebird
databases. It lets analysts keep Firebird as the transactional source of truth
and use DuckDB as the local OLAP engine for ad hoc analysis, materialized
tables, Parquet exports, lakehouse-style caches, and BI serving through tools
such as GizmoSQL.

## Principles

- **Read-only by design** - optimized for analytics without mutating the OLTP
  source.
- **Push work to Firebird when safe** - projection, filters, paging, and
  PK-range partitioning reduce wire traffic.
- **Keep DuckDB in charge of analytics** - joins, aggregations, Parquet/S3, and
  local materializations stay in DuckDB's vectorized engine.
- **Legacy friendly** - `CHARACTER SET NONE` databases default to
  `none_encoding='win1252'`, with `strict`, `iso8859_1`, and `blob` available.
- **Public-repo ready** - security policy, contribution docs, CI, Dependabot,
  and branch protection are in place.

## Features

- **`firebird_scan(conn, table)`** - scan a Firebird table from DuckDB SQL.
- **Native `ATTACH`** - query Firebird tables as `fb.main.TABLE`.
- **Metadata discovery** - `SHOW TABLES`, `DESCRIBE`, and
  `information_schema.tables` / `information_schema.columns`.
- **Projection pushdown** - only requested columns are fetched.
- **Predicate pushdown** - comparisons, `IS NULL`, `BETWEEN`, `IN`, `NOT IN`,
  `NOT bool`, `AND` / `OR`, and safe `LIKE 'prefix%'` cases.
- **Streaming batches** - rows flow into DuckDB `DataChunk`s instead of a
  whole-table in-memory copy.
- **Prepared statements + bind variables** - string/date/numeric filters avoid
  unsafe SQL interpolation.
- **Paging** - `row_limit=N` and `row_offset=M` map to Firebird `ROWS`.
- **Parallel PK-range scans** - opt in with `partitions=N`.
- **Firebird 3/4/5 type mapping** - including `INT128`, `DECIMAL(38)`,
  `TIMESTAMP WITH TIME ZONE`, text BLOBs, binary BLOBs, booleans, dates, and
  timestamps.
- **DuckDB ecosystem output** - materialize to DuckDB tables, Parquet, S3/MinIO,
  or serve through Arrow Flight SQL via GizmoSQL.

## Quick Start

```sql
LOAD firebird;

-- Live scan
SELECT *
FROM firebird_scan(
    'firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb?charset=UTF8',
    'EMPLOYEE'
)
LIMIT 10;

-- Projection + predicate pushdown
SELECT EMP_NO, FIRST_NAME
FROM firebird_scan('firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb', 'EMPLOYEE')
WHERE DEPT_NO = '600'
  AND HIRE_DATE >= DATE '2020-01-01';

-- Native catalog
ATTACH 'firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb' AS fb
    (TYPE firebird);

SELECT *
FROM fb.main.EMPLOYEE
WHERE DEPT_NO = '600';
```

## Analyst Workflow

Use live scans when you need fresh operational data:

```sql
SELECT o.CUSTOMER_ID, SUM(o.TOTAL_VALUE) AS revenue
FROM fb.main.ORDERS o
WHERE o.ORDER_DATE >= DATE '2026-01-01'
GROUP BY 1;
```

Materialize when the query is reused or joins large historical tables:

```sql
CREATE OR REPLACE TABLE mart_orders_2026 AS
SELECT *
FROM fb.main.ORDERS
WHERE ORDER_DATE >= DATE '2026-01-01';
```

Export to Parquet for a local lake cache:

```sql
COPY (
    SELECT *
    FROM fb.main.ORDERS
    WHERE ORDER_DATE >= DATE '2026-01-01'
)
TO 'lake/erp/orders_2026.parquet'
(FORMAT PARQUET);
```

For a longer, analyst-oriented walkthrough, including materialized tables,
incremental refresh, Parquet partitioning, MinIO, and Power BI/GizmoSQL usage,
see [docs/en/usage_guide.md](docs/en/usage_guide.md). For a public how-to of
each SQL function and option, see
[docs/pt/function_manual.md](docs/pt/function_manual.md).

## Connection Strings

```text
firebird://USER:PASS@HOST:PORT/DB_PATH?charset=UTF8&dialect=3&role=ROLE
user=SYSDBA;password=masterkey;database=server:/data/db.fdb;charset=UTF8
C:/data/local.fdb
/var/lib/firebird/data/local.fdb
```

Security note: connection strings may end up in shell history, notebooks,
DuckDB logs, or BI configuration. Use least-privilege Firebird users and avoid
committing real credentials.

## Named Parameters

```sql
SELECT *
FROM firebird_scan(
    '/data/prod.fdb',
    'EMPLOYEE',
    user='analytics',
    password='secret',
    charset='UTF8',
    role='READER',
    dialect=3,
    partitions=4,
    row_limit=100,
    row_offset=200
);
```

`row_offset` requires `row_limit`. Paging is a global Firebird slice and runs
serially; explicit `partitions > 1` with paging is rejected to avoid returning
incorrect slices.

## Charset Handling

DuckDB strings are UTF-8. Firebird databases declared with a real character set
can usually use the default `charset=UTF8`; Firebird transliterates values on
the wire.

`CHARACTER SET NONE` is different: Firebird returns raw bytes. The extension
defaults to `none_encoding='win1252'`, matching many legacy Brazilian and
Western-European ERPs. Override it when needed:

| `none_encoding` | Behavior |
|---|---|
| `win1252` | Default. Decode NONE text bytes as Windows-1252 to UTF-8. |
| `iso8859_1` / `latin1` | Decode NONE text bytes as ISO-8859-1 to UTF-8. |
| `strict` | Accept only already-valid UTF-8; error on invalid bytes. |
| `blob` | Surface NONE text columns as DuckDB `BLOB` raw bytes. |

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA',
                   none_encoding='win1252');

ATTACH 'C:/legacy/erp.fdb' AS erp
    (TYPE firebird, user 'SYSDBA', password 'masterkey',
     none_encoding 'win1252');
```

When `none_encoding` is not `strict`, text filter pushdown on NONE columns is
disabled deliberately. DuckDB applies those filters after transcoding so query
results remain correct.

## Metadata

```sql
SHOW TABLES FROM fb;
DESCRIBE fb.main.EMPLOYEE;

SELECT table_name
FROM information_schema.tables
WHERE table_schema = 'main';

SELECT column_name, data_type, is_nullable
FROM information_schema.columns
WHERE table_name = 'EMPLOYEE'
ORDER BY ordinal_position;
```

Firebird has a flat namespace, so the extension exposes a single DuckDB schema:
`main`. Name lookup is case-insensitive unless quoted identifiers require exact
DuckDB behavior.

## Current Status

Release: **v0.5.1**.

| Area | Status |
|---|---|
| `firebird_scan` table function | Done |
| Native `ATTACH ... (TYPE firebird)` | Done |
| `firebird_tables` discovery | Done |
| `firebird_attach_sql` generated-view helper | Done |
| Projection pushdown | Done |
| Predicate pushdown | Done |
| Manual paging with `row_limit` / `row_offset` | Done |
| Prepared statement bind variables | Done |
| PK-range parallel scan via `partitions=N` | Done |
| `CHARACTER SET NONE` decoding | Done |
| Metadata via `SHOW`, `DESCRIBE`, `information_schema` | Done |
| Automatic DuckDB `LIMIT` pushdown | Deferred upstream hook |
| LRU prepared statement cache | Deferred benchmark item |
| Scanner-native Arrow `RecordBatch` output | v1.x candidate |

Arrow note: the scanner produces DuckDB `Vector` / `DataChunk` columns. When a
Flight SQL client consumes query results through DuckDB or GizmoSQL, DuckDB
converts the result stream to Arrow at the boundary. Direct scanner-native Arrow
is intentionally deferred.

## Build

```bash
# Linux dependencies
sudo apt-get install -y cmake ninja-build g++ pkg-config python3 ccache firebird-dev

# Build and package
scripts/build_linux_local.sh
scripts/package_dist_linux.sh

# Tests require a reachable Firebird test database
./build/release/test/unittest test/sql/firebird_scan.test
```

Windows builds use the companion batch scripts:

```cmd
scripts\build_windows_local.bat
scripts\package_dist.bat
```

Release assets are automated by `.github/workflows/release-assets.yml`.
Pushing a `v*` tag builds and uploads Linux `.tar.gz` and Windows `.zip`
archives to the GitHub Release. The workflow can also be run manually with a
tag input.

See [docs/en/guide_linux.md](docs/en/guide_linux.md) and
[docs/en/guide_windows.md](docs/en/guide_windows.md).

## Documentation

Docs are split by language under `docs/en/` (English, primary) and
`docs/pt/` (Portuguese translations).

- [docs/en/usage_guide.md](docs/en/usage_guide.md) - analyst guide: live scans,
  `ATTACH`, views, materialization, incremental refresh, Parquet, MinIO,
  dbt/GizmoSQL/Power BI patterns, and troubleshooting.
  (PT: [docs/pt/usage_guide.md](docs/pt/usage_guide.md))
- [docs/en/guide_windows.md](docs/en/guide_windows.md) - Windows build and smoke
  tests.
- [docs/en/guide_linux.md](docs/en/guide_linux.md) - Linux build and smoke tests.
- [docs/en/roadmap.md](docs/en/roadmap.md) - performance roadmap and deferred work.
- [docs/en/test_report.md](docs/en/test_report.md) - release verification notes.
- [docs/en/architecture.md](docs/en/architecture.md) - implementation architecture.

## Repository Layout

```text
src/
  firebird_client.cpp      libfbclient wrapper
  firebird_types.cpp       Firebird to DuckDB type mapping
  firebird_query.cpp       SQL builder and pushdown
  firebird_scanner.cpp     table functions
  firebird_storage.cpp     native ATTACH catalog
  include/
test/sql/                  sqllogictest coverage
scripts/                   fixtures and smoke scripts
docs/en/                   English guides, roadmap, reports (primary)
docs/pt/                   Portuguese translations
community-extensions/      DuckDB community descriptor copy
```

## Governance

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- [SECURITY.md](SECURITY.md)
- [LICENSE](LICENSE)

## Community Catalog

The DuckDB community-extension submission is tracked at
[duckdb/community-extensions#1980](https://github.com/duckdb/community-extensions/pull/1980).
The descriptor currently points to `repo.ref: v0.5.1`.

After it is accepted, users should be able to install with:

```sql
INSTALL firebird FROM community;
LOAD firebird;
```

Until then, load a signed release artifact or a locally built extension.

## Author

**Fernando Lozer** — GitHub [@flozer](https://github.com/flozer) ·
LinkedIn [/fernandolozer](https://www.linkedin.com/in/fernandolozer)

## Credits

Design and type-mapping decisions build on
[firebird_peregrine_falcon](https://github.com/flozer/firebird_peregrine_falcon)
and the encoding work from `fb-cdc-rust`.
