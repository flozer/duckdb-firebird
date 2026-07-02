# firebird_index_profile — Design (v0.8 Diagnostics Bridge)

Date: 2026-07-02
Status: approved
Branch: `feat/v0.8-index-profile`

## Purpose

Per the roadmap: `firebird_index_profile('fb.schema.table')` — candidate
indexes, selectivity when available, dependencies, and cheap-filter
recommendations. A per-table, **per-index** diagnostic complementing
`firebird_indexes` (whole-catalog metadata dump) and `firebird_profile_table`
(single-row table-level diagnostic, which already lists indexes as a
formatted string and carries structured `alerts`).

## Surface

```
firebird_index_profile('<catalog.schema.table>' | '<catalog.table>') -> N rows
```

- Same qualified-name parsing as `firebird_profile_table` (schema part
  accepted only as `main`, or omitted).
- Missing relation → `BinderException` (same contract as
  `firebird_profile_table`).
- Read-only: SELECT against `RDB$INDICES`, `RDB$INDEX_SEGMENTS`,
  `RDB$RELATION_CONSTRAINTS`, plus the existing `LoadTableSchema` path for
  column types. No DDL/DML, no business-data row values.
- **Grain: one row per existing index.** When the table has **zero indexes**,
  the function emits exactly **one synthetic row** instead of zero rows (see
  below) — every existing relation therefore returns at least one row.

## Output columns

| # | column | type | source |
|---|---|---|---|
| 1 | `index_name` | VARCHAR, nullable | `RDB$INDICES.RDB$INDEX_NAME`. **NULL marks the synthetic "no indexes" row** — never a real index name. |
| 2 | `columns` | LIST(VARCHAR) | `RDB$INDEX_SEGMENTS`, ordered by `RDB$FIELD_POSITION`. Empty for an expression index or the synthetic row. |
| 3 | `is_unique` | BOOLEAN, nullable | `RDB$UNIQUE_FLAG`. NULL on the synthetic row. |
| 4 | `is_active` | BOOLEAN, nullable | `NOT RDB$INDEX_INACTIVE`. NULL on the synthetic row. |
| 5 | `is_primary_key` | BOOLEAN, nullable | backs a PRIMARY KEY constraint (`RDB$RELATION_CONSTRAINTS`). NULL on the synthetic row. |
| 6 | `is_foreign_key` | BOOLEAN, nullable | backs a FOREIGN KEY constraint (same table, `CONSTRAINT_TYPE='FOREIGN KEY'`). NULL on the synthetic row. |
| 7 | `selectivity` | DOUBLE, nullable | `RDB$INDICES.RDB$STATISTICS`. In Firebird, a **lower** value tends to indicate a **more selective** index; `NULL` means the statistic was never computed (no `SET STATISTICS` / analyze pass). NULL on the synthetic row. |
| 8 | `alerts` | LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)) | catalog below |
| 9 | `unindexed_filter_candidates` | LIST(VARCHAR) | table-level fact, **repeated on every row** (same denormalization `firebird_indexes` already uses for `table_name`) |

## Alert catalog (factual only — no numeric selectivity threshold)

| code | severity | trigger |
|---|---|---|
| `no_indexes_on_table` | HIGH | table has zero indexes (drives the synthetic row) |
| `index_inactive` | MEDIUM | `is_active = false` |
| `missing_statistics` | LOW | `selectivity IS NULL` — statistic was never computed. Named for what NULL actually proves (absence), not "staleness", which NULL does not prove. |

Explicitly **out of scope for this version**: a `low_selectivity` alert with a
numeric threshold on `RDB$STATISTICS`. The value's scale is not yet validated
against a real production database, and a guessed threshold would be an
opaque, indefensible advisor rule — the same discipline applied to
`firebird_health`'s gap thresholds. `selectivity` is exposed as a raw,
nullable column; the caller judges it. A threshold-based alert can be added
later with a documented, validated value.

## `unindexed_filter_candidates` semantics

Table-level (not per-index), computed once per call, repeated on every
returned row. Reuses the existing `IsCheapFilterType` heuristic from
`firebird_profile_table.cpp` (numeric / DATE / TIMESTAMP / DECIMAL / VARCHAR
that is not `CHARACTER SET NONE`), applied to every column **not covered by
any index segment on this table — including segments of an *inactive*
index**. This means the column means "no index structure declares coverage
over this column", not "no *active* index covers it": if a column is only
covered by an inactive index, it is excluded here (that condition is signaled
separately via `index_inactive` on that index's row) and does not
double-count as a "candidate".

## Mechanism

New file, following the established pattern of small per-file helper
duplication (`firebird_profile_table.cpp` already duplicates `SqlLiteral`
locally rather than sharing across files):

1. `ValidateFirebirdAttachAlias` + parse the qualified name (same parser
   shape as `firebird_profile_table`).
2. Acquire the catalog lease; resolve the relation exists (reuse the same
   `RDB$RELATIONS` existence check as `firebird_profile_table`, or fail with
   a `BinderException` if not found).
3. Load one row per index: `RDB$INDICES` + `RDB$INDEX_SEGMENTS` (LEFT JOIN,
   ordered by segment position) + two LEFT JOINs to
   `RDB$RELATION_CONSTRAINTS` (one for `PRIMARY KEY`, one for
   `FOREIGN KEY`) keyed by `RDB$INDEX_NAME`, plus `RDB$STATISTICS` read via
   `GetDouble` (existing accessor, not yet used anywhere in the codebase).
4. Load the table's columns + types (`LoadTableSchema`, existing) to compute
   `unindexed_filter_candidates` from columns with no covering index segment.
5. Build `alerts` per index (`index_inactive`, `missing_statistics`); if the
   index list is empty, emit exactly one synthetic row with `index_name` NULL,
   all other index-scoped columns NULL, `alerts = [no_indexes_on_table/HIGH]`,
   and `unindexed_filter_candidates` populated normally (all eligible columns,
   since none are covered).
6. Emit via a multi-row `GlobalTableFunctionState` (row-cursor pattern, like
   the existing multi-row metadata functions), not the single-row
   `emitted`-flag pattern `firebird_profile_table` uses.

## Test fixtures (new — none of the existing fixtures have zero indexes or an
inactive index)

