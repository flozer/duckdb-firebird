# firebird_health — Design (v0.8 Diagnostics Bridge, item 1)

Date: 2026-06-23
Status: approved
Branch: `feat/v0.8-firebird-health`

## Purpose

Single-row, read-only database/server health diagnostic for an attached
Firebird catalog. Surfaces the factual operational metrics a DBA checks first
(engine/ODS version, dialect, charset, page size, durability, sweep, the
transaction-gap counters, attachment count) plus a structured `warnings` list
with **explicit, documented trigger criteria**. It is a factual diagnostic, not
an opaque advisor: every warning maps to a stated, auditable condition, and the
raw counters that drive each warning are exposed as columns so the user can
verify the signal themselves.

Analogous to `firebird_profile_table` (single-row + `warnings`), but at the
database/server level rather than per-table.

## Surface

```
firebird_health('<attach_alias>') -> 1 row
```

- Table function, exactly one argument: the ATTACH alias (validated via
  `ValidateFirebirdAttachAlias`).
- Read-only. Issues only `SELECT` against monitoring/system tables. Never runs
  user SQL, never touches business data, never emits row values.
- Returns exactly one row.

## Output columns (15)

| # | column | type | source |
|---|---|---|---|
| 1 | `engine_version` | VARCHAR | `rdb$get_context('SYSTEM','ENGINE_VERSION')` |
| 2 | `ods_version` | VARCHAR | `MON$DATABASE.MON$ODS_MAJOR \|\| '.' \|\| MON$ODS_MINOR` |
| 3 | `sql_dialect` | INTEGER | `MON$DATABASE.MON$SQL_DIALECT` |
| 4 | `default_charset` | VARCHAR | `RDB$DATABASE.RDB$CHARACTER_SET_NAME` (TRIM) |
| 5 | `page_size` | INTEGER | `MON$DATABASE.MON$PAGE_SIZE` |
| 6 | `forced_writes` | BOOLEAN | `MON$DATABASE.MON$FORCED_WRITES <> 0` |
| 7 | `sweep_interval` | INTEGER | `MON$DATABASE.MON$SWEEP_INTERVAL` |
| 8 | `oldest_transaction` | BIGINT | `MON$DATABASE.MON$OLDEST_TRANSACTION` (OIT) |
| 9 | `oldest_active` | BIGINT | `MON$DATABASE.MON$OLDEST_ACTIVE` (OAT) |
| 10 | `oldest_snapshot` | BIGINT | `MON$DATABASE.MON$OLDEST_SNAPSHOT` (OST) |
| 11 | `next_transaction` | BIGINT | `MON$DATABASE.MON$NEXT_TRANSACTION` |
| 12 | `oit_gap` | BIGINT | `next_transaction - oldest_transaction` (derived) |
| 13 | `oat_gap` | BIGINT | `next_transaction - oldest_active` (derived) |
| 14 | `attachments` | INTEGER | `COUNT(*) FROM MON$ATTACHMENTS` |
| 15 | `warnings` | LIST(VARCHAR) | rules below |

Column-type accessor discipline (per the SMALLINT/INTEGER/BIGINT rule): the
`MON$ODS_*`, `MON$SQL_DIALECT`, `MON$FORCED_WRITES` fields are SMALLINT
(read with `GetShort`/`ShortOrNull`); `MON$PAGE_SIZE`, `MON$SWEEP_INTERVAL`,
`COUNT(*)` are INTEGER (`GetLong`/`IntOrNull`); the four transaction counters
are BIGINT (`GetInt64`). `ENGINE_VERSION` and `RDB$CHARACTER_SET_NAME` are text.
Derived gaps are computed in C++ from the BIGINT values (not in SQL), so they
stay correct and NULL-aware.

## Warnings — explicit, documented criteria

Each warning is a fixed string emitted when its stated condition holds. The
`1_000_000` gap threshold is a **conservative default signal, not a universal
limit** — documented in the manual as such. The driving counters are columns
(`oit_gap`, `oat_gap`, `sweep_interval`, `forced_writes`, `default_charset`),
so every warning is independently auditable.

