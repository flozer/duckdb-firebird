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
FROM firebird_scan('database=C:/data/erp.fdb user=APP_READONLY password=secret',
                   'CUSTOMER');
```

Remote database:

```sql
SELECT *
FROM firebird_scan(
  'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb?charset=UTF8',
  'CUSTOMER'
);
```

The equivalent libfbclient-style form is:

```sql
SELECT *
FROM firebird_scan(
  'database=db.example.com/3050:/path/to/database.fdb;user=APP_READONLY;password=secret;charset=UTF8',
  'CUSTOMER'
);
```

For remote connections, use `firebird://USER:PASSWORD@HOST:PORT/path`
or `database=HOST/PORT:/path;user=USER;password=PASSWORD`. Do not use
`HOST:PORT://path`.

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
FROM firebird_tables('database=C:/data/erp.fdb user=APP_READONLY password=secret');
```

### `information_schema` via `ATTACH`

After attaching a database, use DuckDB catalog views to inspect exposed
Firebird tables and columns:

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT *
FROM information_schema.tables
WHERE table_schema = 'main';
```

## Level 3 - Attached database

### `ATTACH ... (TYPE firebird)`

Attaches a Firebird database as a DuckDB catalog.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
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
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT firebird_generate_dbt_sources('fb');
```

The generated YAML is a starting point. Users should still review names,
tests, descriptions, and project conventions.

## Level 3b - Metadata Bridge 2.0

Starting with v0.7, the extension populates standard `information_schema` views
with constraint data and provides additional catalog functions for deep
inspection of Firebird metadata.

### Populated `information_schema` views

After `ATTACH ... (TYPE firebird)`, the following views are populated:

#### `information_schema.table_constraints`

Exposes PK, UNIQUE, and FK constraints for all user tables.

```sql
SELECT constraint_name, constraint_type, table_name
FROM information_schema.table_constraints
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_type;
```

#### `information_schema.key_column_usage`

Maps columns participating in PK, UNIQUE, and FK constraints.

```sql
SELECT constraint_name, table_name, column_name, ordinal_position
FROM information_schema.key_column_usage
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_name, ordinal_position;
```

#### `information_schema.referential_constraints`

Exposes FK constraints with reference information.

**Note**: the `update_rule` and `delete_rule` columns always return
`'NO ACTION'` due to a DuckDB limitation. For the real Firebird rules
(`CASCADE`, `SET NULL`, etc.) use `firebird_foreign_keys` (described below).

```sql
SELECT constraint_name, unique_constraint_name
FROM information_schema.referential_constraints
WHERE constraint_catalog = 'fb';
```

### `firebird_foreign_keys(catalog_name)`

Lists all foreign-key constraints with real Firebird referential rules.

Output columns:

| Column | Type | Description |
|---|---|---|
| `fk_schema` | VARCHAR | Always `'main'` |
| `fk_table` | VARCHAR | Table declaring the FK |
| `fk_constraint` | VARCHAR | FK constraint name |
| `ordinal_position` | INTEGER | Column position in the key (0-based) |
| `fk_column` | VARCHAR | Column in the child table |
| `pk_table` | VARCHAR | Referenced table |
| `pk_constraint` | VARCHAR | Referenced PK/UNIQUE constraint name |
| `update_rule` | VARCHAR | Firebird rule (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |
| `delete_rule` | VARCHAR | Firebird rule (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |

```sql
SELECT * FROM firebird_foreign_keys('fb');
```

**Note**: `firebird_foreign_keys.ordinal_position` is 0-based (the raw Firebird `RDB$FIELD_POSITION`), whereas `information_schema.key_column_usage.ordinal_position` is 1-based. Account for the off-by-one when joining the two surfaces.

### `firebird_indexes(catalog_name)`

Lists all user indexes with their segments.

Output columns:

| Column | Type | Description |
|---|---|---|
| `table_schema` | VARCHAR | Always `'main'` |
| `table_name` | VARCHAR | Table owning the index |
| `index_name` | VARCHAR | Index name |
| `is_unique` | BOOLEAN | `true` if the index is unique |
| `is_active` | BOOLEAN | `true` if the index is active |
| `segment_position` | INTEGER | Column position in the index (0-based) |
| `column_name` | VARCHAR | Segment column name (`NULL` for expression indexes) |
| `expression_source` | VARCHAR | Expression (expression indexes only; `NULL` otherwise) |

```sql
SELECT * FROM firebird_indexes('fb');
```

### `firebird_generators(catalog_name)`

Lists user generators/sequences with initial and current values.

Output columns:

| Column | Type | Description |
|---|---|---|
| `generator_name` | VARCHAR | Generator name |
| `initial_value` | BIGINT | Configured initial value |
| `current_value` | BIGINT | Current value via `GEN_ID(name, 0)`; `NULL` if no privilege. Read per-generator (one round-trip each) to preserve per-generator isolation; on databases with very many generators this is N+1 round-trips. |

```sql
SELECT * FROM firebird_generators('fb');
```

### `firebird_domains(catalog_name)`

Lists user-defined domains with type, nullability, charset, and constraints.

Output columns:

| Column | Type | Description |
|---|---|---|
| `domain_name` | VARCHAR | Domain name |
| `base_type` | VARCHAR | Firebird type string (e.g. `VARCHAR(100)`, `NUMERIC(10,2)`) |
| `length` | INTEGER | Length in bytes (for text types) |
| `scale` | INTEGER | Decimal scale (absolute value) |
| `is_nullable` | BOOLEAN | `true` if the domain allows `NULL` |
| `charset_name` | VARCHAR | Character set (text types only) |
| `check_source` | VARCHAR | Domain `CHECK` clause (`NULL` if none) |
| `default_source` | VARCHAR | Domain `DEFAULT` clause (`NULL` if none) |

```sql
SELECT * FROM firebird_domains('fb');
```

**Note**: for `CHAR`/`VARCHAR` domains, the reported length is the Firebird-declared byte length (`RDB$FIELD_LENGTH`). Under multi-byte charsets (e.g. UTF8) this is larger than the character length (a UTF8 `VARCHAR(10)` reports `VARCHAR(40)`).

### `firebird_computed_columns(catalog_name)`

Lists computed columns (`COMPUTED BY`) for all user tables.

Output columns:

| Column | Type | Description |
|---|---|---|
| `table_schema` | VARCHAR | Always `'main'` |
| `table_name` | VARCHAR | Table containing the column |
| `column_name` | VARCHAR | Computed column name |
| `expression_source` | VARCHAR | `COMPUTED BY` expression |

```sql
SELECT * FROM firebird_computed_columns('fb');
```

### `firebird_dependencies(catalog_name)`

Lists dependencies between database objects (tables, procedures, triggers, etc.).

Output columns:

| Column | Type | Description |
|---|---|---|
| `object_name` | VARCHAR | Dependent object |
| `object_type` | VARCHAR | Human-readable type (e.g. `TABLE`, `TRIGGER`, `PROCEDURE`) |
| `object_type_code` | INTEGER | Raw `RDB$DEPENDENT_TYPE` code |
| `depends_on_name` | VARCHAR | Object depended upon |
| `depends_on_type` | VARCHAR | Human-readable type of the referenced object |
| `depends_on_type_code` | INTEGER | Raw `RDB$DEPENDED_ON_TYPE` code |
| `field_name` | VARCHAR | Specific column referenced (`NULL` for object-level dependencies) |

```sql
SELECT * FROM firebird_dependencies('fb');
```

### `firebird_comments(catalog_name)`

Lists `RDB$DESCRIPTION` comments for user tables, views, and columns.

Output columns:

| Column | Type | Description |
|---|---|---|
| `object_schema` | VARCHAR | Always `'main'` |
| `object_name` | VARCHAR | Table or view name |
| `object_type` | VARCHAR | `'TABLE'`, `'VIEW'`, or `'COLUMN'` |
| `column_name` | VARCHAR | Column name (column rows only; `NULL` for object rows) |
| `comment` | VARCHAR | Comment text |

```sql
SELECT * FROM firebird_comments('fb');
```

## Level 4 - Diagnostics and observability

### `firebird_profile_table(qualified_name)`

Returns a single-row factual diagnostic for one Firebird relation reachable
through an attached Firebird catalog. The argument is a qualified name in
`catalog.schema.table` form; the schema part is accepted only as `main`
(the Firebird ATTACH path exposes exactly one schema) and may be omitted.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
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

### `recommended_partitions` — how it is computed

`recommended_partitions` is an **advisory** number. It does **not** change
how `firebird_scan` runs; nothing is parallelized automatically and there is
no promise of a performance gain. It only tells you what `partitions=N` you
*could* try.

It is derived only from the single-column numeric primary key's `MIN`/`MAX`
range width (no row count, no `COUNT(*)`, no full scan):

- **No single-column numeric PK** (view, no PK, composite PK, non-numeric
  PK): `recommended_partitions = 1`, with a `warnings` entry explaining why
  the PK-range parallel lever is unavailable.
- **Numeric PK but a small range** (`MIN`/`MAX` span < 10000): still
  `recommended_partitions = 1`, with a note that partitioning would add
  overhead without meaningful parallelism. A numeric PK does **not**
  automatically mean parallel is recommended.
- **Numeric PK with a wide range**: a conservative `partitions=N` between 2
  and 8 (scaled by range width, capped at 8). The `warnings` make clear this
  is advisory, derived from range width (so it can be uneven if the PK is
  sparse), and must be validated against the live server.

When `partitions > 1` is recommended, `warnings` also carries a
**server-side parallelism caveat**: if Firebird server-side parallelism is
already enabled/configured (e.g. Firebird 5 `ParallelWorkers`), prefer
starting with `partitions=1` or benchmark before combining server-side and
client-side parallelism. The extension does not probe the server's parallel
setting, so this is a generic caveat, not a detected condition.

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
- residual (not-pushed) filters and a coarse reason for each
  (`not_pushed_reasons`)
- paging actually pushed to Firebird (`limit_pushed` / `offset_pushed`,
  `NULL` when none)
- elapsed time
- rows read
- parallel scan details
- connection reuse details

The schema is 18 columns (shared with `firebird_query_log()`). The
pushdown-explainability columns (`limit_pushed`, `offset_pushed`,
`not_pushed_reasons`) make it explicit what reached Firebird and why a
filter stayed local. See `docs/en/observability.md` for the full column
reference and the reason vocabulary.

### `firebird_explain_pushdown(sql)`

Returns an a-priori, plan-only analysis of what a `SELECT` statement would
push down to Firebird — **without executing the query** and **without
opening a connection or cursor**. This is the complement of
`firebird_last_query()`: explain is prospective (before any run), while
`firebird_last_query()` is post-hoc (after the last real scan).

Because explain never runs the query, it leaves no entry in
`firebird_last_query()` telemetry.

#### Accepted input

- A single `SELECT` statement referencing one or more Firebird tables via an
  `ATTACH` alias (e.g. `fb.main.EMPLOYEE`).
- A `WITH ... SELECT` (CTE) whose body is a Firebird scan.

Rejected input (raises an error):

- DML statements (`UPDATE`, `DELETE`, `INSERT`, `MERGE`).
- Direct `firebird_scan(...)` calls — use the `ATTACH` alias form instead.
- `WITH ... DELETE` or other non-SELECT CTE statements.

#### Output columns (14)

| Column | Type | Notes |
| --- | --- | --- |
| `scan_ordinal` | BIGINT | 1-based ordinal; distinct for each `LogicalGet` node, including self-joins |
| `table_name` | VARCHAR | Firebird table name (never the connection string) |
| `remote_sql` | VARCHAR | SQL that would be sent to Firebird; bind values appear as `?` — no literals, no connection-string fragments |
| `projected_columns` | VARCHAR[] | Columns selected from Firebird (post projection-pruning) |
| `pushed_filters` | VARCHAR[] | Predicates the scanner would push to Firebird |
| `residual_filters` | VARCHAR[] | Predicates DuckDB would revalidate above the scan; always `len(residual_filters) == len(not_pushed_reasons)` |
| `not_pushed_reasons` | VARCHAR[] | One coarse reason per `residual_filters` entry, same order; values: `NONE_CHARSET`, `UNSUPPORTED_OP`, `ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING` |
| `limit_pushed` | BIGINT | `row_limit=` named-parameter value if it would be pushed to a Firebird `ROWS` clause; `NULL` for SQL `LIMIT` (DuckDB applies SQL `LIMIT` above the scan) |
| `offset_pushed` | BIGINT | `row_offset=` named-parameter value if pushed; `NULL` otherwise |
| `rows_clause` | VARCHAR | Firebird `ROWS m TO n` clause that would be emitted, or `NULL` |
| `pk_range_eligible` | BOOLEAN | `true` only for a single-column numeric primary key |
| `pk_range_column` | VARCHAR | Name of that PK column, or `NULL` when not eligible (e.g. composite or non-numeric PK) |
| `pk_range_reason` | VARCHAR | One of four normalized values: `single numeric PK`, `non-numeric PK`, `composite PK`, `no primary key` |
| `scan_strategy` | VARCHAR | `pk-range-partitionable` when `pk_range_eligible` is true; `serial` otherwise |

#### Invariants

- `len(residual_filters) == len(not_pushed_reasons)` always holds.
- A `NOT IN` on a `CHARACTER SET NONE` text column is recorded in
  `residual_filters` as `complex_filter[none_gated]` with the parallel
  `not_pushed_reasons` entry `NONE_CHARSET`.
- `limit_pushed` / `offset_pushed` / `rows_clause` are `NULL` for SQL
  `LIMIT` — only the `row_limit=` / `row_offset=` named parameters of the
  scanner are considered for pushdown.
- `pk_range_column` is `NULL` whenever `pk_range_eligible` is `false`.

#### Usage example

```sql
ATTACH 'C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

