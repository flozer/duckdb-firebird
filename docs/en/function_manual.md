# Extension function manual

Public function and setting reference for `duckdb-firebird`.

This is the English counterpart of `docs/pt/function_manual.md`. Keep both
files aligned when public behavior changes.

## How to read this manual

Use this document as a behavior reference, not as a product roadmap. Future
functions mentioned in `docs/en/roadmap.md` are not available until they are
implemented, tested, and documented here.

## Runtime requirement

The extension loads the Firebird client library (`libfbclient`) at runtime.
Install the Firebird client/runtime matching your platform before loading the
extension.

## Level 1 - Direct reads

### `firebird_scan(connection_string, table_name)`

Reads a Firebird table directly into DuckDB.

Common use:

```sql
SELECT *
FROM firebird_scan('database=C:/data/erp.fdb user=SYSDBA password=masterkey',
                   'CUSTOMER');
```

Supported named parameters include:

- `schema`
- `columns`
- `filter`
- `row_limit`
- `row_offset`
- `partitions`
- charset-related options documented in the usage guide

Use this when you need one direct table scan without attaching the whole
database catalog.

## Level 2 - Catalog discovery

### `firebird_tables(connection_string)`

Lists Firebird tables visible to the connection.

```sql
SELECT *
FROM firebird_tables('database=C:/data/erp.fdb user=SYSDBA password=masterkey');
```

### `information_schema` via `ATTACH`

After attaching a database, use DuckDB catalog views to inspect exposed
Firebird tables and columns:

```sql
ATTACH 'database=C:/data/erp.fdb user=SYSDBA password=masterkey'
  AS fb (TYPE firebird);

SELECT *
FROM information_schema.tables
WHERE table_schema = 'main';
```

## Level 3 - Attached database

### `ATTACH ... (TYPE firebird)`

Attaches a Firebird database as a DuckDB catalog.

```sql
ATTACH 'database=C:/data/erp.fdb user=SYSDBA password=masterkey'
  AS fb (TYPE firebird);

SELECT *
FROM fb.main.CUSTOMER;
```

Prefer `ATTACH` for analytics sessions that query multiple Firebird tables.
It enables catalog discovery, normal SQL references, and connection-pool
reuse.

### `firebird_attach_sql(connection_string [, schema])`

Generates DuckDB SQL views for Firebird tables.

Use this when you want a view-based workflow instead of a storage attach.

### `firebird_generate_dbt_sources(catalog_name)`

Generates dbt `sources.yml` content from an attached Firebird catalog.

```sql
ATTACH 'database=C:/data/erp.fdb user=SYSDBA password=masterkey'
  AS fb (TYPE firebird);

SELECT firebird_generate_dbt_sources('fb');
```

The generated YAML is a starting point. Users should still review names,
tests, descriptions, and project conventions.

## Level 4 - Diagnostics and observability

### `firebird_profile_table(qualified_name)`

Returns a single-row factual diagnostic for one Firebird relation reachable
through an attached Firebird catalog. The argument is a qualified name in
`catalog.schema.table` form; the schema part is accepted only as `main`
(the Firebird ATTACH path exposes exactly one schema) and may be omitted.

```sql
ATTACH 'database=C:/data/erp.fdb user=SYSDBA password=masterkey'
  AS fb (TYPE firebird);

SELECT *
FROM firebird_profile_table('fb.main.CUSTOMER');
-- equivalent short form:
SELECT *
FROM firebird_profile_table('fb.CUSTOMER');
```

Output columns:

- `table_name`
- `object_type` - `TABLE` or `VIEW`
- `has_primary_key` - boolean
- `primary_key_columns` - list of columns
- `indexes` - list of `NAME (COL, ...) [UNIQUE] [PK]` descriptions
- `watermark_candidates` - columns whose type makes them plausible
  high-water-mark columns (integer-family and date/timestamp)
- `filter_candidates` - indexed columns whose type pushes down cheaply
- `full_scan_risk` - `LOW`, `MEDIUM`, or `HIGH`
- `recommended_partitions` - advisory `partitions=N` value
- `warnings` - list of explicit caveats

This is a factual diagnostic, not a cost-based advisor. Heuristics are
simple and explicit: a watermark candidate is judged by type, not by proven
monotonicity; `recommended_partitions` is derived from the primary-key
`MIN`/`MAX` range, not a row count, and is advisory only. The `warnings`
column carries those caveats inline. Use it to decide whether to scan live,
filter harder, partition, or materialize through DuckDB/dbt/Parquet.

For `object_type = VIEW`, the function additionally performs a shallow,
conservative inspection of the stored view definition
(`RDB$VIEW_SOURCE`) and emits `warnings` when it detects:

- a `JOIN` in the definition,
- aggregation (`GROUP BY` or `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/`LIST`),
- no `WHERE` filter in the definition.

The detection is token-level pattern matching, not a SQL parser, and the
view source text itself is never returned — only the shape flags drive the
warnings. String literals (including SQL-escaped doubled quotes) and SQL
comments are blanked before matching, so keyword-shaped text inside a
literal or comment does not produce false positives. Whitespace is
collapsed first, so keywords split across newlines or tabs in the stored
view text (e.g. `GROUP` and `BY` on separate lines) are still detected. When the definition cannot be read, a `view definition not
inspected` warning is emitted instead. These warnings point toward
materializing heavy views through DuckDB/dbt/Parquet rather than scanning
them repeatedly.

The diagnostic reads only the Firebird system tables (`RDB$*`) plus a
best-effort primary-key `MIN`/`MAX` probe; it never reads business rows and
never returns the connection string or credentials.

### `firebird_last_query()`

Returns telemetry for the most recent Firebird query captured by the
extension.

Useful fields include:

- remote SQL
- pushed filters/projection details where available
- elapsed time
- rows read
- parallel scan details
- connection reuse details

### `firebird_query_log()`

Returns the in-memory query log for the current DuckDB session.

The log is bounded by `firebird_query_log_size` and is intended for
debugging, diagnostics, and support.

## Level 5 - Session options

### `SET firebird_query_log_size = N`

Controls the maximum number of query telemetry entries kept in memory.

### `SET firebird_pool_enabled = true|false`

Enables or disables the ATTACH connection pool for future attaches.

### `SET firebird_pool_max_size = N`

Controls the maximum number of idle connections kept by the pool.

### `SET firebird_pool_idle_timeout_ms = MS`

Controls how long idle pooled connections may remain reusable.

## Documentation premise

When public behavior changes:

- update this file
- update `docs/pt/function_manual.md`
- update usage guides when examples or workflows change
- update roadmap files when status changes

PT/EN docs must stay aligned in meaning, status, caveats, and examples.
