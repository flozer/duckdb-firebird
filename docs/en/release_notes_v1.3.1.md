# Release notes — v1.3.1

Patch release completing DuckDB v2.0 compatibility. No new SQL functions,
parameters, or changes to default behavior.

## Fixes

### DuckDB v2.0 `NOT IN` pushdown and observability

DuckDB v1.5 presents `NOT IN` to the extension as `COMPARE_NOT_IN`.
DuckDB v2.0 normalizes the same predicate to `OPERATOR_NOT` around
`COMPARE_IN`. The scanner now accepts both representations.

This closes the five v2.0 test failures caused by that expression-shape
change and restores:

- remote `NOT IN` pushdown, including DECFLOAT's lossless VARCHAR path;
- residual-filter and `NONE_CHARSET` telemetry for gated NONE columns;
- `firebird_unpushed_mode` warnings/errors for those residual filters;
- consistent `firebird_explain_pushdown` reports across both API lines.

### Cross-version test suite

Five assertions used the older lambda spelling. They now use DuckDB's
explicit `lambda` syntax, accepted by both v1.5.6 and v2.0. The telemetry
failure previously attributed to sqllogictest context handling did not
reproduce after that correction; no product-state change was needed.

The local build matrix no longer maintains a list of tolerated DuckDB
v2.0 test deltas.

### Required release assets

All four release-asset jobs are now required. Failure of either DuckDB
v2.0 package fails the workflow instead of being ignored with
`continue-on-error`.

## Validation

- DuckDB v1.5.6: **26/26 test files, 1,082 assertions**, Windows,
  Firebird 5.0.3.
- DuckDB v2.0.0: **26/26 test files, 1,082 assertions**, Windows,
  Firebird 5.0.3.
- The separate FB4-only type-audit test remained skipped locally because
  `FIREBIRD_BIZ4_DB` was not configured; it is covered by the FB4/FB5 CI
  fixtures.
- EMPLOYEE smoke passed on both DuckDB lines: native ATTACH, CUSTOMER
  scan, metadata discovery, and `firebird_last_query()` telemetry.

## Downloads and compatibility

Use the package built for your DuckDB line:

- `duckdb-firebird-1.3.1-linux-x64.tar.gz` and
  `duckdb-firebird-1.3.1-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.3.1-linux-x64-duckdb2.tar.gz` and
  `duckdb-firebird-1.3.1-windows-x64-duckdb2.zip`: DuckDB v2.0+.

The community catalog remains independent of this repository release.
