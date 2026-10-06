# Observability — Firebird query telemetry

The extension exposes two zero-argument table functions that surface
the SQL the Firebird scanner sends to the server, the redacted bind
values, pushdown metadata, and per-scan metrics. All state is scoped
to the current DuckDB `ClientContext` — one session never reads
another session's queries.

Both functions share the same 19-column schema. The difference is the
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
| `bytes_read_estimate` | BIGINT | **ESTIMATE** (G5) of Firebird payload bytes pulled by this query: `rows_read × XSQLDA row width + BLOB segment bytes read`. Definition and exclusions below. |

`limit_pushed` / `offset_pushed` / `not_pushed_reasons` are the Phase 4 #3
pushdown-explainability columns. They make it explicit what paging reached
Firebird and why a filter stayed local, without adding a new function.
`bytes_read_estimate` (G5) is the additive tail column — the schema is now
19 columns, shared by `firebird_last_query()` and `firebird_query_log()`.

The reasons are factual and coarse, not a planner trace:

- `NONE_CHARSET` — the column is `CHARACTER SET NONE` text and pushdown is
  gated off so UTF-8 literals can't be miscompared against raw bytes. This
  is recorded for lifted complex predicates (`NOT IN`, and `LIKE` when it
  reaches the complex-filter path) that the scanner would otherwise push;
  the gated complex filter surfaces as a `complex_filter[none_gated]` entry
  in `residual_filters`. With `none_pushdown=true` it additionally records
  the conservative fallback of the new `=` / IN pushdown: a literal with no
  byte in the target encoding (e.g. `'€'` under `iso8859_1`), or an `=` / IN
  over a NONE text BLOB (Firebird cannot compare BLOB values at all).

  Known limitation (without the flag): a *simple* comparison (`col = 'x'`,
  `col > 'x'`) on a `CHARACTER SET NONE` text column is applied by DuckDB
  above the scan and never offered to the connector as a pushable filter, so
  it does not appear in `residual_filters` / `not_pushed_reasons` at all. A
  prefix `LIKE 'x%'` is likewise rewritten by DuckDB into a range comparison
  upstream and follows the same invisible path. The opt-in fix is
  `none_pushdown=true`, which pushes `=` / IN over NONE CHAR/VARCHAR
  columns (re-encoding each literal to the storage bytes); ranges and
  `LIKE` remain client-side. Note `firebird_explain_pushdown` reports the
  builder-level view, so it does surface the NONE gate for simple
  comparisons that the post-run telemetry cannot see.
- `UNSUPPORTED_OP` — the filter shape/operator/constant type is not one the
  builder translates to a Firebird predicate.
- `ROWID_OR_INVALID_COLUMN` — the filter targets the virtual rowid or a
  column outside the resolved schema.
- `UNSUPPORTED_PROJECTION_MAPPING` — the filter's projected column index
  could not be mapped back to a source column.

#### Remedy per reason

The codes above are a stable API. Each maps to one factual remedy:

| Reason | Meaning | Remedy |
|---|---|---|
| `NONE_CHARSET` | The filter targets a `CHARACTER SET NONE` text column and was not pushed: either the flag is off (all text filters gated), the column is a NONE text BLOB, or — with `none_pushdown=true` — a literal has no byte in the target encoding. | Use `none_encoding='strict'` for known-UTF-8 data, or set `none_pushdown=true` (`firebird_scan` parameter / ATTACH option) to push `=` / IN over NONE CHAR/VARCHAR; ranges, `LIKE` and `NOT IN` always stay client-side. Confirm with `firebird_explain_pushdown` / `firebird_last_query()`. |
| `UNSUPPORTED_OP` | The filter operator, shape, or constant type has no Firebird SQL translation yet. | Simplify the predicate or accept DuckDB-side filtering for that term. |
| `ROWID_OR_INVALID_COLUMN` | The filter targets the virtual `rowid` or a column outside the resolved schema. | Filter on real columns instead of `rowid`. |
| `UNSUPPORTED_PROJECTION_MAPPING` | The filter's projected column index could not be mapped back to a source column. | Check the query's column list / projection shape. |

#### Unpushed-filter guard — `firebird_unpushed_mode`

Telemetry is passive by default. To make residual filters **act** instead
of only report, arm the guard:

```sql
SET firebird_unpushed_mode = 'silent';  -- default: telemetry only
SET firebird_unpushed_mode = 'warn';    -- one warning per scan
SET firebird_unpushed_mode = 'error';   -- fail the scan
```

- `silent` (default) — no behaviour change; at most one setting read per
  query.
- `warn` — emits one warning per scan that kept filters, through DuckDB's
  native warning channel (the same mechanism the engine uses for its own
  deprecation notices). The DuckDB CLI prints it to the console out of the
  box; embedded hosts receive it through the logging subsystem, and
  `SET warnings_as_errors = true` promotes it to an exception.
- `error` — the scan fails with an Invalid Input Error listing the residual
  filter count, the reason codes present, and the per-reason remedy. It
  fires **before any row is fetched**, so arming it before a long load
  fails fast instead of after the fact.

One report per scan: parallel scans (`partitions > 1`) report once, at the
first partition cursor open — the residual set is identical for every
partition.

Example — arm the guard before a long load:

```sql
SET firebird_unpushed_mode = 'error';
COPY (SELECT * FROM fb.main.BIG_TABLE WHERE NAME NOT IN ('X','Y'))
  TO 'load.parquet';
-- Invalid Input Error: firebird_unpushed_mode='error': this scan kept 1
-- filter(s) in DuckDB instead of pushing them down to Firebird.
--  NONE_CHARSET (1): columns declared CHARACTER SET NONE only push text
--  filters under none_encoding='strict' or, for '=' / IN on CHAR /
--  VARCHAR, with the none_pushdown=true opt-in - ranges, LIKE and NOT IN
--  are always transcoded client-side. Verify with
--  firebird_explain_pushdown() / firebird_last_query(). Reset with
--  SET firebird_unpushed_mode = 'silent'.

SET firebird_unpushed_mode = 'silent';  -- back to default in-session
```

Known limitation (pre-existing, see the `NONE_CHARSET` bullet above):
simple comparisons (`col = 'x'`) on `CHARACTER SET NONE` text columns are
applied by DuckDB above the scan and never offered to the connector, so
they do not surface as residual filters and do not trigger the guard.
`NOT IN` on such a column is the reliably-captured trigger. With the
`none_pushdown=true` opt-in, `=` / IN over NONE CHAR/VARCHAR columns are
pushed instead of remaining invisible; the guard still fires for the
fallback cases (unencodable literal, NONE text BLOB).

### Re-encoded literals in telemetry (none_pushdown)

Pushed `=` / IN fragments over `CHARACTER SET NONE` columns carry the
re-encoded storage bytes (for example `_WIN1252 'S\xE3o Paulo'`-shaped
SQL whose byte between `S` and `o` is `0xE3`, not valid UTF-8). DuckDB
validates `VARCHAR` on value construction, so `firebird_last_query()`,
`firebird_query_log()`, and `firebird_explain_pushdown()` display each
such byte escaped as `\xNN`. This is display-only: the SQL actually sent
to Firebird contains the real bytes, which is exactly what makes the
server-side byte-wise comparison lossless.

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

### Fetch-failure context (long fetches, -504 / -902)

When the data fetch of a scan dies mid-cursor — the classic
`-504 ... cursor lost` or `-902` after a NAT/firewall silently drops an
idle connection — the scanner re-raises the error with actionable
context attached, keeping the original message:

```
IO Error: Firebird fetch failed for table 'BIG_TABLE' after 4123877 rows
in this scan. The connection/cursor may have been dropped (idle
NAT/firewall timeout on long fetches - consider SET
firebird_dummy_packet_interval = 60). Inspect firebird_last_query() for
the exact remote SQL. Original error: <original Firebird message>
```

Where it appears: the query's `IO Error` in the client, and the same
enriched text in `firebird_last_query().error_message` (prefixed with
`Fetch: ` by the existing telemetry capture). What it means: the
original Firebird error is preserved verbatim after `Original error:`;
the row count is how far **this worker's** cursor got — under parallel
scans (`partitions > 1`) each worker has its own connection and cursor,
so the count is per-worker, not the query-wide total (the query-wide
`rows_read` in telemetry is the aggregate). The remedy named in the
message, `SET firebird_dummy_packet_interval = 60` (seconds), arms the
attach-DPB keepalive — see the session-options section of
`function_manual.md` for the exact semantics and the server-side
caveat. Only the scan data-fetch path carries this context; metadata
cursors and cursor-open failures surface their errors unchanged.

