# duckdb-firebird — exhaustive integration test report

Local container running Firebird 3.0.11 SuperServer + [DuckDB](https://github.com/duckdb/duckdb) v1.5.3 with
the extension statically linked. All tests executed against a live
Firebird instance with a realistic ERP-style fixture.

## Test fixtures

| Database | Tables | Rows | Notes |
|---|---|---|---|
| `test.fdb` | EMPLOYEE, FILE_STORAGE, BIG_T, BIG_T2 | 5 + 3 + 200k + 1M | Original CI fixture + binary BLOB |
| `biz.fdb` | DEPARTMENT, PRODUCT, CUSTOMER, SALES_HEADER, SALES_LINE, ACTIVITY_LOG, AUDIT_TRAIL, EMPTY_TABLE | 8 + 500 + 10k + 50k + 50k + 1.5M + 50 + 0 | Realistic ERP, mixed types, FK relations, indexes |
| `legacy_win1252.fdb` | FORNECEDOR | 3 | Brazilian Portuguese diacritics in a WIN1252 storage charset |

## Function coverage

| Function | Tests | Status |
|---|---|---|
| `firebird_scan(conn, table)` | basic scan, all 11 mapped types, NULL handling, BLOB sub_type 1, BLOB sub_type 0, named-param overrides (user/password/charset/role/dialect/partitions/row_limit) | ✅ |
| `firebird_tables(conn)` | discovers all 8 user tables, reports correct PK columns, filters out system tables | ✅ |
| `firebird_attach_sql(conn[, schema])` | emits well-formed DDL (`CREATE SCHEMA` + `CREATE OR REPLACE VIEW` per table), `.read`-able directly | ✅ |
| `ATTACH 'fb://...' AS x (TYPE firebird)` | catalog visible via `information_schema`, `DESCRIBE`, case-insensitive lookup, federated JOIN, CTAS, window functions, sub-queries, CTE, re-attach round-trip | ✅ |

## Pushdown coverage

Verified via `EXPLAIN` that the listed predicates reach the `FIREBIRD_SCAN`
node's `Filters:` line (i.e. they are translated to Firebird SQL, not
re-evaluated above the scan):

- `col = literal` (VARCHAR, INTEGER, DECIMAL, DATE, BOOLEAN, CHAR)
- `col <> literal`
- `col < / > / <= / >= literal`
- `col IS NULL` / `col IS NOT NULL`
- `col BETWEEN a AND b` (DuckDB decomposes to `>= AND <=`)
- `col IN (a, b, c)` — single SQL `IN (…)` clause on the Firebird side
- `OPTIONAL_FILTER` (unwrapped to its child)
- `AND` / `OR` conjunctions of the above

Projection pushdown verified: `EXPLAIN SELECT col1 FROM firebird_scan(...)
WHERE col2 = …` shows `Projections: col2, col1` (only what the query
needs plus what the filter needs).

## Type roundtrip

All types confirmed with `typeof()` and value-equality assertions:

| Firebird storage | DuckDB type | Verified by |
|---|---|---|
| `INTEGER` PK | `INTEGER` | DEPARTMENT.DEPT_ID |
| `BIGINT` PK | `BIGINT` | SALES_HEADER.SALE_ID, ACTIVITY_LOG.LOG_ID |
| `SMALLINT` | `SMALLINT` (`int16`) | SALES_HEADER.STATUS, ACTIVITY_LOG.SEVERITY |
| `CHAR(N)` | `VARCHAR` (trailing spaces trimmed) | DEPARTMENT.DEPT_CODE |
| `VARCHAR(N)` | `VARCHAR` | many |
| `NUMERIC(p,s)` scale > 0 | `DECIMAL(18, s)` (or `DECIMAL(38, s)` for INT128) | CUSTOMER.CREDIT_LIMIT, PRODUCT.PRICE |
| `FLOAT` | `FLOAT` | PRODUCT.DISCOUNT_PCT, CUSTOMER.LOYALTY_SCORE |
| `DOUBLE PRECISION` | `DOUBLE` | PRODUCT.WEIGHT_KG |
| `DATE` | `DATE` (MJD offset applied correctly) | many |
| `TIME` | `TIME` | CUSTOMER.SIGNUP_TIME, SALES_HEADER.SALE_TIME |
| `TIMESTAMP` | `TIMESTAMP` | many |
| `BOOLEAN` | `BOOLEAN` | PRODUCT.ACTIVE, CUSTOMER.IS_VIP |
| `BLOB SUB_TYPE 1` | `VARCHAR` (whole blob streamed) | PRODUCT.DESCRIPTION, ACTIVITY_LOG.PAYLOAD |
| `BLOB SUB_TYPE 0` | `BLOB` (octet_length correct) | FILE_STORAGE.PAYLOAD |
| NULL roundtrip | `NULL` | every nullable column |

