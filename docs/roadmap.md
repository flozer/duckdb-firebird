# duckdb-firebird — Roadmap

Living plan. Ordered by dependency / ROI. Acceptance criteria spelled
out so each item can be closed independently.

## Compatibility matrix (target)

| Component | Today (v0.3) | Target (v0.4) | Target (v1.0) |
|---|---|---|---|
| DuckDB | v1.5.3 | v1.5.3 | v1.5.x + Stable C ABI when StorageExtension lands in it |
| Firebird server | 3.0 (tested live), 4/5 (compile only) | 3.0, **4.0**, **5.0** all tested live | same |
| Firebird client (`libfbclient`) | 3.0+ | 4.0+ recommended (INT128/TZ/DECFLOAT) | 4.0+ |
| GizmoSQL | works *if* `extensions.duckdb.org` reachable | smoke test in CI + air-gapped recipe | bundled docker image |
| Platforms | Linux x64, Windows x64 | + macOS arm64 | + Linux arm64 |

The XSQLDA codes for `SQL_INT128 / SQL_TIMESTAMP_TZ / SQL_TIME_TZ /
SQL_DEC16 / SQL_DEC34` are already in `firebird_types.cpp` and the
mapping compiles against `ibase.h` headers shipped in Firebird 4+. What
is missing is **live verification against a real FB4/FB5 server**, plus
a couple of fixes the live test will surface (DECFLOAT, parallel
workers, FB5 catalog drift).

---

## Milestone v0.4 — Firebird 4/5 live coverage

### 1. FB4 + FB5 docker fixtures (foundation)

Without these the rest of the milestone is unverifiable.

- Add `scripts/setup_test_firebird_v4.sh` and `setup_test_firebird_v5.sh`
  mirroring the existing v3 script. Use `firebirdsql/firebird:v4` and
  `firebirdsql/firebird:v5` images (or build from `C:\tmp\firebird-src`
  for v5 RC1 if no image exists yet).
- Add a `biz4.fdb` fixture exercising **every** new FB4+ type:
  `INT128`, `NUMERIC(38,…)`, `DECFLOAT(16)`, `DECFLOAT(34)`,
  `TIMESTAMP WITH TIME ZONE`, `TIME WITH TIME ZONE`.
- New CI workflow `build-linux-fb4.yml` (matrix: fb-version ∈ {3, 4, 5})
  replays both sqllogictest files plus a new `firebird_fb4_types.test`.

**Acceptance**: green CI on FB3 / FB4 / FB5 with all 11 existing types +
the 5 new FB4 types.

### 2. Live type verification — INT128 / TIMESTAMP_TZ / TIME_TZ — **DONE**

Verified against Firebird 5.0.4 on Windows (May 2026). All three map
correctly:

- INT128 max/min (`±170141183460469231731687303715884105727`) → HUGEINT
  round-trip exact.
- DECIMAL(38, 5) backed by INT128 → DECIMAL(38, 5) exact.
- TIMESTAMP_TZ with IANA zone (`America/Sao_Paulo`) → TIMESTAMP WITH
  TIME ZONE; the IANA zone is *not* preserved (DuckDB's type is
  offset-only), but the UTC instant matches.
- TIME_TZ → TIME WITH TIME ZONE, same offset-only behaviour.

A bug found during this verification has been fixed (`LoadTableSchema`
was missing the FB4 RDB$FIELD_TYPE codes 24–31 — see test_report.md
"Firebird 5 live coverage" section).

### 3. DECFLOAT (DEC16 / DEC34) — fix the silent NULL

Today `firebird_types.cpp:197-202` writes NULL for `SQL_DEC16/SQL_DEC34`
because DuckDB has no native IEEE decimal-floating-point type. Three
viable options, decide by user value:

| Option | Pros | Cons |
|---|---|---|
| **VARCHAR with full precision** | lossless, easy `CAST(... AS DECIMAL(38, s))` from SQL | column type is text — users must cast |
| **DOUBLE with a one-time warning** | usable as a number out of the box | silent precision loss on long-tail values; loud `WARN` only fires once per session |
| **DECIMAL(38, scale) when scale ≤ 38** | exact for the bulk of real-world DECFLOAT use | falls back to one of the other two when value width exceeds 38 |

Recommended path: **VARCHAR by default + named param
`decfloat='double'` to opt into the fast/lossy path**. Pre-emptive
casting (`SELECT CAST(x AS DECIMAL(38, 5)) FROM …`) is what most users
will end up writing anyway.