-- Check what would be pushed before actually running the query:
SELECT table_name, pushed_filters, residual_filters, not_pushed_reasons,
       pk_range_eligible, scan_strategy
FROM firebird_explain_pushdown(
  'SELECT EMP_ID, EMP_NAME FROM fb.main.EMPLOYEE WHERE EMP_ID > 10');
```

### `firebird_query_log()`

Returns the in-memory query log for the current DuckDB session.

The log is bounded by `firebird_query_log_size` and is intended for
debugging, diagnostics, and support.

### `firebird_pool_stats(catalog_name)`

Returns one row of factual connection-pool state for a single attached
Firebird catalog. The argument is the **explicit ATTACH alias** — the
function does not enumerate catalogs.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT EMP_ID FROM fb.main.EMPLOYEE WHERE EMP_ID = 1;

SELECT * FROM firebird_pool_stats('fb');
```

Output columns (8):

| Column | Type | Notes |
|---|---|---|
| `catalog_name` | VARCHAR | The alias passed in |
| `pool_enabled` | BOOLEAN | Whether the ATTACH pool is enabled |
| `max_idle_size` | BIGINT | Configured idle-queue cap (`firebird_pool_max_size`); `0` = unlimited |
| `idle_timeout_ms` | BIGINT | Configured idle expiry (`firebird_pool_idle_timeout_ms`); `0` = no expiry |
| `idle_connections` | BIGINT | Connections currently parked in the idle queue |
| `total_created` | BIGINT | Lifetime physical connections created |
| `total_reused` | BIGINT | Lifetime connections served from the idle queue |
| `total_discarded` | BIGINT | Lifetime connections destroyed (pool disabled, or idle cap/expiry hit) |