Firebird 4 types (`INT128`, `TIMESTAMP_TZ`, `TIME_TZ`, `DECFLOAT`)
compile cleanly and have unit-level mapping code, but the test fixture
is Firebird 3 so they aren't exercised against a live server here.

### Firebird 5 live coverage (Windows local, May 2026)

A `biz4.fdb` fixture on Firebird 5.0.4 SuperServer was used to exercise
every type added in FB4+. All results confirmed against a fresh build
of the extension (DuckDB v1.5.3, MSVC 19.44):

| Firebird storage | RDB$FIELD_TYPE | DuckDB type | Verified |
|---|---|---|---|
| `INT128` | 26, sub=0 | `HUGEINT` | ✅ `170141183460469231731687303715884105727` round-trip |
| `DECIMAL(38, s)` | 26, sub=2, scale<0 | `DECIMAL(38, s)` | ✅ `12345678901234567890.12345` round-trip |
| `DECFLOAT(16)` | 24 | `VARCHAR` (was `DOUBLE`/NULL) | ✅ v0.6.0 — lossless VARCHAR via server-side `CAST(... AS VARCHAR(64))`; ends prior silent NULL. Tested locally (`firebird_decfloat.test`, dedicated `FIREBIRD_DECFLOAT_DB` fixture, skips in CI). |
| `DECFLOAT(34)` | 25 | `VARCHAR` (was `DOUBLE`/NULL) | ✅ v0.6.0 — same as above |
| `TIMESTAMP WITH TIME ZONE` | 29 | `TIMESTAMP WITH TIME ZONE` | ✅ UTC instant preserved (`2026-05-25 17:30:00.123+00`) |
| `TIME WITH TIME ZONE` | 28 | `TIME WITH TIME ZONE` | ✅ |

End-to-end demonstrated:

- **CTAS materialization** — `CREATE TABLE local_fb4 AS SELECT * FROM
  firebird_scan(…)`: 4 rows, all types preserved including HUGEINT
  and TIMESTAMP_TZ.
- **COPY to Parquet** — `COPY (...) TO 'fb4.parquet'`: Parquet writer
  degrades HUGEINT → DOUBLE (writer limitation, not the extension);
  DECIMAL(38,5) preserved exactly.
- **Federated JOIN** — Firebird `FB4_TYPES ⋈ read_parquet('fb4.parquet')`
  on a HUGEINT key column resolves correctly.
- **ATTACH catalog** — `ATTACH 'C:/fbtest/biz4.fdb' AS fb
  (TYPE firebird, user 'SYSDBA', password 'masterkey');` exposes all
  three tables with full FB4 type info in `SHOW ALL TABLES`. Subsequent
  `SELECT typeof(BIG_NUM) FROM fb.main.FB4_TYPES` returns `HUGEINT`.
- **Aggregates over HUGEINT** — `MIN/MAX/AVG` over the materialized
  local copy return the correct extremes.

A bug was found and fixed during this verification:
`LoadTableSchema` was translating the FB <=3 `RDB$FIELD_TYPE` codes
but had no entries for the FB4 additions (24=DECFLOAT(16),
25=DECFLOAT(34), 26=INT128, 28=TIME_TZ, 29=TIMESTAMP_TZ, 30=TIME_TZ_EX,
31=TIMESTAMP_TZ_EX). Without those, every FB4 column degraded to
VARCHAR. The mapping now matches Firebird's `blr.h` upstream.

## Edge cases