**Acceptance**: scan over `t (d16 DECFLOAT(16), d34 DECFLOAT(34))`
returns the exact decimal string for at least these edge cases:
`+Inf`, `-Inf`, `NaN`, `0`, `0.0000000000000001`, `1.7976931348623157E+308`.

### 4. FB5 compatibility sweep

Firebird 5.0 RC1 changelog flags a few items worth verifying:

- **#7682 ParallelWorkers default from `firebird.conf`** — server-side
  parallelism. The existing `partitions=N` named param still controls
  client-side parallelism; we should *document* the interaction (use
  `partitions=1` on FB5 with high `ParallelWorkers` to avoid 2x
  parallelism).
- **#7707 Better IN-list optimization** — verify our `IN (…)` pushdown
  still wins over per-element OR when the list has 1000+ items.
- **`RDB$RELATION_TYPE`** values — confirm types 1 (view), 2 (external),
  4 (GTT preserve), 5 (GTT delete) still match in FB5; spot-check
  `RDB$INDICES`, `RDB$RELATION_CONSTRAINTS`, `RDB$INDEX_SEGMENTS` for
  the PK probe path didn't add required columns.
- **DSQL_unprepare on close** — confirm the legacy ISC API path
  (`isc_dsql_prepare` / `isc_dsql_fetch`) still works on FB5; the
  modern OO API (`Attachment::prepare`) is preferred upstream but
  switching is a v1.0 task.

**Acceptance**: the existing test suite passes unchanged against FB5;
README adds a "Firebird 5 + ParallelWorkers" note.

### 5. GizmoSQL smoke test — **scripts shipped, CI wiring pending**

Two scripts now in `scripts/`:

- `gizmosql_aircache.sh` — downloads `icu` + `spatial` for
  `${DUCKDB_VERSION:-v1.5.3}` / `${PLATFORM:-linux_amd64}` and
  populates `~/.duckdb/extensions/…`. Bind-mount that dir into
  GizmoSQL on air-gapped hosts.
- `gizmosql_smoke.sh` — brings up Firebird 5 + GizmoSQL containers,
  loads the extension, applies the biz4 fixture, and asserts a row
  count + dtype over Arrow Flight SQL via the Python ADBC driver.

**Acceptance** (still open): a CI job that runs `gizmosql_smoke.sh`
on every push to main. The matrix workflow `build-linux-fb-matrix.yml`
is the obvious host for it; deferred until the GitHub Actions billing
hold on this repo lifts.

---

## Milestone v0.5 — Pushdown completeness

### 6. LIMIT pushdown — **deferred (upstream gap)**

Investigation against DuckDB v1.5.3 source shows `TableFunction` has
no `pushdown_limit` hook and `LogicalGet` has no `limit` field; a SQL
`LIMIT N` lands in a `LogicalLimit` *above* the scan, so the planner
never tells our scanner about it. The optimiser stops calling the
scan once N rows are emitted (cheap on a small N), so the missing
piece only matters when the cost of *opening* the cursor over a huge
result set is itself prohibitive.

**Workaround in place:** `firebird_scan(…, row_limit=N)` named param
already emits `ROWS N` in the Firebird SQL — the user just has to
write it explicitly when they know the limit at query time. The
`firebird_query.cpp` builder fully supports it; no further change
needed for the manual path.

**Re-evaluate** when DuckDB exposes a `pushdown_limit` hook on
`TableFunction` (tracked upstream — no fixed milestone). At that
point: wire the hook, set `partitions = 1` whenever a limit is
present (avoid per-partition N×partitions over-fetch), drop this
deferred note.

### 7. LIKE prefix pushdown — **DONE**

`pushdown_complex_filter` hook now lifts `BoundFunctionExpression` of
the `~~` (LIKE) scalar function with a constant RHS, requires at
least one literal character before any `%`/`_`, and emits
`col LIKE 'pattern' ESCAPE '\'` into the Firebird WHERE. Patterns
starting with `%`/`_`, non-LIKE expressions, and any shape we don't
recognise stay in `filters` so DuckDB re-applies them above the scan.

**Observation from live testing on FB5**: DuckDB v1.5.3 already
decomposes the simple `LIKE 'prefix%'` case into `col >= 'prefix'
AND col < 'prefiq'` *before* the hook runs, so the normal TableFilter
pushdown path consumes those. Our hook catches the patterns DuckDB
does **not** decompose — embedded `%`/`_` (e.g. `'S%Textil'`,
`'C_racao%'`) and any future LIKE variant that doesn't fit the
range-rewrite. The two paths cooperate.

