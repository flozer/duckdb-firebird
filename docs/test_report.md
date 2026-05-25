# duckdb-firebird — exhaustive integration test report

Local container running Firebird 3.0.11 SuperServer + DuckDB 1.4.4 with
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

## sqllogictest summary

```
test/sql/firebird_scan.test    — 111 assertions, all green
test/sql/firebird_attach.test  —  63 assertions, all green
total                          — 174 assertions
```

CI: `.github/workflows/build-linux.yml` reproduces this entire setup
(installs `firebird3.0-server`, runs `scripts/setup_test_firebird.sh`,
builds, and replays both test files) on every push to a
`claude/duckdb-firebird-plugin-*` or `main` branch.

`.github/workflows/build-windows.yml` builds the same code with
`windows-latest` against a downloaded Firebird Windows SDK ZIP and
uploads the resulting `firebird.duckdb_extension` as a release artifact.
