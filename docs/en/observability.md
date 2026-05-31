# Observability — Firebird query telemetry

The extension exposes two zero-argument table functions that surface
the SQL the Firebird scanner sends to the server, the redacted bind
values, pushdown metadata, and per-scan metrics. All state is scoped
to the current DuckDB `ClientContext` — one session never reads
another session's queries.

Both functions share the same 18-column schema. The difference is the
window: `firebird_last_query()` returns at most one row (the most
recent attempt on this connection); `firebird_query_log()` returns a
ring buffer, opt-in via setting.

---

## `firebird_last_query()`

### What it does

Snapshot of the most recently **attempted** remote query for the
current connection. The capture point sits in the scanner just before
`OpenCursor`, so if the server rejects the SQL the slot still shows
what was sent — useful for debugging an exception in isolation.

### How to use

```sql
SELECT * FROM firebird_last_query();
```

Empty result (zero rows) when nothing has been captured yet on this
connection.

### Output columns

| Column | Type | Notes |
|---|---|---|
| `remote_sql` | VARCHAR | Final `SELECT` emitted to Firebird |
| `binds` | VARCHAR[] | One entry per `?` placeholder, **redacted** |
| `table_name` | VARCHAR | Relation only; never a connection string |
| `projected_columns` | VARCHAR[] | Columns the planner asked for; `<rowid>` for the virtual rowid |
| `pushed_filters` | VARCHAR[] | WHERE fragments accepted by the builder + lifted predicates |
| `residual_filters` | VARCHAR[] | `filter[i]` references for filters DuckDB tried to push but the builder rejected |
| `rows_read` | BIGINT | Rows pulled from Firebird this scan |
| `firebird_time_us` | BIGINT | Sum of `Fetch()` wire time |
| `total_time_us` | BIGINT | Wall clock since capture |
| `connection_id` | BIGINT | Process-wide monotonic id assigned by the extension at `FirebirdConnection` construction; not a Firebird attachment id. Surfaces a real value for both `ATTACH` (from the pool lease) and direct `firebird_scan()` (no pool, but still a real id). |
| `connection_reused` | BOOLEAN | `true` when the connection came back from the pool's idle queue; `false` when freshly constructed. Direct `firebird_scan()` always reports `false`. Under `ATTACH`, the very first user SELECT may already report `true` because catalog initialization warmed the pool first - that is expected, not a bug. |
| `parallel_scan` | BOOLEAN | `true` when `partitions > 1` |
| `partitions` | INTEGER | Partition count for this scan |
| `captured_at` | TIMESTAMP | Local capture time |
| `error_message` | VARCHAR | Empty on success; sanitized exception text on failure |
| `limit_pushed` | BIGINT | The `ROWS` limit actually pushed to Firebird (`row_limit`), or `NULL` when no limit was pushed. `NULL` (not `0`) so a real limit of `0` is never ambiguous. |
| `offset_pushed` | BIGINT | The `ROWS m TO n` offset actually pushed (`row_offset`), or `NULL` when none. |
| `not_pushed_reasons` | VARCHAR[] | One coarse reason per `residual_filters` entry, same order/length. One of `NONE_CHARSET`, `UNSUPPORTED_OP`, `ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING`. |

`limit_pushed` / `offset_pushed` / `not_pushed_reasons` are the Phase 4 #3
pushdown-explainability columns. They make it explicit what paging reached
Firebird and why a filter stayed local, without adding a new function — the
schema is now 18 columns, shared by `firebird_last_query()` and
`firebird_query_log()`.

The reasons are factual and coarse, not a planner trace:

- `NONE_CHARSET` — the column is `CHARACTER SET NONE` text and pushdown is
  gated off so UTF-8 literals can't be miscompared against raw bytes. This
  is recorded for lifted complex predicates (`NOT IN`, and `LIKE` when it
  reaches the complex-filter path) that the scanner would otherwise push;
  the gated complex filter surfaces as a `complex_filter[none_gated]` entry
  in `residual_filters`.

  Known limitation: a *simple* comparison (`col = 'x'`, `col > 'x'`) on a
  `CHARACTER SET NONE` text column is often applied by DuckDB above the scan
  and never offered to the connector as a pushable filter, so it does not
  appear in `residual_filters` / `not_pushed_reasons` at all. A prefix
  `LIKE 'x%'` is likewise rewritten by DuckDB into a range comparison
  upstream and follows the same invisible path. Today the reliably-captured
  NONE gate is the complex `NOT IN` (and complex `LIKE`) case. Making the
  simple-comparison gate observable is future work.
- `UNSUPPORTED_OP` — the filter shape/operator/constant type is not one the
  builder translates to a Firebird predicate.
- `ROWID_OR_INVALID_COLUMN` — the filter targets the virtual rowid or a
  column outside the resolved schema.
- `UNSUPPORTED_PROJECTION_MAPPING` — the filter's projected column index
  could not be mapped back to a source column.

### Bind redaction

| Input | Surfaced as |
|---|---|
| NULL of any type | `<null>` |
| VARCHAR / CHAR / BLOB | `<text:redacted>` (content and length both hidden) |
| BOOLEAN, INTEGER family, FLOAT/DOUBLE, DATE/TIME/TIMESTAMP | Raw `Value::ToString()` |

The connection string is never stored. Only the table name appears.

### Error sanitization

When OpenCursor or Fetch raises, the scanner records the exception
text into `error_message` after passing it through
`SanitizeErrorMessage`:

- `password=...` (case-insensitive, up to next separator) → `password=<redacted>`
- `scheme://user:pass@host` → `scheme://<redacted>@host`

The slot stays populated so a failed SQL is still recoverable for
debugging.

### Examples

**Confirm the WHERE was pushed to Firebird (not filtered locally):**

```sql
SELECT COUNT(*) FROM firebird_scan('/path/db.fdb', 'EMPLOYEE')
 WHERE EMP_ID > 2;

SELECT remote_sql, pushed_filters, residual_filters
  FROM firebird_last_query();
```

Expected `remote_sql` contains `WHERE ("EMP_ID" > ?)`. `pushed_filters`
lists `'"EMP_ID" > ?'`.

**Spot a lost pushdown:**

```sql
SELECT COUNT(*) FROM firebird_scan('/path/db.fdb', 'EMPLOYEE')
 WHERE LENGTH(EMP_NAME) > 5;

SELECT remote_sql, pushed_filters FROM firebird_last_query();
```

Expected `pushed_filters` empty and `remote_sql` carries only the
partition predicate. DuckDB applied `LENGTH()` above the scan.

**Verify text bind redaction:**

```sql
SELECT COUNT(*) FROM firebird_scan('/path/db.fdb', 'EMPLOYEE')
 WHERE EMP_NAME = 'sensitive_value';

SELECT binds FROM firebird_last_query();
-- ['<text:redacted>']
```

**Inspect timing vs row count:**

```sql
SELECT COUNT(*) FROM firebird_scan('/path/db.fdb', 'EMPLOYEE');

SELECT rows_read,
       firebird_time_us,
       total_time_us,
       total_time_us - firebird_time_us AS overhead_us
  FROM firebird_last_query();
```

`firebird_time_us` is wire-level Fetch time only; the delta against
`total_time_us` is local work (type conversion, transcoding).

**Cross-check no-leak (compliance):**

```sql
SELECT COUNT(*) FROM firebird_last_query()
 WHERE remote_sql                              LIKE '%sensitive_value%'
    OR array_to_string(binds, ',')             LIKE '%sensitive_value%'
    OR table_name                              LIKE '%sensitive_value%'
    OR array_to_string(pushed_filters, ',')    LIKE '%sensitive_value%'
    OR array_to_string(residual_filters, ',')  LIKE '%sensitive_value%'
    OR error_message                           LIKE '%sensitive_value%';
-- Must return 0.
```

---

## `firebird_query_log()`

### What it does

Per-`ClientContext` ring buffer of the most recent captured queries,
most-recent first. Default **disabled** (`firebird_query_log_size = 0`).
Opt-in per session.

### How to enable

```sql
SET firebird_query_log_size = 32;        -- keep last 32 queries
```

Disable + clear:

```sql
SET firebird_query_log_size = 0;          -- next capture clears buffer
```

### How to use

```sql
SELECT remote_sql, rows_read, firebird_time_us
  FROM firebird_query_log()
 ORDER BY captured_at DESC;
```

### Output columns

Identical schema to `firebird_last_query()`. Same redaction policy
applies to every entry.

### Rotation

When the buffer reaches `firebird_query_log_size`, the oldest entry
is dropped on insert. The "current" entry (most recent push) keeps
updating its metrics as the scan progresses — it freezes when the
next `RecordQuery` arrives.

### Examples

**Audit a full BI report run:**

```sql
SET firebird_query_log_size = 32;
ATTACH '/path/db.fdb' AS fb (TYPE firebird);

SELECT DEPT_NO, COUNT(*) FROM fb.main.EMPLOYEE GROUP BY DEPT_NO;
SELECT AVG(SALARY) FROM fb.main.EMPLOYEE WHERE ACTIVE = TRUE;
SELECT EMP_NAME FROM fb.main.EMPLOYEE WHERE HIRE_DATE >= DATE '2020-01-01';

SELECT table_name, remote_sql, rows_read, firebird_time_us
  FROM firebird_query_log()
 ORDER BY captured_at DESC;
```

**Find the slow query in a relatório:**

```sql
SET firebird_query_log_size = 16;
-- ... run report ...
SELECT remote_sql, firebird_time_us
  FROM firebird_query_log()
 ORDER BY firebird_time_us DESC
 LIMIT 1;
```

**Validate no-leak across the buffer:**

```sql
SELECT COUNT(*) FROM firebird_query_log()
 WHERE remote_sql                              LIKE '%sensitive%'
    OR array_to_string(binds, ',')             LIKE '%sensitive%'
    OR error_message                           LIKE '%sensitive%';
-- Must return 0.
```

---

## Setting reference

| Setting | Type | Default | Effect |
|---|---|---|---|
| `firebird_query_log_size` | BIGINT | 0 | Ring-buffer size. `0` disables and clears the log. |

```sql
SELECT current_setting('firebird_query_log_size');
SET firebird_query_log_size = 16;
RESET firebird_query_log_size;
```

---

## Limitations

- **Connection metadata** — `connection_id` / `connection_reused`
  surface as `-1` / `false`. Future work: the pool needs to expose a
  cheap identifier.
- **Numeric / temporal redaction** — Phase 1 leaves these raw to keep
  debugging useful. A future
  `SET firebird_observability_redaction = 'strict' | 'debug'` switch
  will let strict mode redact by category.
- **Parallel scans** — when `partitions > 1`, the captured row
  reflects the most recently opened partition rather than an
  aggregate. The `parallel_scan` and `partitions` columns flag this.