Single-quote escaping in the pattern is handled by `SqlLiteral`;
backslash escape semantics by the `ESCAPE '\'` clause.

---

## Milestone v0.5 — Analytics performance

PM review on v0.4.4 set the v0.5 scope: **paging**, **prepared
statements with bind variables**, and **predicate-pushdown gap fill**.
Arrow `RecordBatch` produced directly by the scanner was explicitly
moved to **v1.x** — that is a different product/API, not part of v0.5.
The current Arrow integration (DuckDB converts `Vector` / `DataChunk`
to Arrow at the Flight SQL boundary) is already correct.

Items are listed in delivery order; each has a clear acceptance test.

### 9. `row_offset` + physical paging — **DONE**

Shipped in the v0.5 series and released in v0.5.0+.

What landed:

- New `row_offset=M` named param on `firebird_scan(...)` alongside
  the existing `row_limit=N`. `FirebirdBindData.offset_override`
  carries it through the bind path.
- `FirebirdQueryBuilder::Build` accepts both and emits
  `ROWS M+1 TO M+N` when both are set (Firebird is 1-based,
  inclusive); falls back to the existing `ROWS N` when only
  limit is set.
- Bind-time guards keep the API honest without breaking the v0.4
  `row_limit=N` syntax:
  1. `row_offset` without `row_limit` raises a `BinderException`
     (skip-then-drain is expensive and surprising).
  2. `row_limit` / `row_offset` combined with `partitions > 1`
     (the user explicitly opted into parallel scan *and* paging)
     raises — those two are incompatible. The implicit case
     (no `partitions=` argument, default 0) is silently coerced
     to serial so the v0.4 `firebird_scan(..., row_limit=100)`
     spelling keeps working.
  3. `row_offset + row_limit` overflow guard (sum > idx_t::max
     raises at bind time so the scanner never builds malformed
     SQL).

`row_offset` is per-scan only — deliberately *not* surfaced as an
`ATTACH` option, because paging is a per-query decision rather
than a catalog-wide policy.

Caveat documented in the README and the named-parameter table:
physical paging is unstable under concurrent writes — rows at
positions `[M, M+N)` can shift between calls. Recommend keyset
pagination over a sortable PK for live workloads.

### 10. Prepared statements with bind variables — **second**

Two-part item. Part (a) — input XSQLDA bind variables — is **DONE**
in the v0.5 series. Part (b) — LRU statement
cache per connection — is **deferred** until there's a benchmark
showing the reuse-gain justifies the lifetime-management complexity
under the ATTACH connection pool.

#### Part (a): bind variables — **DONE**

`FirebirdStatement` gained a parametrised constructor that runs
`isc_dsql_describe_bind`, allocates the input XSQLDA + per-column
buffers, encodes each `Value` positionally, and passes the input
sqlda to `isc_dsql_execute`. The builder's `TranslateFilter`
emits `?` placeholders for bindable types and accumulates the
matching values into `Result.params`; the scanner threads them
through `FirebirdConnection::OpenCursor(sql, params)`.

Encoding covers SQL_TEXT / SQL_VARYING (length-prefixed), SHORT /
LONG / INT64, FLOAT / DOUBLE, BOOLEAN, DATE, TIME, TIMESTAMP. NULL
is signalled through the indicator slot.