- **No PK** (AUDIT_TRAIL): scans single-threaded, correct row count.
- **Composite PK** (SALES_LINE): PK probe returns nullopt → scans
  single-threaded, correct row count.
- **Empty table** (EMPTY_TABLE): returns 0 rows cleanly, no special-case
  errors.
- **Re-ATTACH after DETACH**: pool tears down cleanly, second attach
  works.
- **Case-insensitive table lookup**: `fb.main.PRODUCT`, `fb.main.product`,
  and `fb.main."PRODUCT"` all resolve to the same entry.
- **Schema not found in attached catalog**: clear error from
  `LookupSchema` with the hint `only 'main' is exposed`.
- **Read-only enforcement**: `CREATE TABLE fb.main.…`, `DROP TABLE
  fb.main.…`, `INSERT INTO fb.main.…` all raise `NotImplemented` with a
  message pointing the user at a regular DuckDB schema for writes.
- **WIN1252 storage charset**: default `charset='UTF8'` produces correct
  UTF-8 strings (`Açúcar União Ltda`, `São Paulo`, …) because Firebird
  transliterates on-the-wire. Explicit `charset='WIN1252'` is now
  rejected at bind/connect with a hint to keep the default.

## Performance (Firebird 3 SuperServer, embedded, 4 vCPU, 15 GB RAM)

| Workload | Time | Notes |
|---|---|---|
| Schema discovery on ATTACH (7 user tables) | 1 ms | Lazy — runs on first table access |
| 50k-row COUNT(*) cold (pool warming) | 96 ms | First hit on SALES_HEADER |
| 50k-row COUNT(*) warm | 87 ms | Pool reuse confirmed |
| 10k-row COUNT(*) warm | 20 ms | Smaller table |
| Realistic dashboard JOIN (10 customers, 50 k orders) | 151 ms | Firebird ⋈ Firebird via DuckDB |
| 1.5 M-row scan, partitions=1 | 2.29 s | Single-thread, no PK probe |
| 1.5 M-row scan, partitions=4 | 2.24 s | Parallel helps marginally on SuperServer (server-side scheduler serializes) |
| 1.5 M-row scan + GROUP BY EVENT, SEVERITY | 3.42 s | Aggregation in DuckDB on top of full scan |
| row_limit=1000 on 1.5 M-row table | 0.63 s | `ROWS 1000` makes Firebird return only 1000 rows |
| 10 sequential point-lookups via attached catalog | 1-2 ms each | Pool + cache fully warmed after first |

## Federated query coverage

- Firebird ⋈ Firebird (DEPARTMENT ⋈ SALES_HEADER) — done
- Firebird ⋈ DuckDB local temp table — done
- Firebird ⋈ Parquet file (CUSTOMER → /tmp/customer.parquet, then JOIN
  back) — done
- `COPY (SELECT … FROM firebird_scan(…)) TO 'x.parquet'` — done
- CTAS (`CREATE TABLE local AS SELECT * FROM fb.main.DEPARTMENT`) — done
- DROP local table after CTAS — done

## Error-path coverage

| Scenario | Result |
|---|---|
| Unknown table | `BinderException: firebird_scan: table 'X' not found...` |
| Bad password (TCP) | `IOException: ...Your user name and password are not defined...` |
| Missing database file | `IOException: isc_attach_database... No such file or directory` |
| `partitions=-1` | `BinderException: partitions must be >= 0 (0 = auto)` |
| `row_limit=0` | `BinderException: row_limit must be > 0` |
| `charset='WIN1252'` (direct scan) | `BinderException: charset='WIN1252' would deliver non-UTF-8 bytes...` |
| `charset='WIN1252'` (ATTACH) | `IOException: firebird: charset='WIN1252' would deliver non-UTF-8 bytes...` |
| `CREATE TABLE fb.main.X` | `NotImplemented: Firebird ATTACH catalog is read-only...` |
| `DROP TABLE fb.main.X` | `NotImplemented: ...DROP is not supported...` |
| `INSERT INTO fb.main.X` | `NotImplemented: INSERT not supported on Firebird catalog` |
| `SELECT FROM fb.unknown_schema.Y` | `Catalog Error: Table with name Y does not exist!` |