It reads only counters and config the pool already tracks, and it does
**not** lease a connection — calling it never perturbs the pool it reports
on. The configured values mirror the pool settings read at `ATTACH` time;
a later `SET` does not retune an existing pool (re-`ATTACH` to apply).

Use it to confirm pooling is actually reusing connections, to size
`firebird_pool_max_size`, or to verify a `firebird_pool_enabled = false`
catalog never parks idle connections.

Current limitations:

- **Per-catalog, alias-only**: you pass one ATTACH alias; there is no
  no-argument form that lists every attached catalog.
- **No active/in-use count**: the pool tracks the idle queue and lifetime
  counters, not how many connections are currently leased out. An
  active-connection count is possible future work.
- **No last-error field**: pool-level error history is not surfaced in this
  version.

## Level 5 - Session options

### `SET firebird_query_log_size = N`

Controls the maximum number of query telemetry entries kept in memory.

### `SET firebird_pool_enabled = true|false`

Enables or disables the ATTACH connection pool for future attaches.

### `SET firebird_pool_max_size = N`

Controls the maximum number of idle connections kept by the pool.

### `SET firebird_pool_idle_timeout_ms = MS`

Controls how long idle pooled connections may remain reusable.

## Type mapping notes

### `DECFLOAT(16)` / `DECFLOAT(34)`

