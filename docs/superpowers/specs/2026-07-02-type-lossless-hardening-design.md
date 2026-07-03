# Type Lossless Hardening — Design (v1.0 Production Bridge, item #33)

Date: 2026-07-02
Status: approved
Branch: `feat/v1.0-type-lossless-hardening`

## Purpose

v1.0's "Type Lossless Hardening" roadmap bullet targets hard Firebird types
(`INT128`, high-precision `NUMERIC`/`DECIMAL`, `DECFLOAT(16/34)`,
`TIME`/`TIMESTAMP WITH TIME ZONE`, `BLOB` text, `CHARACTER SET NONE`/`WIN1252`).
This increment does **one thing**: fix issue #33 (`INTERNAL Error: Expected
vector of type INT64, but found vector of type INT128` on
`SELECT * FROM V_DEPT_HEADCOUNT`). It also documents a **central rule** for
how this codebase already treats — and should keep treating — every hard
type, and records two new findings discovered while mapping current
behavior, without fixing them (they get their own issues/increments).

## Central rule (policy, documented — not a new enforcement mechanism)

For every Firebird type this extension surfaces:

1. If DuckDB has a lossless equivalent representation, use it.
2. If it doesn't, expose the value as a lossless `VARCHAR` (never truncate
   it to fit an arbitrary width) or fail explicitly (raise, don't silently
   substitute a wrong/default value).
3. Never truncate, round, or return an incorrect value without a visible
   error or an explicit, documented caveat.

## Current-state map against the rule (research finding, not new work)

| Type | Path | Verdict |
|---|---|---|
| `INT128` (unscaled) | `firebird_types.cpp` fetch, `GetInt128` | Lossless — correct today for real stored columns |
| `NUMERIC`/`DECIMAL`, any storage width, with/without scale | `ScaledIntegerToDecimal` bucketing by storage width | Lossless for real stored columns — **but see #33 below: the *bind-time type derivation* for a VIEW's computed/aggregate column can disagree with the runtime wire type**, which is exactly what #33 is |
| `DECFLOAT(16/34)` | Server-side `CAST(... AS VARCHAR(64))` | Lossless by design (validated: zero, decimals, exponent form, NaN, ±Inf) |
| `TIME WITH TIME ZONE` | `GetTimeTzUtc`, offset hardcoded to `0` | **Lossy** — the original zone/offset is discarded. Already flagged by `firebird_type_audit`'s `time_tz` finding. Not fixed in this increment (different root cause than #33; tracked as a future increment). |
| `TIMESTAMP WITH TIME ZONE` | Normalized to a UTC instant | Lossless-for-instant (matches DuckDB's own `TIMESTAMP_TZ` semantics), zone identity not preserved — same caveat already documented via `firebird_type_audit`'s `timestamp_tz` finding. Not fixed here. |
| `BLOB` text (`SUB_TYPE 1`) | `ReadBlob` | **Lossy** — a NEW finding from this mapping pass: `FirebirdStatement::ReadBlob` (`firebird_client.cpp:833`) exits its segment-read loop on the first `rc==0`, but `rc==0` only means "this segment fit the 8192-byte buffer," not "the BLOB is exhausted." A multi-segment BLOB whose first segment is ≤8192 bytes silently loses every subsequent segment. `firebird_type_audit`'s `blob_text` finding documents a *caveat*, but does not detect *this specific bug*. **Not fixed in this increment** — different root cause (a storage/fetch-loop defect, not a type-mapping mismatch) and different risk profile than #33's fix. Tracked as a separate GitHub issue, opened alongside this spec, for its own future increment with a dedicated multi-segment fixture. |
| `CHARACTER SET NONE` | `TranscodeNoneCharset`, 4 modes | Matches the rule exactly: `STRICT` fails explicitly on invalid UTF-8; `WIN1252`/`ISO_8859_1` are lossy-by-explicit-opt-in (the caller chose a specific encoding assumption); `BLOB` mode is lossless (raw bytes). Already correct. |

## Root cause of #33 (confirmed by live reproduction + XSQLDA trace)

`V_DEPT_HEADCOUNT` is `SELECT e.DEPT_NO, COUNT(*), SUM(e.SALARY) FROM EMPLOYEE
e JOIN EMPLOYEE m ON m.DEPT_NO = e.DEPT_NO GROUP BY e.DEPT_NO`, where
`EMPLOYEE.SALARY` is `NUMERIC(10,2)`.

- **Catalog metadata** (`RDB$RELATION_FIELDS ⋈ RDB$FIELDS` for
  `V_DEPT_HEADCOUNT.TOTAL_SALARY`, the column Firebird froze at `CREATE VIEW`
  time): `RDB$FIELD_TYPE=26` (`blr_int128`), `RDB$FIELD_SUB_TYPE=1`,
  `RDB$FIELD_SCALE=-2`, `RDB$FIELD_PRECISION=38`. `LoadTableSchema`
  (`firebird_scanner.cpp`) reads exactly this row and `FirebirdToDuckDBType`
  maps it to `DECIMAL(38,2)` — an `INT128`-*physical* DuckDB type. This is
  what `FirebirdScanBind` uses to build `return_types`, which is what
  DuckDB uses to **allocate** the output `Vector` before any row is fetched.
- **The actual runtime XSQLDA** (confirmed via a standalone `isc_dsql_prepare`
  + `isc_dsql_describe` probe against the exact SQL
  `FirebirdQueryBuilder::Build` emits for this scan, and independently
  confirmed identical for the bare `SELECT SUM(SALARY) FROM EMPLOYEE`):
  `sqltype base=580` (`SQL_INT64`), `sqlsubtype=1`, `sqlscale=-2`,
  `sqllen=8`. Firebird's own DSQL compiler evaluates `SUM(NUMERIC(10,2))` as
  **BIGINT-backed** `NUMERIC(18,2)` at prepare time — narrower than the
  view's frozen catalog metadata claims.
- At fetch, `AllocateBuffers` sets the live cursor's own
  `FirebirdColumnDesc.sqltype` from this same live XSQLDA (`SQL_INT64`), so
  `FirebirdAppendValue`'s `SQL_INT64` branch calls
  `StoreDecimal<int64_t>` → `FlatVector::GetData<int64_t>(target)` — but
  `target` was allocated as `DECIMAL(38,2)` (`INT128` physical) from the
  stale bind-time catalog type. DuckDB's own `Vector::VerifyVectorType`
  assertion fires: `Expected vector of type INT64, but found vector of type
  INT128`.

**This is not a generic "`INT128` unhandled" gap.** `INT128` fetch is
correct when the bind-time type and the runtime wire type agree (true for
every real stored column, since a column's storage width cannot change
per-query). The bug is specific to a **view's computed/aggregate column**,
whose catalog-frozen type can legitimately disagree with what Firebird's
live SQL compiler produces for the identical expression today.

## Fix

`FirebirdScanBind` already has (from the Smart Scan Planning Report work) a
shared `LookupObjectType` call to detect whether the scan target is a view.
When it is:

1. Run a cheap, execute-free `FirebirdStatement::Prepare`-only call against
   the actual projected SQL (`FirebirdQueryBuilder::Build`'s output for this
   scan, built with no `WHERE`/pushdown needed — just the column list) —
   this is the exact call already used elsewhere in this codebase for
   statement preparation, not new machinery.
2. **Reconcile the FULL column descriptor from the live XSQLDA, not just
   the DuckDB `LogicalType`.** This is the part that must not be
   half-done: `FirebirdColumnDesc.sqltype`, `.sqlsubtype`, `.sqlscale`, and
   `.sqllen` (whatever `AllocateBuffers`/`FirebirdAppendValue` actually
   read to decide the fetch's accessor width) must ALL be overwritten from
   the live XSQLDA for a view target — not just the derived
   `LogicalType` used for `return_types`. Fixing only `return_types` while
   leaving a stale `column_descs` entry would still crash or, worse,
   silently misread bytes at the wrong width — the exact class of bug
   this whole increment exists to prevent. Both `column_types` (feeds
   `return_types`, i.e. Vector allocation) and `column_descs` (feeds
   `FirebirdAppendValue`'s fetch-time accessor choice) must be derived
   from the SAME live XSQLDA source, together, for every column of a view
   target. (Correction from final review: the crash itself is prevented
   specifically by reconciling `column_types` — it drives DuckDB's
   `return_types`/Vector allocation. `column_descs` does not
   independently drive the fetch-time accessor width the way this
   paragraph originally implied — the actual fetch reads accessor width
   from the live fetch cursor's own re-described XSQLDA, not from
   `bind.column_descs`. Reconciling `column_descs` is still correct and
   kept: it keeps pushdown-gating and `character_set_id` consistent with
   the real wire type. But it is not, on its own, what prevents the
   `Vector::VerifyVectorType` assertion.)
3. Only the `character_set_id` (which XSQLDA cannot supply for text/BLOB
   columns) still comes from the catalog read — matching
   `OpenNextPartitionCursor`'s existing per-cursor precedent of trusting
   live XSQLDA for wire-shape and catalog only for what XSQLDA can't give.

**Decision, recorded explicitly: this extra `Prepare`-only round trip runs
ONLY when the target is a view** (gated on the existing `LookupObjectType`
check). A base table's storage width cannot legitimately disagree with its
own catalog metadata between prepare and now — the risk this fix addresses
is specific to a view's frozen-at-`CREATE VIEW`-time column metadata, not
to real columns. Gating on view-only keeps the common (base table) scan
bind path at zero added I/O cost.

## Testing

`test/sql/firebird_scan.test` (or a new dedicated case file, implementer's
call) gains a case using the existing `V_DEPT_HEADCOUNT` fixture (no new
fixture needed — it already reproduces #33 deterministically) that proves
**both** of the following, not just the absence of a crash:

1. `SELECT * FROM fb.main.V_DEPT_HEADCOUNT` (no `row_limit`, exactly the
   failing case from #33) completes without error.
2. `TOTAL_SALARY`'s **value** is numerically correct (compare against the
   same aggregate computed independently, e.g. via
   `SELECT SUM(SALARY) FROM fb.main.EMPLOYEE WHERE DEPT_NO = '600'` for one
   known group) **and** its **DuckDB column type** matches what the
   reconciled live XSQLDA implies (`DECIMAL(18,2)`/`BIGINT`-physical, not
   the catalog's stale `DECIMAL(38,2)`/`INT128`-physical) — asserting only
   "it didn't crash" would not catch a silent wrong-width read if the fix
   were incomplete (e.g. `return_types` fixed but `column_descs` left
   stale, or vice versa).

## `firebird_type_audit`

No new detection logic. Confirm the existing `int128` finding (unscaled
`INT128`, scale=0) still fires correctly and is unaffected by this fix
(the fix only changes how a VIEW's column type is derived at scan-bind
time; `firebird_type_audit` reads the catalog directly for its own
reporting purpose and is not changed). Document the central rule in the
function manual, referencing the existing 6 finding codes
(`decfloat_as_varchar`, `int128`, `time_tz`, `timestamp_tz`, `none_charset`,
`blob_text`) as the audit surface for the categories this rule governs.

## Fixture validation (no new fixture)

Confirm — without adding a new fixture — that the existing
`none.fdb`/`firebird_none_charset.test` already exercises all three
`none_encoding` branches relevant to the central rule (`WIN1252`,
`ISO_8859_1`, `STRICT`) plus the `BLOB`-mode caveat and the
`firebird_type_audit` `none_charset`/`blob_text` findings. If any of these
is not already asserted, add the missing assertion(s) against the
**existing** fixture — do not create a new one.

## Out of scope (tracked separately)

- `ReadBlob`'s multi-segment truncation bug (new GitHub issue, opened
  alongside this spec) — different root cause (a storage/fetch-loop
  defect), different risk profile, needs its own multi-segment fixture.
  Not fixed here.
- `TIME WITH TIME ZONE`'s discarded offset — already flagged by
  `firebird_type_audit`'s `time_tz` finding; a real fix (if ever pursued)
  is a separate future increment.
- Any broader refactor of `LoadTableSchema` for its OTHER callers
  (`firebird_profile_table`, `firebird_index_profile`,
  `firebird_explain_pushdown` all call it for column-metadata-only
  purposes — heuristic classification, not row-value fetching — so a
  stale catalog type there cannot crash the way it does in the scan path;
  not touched by this increment).
- A new WIN1252/NONE-charset fixture.
- Any duckdb/community-extensions / upstream action.
