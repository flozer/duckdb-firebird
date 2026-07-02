# firebird_profile_table structured alerts — Design (v0.8 Diagnostics Bridge)

Date: 2026-06-23
Status: approved
Branch: `feat/v0.8-profile-warnings`

## Purpose

`firebird_profile_table` (shipped in v0.7) emits a `warnings LIST(VARCHAR)` of
free-text prose. This adds a parallel, machine-readable `alerts` column —
`LIST(STRUCT(code, severity, message))` — so consumers can branch on a stable
code and a severity instead of parsing prose. The change is **purely additive**:
`warnings` keeps its exact current shape, content, and order.

## Output

A new column `alerts` is appended after `warnings`:

```
alerts  LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR))
```

`firebird_profile_table` goes from 10 to 11 columns. All existing columns
(including `warnings`) are unchanged.

- `code` — stable public identifier (see catalog). **Contract:** a code is a
  stable public API. Once shipped, a code is never reused for a different
  condition and its meaning never changes. New conditions get new codes.
- `severity` — one of `LOW` | `MEDIUM` | `HIGH`, reusing the vocabulary of the
  existing `full_scan_risk` column (LOW = advisory, MEDIUM = degraded but
  functional, HIGH = real operational risk).
- `message` — the human-readable prose, byte-for-byte the same string that
  appears in `warnings`.

## Single source of truth + 1:1 invariant

Every diagnostic condition is raised through one helper,
`AddAlert(profile, code, severity, message)`, which appends an `Alert{code,
severity, message}` to the profile's alert list. At emit time:

- `alerts[i]` = `{code, severity, message}` of the i-th raised alert.
- `warnings[i]` = `alerts[i].message`.

**Tested invariant:** `len(alerts) == len(warnings)` and, for every `i`,
`alerts[i].message == warnings[i]`. `warnings` is no longer populated by hand —
it is derived from `alerts`, so the two can never diverge.

## Code + severity catalog

The ~14 existing prose warnings map to these codes and severities. The `message`
is the existing prose, preserved verbatim.

| code | severity | trigger |
|---|---|---|
| `view_no_scan_lever` | HIGH | object is a VIEW: no PK / index / partition lever |
| `view_definition_not_inspected` | MEDIUM | RDB$VIEW_SOURCE unreadable; shape unknown |
| `view_contains_join` | HIGH | view definition contains a JOIN |
| `view_contains_aggregation` | HIGH | view definition has GROUP BY / aggregate |
| `view_no_filter` | MEDIUM | view definition has no WHERE filter |
| `partition_advisory` | LOW | recommended partitions (from PK range) is advisory |
| `server_parallelism_caveat` | LOW | server-side parallelism (FB5) caveat |
| `pk_range_small_serial` | LOW | PK MIN/MAX span small → serial scan |
| `no_primary_key` | HIGH | no PK → full scan, not range-partitionable |
| `composite_pk_serial` | LOW | composite PK → serial scan |
| `numeric_pk_no_range_serial` | LOW | single numeric PK, no usable MIN/MAX range |
| `non_numeric_pk_serial` | LOW | single non-numeric PK → serial scan |
| `no_indexed_filter_columns` | MEDIUM | no cheap indexed filter columns |
| `none_charset_text_columns` | MEDIUM | CHARACTER SET NONE text columns present |

This catalog is the public contract. Severities are fixed as above.

## Mechanism (minimal refactor)

In `src/firebird_profile_table.cpp`:

- Add `struct Alert { std::string code; std::string severity; std::string message; };`
- Add `std::vector<Alert> alerts;` to `TableProfile` (replaces hand-populated
  `std::vector<std::string> warnings;` as the source — `warnings` is derived at
  emit).
- Add a helper `static void AddAlert(TableProfile &p, const char *code, const
  char *severity, std::string message)` that pushes one `Alert`.
- Replace every `p.warnings.push_back("…")` with the equivalent
  `AddAlert(p, "<code>", "<SEVERITY>", "…")`, keeping the prose identical.
- In `ProfileTableBind`, add the `alerts` column name/type after `warnings`.
- In `ProfileTableFunction`, emit `alerts` as a `LIST(STRUCT(...))` and derive
  the `warnings` list from `alerts[i].message` in order.

The STRUCT list is built with the DuckDB `Value::STRUCT` / `Value::LIST`
helpers. This is additive and stays within the existing file (no split).

## Testing

`test/sql/firebird_profile_table.test`:

- **Compatibility (unchanged):** every existing `warnings` assertion stays
  byte-for-byte identical — same content, same order. This proves the additive
  change did not alter the shipped contract.
- **Invariant:** `len(alerts) == len(warnings)` and
  `alerts[i].message == warnings[i]` for the profiled fixtures (assert via
  `unnest` / list element comparison, e.g. a query that confirms the two lists
  align element-by-element).
- **Deterministic code/severity by fixture:**
  - A table with no PK → `alerts` contains `{code:'no_primary_key',
    severity:'HIGH'}`.
  - A composite-PK fixture (`TPK_COMPOSITE`) → `composite_pk_serial` with
    severity `LOW`.
  - The view fixture (`V_ACTIVE_EMP`) → `view_no_scan_lever` HIGH present.
  - A NONE-charset text column fixture → `none_charset_text_columns` MEDIUM.
  - `EMPLOYEE` (has single numeric PK) → no `no_primary_key` alert.
- **Severity domain:** every emitted `alerts[*].severity` ∈ {LOW, MEDIUM, HIGH}.

Fixtures are the existing profile_table fixtures (EMPLOYEE / FILE_STORAGE /
TPK_COMPOSITE / V_ACTIVE_EMP) plus the NONE fixture where needed. No new
business data.

## Files

- `src/firebird_profile_table.cpp` — Alert struct, AddAlert helper, column,
  emit logic.
- `src/include/firebird_profile_table.hpp` — document the `alerts` column +
  the stable-code contract in the header comment.
- `test/sql/firebird_profile_table.test` — compatibility + invariant + code/
  severity assertions.
- `docs/pt/function_manual.md`, `docs/en/function_manual.md` — document the
  `alerts` column, the code/severity catalog, and the stable-code contract
  (PT/EN parity).

The local matrix and Linux FB matrix already run `firebird_profile_table.test`;
no matrix wiring change needed.

## Out of scope

- `firebird_index_profile` (the next v0.8 item).
- Any new alert condition beyond the 14 existing ones.
- Changing the underlying heuristics, `full_scan_risk`, or
  `recommended_partitions` logic.
- Removing or changing the `warnings` column.
- Any duckdb/community-extensions / upstream action.