Strings always parametrise (the security gain is unconditional —
a user-supplied apostrophe can't break the SQL). HUGEINT /
DECIMAL still ship as inline literals via `SafeLiteralInline`
(numeric, no quoting hazard). Unsigned ints (UINTEGER, UBIGINT, …)
and TZ types stay residual — Firebird has no unsigned counterpart
on the bind path, and FB4's TZ-literal syntax varies enough that
inlining would regress.

#### Part (b): LRU statement cache — **deferred**

Skipped in v0.5. Bind variables already deliver the
security/correctness win that motivated the item; the
prepare-per-cursor cost is bounded by the ATTACH connection
pool's reuse, and the LRU would have to reason about cursor
lifetime under that pool to be safe. Revisit when there is a
concrete workload + microbenchmark showing prepare time is
the bottleneck.

Risk to track when we do it: parameter XSQLDA encoding for FB4
types (`INT128`, `TIMESTAMP_TZ`, `DECFLOAT`) differs from the
output side. Each needs a live test on FB4 + FB5 before being
claimed.

**Acceptance** (when we do it): a benchmark issuing 10 000
sequential point lookups `fb.main.CUSTOMER WHERE CUST_ID = $1`
against a remote Firebird returns end-to-end at ≤ 1.2× the cost
of the equivalent direct-isql parameterised loop.

### 11. Predicate pushdown — fill the gaps — **DONE (NOT IN + NOT bool)**

Shipped in v0.5. The `pushdown_complex_filter` hook now lifts
`BoundOperatorExpression(COMPARE_NOT_IN)` and
`BoundOperatorExpression(OPERATOR_NOT)` of a boolean column ref
in addition to the existing `LIKE 'prefix%'` translation.
BETWEEN doesn't need a hook — DuckDB already decomposes it into
`col >= a AND col <= b` and the `TableFilterSet` path handles
both sides. Safe `CAST(...)` rewrites are intentionally still
residual; DuckDB's expression simplifier folds the no-op cases
and the rest don't move the needle on the workloads we care
about today (revisit when there is one that does).

All new pushdowns honour the `IsNoneTextColumnGated` invariant —
NONE-text columns under non-strict transcoding stay residual,
matching the v0.4.2 contract.

**Original v0.5 list still open** (revisit only if measured):



Plug the holes that `pushdown_complex_filter` still leaves on the
DuckDB-side `FILTER` node. Each addition is conditional on the
NONE-text gate already in place (`IsNoneTextColumnGated`): a
predicate that touches a transcoded NONE column never pushes.

- **`NOT IN (...)`**: structurally identical to `IN`; emit
  `col NOT IN (...)` and reuse the same literal-formatting path.
- **Boolean `NOT expr`**: detect `BoundOperatorExpression(NOT)`
  with a translatable child; emit `NOT (...)`.
- **`COL BETWEEN a AND b`** when DuckDB hasn't decomposed it:
  detect the function shape in `pushdown_complex_filter` and emit
  `col BETWEEN a AND b` directly.
- **`CAST(col AS X) op literal`** for safe `X` (INTEGER, BIGINT,
  DECIMAL, DATE) — only when Firebird's `CAST(...)` shares
  semantics with DuckDB's.
- **`LIKE 'suffix'` / `LIKE '%mid%'`** — measured benchmark first.
  On indexed columns these will already use a seq scan on
  Firebird's side, so the pushdown may not move the needle;
  document the finding and only ship if the benchmark is
  clearly positive.

Invariant: a predicate only goes to Firebird if we can prove it
is bit-for-bit equivalent to DuckDB's evaluation. Anything else
stays residual.

**Acceptance**: `EXPLAIN` shows the new predicates on the
FIREBIRD_SCAN's `Filters:` line; existing 245 sqllogictest
assertions stay green and new ones (one per added predicate)
cover the happy path + the NONE-text-gate residual path.

### 12. Automatic `LIMIT` pushdown — contingent on DuckDB API

Hold this until the upstream DuckDB API exposes a real
`pushdown_limit` hook on `TableFunction` or a stable
`OptimizerExtension` entry point — neither is in v1.5.3. The
existing `row_limit=N` named param remains the documented path
in the meantime; the README already labels it as the supported
v0.4.x mechanism.

If we ship a custom `OptimizerExtension` rewrite (the
duckdb-postgres approach) without an upstream commitment, we
inherit the maintenance burden of tracking DuckDB's planner
changes release by release. The cost-benefit only favours that
when there is no near-term upstream alternative; revisit each
DuckDB minor.

**Acceptance** (when we do it): `EXPLAIN SELECT * FROM
fb.main.BIG_T LIMIT 100` shows `ROWS 100` on the FIREBIRD_SCAN
node; same query without an explicit `row_limit=` runs in ms not
seconds on a 1 M-row remote table.

### 13. Discovery / metadata coverage tests — **DONE**

Shipped in v0.5. `test/sql/firebird_metadata.test` (79 assertions)
covers `SHOW TABLES FROM fb`, `DESCRIBE fb.main.TAB`,
`information_schema.tables`, `information_schema.columns`,
case-insensitive table lookup (`fb.main.employee` ==
`fb.main."EMPLOYEE"`), schema-not-found errors, and the read-only
enforce on `INSERT` / `DROP TABLE` / `CREATE TABLE` against the
ATTACH catalog. Documented gaps the suite intentionally pins
the current behaviour for:

- `is_nullable` now reflects `RDB$NULL_FLAG` for table columns.
  Domain / per-column precedence handled by
  `COALESCE(rf.RDB$NULL_FLAG, f.RDB$NULL_FLAG, 0)` in
  `LoadTableSchema`; declared `NOT NULL` columns surface as `NO`
  and get a `NotNullConstraint` in the ATTACH `CreateTableInfo`.
- Views still appear as `table_type='BASE TABLE'` in
  `information_schema.tables`. The catalog layer doesn't yet
  differentiate `RDB$RELATION_TYPE = 1` from regular tables.
  Deferred past v0.5 (needs `ViewCatalogEntry` path).

### 13b. (Historical placeholder — see above)

`firebird_tables(...)` + the `ATTACH (TYPE firebird)` catalog
already populate the entries DuckDB clients query. Lock that in
with explicit sqllogictest coverage so a future refactor cannot
break BI / dbt-style introspection:

- `SHOW TABLES FROM fb;`
- `DESCRIBE fb.main.TAB1;`
- `SELECT * FROM information_schema.tables WHERE table_catalog = 'fb';`
- `SELECT * FROM information_schema.columns WHERE table_catalog = 'fb' ORDER BY ordinal_position;`

**Acceptance**: a new `test/sql/firebird_metadata.test` exercises
each form against the EMPLOYEE + FB4_TYPES fixtures and matches
the expected name/type rows verbatim.

---

## Platform — separation of concerns

The duckdb-firebird extension is one component of a larger
analytics stack. v0.5 docs make the boundary explicit so reviewers
and integrators stop conflating responsibilities:

| Layer            | Responsibility                                 | Owned by                     |
|---|---|---|
| Source           | OLTP data, RDB$ catalog                        | Firebird server (3 / 4 / 5)  |
| Bridge           | Firebird ⇄ DuckDB types, pushdown, paging       | **duckdb-firebird** (this repo) |
| Engine           | Vectorised execute, optimiser, Parquet, S3 IO  | DuckDB v1.5.3                |
| Server (remote)  | Arrow Flight SQL over the wire                 | GizmoSQL (DuckDB embedded)   |
| Object store     | Bronze / silver / gold artefacts               | MinIO / S3 / R2 / GCS        |
| Transformation   | Medallion modelling, contract testing          | dbt-duckdb                   |
| Consumption      | Dashboards / ad-hoc                            | Power BI (Parquet import primary; ADBC Flight SQL for tooling that supports it) |

The MVP flow against a legacy ERP database:

```sql
LOAD firebird;
INSTALL httpfs; LOAD httpfs;

ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'SYSDBA', password 'masterkey');

CREATE SCHEMA IF NOT EXISTS bronze;
CREATE SCHEMA IF NOT EXISTS silver;
CREATE SCHEMA IF NOT EXISTS gold;

CREATE OR REPLACE VIEW bronze.vendas AS
    SELECT * FROM fb.main.TABENTRADASAIDA;

CREATE OR REPLACE TABLE silver.vendas AS
    SELECT *
      FROM bronze.vendas
     WHERE DATAMOVIMENTO >= DATE '2024-01-01';

-- MinIO / S3-compatible: use CREATE SECRET, NOT the legacy
-- SET s3_* keys (those still work but DuckDB now recommends
-- the secret-based path).
CREATE SECRET IF NOT EXISTS s3_minio (
    TYPE s3,
    KEY_ID    'minioadmin',
    SECRET    'minioadmin',
    ENDPOINT  'minio.local:9000',
    URL_STYLE 'path',
    USE_SSL   false);

COPY (
    SELECT *,
           year(DATAMOVIMENTO)  AS ano,
           month(DATAMOVIMENTO) AS mes
      FROM silver.vendas
) TO 's3://lake/erp/vendas'
  (FORMAT parquet, PARTITION_BY (ano, mes));
```

### Power BI integration — what to promise

For an MVP commercial release:

1. **Primary**: Power BI imports the Parquet produced by the
   DuckDB pipeline above. Stable, supported, no driver questions.
2. **Secondary**: ODBC / JDBC gateway against DuckDB. Validate in
   the target environment; behaviour varies by Power BI release.
3. **Advanced**: Arrow Flight SQL via GizmoSQL — useful for
   Python / JDBC / ADBC tooling and for Power BI integrations
   that ship ADBC support; **do not** sell this as
   first-class universal Power BI today. Microsoft's ADBC story
   is still ramping (currently anchored on specific connectors
   like Databricks); avoid overclaim.

---

## Release-testing checklist (run before every push to main)

Live, against an anonymized legacy ERP fixture and the EMPLOYEE / FB4_TYPES
fixtures, on both FB4 and FB5 (swap `fbclient.dll` in
`build\release\` between runs):

```sql
LOAD firebird;

ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'SYSDBA', password 'masterkey');

-- Smoke: connectivity + row counts.
SELECT COUNT(*) FROM fb.main.TABPESSOAS;
SELECT COUNT(*) FROM fb.main.TABENTRADASAIDA;

-- NONE-charset round-trip (default win1252).
SELECT * FROM fb.main.TABPESSOAS WHERE BAIRRO IS NOT NULL LIMIT 20;

-- Paging (once #9 lands).
SELECT * FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS',
                            row_limit=100, row_offset=900);

-- Materialisation.
CREATE OR REPLACE TABLE silver.tabpessoas AS
SELECT * FROM fb.main.TABPESSOAS;

-- Parquet export.
COPY silver.tabpessoas
  TO 'C:/tmp/lake/tabpessoas.parquet'
  (FORMAT parquet, COMPRESSION zstd);
```

Then re-run the full sqllogictest suite against both server majors:

```cmd
set FIREBIRD_TEST_DB=C:/fbtest/test.fdb
set FIREBIRD_NONE_DB=C:/fbtest/none.fdb
set ISC_USER=SYSDBA & set ISC_PASSWORD=masterkey

build\release\test\unittest.exe "%CD%/test/sql/firebird_scan.test"
build\release\test\unittest.exe "%CD%/test/sql/firebird_attach.test"
build\release\test\unittest.exe "%CD%/test/sql/firebird_none_charset.test"
build\release\test\unittest.exe "%CD%/test/sql/firebird_metadata.test"   # once #13 lands
```

Expected: `All tests passed (124 + 79 + 42 + N assertions)` with no
regressions vs. the previous tag.

---

## Milestone v1.x — Arrow-native scanner (deferred)

A `firebird_arrow_scan` function that emits `arrow::RecordBatch`
directly from the XSQLDA fetch loop — bypassing the
`Vector` / `DataChunk` step DuckDB normally drives. Only worth
building when there's a concrete consumer that benefits *measurably*
over the current DuckDB→Arrow conversion path. v0.5 deliberately
stays out of this and keeps the README's "Arrow note" honest.

---

## Milestone v1.0 — Distribution

### 8. Community-extensions submission

`community-extensions/description.yml` already exists. Process:

1. Tag the release on this repo (`v0.5.1` is the current public tag).
2. Fork `duckdb/community-extensions`.
3. Copy `description.yml` to `extensions/firebird/description.yml`,
   set `repo.ref` to the current public tag (`v0.5.1`).
4. Open PR; their CI builds Linux/Windows/macOS binaries from the tag.

**Acceptance**: `INSTALL firebird FROM community; LOAD firebird;`
works on a vanilla DuckDB CLI on three platforms.

### 9. (Deferred — depends on upstream) Stable C ABI

The DuckDB Stable C ABI (`duckdb_extension.h`) currently does **not**
support `StorageExtension`. Migrating today would mean dropping
`ATTACH 'firebird://…' AS fb (TYPE firebird)`. Watch
[`duckdb/duckdb#…`](https://github.com/duckdb/duckdb/issues) for the
catalog C API; when it lands, port `firebird_storage.cpp` to it. Until
then, ship per-DuckDB-minor-version binaries via community-extensions.

---

## Out of scope (rejected for now)

- **Writes** — `INSERT / UPDATE / DELETE / CREATE TABLE` against
  `fb.main.…`. The use case is "lift Firebird OLTP data into DuckDB
  for analytics", not "use Firebird through DuckDB". Adding writes
  changes the transaction profile (`isc_tpb_read` is hard-wired) and
  the risk envelope (a bug deletes user data).
- **DDL emission** — `firebird_attach_sql()` already covers the view-
  based path; replicating Firebird's DDL inside DuckDB is a much bigger
  surface than the wins justify.
- **Replacement scan** (so `SELECT * FROM 'file.fdb'.EMPLOYEE` Just
  Works) — neat trick but `ATTACH` is the right surface for a
  multi-table source.
