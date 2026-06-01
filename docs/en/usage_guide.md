# duckdb-firebird - Usage guide for analysts

This guide shows how to use the `firebird` extension to query Firebird
databases from [DuckDB](https://github.com/duckdb/duckdb), build a local
analytics layer in a `.duckdb`
file, materialize data into fast tables, and export results to
Parquet/CSV.

The target audience is the data analyst who already knows SQL but does
not yet use DuckDB day to day. The main idea is simple:

1. Firebird remains the transactional system.
2. DuckDB queries Firebird when you need live data.
3. For heavy, repeated analyses, you create local tables in DuckDB.
4. Dashboards, notebooks, and exploratory queries then hit the
   `.duckdb` file, which is much faster for analytics.

## Official DuckDB references

This guide follows concepts documented by DuckDB:

- [`CREATE VIEW`](https://duckdb.org/docs/stable/sql/statements/create_view):
  views are saved queries; they are not physically materialized.
- [`CREATE TABLE`](https://duckdb.org/docs/stable/sql/statements/create_table):
  `CREATE TABLE ... AS SELECT` creates a physical table from a query.
- [`INSERT`](https://duckdb.org/docs/stable/sql/statements/insert):
  `INSERT INTO ... SELECT` appends the result of a query into an
  existing table.
- [`COPY`](https://duckdb.org/docs/stable/sql/statements/copy):
  imports/exports data between DuckDB and files such as CSV, Parquet,
  and JSON.
- [`ATTACH`](https://duckdb.org/docs/current/sql/statements/attach.html):
  attaches another catalog to DuckDB. This extension uses the same
  model to expose Firebird as a read-only catalog.
- [`Indexes`](https://duckdb.org/docs/current/sql/indexes.html) and
  [index performance](https://duckdb.org/docs/current/guides/performance/indexing.html):
  DuckDB builds zonemaps automatically; ART indexes help with very
  selective filters.
- [`EXPLAIN` / `EXPLAIN ANALYZE`](https://duckdb.org/docs/stable/sql/statements/profiling):
  inspect the plan and the actual runtime of a query.
- [DuckDB CLI](https://duckdb.org/docs/current/clients/cli/overview.html)
  and [dot commands](https://duckdb.org/docs/current/clients/cli/dot_commands.html):
  commands like `.open`, `.read`, `.tables`, `.schema`, `.timer`.

## 1. Concepts before you start

### In-memory DuckDB vs `.duckdb` file

When you launch `duckdb` with no file path, you work in memory. That is
fine for quick tests, but the objects disappear when the session ends.

To build a small local data mart, open a persistent file:

```bash
duckdb analytics.duckdb
```

or, on Windows:

```powershell
duckdb.exe analytics.duckdb
```

Everything you create without qualifying a different catalog is saved
in that file: tables, views, schemas, indexes, and macros.

### A view is not a materialized table

In DuckDB, `CREATE VIEW` stores the query. The view does not hold the
data. Every time you query the view, the query runs again.

```sql
CREATE OR REPLACE VIEW vw_clientes_firebird AS
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

This is great for a thin semantic layer, but it does not speed up a
heavy query if it keeps reading Firebird every time.

To materialize, create a table:

```sql
CREATE TABLE clientes AS
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

That table lives inside `analytics.duckdb`. After that, your analyses
read the local DuckDB, not Firebird.

### Firebird via scan vs Firebird via ATTACH

The extension offers two main ways to use it.

Use `firebird_scan(...)` when you want to query a specific table:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABPESSOAS')
LIMIT 10;
```

Use `ATTACH ... (TYPE firebird)` when you want to browse the Firebird
database as if it were a catalog:

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
    (TYPE firebird, user 'APP_READONLY', password 'secret');

SELECT *
FROM fb.main.TABPESSOAS
LIMIT 10;
```

The Firebird catalog is read-only in this extension. Create analytical
tables in the local DuckDB catalog.

## 2. Installation and loading

### Current status

The current public version of the project is `v0.5.1`. The publishing
request to the DuckDB Community catalog is open at
[`duckdb/community-extensions#1980`](https://github.com/duckdb/community-extensions/pull/1980)
and points to that tag.

### Once the extension is published in the community catalog

After the extension is available in the DuckDB community catalog:

```sql
INSTALL firebird FROM community;
LOAD firebird;
```

Use this on every new environment. `INSTALL` downloads the extension;
`LOAD` loads it into the current session.

### While using a local build

If you are using a locally produced `.duckdb_extension` file:

```sql
LOAD 'D:/01_Projetos/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
```

In some local builds, you may need to start DuckDB with `-unsigned`:

```bash
duckdb analytics.duckdb -unsigned
```

### Verify that it loaded

```sql
SELECT *
FROM duckdb_extensions()
WHERE extension_name = 'firebird';
```

Then test the table listing:

```sql
SELECT *
FROM firebird_tables('C:/dados/empresa.fdb');
```

## 3. Connect to Firebird

### Local path

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 10;
```

### Remote server

For remote databases, prefer the URL form:

```text
firebird://USER:PASSWORD@HOST:PORT/path/to/database.fdb?charset=UTF8
```

The database path is the path as seen by the Firebird server, not a path
on the DuckDB client machine. Do not use `HOST:PORT://path/to/database.fdb`;
that is not a valid Firebird connection string for this extension.

```sql
SELECT *
FROM firebird_scan(
    'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb?charset=UTF8',
    'CUSTOMER'
)
LIMIT 10;
```

The equivalent libfbclient-style connection string puts the port after
the host with `/PORT`:

```sql
SELECT *
FROM firebird_tables(
    'database=db.example.com/3050:/path/to/database.fdb;user=APP_READONLY;password=secret;charset=UTF8'
);
```

`ATTACH` accepts the same connection strings:

```sql
ATTACH 'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb?charset=UTF8'
    AS fb (TYPE firebird);
```

### Named parameters

Prefer named parameters when you want to separate path, user, and
password:

```sql
SELECT *
FROM firebird_scan(
    'C:/legacy/erp.fdb',
    'TABPESSOAS',
    user='APP_READONLY',
    password='secret',
    charset='UTF8'
)
LIMIT 10;
```

Security note: connection strings and SQL can end up in terminal
history, notebooks, and logs. In production, use a read-only Firebird
user and do not commit real passwords into scripts.

## 4. Charset and Brazilian legacy databases

Many older Firebird databases were created with `CHARACTER SET NONE`.
In those databases, text bytes may have been written as Windows-1252
even though the database does not declare that formally.

The extension uses `none_encoding='win1252'` by default because that is
the most common case in Brazilian and Western legacy ERPs.

In most cases, pass nothing:

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 10;
```

If you need to be explicit:

```sql
SELECT *
FROM firebird_scan(
    'C:/legacy/erp.fdb',
    'TABPESSOAS',
    none_encoding='win1252'
);
```

Alternatives:

```sql
-- Fails if it finds bytes that are not valid UTF-8.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='strict');

-- Plain Latin-1.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='iso8859_1');

-- Preserve raw bytes as BLOB.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='blob');
```

Important: `charset=` and `none_encoding=` are not the same thing.
`charset=` controls the Firebird client charset on the connection.
`none_encoding=` controls how the extension interprets bytes from
Firebird columns declared as `CHARACTER SET NONE`.

## 4.1 Row-based pagination (v0.5)

For large slices where you do not want to pull the whole table,
combine `row_limit` and `row_offset`:

```sql
-- First page: 1000 rows.
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA',
                   row_limit=1000);

-- Next page: skip 1000, fetch the next 1000.
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA',
                   row_limit=1000, row_offset=1000);
```

Internally the extension emits `ROWS M+1 TO M+N` against Firebird
(1-based, inclusive). The scan automatically becomes serial when
paging is used, because ordering only makes sense with a single
producer. Asking for `partitions > 1` together with explicit paging is
rejected at bind time.

Important caveat: this is **physical** pagination (offset/limit), not
**keyset pagination**. Without a server-side `ORDER BY`, successive
pages can repeat or skip rows if Firebird reorders reads between
calls. For stable results across a sequence of pages, prefer to
materialize a local table and page through it, or apply
`ORDER BY <stable key>` in the query.

## 4.2 Filters and text search (v0.5)

The extension pushes down to Firebird the predicates it knows how to
translate safely, including (v0.5):

- `col NOT IN (...)` — becomes a server-side `NOT IN` clause.
- `NOT bool_col` / `bool_col = FALSE` — pushed down as `NOT col`.
- `col LIKE 'prefix%'` with a static literal — pushed down.
- `BETWEEN a AND b` — DuckDB decomposes into `>= AND <=`, both pushed.

Examples:

```sql
-- NOT IN
SELECT COUNT(*)
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE CODIGOEMPRESA NOT IN (99, 100, 101);

-- LIKE with prefix (apostrophe in the literal is safe):
SELECT IDMASTER, OBSERVACOES
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE OBSERVACOES LIKE 'CONTA PARA%'
LIMIT 5;
```

Internal detail (v0.5): for safe types and non-literal values
(parameters, dates, strings with accents or apostrophes), the filter
is sent to Firebird as a **bind variable** via the input XSQLDA,
rather than inlined in the SQL. That avoids escaping mistakes and
lets Firebird reuse plans. You do not have to do anything — the
extension handles it. On `CHARACTER SET NONE` columns with
`none_encoding != 'strict'`, text filters are kept as residual filters
in DuckDB (the comparison needs the extension's decode step to be
correct).

## 5. Explore the Firebird database

Start by discovering the tables:

```sql
SELECT *
FROM firebird_tables('C:/legacy/erp.fdb')
ORDER BY table_name;
```

Look at a few rows:

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 20;
```

Count rows before materializing:

```sql
SELECT count(*) AS rows
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

Inspect the types inferred by DuckDB:

```sql
DESCRIBE
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

When you use `ATTACH`, browse with qualified names:

```sql
ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'APP_READONLY', password 'secret');

SHOW DATABASES;

SELECT *
FROM fb.main.TABPESSOAS
LIMIT 20;
```

## 5.1 Catalog discovery via ATTACH (v0.5)

When you attach the database as a catalog, standard DuckDB tooling
sees everything:

```sql
ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'APP_READONLY', password 'secret');

-- List tables (and views) in the Firebird catalog:
SHOW TABLES FROM fb;

-- Table shape:
DESCRIBE fb.main.TABPESSOAS;

-- Standard information_schema, useful for BI / dbt / ADBC clients:
SELECT table_catalog, table_schema, table_name, table_type
  FROM information_schema.tables
 WHERE table_catalog = 'fb'
 ORDER BY table_name;

SELECT column_name, data_type, ordinal_position, is_nullable
  FROM information_schema.columns
 WHERE table_catalog = 'fb' AND table_name = 'TABPESSOAS'
 ORDER BY ordinal_position;
```

Points to keep in mind:

- Only one schema, `main`, is exposed, regardless of `RDB$OWNER_NAME`
  in the source. Querying `fb.public.X` or any other schema returns an
  error.
- The extension normalizes identifiers to upper case internally, just
  like Firebird; `fb.main.tabpessoas`, `fb.main.Tabpessoas`, and
  `fb.main."TABPESSOAS"` all resolve to the same entry.
- `is_nullable` in `information_schema.columns` reflects the Firebird
  definition (`RDB$NULL_FLAG` on the field or the domain). Columns
  declared `NOT NULL` show as `NO`.
- Firebird views (`RDB$RELATION_TYPE = 1`) appear alongside tables and
  can be queried the same way. In `information_schema.tables` they
  currently all show as `BASE TABLE` — this catalog-layer
  simplification is tracked by tests for a future revisit.

## 6. Build a layer of views

Views are good for standardizing names, hiding technical columns, and
documenting business rules. They still read from Firebird whenever
they are queried.

```sql
CREATE SCHEMA IF NOT EXISTS bronze;

CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT
    CODIGO,
    NOME,
    APELIDO,
    CNPJCPF,
    CIDADE,
    UF
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

Create views for the main tables:

```sql
CREATE OR REPLACE VIEW bronze.entrada_saida AS
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGO,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL,
    TIPO
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA');
```

Now the analyst queries objects with friendlier names:

```sql
SELECT *
FROM bronze.pessoas
WHERE UF = 'SP'
LIMIT 100;
```

Since the view is not materialized, use filters and projections
whenever possible. The extension tries to push filters and columns
down to Firebird when it is safe to do so.

## 7. Materialize snapshots into DuckDB tables

For performance, create a local table. That is the practical
substitute for "materialized view" in DuckDB.

```sql
CREATE SCHEMA IF NOT EXISTS silver;

CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;
```

If your version/environment does not accept `CREATE OR REPLACE TABLE`,
use:

```sql
DROP TABLE IF EXISTS silver.pessoas;

CREATE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;
```

Create a local fact table:

```sql
CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01';
```

After that:

```sql
SELECT count(*)
FROM silver.entrada_saida;
```

This query reads from the `analytics.duckdb` file, not from Firebird.

## 8. The bronze, silver, and gold pattern

A simple layout for analyses:

- `bronze`: views or tables that are almost identical to Firebird.
- `silver`: clean, typed, filtered local tables.
- `gold`: aggregated tables for dashboards and KPIs.

Example:

```sql
CREATE SCHEMA IF NOT EXISTS gold;

CREATE OR REPLACE TABLE gold.vendas_mes AS
SELECT
    date_trunc('month', DATAMOVIMENTO)::DATE AS mes,
    CODIGOEMPRESA,
    CODIGOFILIAL,
    count(*) AS qtd_movimentos,
    sum(VALORTOTAL) AS valor_total
FROM silver.entrada_saida
WHERE TIPO = 'S'
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3;
```

Now the dashboard queries `gold.vendas_mes`, which is usually much
smaller and much faster than the original transactional table.

## 9. Simple full refresh

For small databases or nightly jobs, a full refresh is the simplest
and most reliable approach.

Create a `refresh.sql` file:

```sql
LOAD firebird;

CREATE SCHEMA IF NOT EXISTS bronze;
CREATE SCHEMA IF NOT EXISTS silver;
CREATE SCHEMA IF NOT EXISTS gold;

CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');

CREATE OR REPLACE VIEW bronze.entrada_saida AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA');

CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;

CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01';

CREATE OR REPLACE TABLE gold.vendas_mes AS
SELECT
    date_trunc('month', DATAMOVIMENTO)::DATE AS mes,
    CODIGOEMPRESA,
    CODIGOFILIAL,
    TIPO,
    count(*) AS qtd_movimentos,
    sum(VALORTOTAL) AS valor_total
FROM silver.entrada_saida
GROUP BY 1, 2, 3, 4;
```

Run it:

```bash
duckdb analytics.duckdb -unsigned -init refresh.sql
```

Or from inside the CLI:

```sql
.read refresh.sql
```

## 10. Incremental refresh by date

When the Firebird table is large, avoid reloading everything. A simple
pattern is to load by date window.

First create the local table once:

```sql
CREATE TABLE IF NOT EXISTS silver.entrada_saida (
    CODIGOEMPRESA INTEGER,
    CODIGOFILIAL INTEGER,
    CODIGO INTEGER,
    CODIGOPESSOA INTEGER,
    DATAMOVIMENTO DATE,
    VALORTOTAL DECIMAL(18, 2),
    TIPO VARCHAR
);
```

Then, on every refresh, delete and reload a recent window:

```sql
BEGIN TRANSACTION;

DELETE FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= current_date - INTERVAL 30 DAY;

INSERT INTO silver.entrada_saida
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGO,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL,
    TIPO
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= current_date - INTERVAL 30 DAY;

COMMIT;
```

This pattern works well when old movements do not change. If the ERP
edits old records, use a wider window or run a periodic full refresh.

## 11. Ordering and indexes to speed up analyses

DuckDB builds zonemaps automatically for common types. They work best
when the data is roughly sorted by the columns used in filters.

When materializing a fact table, order by date and common keys:

```sql
CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01'
ORDER BY DATAMOVIMENTO, CODIGOEMPRESA, CODIGOFILIAL;
```

For very selective filters on a single column, an ART index can help:

```sql
CREATE INDEX idx_pessoas_codigo
ON silver.pessoas (CODIGO);

CREATE INDEX idx_entrada_saida_pessoa
ON silver.entrada_saida (CODIGOPESSOA);
```

Do not create indexes reflexively. They speed up some point filters,
but they also consume memory and slow down loads and updates. For
large aggregate analyses, ordering and well-chosen columns usually pay
off more.

## 12. Joining Firebird with local files

A strong point of DuckDB is querying multiple sources in the same SQL.

Example: a Parquet table of targets plus sales coming from Firebird:

```sql
CREATE OR REPLACE TABLE silver.metas AS
SELECT *
FROM read_parquet('C:/dados/metas/*.parquet');

CREATE OR REPLACE TABLE gold.realizado_vs_meta AS
SELECT
    v.mes,
    v.CODIGOEMPRESA,
    v.CODIGOFILIAL,
    v.valor_total AS realizado,
    m.meta_valor AS meta,
    v.valor_total / NULLIF(m.meta_valor, 0) AS pct_meta
FROM gold.vendas_mes v
LEFT JOIN silver.metas m
  ON m.mes = v.mes
 AND m.codigoempresa = v.CODIGOEMPRESA
 AND m.codigofilial = v.CODIGOFILIAL;
```

## 13. Export results

To hand a dataset to BI, Python, R, or another team, use `COPY`.

Parquet is the best option for analytics:

```sql
COPY gold.vendas_mes
TO 'C:/exports/vendas_mes.parquet'
(FORMAT parquet, COMPRESSION zstd);
```

CSV for end users:

```sql
COPY (
    SELECT *
    FROM gold.vendas_mes
    WHERE mes >= DATE '2024-01-01'
)
TO 'C:/exports/vendas_mes_2024.csv'
(HEADER, DELIMITER ';');
```

You can also produce one file per partition:

```sql
COPY gold.vendas_mes
TO 'C:/exports/vendas_mes_partitioned'
(FORMAT parquet, PARTITION_BY (CODIGOEMPRESA));
```

## 14. Use from Python

Minimal example for notebooks:

```python
import duckdb

con = duckdb.connect("analytics.duckdb")
con.execute("LOAD firebird")

df = con.execute("""
    SELECT *
    FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
    LIMIT 1000
""").df()

print(df.head())
```

Once you materialize tables in the `.duckdb`, notebooks can query just
the local file:

```python
import duckdb

con = duckdb.connect("analytics.duckdb", read_only=True)

df = con.execute("""
    SELECT mes, sum(valor_total) AS total
    FROM gold.vendas_mes
    GROUP BY 1
    ORDER BY 1
""").df()
```

## 15. Check plan and performance

Use `EXPLAIN` to see the plan without running:

```sql
EXPLAIN
SELECT
    CODIGOEMPRESA,
    sum(VALORTOTAL)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01'
GROUP BY 1;
```

Use `EXPLAIN ANALYZE` to run and measure:

```sql
EXPLAIN ANALYZE
SELECT
    CODIGOEMPRESA,
    sum(VALORTOTAL)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01'
GROUP BY 1;
```

In the CLI, enable the timer:

```sql
.timer on
```

Compare the same query reading Firebird live and reading the
materialized table:

```sql
-- Live: good for recent data and initial exploration.
SELECT count(*)
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= DATE '2024-01-01';

-- Local: better for repeated analysis.
SELECT count(*)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01';
```

## 16. Best practices

- Use `firebird_scan` for exploration and one-off queries.
- Use `ATTACH` when you want to browse several Firebird tables as a
  catalog.
- Materialize repeatedly-used tables in DuckDB.
- Create views for semantics, not for performance.
- Prefer `CREATE TABLE AS SELECT` for full snapshots.
- Prefer `INSERT INTO ... SELECT` for incremental loads.
- Filter and project columns before materializing large tables.
- Order fact tables by date and filter keys.
- Create indexes only for very selective point lookups.
- Export analytical datasets as Parquet whenever possible.
- Use a read-only Firebird user for analytics.
- Do not run heavy loads against Firebird during critical hours
  without validating the impact.

## 17. Quick checklist for a new project

```sql
-- 1. Open a persistent DuckDB file:
--    duckdb analytics.duckdb -unsigned

LOAD firebird;

-- 2. Explore the source.
SELECT *
FROM firebird_tables('C:/legacy/erp.fdb')
ORDER BY table_name;

-- 3. Create schemas.
CREATE SCHEMA IF NOT EXISTS bronze;
CREATE SCHEMA IF NOT EXISTS silver;
CREATE SCHEMA IF NOT EXISTS gold;

-- 4. Create a source view.
CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');

-- 5. Materialize locally.
CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;

-- 6. Build a business aggregate.
CREATE OR REPLACE TABLE gold.pessoas_por_uf AS
SELECT
    UF,
    count(*) AS qtd_pessoas
FROM silver.pessoas
GROUP BY 1
ORDER BY 2 DESC;

-- 7. Export.
COPY gold.pessoas_por_uf
TO 'C:/exports/pessoas_por_uf.parquet'
(FORMAT parquet, COMPRESSION zstd);
```

## 18. Troubleshooting

### `Extension "firebird" could not be loaded`

Make sure the extension was installed, or that the `LOAD` path points
to the correct file. On a local build, run the CLI with `-unsigned`.

### Wrong `fbclient.dll` on Windows

Make sure the correct `fbclient.dll` is on `PATH` or next to
`duckdb.exe`. Mixing an old Firebird client with a newer database can
cause an unknown-type error.

### Broken accents

If the database uses `CHARACTER SET NONE`, start with no parameters:
the default is `none_encoding='win1252'`. If you still see issues,
try:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='iso8859_1')
LIMIT 20;
```

If you need to inspect raw bytes:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='blob')
LIMIT 20;
```

### Slow live queries

Read fewer columns, apply selective filters, and materialize tables
that are used repeatedly.

```sql
CREATE OR REPLACE TABLE silver.movimentos_2024 AS
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= DATE '2024-01-01';
```

### I need to share the analysis

Share the `.duckdb` file when the consumer also uses DuckDB, or export
the `gold` tables to Parquet/CSV with `COPY`.