Add to `scripts/setup_test_firebird.sh`:

```sql
-- Zero-index fixture: no PK, no unique constraint, no FK, no explicit
-- index. Exercises firebird_index_profile()'s synthetic "no indexes" row,
-- the no_indexes_on_table alert, and unindexed_filter_candidates.
CREATE TABLE TNO_INDEX (
    CODE  VARCHAR(10),
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TNO_INDEX VALUES ('A1', 10, 'first');
INSERT INTO TNO_INDEX VALUES ('B2', 20, 'second');
COMMIT;

-- Inactive-index fixture: a PK (so this table is NOT a zero-index case)
-- plus one secondary index later disabled. Exercises the index_inactive
-- alert, and confirms unindexed_filter_candidates excludes a column
-- covered by an index even when that index is inactive.
CREATE TABLE TIDX_INACTIVE (
    ID    INTEGER NOT NULL PRIMARY KEY,
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TIDX_INACTIVE VALUES (1, 5, 'x');
COMMIT;
CREATE INDEX IDX_TIDX_INACTIVE_QTY ON TIDX_INACTIVE (QTY);
ALTER INDEX IDX_TIDX_INACTIVE_QTY INACTIVE;
COMMIT;
```

**Required companion change:** `test/sql/firebird_metadata.test` (around
lines 83–97) asserts the *exact, alphabetically ordered* full relation list
of the fixture database. Adding `TNO_INDEX` and `TIDX_INACTIVE` requires
inserting both names into that expected list in alphabetical order
(`TCHILD`, `TIDX_INACTIVE`, `TNO_INDEX`, `TPK_COMPOSITE`, `TQUOTES`, ...). This
is a required, explicit edit — not a side effect to discover via test
failure.

## Testing (`test/sql/firebird_index_profile.test`)

- `EMPLOYEE` (PK + FK + one expression index): row count > 1; the PK's row
  has `is_primary_key=true`, `is_unique=true`; the FK-backing index's row
  has `is_foreign_key=true`; the expression index (`EMP_UPPER_NAME_IDX`) row
  has `columns = []`.
- `TNO_INDEX`: exactly 1 row, `index_name IS NULL`, `alerts` contains
  `no_indexes_on_table`/HIGH, `unindexed_filter_candidates` contains `CODE`,
  `QTY`, `NOTE` (nothing excluded — nothing is indexed).
- `TIDX_INACTIVE`: 2 rows (PK index + `IDX_TIDX_INACTIVE_QTY`); the inactive
  index's row has `is_active=false` and `alerts` contains
  `index_inactive`/MEDIUM; `unindexed_filter_candidates` contains `NOTE` but
  **not** `QTY` (covered by the inactive index) and not `ID` (covered by the
  PK).
- `selectivity`: assert the column type is DOUBLE and is nullable (no fixture
  in this DB has had `SET STATISTICS` run, so every real row's `selectivity`
  is expected NULL — assert that, plus `missing_statistics`/LOW present on
  every real index row).
- Missing relation → `BinderException`, same message shape as
  `firebird_profile_table`.

## Files

- `src/firebird_index_profile.cpp` (new).
- `src/include/firebird_index_profile.hpp` (new).
- `src/firebird_extension.cpp` — register the function.
- `test/sql/firebird_index_profile.test` (new).
- `scripts/setup_test_firebird.sh` — the two new fixture tables.
- `test/sql/firebird_metadata.test` — update the exact relation-list
  assertion.
- `scripts/build_matrix.ps1`, `.github/workflows/build-linux-fb-matrix.yml` —
  matrix entries (mirroring the EMPLOYEE-fixture-backed test wiring already
  used for `firebird_profile_table.test`/`firebird_health.test`).
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` — new function
  section (parity), including the selectivity-direction note and the
  no-numeric-threshold rationale for `missing_statistics` vs a future
  `low_selectivity`.

## Out of scope

- `low_selectivity` alert with a numeric threshold (needs real-DB evidence
  first).
- Long-running-transaction warnings (separate roadmap bullet).
- Any change to `firebird_indexes`, `firebird_profile_table`'s existing
  columns/alerts, or the DuckDB submodule pin.
- Any duckdb/community-extensions / upstream action.