## Views + materialized-view pattern

- `RDB$RELATION_TYPE = 1` (Firebird views) now appear alongside tables in
  `firebird_tables()`, `firebird_attach_sql()`, and the attached
  catalog. The `firebird_tables()` output gained a `kind` column
  (`table`, `view`, `external`, `gtt`).
- Server-side joins are honored: a `CREATE VIEW V_CUSTOMER_SUMMARY AS
  SELECT CUST_ID, COUNT(*), SUM(TOTAL_AMT) FROM CUSTOMER LEFT JOIN
  SALES_HEADER GROUP BY …` returns the aggregated rows from a single
  `SELECT * FROM biz.main.V_CUSTOMER_SUMMARY` — Firebird runs the join
  and DuckDB sees the rolled-up rows.
- Materialized-view pattern via `CREATE TABLE local AS SELECT * FROM
  biz.main.V_CUSTOMER_SUMMARY`: the first run is bounded by the
  underlying view cost (~0.2 s with a PK index on the join column,
  ~200 s without); subsequent queries against the local table return
  in ~2 ms.

## Arrow Flight SQL (GizmoSQL)

| Property | Result |
|---|---|
| Extension ABI compat with GizmoSQL's bundled DuckDB v1.5.3 | ✅ — rebuilt against v1.5.3 cleanly |
| Extension loads as a custom `init.sql` step under `--init-sql-commands-file` | ✅ (when allowed) |
| GizmoSQL startup in this sandboxed container | ⚠️ blocked: hardcoded prelude runs `INSTALL icu; LOAD icu; INSTALL spatial; LOAD spatial;` before user init can run, and our network policy denies `extensions.duckdb.org`. Sites with that endpoint reachable, or with the official extensions pre-cached, run fine. |

The README's "Arrow Flight SQL via GizmoSQL" section documents the
init.sql shape and the deployment constraint.

## v0.5 additions (May 2026 release series)

The v0.5 series adds analyst-facing capabilities on top of the v0.4
core. All items below are verified against both FB5 (Windows local)
and FB4 (embedded `C:\fb4`).

| Capability | How verified |
|---|---|
| `row_offset` named param (paging) | `firebird_paging.test` — standalone `row_limit`, `row_limit + row_offset`, partitions guard, overflow guard |
| Prepared statements with input XSQLDA bind | `firebird_bind_params.test` — apostrofe (`D'Agua`), `IN`, DATE, BOOLEAN, IS NULL |
| `NOT IN` + boolean `NOT` pushdown | `firebird_predicates.test` — server-side translation confirmed via `EXPLAIN` |
| `CHARACTER SET NONE` decoding modes | `firebird_none_charset.test` — `strict` / `win1252` (default) / `iso8859_1` / `blob`, with ATTACH + filter coverage |
| `information_schema` + `SHOW` + `DESCRIBE` | `firebird_metadata.test` — full BI-tool introspection surface locked in |
| `RDB$NULL_FLAG` propagation | `firebird_metadata.test` + `firebird_attach.test` — `is_nullable = NO` for declared NOT NULL columns; `NotNullConstraint` applied in CreateTableInfo |

### Paging caveat

`row_limit`/`row_offset` emit `ROWS M+1 TO M+N` in Firebird (1-based,
inclusive). When paging is requested, the scan is forced serial
(`partitions = 1` internally); requesting `partitions > 1` together
with explicit paging is rejected at bind. This is physical paging,
not keyset pagination — without a stable `ORDER BY`, successive pages
may repeat or skip rows.

### Bind variables (internal)

Filter values that are safe to bind (DATE / TIMESTAMP / VARCHAR /
INTEGER / BIGINT / FLOAT / DOUBLE / BOOLEAN, excluding unsigned ints
and TZ types) are emitted as Firebird XSQLDA input parameters rather
than inline. This is transparent to the user; it prevents escape
mistakes and lets Firebird reuse prepared plans. TIMESTAMP_TZ /
TIME_TZ / INT128 / DECFLOAT remain residual in DuckDB on the bind
path.

### Live legacy ERP verification (FB5, May 2026)