Firebird 4+ `DECFLOAT(16)` and `DECFLOAT(34)` are IEEE 754 Decimal64 /
Decimal128. DuckDB has no native decimal-floating-point type, and the
legacy client path the extension uses has no decimal-float decoder.

These columns are surfaced as **VARCHAR**, produced by a server-side
`CAST(col AS VARCHAR(64))` in the generated query. Firebird stringifies the
value losslessly, so the column reads as exact text:

- ordinary decimals and exponent forms round-trip exactly
  (e.g. `123.45`, `1.234567890123456789012345678901234E+200`);
- `NaN`, `Infinity`, `-Infinity` come through as those literal strings;
- a real `NULL` stays `NULL` (the cast of NULL is NULL).

This **replaces the previous behavior**, where the column was typed
`DOUBLE` but always fetched `NULL` — a misleading schema. Lossless `VARCHAR`
is honest and queryable. There is no local Decimal64/Decimal128 decoder and
no `DOUBLE` fallback; a lossy numeric fast path would be a future opt-in.

Pushdown stays consistent with the `VARCHAR` schema: pushed filters (simple
comparisons, `IN` / `NOT IN`) compare against the same
`CAST(col AS VARCHAR(64))` expression, so text semantics match — e.g.
`WHERE D16 = '123.450'` does not match a stored `123.45` (numerically
equal, textually different).

Limitation: the DECFLOAT test fixture is dedicated
(`scripts/fixture_decfloat.sql`, referenced by `FIREBIRD_DECFLOAT_DB`) and
is **not yet in the main CI fixture** — the test skips when that variable is
unset. Promoting it into `setup_test_firebird.sh` with the coordinated
`metadata` / `dbt-sources` test updates is tracked as future work.

## Documentation premise

When public behavior changes:

- update this file
- update `docs/pt/function_manual.md`
- update usage guides when examples or workflows change
- update roadmap files when status changes

PT/EN docs must stay aligned in meaning, status, caveats, and examples.
