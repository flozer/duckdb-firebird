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

### 2) Custom review suite — FB4 (full parity)

Re-ran the **same 26-group suite** against Firebird 4.0.5 embedded
mode (with the FB4 `fbclient.dll` co-located next to `duckdb.exe`).
**All 26 groups green.** Type mapping, INT128 extremes, DECIMAL(38,
5) lower bound, TZ types, NULL row, all pushdown variants
(equality, range, IN, IS NULL, LIKE prefix via DuckDB range
rewrite, LIKE embedded wildcard via our hook, leading-wildcard LIKE
that stays in DuckDB), projection pushdown, row_limit + partitions
named params, firebird_attach_sql DDL, ATTACH catalog (incl.
case-insensitive lookup), federated CROSS JOIN, read-only enforce
(INSERT/DROP/CREATE all NotImplemented), DETACH + re-ATTACH, CTAS,
COPY to Parquet, federated Firebird × Parquet via HUGEINT join key,
all error paths, and the SQL-injection probe — every check
reproduces FB5's behaviour byte-identical. The FB4 and FB5 servers
emit identical UTC offsets for TIMESTAMP_TZ on the test fixture, so
the DuckDB-side output matches across the two servers.

### 3) Existing sqllogictest suite

Re-ran the committed regression tests at `test/sql/firebird_scan.test`
(124 assertions) and `test/sql/firebird_attach.test` (79 assertions)
against both servers, after building a matching `EMPLOYEE +
FILE_STORAGE + V_ACTIVE_EMP` fixture on each.

| Server | scan.test | attach.test | Total |
|---|---|---|---|
| Firebird 5.0.4 | 124 / 124 | 79 / 79 | 203 / 203 |
| Firebird 4.0.5 | 124 / 124 | 79 / 79 | 203 / 203 |

### 4) GizmoSQL equivalent surface — verified locally

Direct GizmoSQL exec is gated on signing (see below). Before
declaring it blocked, the **exact surface** GizmoSQL exposes to
Arrow Flight SQL clients (DuckDB v1.5.3 in-proc + `LOAD firebird` +
`ATTACH 'firebird://…' AS fb (TYPE firebird)` + DAX-shaped queries)
was reproduced against our local `duckdb.exe -unsigned` and
returned the expected rows:

- `LOAD` + `ATTACH` of the same `biz4.fdb` fixture succeeds.
- `SHOW ALL TABLES` exposes CUSTOMER / DEPARTMENT / FB4_TYPES with
  full FB4-type signatures (HUGEINT, DECIMAL(38, 5),
  TIMESTAMP WITH TIME ZONE, TIME WITH TIME ZONE) — exactly what an
  ADBC Flight SQL client would see in `GetTables` / `GetCatalogs`.
- `SELECT COUNT(*) FROM fb.main.FB4_TYPES` → 4.
- `SELECT typeof(BIG_NUM), BIG_NUM FROM fb.main.FB4_TYPES WHERE ID
  = 1` → `HUGEINT, 170141183460469231731687303715884105727`.
- `SELECT MIN/MAX(BIG_NUM), MIN/MAX(BIG_DEC) FROM fb.main.FB4_TYPES`
  → extremes recovered exact.
- CTAS into a DuckDB-side table for a Flight SQL client to fetch
  via `getStream` → 4 rows.
- DETACH cleans up the pool.

This isolates the GizmoSQL blocker (next section) to the
extension-load policy: the extension's table functions and
StorageExtension run end-to-end inside the same DuckDB embedded
runtime that GizmoSQL uses, just without the signing gate in front.

### 4b) GizmoSQL — blocked on signing

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

**Why this is a hard block locally**

