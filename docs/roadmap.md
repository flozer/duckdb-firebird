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

## Milestone v1.0 — Distribution

### 8. Community-extensions submission

`community-extensions/description.yml` already exists. Process:

1. Tag `v0.5.0` on this repo.
2. Fork `duckdb/community-extensions`.
3. Copy `description.yml` to `extensions/firebird/description.yml`,
   set `repo.ref: v0.5.0`.
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