| warning string | trigger | meaning |
|---|---|---|
| `oit_gap_high` | `oit_gap > 1_000_000` | OIT far behind next tx → GC/sweep pressure |
| `oat_gap_high` | `oat_gap > 1_000_000` | long-running active transaction |
| `sweep_disabled` | `sweep_interval = 0` | automatic sweep off |
| `forced_writes_off` | `forced_writes = false` | durability risk on crash |
| `charset_none` | `default_charset = 'NONE'` | transcoding ambiguity (ties to none_encoding) |
| `mon_unavailable` | MON$ query failed | see graceful-degradation below |

Warning order is deterministic (the table order above), so tests can assert on
the list content.

## Mechanism

1. `ValidateFirebirdAttachAlias(context, alias)`; acquire catalog lease
   (`AcquireFirebirdCatalogLease`).
2. Run **one** monitoring query against the lease connection:

   ```sql
   SELECT m.MON$ODS_MAJOR, m.MON$ODS_MINOR, m.MON$SQL_DIALECT,
          m.MON$PAGE_SIZE, m.MON$FORCED_WRITES, m.MON$SWEEP_INTERVAL,
          m.MON$OLDEST_TRANSACTION, m.MON$OLDEST_ACTIVE,
          m.MON$OLDEST_SNAPSHOT, m.MON$NEXT_TRANSACTION,
          (SELECT COUNT(*) FROM MON$ATTACHMENTS)
     FROM MON$DATABASE m
   ```
3. Run a second always-readable query for the universally-permitted fields
   (any user can read these even without MON$ privilege):

   ```sql
   SELECT rdb$get_context('SYSTEM','ENGINE_VERSION'),
          RDB$CHARACTER_SET_NAME
     FROM RDB$DATABASE
   ```
4. Compute `oit_gap`, `oat_gap` in C++; evaluate warning rules; build the row.

Single-row bespoke table function (like `firebird_profile_table`), not the
multi-row metadata scaffold.

## Graceful degradation (MON$ unavailable)

`MON$DATABASE`/`MON$ATTACHMENTS` require monitoring privilege (SYSDBA / DB owner
/ user granted `RDB$ADMIN`). A least-privilege user may be denied. If the
monitoring query (step 2) throws, catch it: leave all MON$-sourced columns
(`ods_version`, `sql_dialect`, `page_size`, `forced_writes`, `sweep_interval`,
the four counters, both gaps, `attachments`) NULL, still populate
`engine_version` + `default_charset` from step 3, and emit the `mon_unavailable`
warning. The function never errors out on privilege. (Same isolation philosophy
as `firebird_generators` current-value reads.) Gap/sweep/forced-writes warnings
are skipped when their inputs are NULL; `charset_none` still evaluates.

## Testing

`test/sql/firebird_health.test` (fixture `FIREBIRD_TEST_DB`, FB5 local;
version-agnostic — MON$ is FB2.1+, runs on all matrix versions):

- `firebird_health('fb')` returns exactly 1 row.
- Type/range assertions (not exact tx numbers, which vary per session):
  `sql_dialect = 3`, `page_size > 0`, `default_charset = 'UTF8'`,
  `engine_version <> ''`, `ods_version` matches `^\d+\.\d+$`,
  `next_transaction > 0`, `oit_gap >= 0`, `oat_gap >= 0`, `attachments >= 1`.
- `warnings` is a LIST (typeof / len assertable).
- Deterministic warning: against `none.fdb` (default CHARACTER SET NONE,
  `FIREBIRD_NONE_DB`), `firebird_health('fbn')` → `charset_none` present in
  `warnings`. Against `FIREBIRD_TEST_DB` (UTF8), `charset_none` absent.

Matrix: add `firebird_health.test` → `FIREBIRD_TEST_DB` (and a `none.fdb`
variant assertion) to `scripts/build_matrix.ps1` `$testFixtureVar` and to the
Linux FB matrix workflow.

## Files

- `src/firebird_health.cpp` (new) — bespoke single-row table function.
- `src/include/firebird_health.hpp` (new) — `GetFirebirdHealthFunction()` decl.
- `src/firebird_extension.cpp` — register the function.
- `test/sql/firebird_health.test` (new).
- `scripts/build_matrix.ps1`, `.github/workflows/build-linux-fb-matrix.yml` —
  matrix entries.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` — public docs
  (PT/EN parity), including the explicit threshold note.

## Out of scope

- History / trending / sampling over time.
- Server-wide config (`firebird.conf`) inspection.
- Per-attachment long-running-transaction detail (future / index-profile item).
- Two-level warn/crit thresholds (single conservative threshold this increment).
- Any duckdb/community-extensions / upstream action.
