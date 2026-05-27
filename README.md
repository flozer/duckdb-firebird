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

Security note: connection strings and SQL examples may be stored in shell
history, notebooks, DuckDB logs, or application traces. Use least-privilege
Firebird users for production access and avoid committing real credentials in
scripts or documentation.

### Named parameter overrides

```sql
SELECT * FROM firebird_scan('/data/prod.fdb', 'EMPLOYEE',
                            user='analytics',
                            password='secret',
                            charset='UTF8',
                            role='READER',
                            dialect=3,
                            partitions=4,
                            row_limit=100);
```

### Charset handling

DuckDB stores strings as UTF-8 internally. The extension **only accepts
`UTF8`, `UTF-8`, `NONE`, or `OCTETS`** for the client `charset`
parameter; anything else (e.g. `WIN1252`, `ISO8859_1`) is rejected at
bind time with a hint.

#### CHARACTER SET NONE

When the **database itself** (or any text column) is declared
`CHARACTER SET NONE`, Firebird does *not* transliterate the bytes
to the client's `lc_ctype` — it just hands the raw bytes through.
DuckDB's VARCHAR requires valid UTF-8, so the extension has to
choose how to decode those bytes.

The default is **`none_encoding='win1252'`** — aligned with
firebird-cdc-rust's `charset_for_none_fields` setting and matches
the overwhelming majority of legacy Brazilian / Western-European
ERPs (Athenas, IBExpert exports, Delphi-era apps, RM Sistemas, …)
that wrote their NONE columns through a Windows-1252 client.

```sql
-- No option needed for the common case:
SELECT * FROM firebird_scan('C:/legacy/athenas.fdb', 'TABENTRADASAIDA');
ATTACH 'C:/legacy/athenas.fdb' AS legacy (TYPE firebird,
                                          user 'SYSDBA',
                                          password 'masterkey');
SELECT * FROM legacy.main.TABENTRADASAIDA;
```

If your database wrote NONE columns through a different encoding,
or if you want a hard fail when the bytes don't decode cleanly,
pass `none_encoding` explicitly:

| `none_encoding=` | Effect |
|---|---|
| `'win1252'` (default) | Decodes bytes as Windows-1252 → UTF-8. |
| `'iso8859_1'` (alias `'latin1'`) | Decodes bytes as ISO-8859-1 → UTF-8 (covers strict Latin-1 inputs). |
| `'strict'` | Accepts only valid UTF-8; raises an informative `IOException` otherwise. Use when the source is guaranteed UTF-8 and you want any drift to surface immediately. |
| `'blob'` | Surfaces NONE text columns as DuckDB `BLOB` instead of `VARCHAR` — use when you don't know the encoding and want raw bytes. |