An anonymized legacy ERP backup was used to smoke
each v0.5 flow end-to-end:

| Flow | Result |
|---|---|
| `row_limit=1000` on `TABENTRADASAIDA` | 1000 rows |
| `row_limit=5, row_offset=100000` | 5 IDMASTER values, paged from offset |
| VARCHAR bind, `OBSERVACOES LIKE 'CONTA PARA%'` | matches preserved with apostrophes |
| NONE col bind, `BAIRRO = 'CENTRO'` on `TABPESSOAS` | 1446 rows (residual + win1252 default) |
| `CODIGOEMPRESA NOT IN (99, 100, 101)` | 1 170 839 rows |
| `DESCRIBE fb.main.TABPESSOAS` | full column list with correct types |
| `information_schema.columns` filter on `fb.TABPESSOAS` | column list with `ordinal_position`, `is_nullable` |
| `SHOW TABLES FROM fb` | 2786 entries |

## sqllogictest summary

```
test/sql/firebird_scan.test          — 124 assertions, all green
test/sql/firebird_attach.test        —  79 assertions, all green
test/sql/firebird_paging.test        —  16 assertions, all green
test/sql/firebird_bind_params.test   —  20 assertions, all green
test/sql/firebird_predicates.test    —  13 assertions, all green
test/sql/firebird_none_charset.test  —  42 assertions, all green
test/sql/firebird_metadata.test      —  79 assertions, all green
total (FB5)                          — 373 assertions
total (FB4 embedded)                 — 373 assertions (FB4 covers TZ + DECFLOAT natively)
```

CI: `.github/workflows/build-linux.yml` reproduces the single-server
fixture setup (installs `firebird3.0-server`, runs
`scripts/setup_test_firebird.sh`, builds, and replays the SQL suite) on
every push to `main`.

`.github/workflows/build-linux-fb-matrix.yml` runs the Firebird 3 / 4 /
5 matrix through Docker, including the Firebird 4+ type smoke coverage.

`.github/workflows/build-windows.yml` builds the same code with
`windows-latest` against a downloaded Firebird Windows SDK ZIP and
uploads the resulting `firebird.duckdb_extension` as a release artifact.

### v0.6.0 public release and community verification

`v0.6.0` is the current public release and the first tag referenced by the
merged DuckDB Community descriptor. It includes the v0.5 public-repo hardening,
runtime Firebird client loading, and the v0.6 diagnostics/reliability work:

- `firebird://host:port/path` URL parsing now emits libfbclient's
  `host/port:/path` remote database form.
- Firebird TZ EX SQL type fallbacks are defined for Linux distro
  headers that omit `SQL_TIMESTAMP_TZ_EX` / `SQL_TIME_TZ_EX`.
- Linux Firebird fixture setup was hardened for GitHub-hosted runners.
- GitHub Actions dependencies were updated by Dependabot.
- `firebird_profile_table`, pushdown explainability, connection-pool stats,
  DECFLOAT lossless fallback, and adaptive parallel-scan recommendations
  shipped in v0.6.0.

GitHub Actions for the public release path:

| Workflow | Result |
|---|---|
| Build + Test Linux x64 | success |
| Build + Test Linux x64 (FB 3/4/5 matrix) | success |
| Build Windows x64 | success |

DuckDB community submission
[`duckdb/community-extensions#1980`](https://github.com/duckdb/community-extensions/pull/1980)
was merged on 2026-06-03 and points to `repo.ref: v0.6.0`.

### Known gaps deferred past v0.5

- `information_schema.tables.table_type` reports `BASE TABLE` for
  Firebird views (`RDB$RELATION_TYPE = 1`). Distinguishing `VIEW`
  needs a ViewCatalogEntry path in the storage extension; punted
  past v0.5.
- No LRU connection cache. The current pool tears down on DETACH and
  rebuilds on next attach; fine for analyst sessions, revisited if
  long-lived ATTACH workloads emerge.
- No automatic `LIMIT` pushdown. The optimizer can move LIMIT below
  the scan in some DuckDB plans, but the storage extension does not
  yet rewrite `LIMIT N` into a Firebird `ROWS 1 TO N` automatically;
  users opt in via `row_limit=`.