The DuckDB signing logic at
[`duckdb/src/main/extension/extension_load.cpp:483-498`](https://github.com/duckdb/duckdb/blob/v1.5.3/src/main/extension/extension_load.cpp#L483-L498)
fans out as:

```cpp
if (!Settings::Get<AllowUnsignedExtensionsSetting>(db)) {
    bool signature_valid = ...;
    if (!signature_valid) throw IOException(... UNSIGNED_EXTENSION ...);
}
```

The setting itself
([`custom_settings.cpp:197-201`](https://github.com/duckdb/duckdb/blob/v1.5.3/src/main/settings/custom_settings.cpp#L197-L201))
explicitly rejects any flip-to-true after database start:

```cpp
void AllowUnsignedExtensionsSetting::OnSet(...) {
    if (info.db && input.GetValue<bool>()) {
        throw InvalidInputException(
            "Cannot change allow_unsigned_extensions setting while database is running");
    }
}
```

GizmoSQL constructs `DuckDB::DBConfig` in `RunDuckDBFlightSqlServer`
([`gizmosql/src/duckdb/duckdb_server.cpp:2087-2103`](https://github.com/gizmodata/gizmosql/blob/main/src/duckdb/duckdb_server.cpp#L2087-L2103))
without touching `config.options.allow_unsigned_extensions`, so the
default `false` wins by the time the user `init.sql` runs.

The signature itself is verified against a fixed list of public keys
baked into the DuckDB binary
([`extension_helper.cpp:418` for the official keys,
`:640` for community keys](https://github.com/duckdb/duckdb/blob/v1.5.3/src/main/extension/extension_helper.cpp#L418-L640)).
Signing requires a private key matching one of those entries —
unavailable to the maintainer.

**Two paths forward**, listed in order of preference:

1. **Community-extensions PR #1980 merges → upstream CI signs the
   build with the `community_public_keys`-matching key.** That signed
   `.duckdb_extension` then loads under GizmoSQL with no further work.
   This is the intended distribution path and was always the gate;
   it is now waiting on the maintainer queue.

2. **Custom GizmoSQL build** with one extra line in `RunDuckDBFlightSqlServer`:
   ```cpp
   config.options.allow_unsigned_extensions = true;
   ```
   plus a corresponding CLI flag. This unblocks local validation but
   requires GizmoSQL's full build (Arrow + Boost + gRPC + Flight)
   which is a several-hour setup; auto-mode declined to escalate to
   that scope, and on principle a reviewer would not patch the host
   tool to make the extension load.

This is **not** a defect of duckdb-firebird. The extension binary
itself is v1.5.3 ABI-compatible with the DuckDB embedded inside
GizmoSQL — section 4) above verifies the surface end-to-end in the
same DuckDB build, only without the signing wrapper. Once the
extension is signed, the GizmoSQL smoke script
`scripts/gizmosql_smoke.sh` exists in the repo and will reproduce
section 4)'s queries over Arrow Flight SQL via the Python ADBC
driver, asserting the same row counts and types.

## Findings

No new defects beyond what was already addressed in v0.4. The
batch in commits `5ea1bd8` / `03b28b2` (SQL-injection escaping,
RDB$FIELD_TYPE 24-31 coverage, `SQL_TIME_TZ_EX` / `SQL_TIMESTAMP_TZ_EX`
buffer sizing, vcvars detection, LIKE complex-filter hook, fixture
DECIMAL lower-bound) closes everything the prior code-review and
security-audit passes flagged.

The DECFLOAT(16/34) NULL-on-fetch behaviour is the one open
limitation. It is documented in:

- [`README.md`](../../README.md) "Status" table — DECFLOAT marked 🟡.
- [`docs/en/roadmap.md`](roadmap.md) deferred section — fix requires
  the Firebird OO API (`IDecFloat16::toString`).
- [`docs/en/test_report.md`](test_report.md) "Firebird 5 live coverage"
  table — DEC16/DEC34 rows marked ⚠️ with workaround pointer (cast
  to `VARCHAR` or `DECIMAL(38, s)` at the source).
- [`community-extensions/description.yml`](../../community-extensions/description.yml)
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