> Note: do **not** confuse `none_encoding=` with `charset=`.
> `charset=` controls Firebird's `lc_ctype` for the connection
> (the wire client's encoding); `none_encoding=` controls how the
> extension interprets bytes from **NONE-declared columns** *after*
> they reach DuckDB. For Athenas-style databases use the defaults:
> `charset=UTF8` + `none_encoding=win1252` (i.e. no overrides).

While `none_encoding` is `'win1252'`, `'iso8859_1'`, or `'blob'`,
filter pushdown on text columns whose Firebird charset is `NONE` is
**disabled** — DuckDB's literals are UTF-8 and would not match the
raw bytes server-side. Numeric / date filters are still pushed.
Filters in DuckDB land are applied *after* transcoding, so they
work as expected. This is the deliberate, conservative behaviour;
do not work around it by re-enabling pushdown without a verified
reverse-transcode plan.

This is not a limitation for legacy Brazilian Portuguese / Latin-1
databases. Firebird stores the bytes in whatever character set the table
was declared with, but **the wire protocol transliterates** to whatever
the client `lc_ctype` asks for. So a `DEFAULT CHARACTER SET WIN1252`
database queried with the default `charset=UTF8` returns proper UTF-8
strings — `São Paulo`, `Açúcar União Ltda`, `Coração Forte`, all
render correctly:

```sql
-- Works against a legacy WIN1252 database without any extra config:
SELECT NOME, CIDADE FROM firebird_scan('/legacy/erp.fdb', 'FORNECEDOR');
--   NOME                    CIDADE
--   Açúcar União Ltda       São Paulo
--   Coração Forte ME        Belo Horizonte
--   Indústrias Pôr-do-Sol   Curitiba
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

Native `ATTACH ... AS fb (TYPE firebird)` is available for read-only
catalog access. The generated-view recipe above remains useful when you
want explicit local DuckDB views over selected Firebird tables.

## Quick-start guides

- [`docs/usage_guide.md`](docs/usage_guide.md) — analyst-oriented guide
  covering live scans, `ATTACH`, views, materialized DuckDB tables,
  refresh patterns, exports, and troubleshooting.
- [`docs/guide_windows.md`](docs/guide_windows.md) — end-to-end build
  and verification on Windows 11 with Firebird 5 + MSVC 2022.
- [`docs/guide_linux.md`](docs/guide_linux.md) — same flow on Linux,
  using `apt`-shipped Firebird and the existing Makefile harness.
- [`docs/roadmap.md`](docs/roadmap.md) — what's done, what's next.

## Project governance

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — development, testing, and pull
  request expectations.
- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — participation rules.
- [`SECURITY.md`](SECURITY.md) — private vulnerability reporting and
  security scope.

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

## Supported DuckDB versions

Built and tested against **DuckDB v1.5.3** (Variegata). The
`StorageExtension::Register` API used by the ATTACH path landed in v1.5,
so this branch targets v1.5.3+. (A v1.4-compatible variant lived earlier
in the history if you need to pin older.)

## What's implemented (v0.5, pre-release on feat/v0.5-analytics-platform)

| Feature                                                 | Status |
|---|---|
| `firebird_scan(conn, table)`                            | ✅ |
| `firebird_tables(conn)` — list user tables + PK info    | ✅ |
| `firebird_attach_sql(conn[, schema])` — DDL for view-based attach | ✅ |
| **`ATTACH 'firebird://…' AS fb (TYPE firebird)`** — native catalog | ✅ |
| Connection pool + lazy metadata cache (per ATTACH)        | ✅ |
| Firebird 4 types: HUGEINT (`INT128`), `TIMESTAMP_TZ`, `TIME_TZ`, DECIMAL(38) | ✅ |
| `row_limit=N` named parameter (Firebird-side `ROWS N`)    | ✅ |
| CHARACTER SET NONE handling (default `none_encoding='win1252'`; `strict` / `iso8859_1` / `blob` available) | ✅ |
| **Views** (`RDB$RELATION_TYPE=1`) — scan + ATTACH catalog | ✅ |
| **External tables** + global temporaries (types 2, 4, 5)  | ✅ |
| Materialized-view pattern (CTAS from a Firebird view)     | ✅ |
| Arrow Flight SQL via GizmoSQL — extension is v1.5 ABI compatible | 🟡 (works once GizmoSQL accepts a signed build; community-extensions publication does that) |
| Named parameter overrides (user / password / charset / role / dialect / **partitions** / **none_encoding**) | ✅ |
| Projection pushdown                                     | ✅ |
| Filter pushdown — `=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `IS NULL`, `BETWEEN`, **`IN(…)`**, optional-filter unwrap, **`LIKE 'prefix%'`** (gated on NONE-text when transcoding) | ✅ |
| PK-range parallel scan (opt-in via `partitions=N`)      | ✅ |
| Type mapping — INTEGER / VARCHAR / DECIMAL(scale) / DATE / TIMESTAMP / TIME / BOOLEAN / BLOB SUB_TYPE 1 → VARCHAR / BLOB | ✅ |
| End-to-end test fixture in CI (Linux + real Firebird 3) | ✅ |
| Windows x64 build via GitHub Actions                    | ✅ |
| `row_limit=N` / `row_offset=M` paging — Firebird `ROWS m TO n` | ✅ (v0.5; auto-serial; explicit `partitions > 1` is rejected) |
| Prepared statements with input XSQLDA bind variables   | ✅ (v0.5; HUGEINT / DECIMAL still inline-literal, TZ types residual) |
| Extended predicate pushdown: `NOT IN`, `NOT bool`      | ✅ (v0.5; via `pushdown_complex_filter`) |
| `information_schema.tables` / `.columns`, `SHOW TABLES`, `DESCRIBE` | ✅ (v0.5; regression-locked) |
| LIMIT pushdown (automatic, from a `LIMIT N` next to the scan) | 🟡 deferred — see `docs/roadmap.md` item 12 (waiting for an upstream DuckDB hook) |
| LRU prepared-statement cache per connection            | ⏳ deferred — bind variables shipped, cache pending a benchmark |
| Arrow `RecordBatch` produced directly by the extension | ⏳ v1.x — today DuckDB does the conversion, see roadmap "Milestone v1.x" |
| Stable C extension ABI                                  | ⏳ next |

> **Arrow note**: this extension hands rows to DuckDB as native
> `Vector` / `DataChunk` columns. When an Arrow Flight SQL client
> (GizmoSQL, ADBC, Polars) consumes the result, the DuckDB engine —
> not the extension — converts to `arrow::RecordBatch` at the
> result-stream boundary. Arrow integration therefore works
> end-to-end through DuckDB; the scanner itself is intentionally
> *not* Arrow-native. A direct `firebird_arrow_scan` would be a
> separate product/API (see `docs/roadmap.md` "Milestone v1.x")
> and is not in scope for v0.5.

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

## Arrow Flight SQL via GizmoSQL

GizmoSQL is an Arrow Flight SQL server backed by DuckDB. Once it can
load this extension you can query Firebird from any Flight SQL client
(JDBC, ADBC, Python, Java, …) without going through DuckDB locally.

The extension binary itself is v1.5 ABI-compatible, which matches the
DuckDB bundled in `gizmosql_server v1.26.x`. Start it with an init
script that loads the extension and (optionally) pre-attaches your
Firebird DB:

```bash
cat > init.sql <<'EOF'
SET allow_unsigned_extensions = true;
LOAD '/path/to/firebird.duckdb_extension';
ATTACH 'firebird://SYSDBA:pwd@host/path/db.fdb' AS fb (TYPE firebird);
EOF

GIZMOSQL_PASSWORD='strong-pw' gizmosql_server \
    --port 31337 \
    --init-sql-commands-file init.sql
```

Then connect from any Flight SQL client. With the Python ADBC driver:

```python
import adbc_driver_flightsql.dbapi
con = adbc_driver_flightsql.dbapi.connect(
    "grpc+tcp://localhost:31337",
    db_kwargs={"username": "gizmosql_user", "password": "strong-pw"})
cur = con.cursor()
cur.execute("SELECT * FROM fb.main.EMPLOYEE WHERE DEPT_NO = '600'")
print(cur.fetchall())
```

**Caveat**: GizmoSQL's startup unconditionally runs
`INSTALL icu; INSTALL spatial;`. Networks that can't reach
`extensions.duckdb.org` will fail before the user `init.sql` is
processed. Two ways to handle it:

- Run `scripts/gizmosql_aircache.sh` on a connected host once; it
  pre-populates `~/.duckdb/extensions/v1.5.3/linux_amd64/` with
  `icu` and `spatial`. Bind-mount that directory into the GizmoSQL
  container (`-v ~/.duckdb:/root/.duckdb`).
- Or run GizmoSQL where `extensions.duckdb.org` is directly reachable.

`scripts/gizmosql_smoke.sh` is an end-to-end smoke test: brings up a
Firebird 5 container, a GizmoSQL container with the extension
bind-mounted, applies the biz4 fixture, and asserts a row count +
type over Arrow Flight SQL.

## Publishing to the community catalog

`community-extensions/description.yml` carries the submission metadata.
To make the extension installable as

```sql
INSTALL firebird FROM community;
LOAD firebird;
```

fork [`duckdb/community-extensions`](https://github.com/duckdb/community-extensions),
copy `community-extensions/description.yml` into
`extensions/firebird/description.yml`, set `repo.ref` to the tag /
commit to publish, and open a PR. The community repo's CI builds and
signs the artefacts for every supported platform from that ref.

## License

MIT — see [`LICENSE`](LICENSE).

## Credits

Design and type-mapping decisions trace directly to
[`firebird_peregrine_falcon`](https://github.com/flozer/firebird_peregrine_falcon)
(the 1 M rows/s Firebird → Parquet extractor); look at its
`src/extractor.rs` and `PERFORMANCE_OPTIMIZATION_REPORT.md` for the
upstream prior art.
