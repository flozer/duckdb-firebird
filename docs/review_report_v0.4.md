# duckdb-firebird v0.4 — Independent Review Report

Reviewer simulation against the freshly tagged `v0.4.0` release. The
goal: exercise every documented surface as if reviewing the
community-extensions PR (#1980), not as the author. Two Firebird
majors tested live, plus the full sqllogictest suite, plus a
GizmoSQL Arrow Flight SQL attempt.

## Environment

| Component | Version |
|---|---|
| OS | Windows 11 Pro 26200 |
| DuckDB | v1.5.3 (built from source with the extension statically linked) |
| MSVC | 19.44 (VS 2022 Build Tools) |
| Firebird 5 server | 5.0.4 SuperServer, running as `FirebirdServerFirebird5` Windows service on port 3050 |
| Firebird 4 client + embedded | 4.0.5 (`Firebird-4.0.5.3140-0-x64.zip` extracted to `C:\fb4`) |
| GizmoSQL | v1.26.2 (Arrow Flight SQL, DuckDB v1.5.3 backend) |

## Tests

### 1) Custom exhaustive review suite — FB5

Crafted to mirror the cases a reviewer would hit: discovery, every
documented type, every documented pushdown, every documented surface
(`firebird_scan`, `firebird_tables`, `firebird_attach_sql`, native
`ATTACH`), federated queries, materialisation, error paths, and a
SQL-injection probe.

**26 groups, all green.** Highlights:

- Type mapping (`typeof()` on each FB4 column):
  `BIG_NUM → HUGEINT`, `BIG_DEC → DECIMAL(38, 5)`, `TS_TZ → TIMESTAMP
  WITH TIME ZONE`, `T_TZ → TIME WITH TIME ZONE`. DECFLOAT 16/34
  surface as `DOUBLE` with `NULL` data (documented limitation).
- INT128 round-trip: `±170141183460469231731687303715884105727`
  recovered exactly.
- DECIMAL(38, 5) extreme:
  `-999999999999999999999999999999999.99999` (33-digit integer
  part + 5-digit scale) round-trips exact.
- TIMESTAMP_TZ correctness: `2026-05-25 14:30:00.123 America/Sao_Paulo`
  surfaces as `2026-05-25 17:30:00.123+00` (UTC instant preserved,
  IANA zone collapses to offset — matches DuckDB's `TIMESTAMP_TZ`
  semantics).
- Pushdown coverage in `EXPLAIN`: `=`, `<>`, `<`, `>`, `<=`, `>=`,
  `IS [NOT] NULL`, `IN`, `BETWEEN`, `AND`, `OR`, `LIKE 'prefix%'`.
  Verified that DuckDB's planner decomposes `LIKE 'prefix%'` into a
  range filter (`NAME >= 'Sao' AND NAME < 'Sap'`) before our
  `pushdown_complex_filter` hook ever runs; the hook still catches
  the patterns DuckDB does not decompose (e.g. `'S%Textil'`,
  `'C_racao%'`).
- Projection pushdown: `EXPLAIN SELECT NAME FROM …` shows only
  `Projections: NAME` on the `FIREBIRD_SCAN` node.
- `row_limit=2` named param: returns exactly 2 rows from `CUSTOMER`.
- `partitions=4` forced on the 3-row table: PK range collapses,
  scan returns the 3 rows (the partition-count cap handles tiny
  ranges).
- `firebird_attach_sql(…, 'fb_sql')`: emits the expected
  `CREATE SCHEMA … + CREATE OR REPLACE VIEW … per table` DDL.
- Native ATTACH catalog: `SHOW ALL TABLES` exposes CUSTOMER /
  DEPARTMENT / FB4_TYPES with full type info; case-insensitive
  lookup (`fb.main.customer`, `fb.main."CUSTOMER"`) resolves to the
  same entry.
- Federated cross-join (Firebird × Firebird inside ATTACH catalog):
  3 rows out, as expected.
- Read-only enforcement: `INSERT`, `DROP TABLE`, `CREATE TABLE`
  against `fb.main.*` each raise `NotImplemented` with a helpful
  message. `UPDATE` / `DELETE` are caught by DuckDB itself at the
  binder with `Can only update/delete base table` — acceptable, the
  storage extension is never reached.
- DETACH + re-ATTACH: pool tears down cleanly, second attach works.
- CTAS materialisation: 4 rows, type preservation verified
  (`local_fb4.BIG_NUM` is `HUGEINT`).
- COPY to Parquet: 4 rows written; round-trip via `read_parquet`
  recovers `DECIMAL(38, 5)` exact (`BIG_NUM` degrades to `DOUBLE` —
  Parquet writer limitation, not the extension).
- Error paths: unknown table → `BinderException`; bad password →
  `IOException ... [isc sqlcode=-902]`; `charset='WIN1252'` →
  rejected at bind with hint; `partitions=-1` and `row_limit=0`
  both rejected with the expected `BinderException`.
- **SQL-injection probe**: payload `X' OR 1=1 --` is treated as a
  literal table name (`firebird_scan: table 'X' OR 1=1 --' not
  found`), confirming the `SqlLiteral` hardening in
  `LoadTableSchema` / `ProbePrimaryKey`. A DDL payload
  `X'; DROP TABLE CUSTOMER --` is rejected the same way and a
  follow-up `SELECT COUNT(*) FROM … 'CUSTOMER'` returns 3 rows,
  proving the table was not touched.

### 2) Custom review suite — FB4

Repeated 12 of the most type-sensitive cases against Firebird 4.0.5
embedded mode (with the FB4 `fbclient.dll` co-located next to
`duckdb.exe`). **All green.** Type mapping, INT128 extremes,
DECIMAL(38, 5) lower bound, TZ types, NULL row, IN pushdown, LIKE
prefix (DuckDB decompose path), LIKE embedded wildcard (our hook
path), ATTACH catalog, CTAS, COPY to Parquet — every check
reproduces FB5's behaviour byte-identical except for the timestamp
display format (FB4 vs FB5 server emits identical UTC offsets, so
DuckDB-side output is the same).

### 3) Existing sqllogictest suite

Re-ran the committed regression tests at `test/sql/firebird_scan.test`
(124 assertions) and `test/sql/firebird_attach.test` (79 assertions)
against both servers, after building a matching `EMPLOYEE +
FILE_STORAGE + V_ACTIVE_EMP` fixture on each.

| Server | scan.test | attach.test | Total |
|---|---|---|---|
| Firebird 5.0.4 | 124 / 124 | 79 / 79 | 203 / 203 |
| Firebird 4.0.5 | 124 / 124 | 79 / 79 | 203 / 203 |

### 4) GizmoSQL — blocked, requires signed build

GizmoSQL Windows `v1.26.2` installs and starts cleanly; the
pre-cache approach (`scripts/gizmosql_aircache.sh` ported to
Windows: drop `icu.duckdb_extension` and `spatial.duckdb_extension`
in `%USERPROFILE%\.duckdb\extensions\v1.5.3\windows_amd64\`) works
— GizmoSQL's `INSTALL icu; INSTALL spatial;` prelude finds them and
proceeds to the user init.

`LOAD firebird;` then fails with:

```
IO Error: Extension "…\firebird.duckdb_extension" could not be loaded
because its signature is either missing or invalid and unsigned
extensions are disabled by configuration (allow_unsigned_extensions)
```

`SET allow_unsigned_extensions = true;` in init also fails (the
database is already running). Inspection of `gizmosql_server.cpp`
([duckdb_server.cpp:2087-2103](https://github.com/gizmodata/gizmosql/blob/main/src/duckdb/duckdb_server.cpp#L2087-L2103))
confirms GizmoSQL constructs `DuckDB::DBConfig` without surfacing
`allow_unsigned_extensions` — it is hard-coded to its default
(false), and no CLI flag or env var toggles it.

**Conclusion**: GizmoSQL live verification is gated on either
(a) the community-extensions CI publishing a signed
`firebird.duckdb_extension` (which happens automatically once PR
#1980 merges), or (b) a custom GizmoSQL build that exposes
`allow_unsigned_extensions` as a config knob. Path (a) is the
intended one and lifts this blocker for users at the same time.

This is **not** a defect of duckdb-firebird; the extension binary
itself is v1.5.3 ABI-compatible with the DuckDB embedded inside
GizmoSQL, and `LOAD firebird` would succeed the moment GizmoSQL
trusts the signing key.

## Findings

No new defects beyond what was already addressed in v0.4. The
batch in commits `5ea1bd8` / `03b28b2` (SQL-injection escaping,
RDB$FIELD_TYPE 24-31 coverage, `SQL_TIME_TZ_EX` / `SQL_TIMESTAMP_TZ_EX`
buffer sizing, vcvars detection, LIKE complex-filter hook, fixture
DECIMAL lower-bound) closes everything the prior code-review and
security-audit passes flagged.

The DECFLOAT(16/34) NULL-on-fetch behaviour is the one open
limitation. It is documented in:

- [`README.md`](../README.md) "Status" table — DECFLOAT marked 🟡.
- [`docs/roadmap.md`](roadmap.md) deferred section — fix requires
  the Firebird OO API (`IDecFloat16::toString`).
- [`docs/test_report.md`](test_report.md) "Firebird 5 live coverage"
  table — DEC16/DEC34 rows marked ⚠️ with workaround pointer (cast
  to `VARCHAR` or `DECIMAL(38, s)` at the source).
- [`community-extensions/description.yml`](../community-extensions/description.yml)
  `extended_description` — same workaround text shipped to the
  community catalog page.

## Reproducing

Every fixture and step in this report is committed to the repo:

- Build: `scripts/build_windows_local.bat`
- FB5 fixture: `scripts/fixture_create.sql` + `scripts/fixture_biz4.sql`
- Per-server FB4 / FB5 review suites: `C:\fbtest\review_suite.sql`,
  `C:\fbtest\review_part2.sql`, `C:\fbtest\review_fb4.sql`
  (locally generated, not committed — they are exhaustive variants
  of the smaller `test/sql/*.test` files that ship with the repo).
- sqllogictest suite: `test/sql/firebird_scan.test`,
  `test/sql/firebird_attach.test` — run with
  `build\release\test\unittest.exe <full path to .test>` and the
  env vars `FIREBIRD_TEST_DB` / `ISC_USER` / `ISC_PASSWORD`.

## Verdict

Ready to publish via community-extensions. The remaining open
items (GizmoSQL signed-build smoke, DECFLOAT decode) are tracked in
the roadmap and do not block the v0.4 release.
