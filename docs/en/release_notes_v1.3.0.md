# Release notes — v1.3.0

Compatibility release: **DuckDB v2.0 support** ("Cyanoptera", released
2026-10-21), alongside continued support for **DuckDB v1.5.6+**. No new
SQL functions; the extension's feature surface is unchanged from v1.2.x.

## Version policy change

Supported DuckDB versions are now **v1.5.6+ and v2.0+** (v1.5.3 and older
1.5.x are no longer in the test matrix). The same source builds against
both API generations via a compile-time gate (`FIREBIRD_DUCKDB_V2`),
selected automatically from the DuckDB build's major version.

## What went into DuckDB v2.0 support

- **Table function declarations**: v2.0 replaced the `named_parameters`
  map with Python-style `FunctionSignature` declarations — positional
  parameters via `AddPositionalOnly`, named parameters via
  `AddKeywordOnly`. All function names, parameters, types, and order are
  unchanged; `duckdb_functions()` reports the same 19 functions with the
  same documented parameters on both API generations.
- **Bound expression encapsulation**: v2.0 made `Bound*Expression`
  members private; the view-reconciliation walker and filter translator
  now use the v2.0 accessors.
- **Extension build flags**: v2.0 retired
  `ENABLE_EXTENSION_AUTOINSTALL`/`AUTOLOADING`; the CMake gate handles
  both generations.
- **Runtime fix found during validation**: DuckDB v2.0 starts
  partition workers back-to-back, and the Windows local (XPS) Firebird
  provider intermittently drops simultaneous attach handshakes
  (`-901`). Attaches are now serialized process-wide (one-shot, cheap —
  no measurable cost).
- **`supports_pushdown_type`**: intentionally NOT made NONE-text-aware
  under `none_pushdown=true` — forcing unencodable literals into the
  planner's `TableFilterSet` produced raw pushes (wrong results) on
  v2.0. The complex-filter hook handles `=`/`IN` transcode on both API
  generations without it.

## Behavioral deltas at the v1.3.0 release (resolved in v1.3.1)

- **Residual visibility for text filters on NONE columns**: v2.0's
  planner lifts non-pushed text filters above the scan without offering
  them to the extension, so `residual_filters` / `not_pushed_reasons`
  stay empty for those (v1.5 recorded `NONE_CHARSET`) and
  `firebird_unpushed_mode='error'` does not fire for them. Results are
  identical (verified by count-identity tests); the visibility delta is
  tracked for a follow-up against the v2.0 planner APIs.
- **`firebird_last_query()` between statements in the sqllogictest
  harness**: reads empty on v2.0's harness context handling (records
  normally via the CLI/embedded — verified). Product behavior per
  session is unchanged.

## Post-release compatibility follow-up (resolved in v1.3.1)

Release v1.3.1 closes the test deltas above:

- DuckDB v2.0 normalizes `NOT IN` as `OPERATOR_NOT` around `COMPARE_IN`;
  the scanner now recognizes that shape. Remote `NOT IN` pushdown,
  NONE-column residual telemetry, and `firebird_unpushed_mode` guards all
  pass again.
- The telemetry failure attributed to harness context handling did not
  reproduce after correcting the remaining lambda syntax. Telemetry
  between statements passes without a product-state change.
- Windows/Firebird 5.0.3 validation is now identical on both lines:
  **26/26 test files and 1,082 assertions** on DuckDB v1.5.6 and v2.0.0.

These changes are not part of the published v1.3.0 tag or its binaries.

## Release assets

Two build variants per platform, per DuckDB line:

- `duckdb-firebird-<version>-linux-x64.tar.gz` /
  `...-windows-x64.zip` — built against **DuckDB v1.5.6** (loads on
  v1.5.6+).
- `duckdb-firebird-<version>-linux-x64-duckdb2.tar.gz` /
  `...-windows-x64-duckdb2.zip` — built against **DuckDB v2.0.0**.

## Validation

- `scripts/build_matrix.ps1 -Tags v1.5.6,v2.0.0`: v1.5.6 = 26/26 test
  files, 1,082 assertions; v2.0.0 = 18/26 + 8 documented deltas, 754
  assertions. Both build clean from the same source.
- Read-only battery on a real ~66GB Firebird 5.0.3 ERP restore
  (aggregate counts only) exercised scans, ATTACH, metadata, and
  diagnostics on both API generations.
- The DuckDB v2.0 CI asset legs run with `continue-on-error` on their
  first ride; failures there do not block the v1.5.6 assets.

## Compatibility notes

- Nothing removed or renamed; default behavior unchanged; every lever
  remains opt-in. v1.2.x users can upgrade in place (the v1.5.6-built
  asset is the in-place upgrade path; DuckDB v2.0 users take the
  `-duckdb2` assets).