### `bytes_read_estimate` — definition (G5)

fbclient exposes no wire-traffic meter, so the extension reports an
**estimate** of the payload transferred per scan:

```
bytes_read_estimate = rows_read × XSQLDA row width + BLOB segment bytes
```

What counts:

- **XSQLDA row width** — the sum of the fetch descriptor sizes of the
  columns the cursor actually projects, fixed once at prepare time:
  `CHAR(n)` → `n` bytes; `VARCHAR(n)` → `n + 2` (2-byte length prefix);
  fixed-width numerics/temporals/booleans → their C sizes (`SMALLINT` 2,
  `INTEGER` 4, `BIGINT` 8, `FLOAT` 4, `DOUBLE` 8, `DATE`/`TIME` 4,
  `TIMESTAMP` 8, `BOOLEAN` 1, `INT128`/`DEC34`/`DECFLOAT(34)` 16,
  `DECFLOAT(16)` 8, TZ variants add 2–6 bytes); `BLOB` → 8 (the BLOB id
  frame; the descriptor carries no content).
- **BLOB content** — counted where it is actually read: every byte
  returned by `isc_get_segment` while materialising a BLOB column is
  added to the estimate.
- Under parallel scans, every worker's cursors contribute (the estimate
  is query-wide, unlike the per-worker row count in the G4 error text).
- Deterministic: identical re-runs report the identical estimate.

What does **not** count (why it is an estimate, not a wire meter):

- Protocol headers, per-row/per-message framing, op codes.
- Prepare/execute/describe-bind round-trips, including the metadata
  queries of the ATTACH/bind path (schema discovery, PK probe).
- Paging (`ROWS m TO n`) bookkeeping beyond the rows themselves.
- The `sqllen` of a `VARCHAR`/`CHAR` is the declared capacity — a row
  with short values still counts full declared width (a deliberate,
  stable over-approximation for text; actual `VARCHAR` payload is
  length-prefixed on the wire).

Use it for network budgeting on slow links (e.g. WAN at ~0.3 MB/s):

```sql
SELECT * FROM fb.main.BIG_TABLE LIMIT 0;   -- probe with a real scan
SELECT rows_read,
       bytes_read_estimate,
       bytes_read_estimate / rows_read     AS bytes_per_row,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb
  FROM firebird_last_query();
-- full-table transfer time ≈ est_mb / 0.3 MB/s
```

The per-catalog lifetime accumulation lives in
`firebird_pool_stats(catalog).bytes_read_estimate` (ATTACH-path scans
only — direct `firebird_scan()` belongs to no catalog).

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
| `firebird_unpushed_mode` | VARCHAR | `silent` | Reaction when a scan keeps filters un-pushed: `silent` (telemetry only), `warn` (one warning), or `error` (fail the scan). See the guard section above. |
| `firebird_dummy_packet_interval` | BIGINT | 0 | Keepalive in seconds sent as `isc_dpb_dummy_packet_interval` on every connection (`0` = off). Companion to the fetch-failure context above. |

```sql
SELECT current_setting('firebird_query_log_size');
SET firebird_query_log_size = 16;
RESET firebird_query_log_size;

SELECT current_setting('firebird_unpushed_mode');
SET firebird_unpushed_mode = 'error';
RESET firebird_unpushed_mode;
```

---

## Pool introspection — `firebird_pool_stats(catalog_name)`

A companion diagnostic to the query telemetry above. Where
`firebird_last_query()` / `firebird_query_log()` explain a single scan,
`firebird_pool_stats('alias')` reports the connection-pool state of one
attached Firebird catalog: `pool_enabled`, configured `max_idle_size` /
`idle_timeout_ms`, current `idle_connections`, lifetime
`total_created` / `total_reused` / `total_discarded`, and — since v1.1.0 —
`active_connections` (leases handed out and not yet returned) and
`last_error` (sanitized message of the most recent failed connection
creation, password redacted, `NULL` when none; only post-`ATTACH`
failures are recorded).

It takes an explicit ATTACH alias (it does not enumerate catalogs), reads
only counters the pool already tracks, and does not lease a connection, so
it never perturbs the pool it reports on. Full column reference lives in
`docs/en/function_manual.md`. The `connection_id` / `connection_reused`
fields on the per-query telemetry pair with these counters to show pool
reuse in action.

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
