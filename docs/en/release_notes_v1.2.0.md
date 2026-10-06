# Release notes — v1.2.0

Feature release closing the G-queue: diagnostics, pushdown and stability
improvements sourced from real production feedback (a Brazilian-ERP
lakehouse reading a legacy `CHARACTER SET NONE` database over a slow WAN).
No new SQL functions — richer behavior on the existing surface, plus two
new `firebird_scan`/ATTACH options, one new session setting, and additive
telemetry columns. Released from `main`; the DuckDB Community catalog
update is a separate, explicitly authorized step.

## Highlights

### `none_pushdown` — `=` / `IN` over CHARACTER SET NONE columns (G2)

The biggest unlock. Text filters on NONE columns never pushed down (a
1000-id `IN` meant a full server scan). With
`firebird_scan(..., none_encoding='win1252', none_pushdown=true)`
(also an ATTACH option), constant `=` and `IN` literals are re-encoded to
the storage bytes behind a `_WIN1252`/`_ISO8859_1` charset introducer —
Firebird compares NONE columns byte-by-byte, so the match is lossless for
encodable literals. Unencodable literals (e.g. `€` under latin1) fall
back to DuckDB automatically with the stable `NONE_CHARSET` reason.
Scope: `=`/`IN` on CHAR/VARCHAR only; ranges, `LIKE` and `NOT IN` remain
client-side. Validated on a real ERP base: the pushed SQL carries
`ANOMES IN (_WIN1252 '202501', ...)` end to end.

### `firebird_unpushed_mode` — no more silent full scans (G1)

`SET firebird_unpushed_mode = 'error'` fails a scan the moment it keeps
filters in DuckDB, naming each residual reason with its remedy;
`'warn'` emits a native DuckDB warning; `'silent'` (default) preserves
the historical behavior. Turns hours of silent full scan into a
seconds-fast, actionable failure.

### `numeric_widen_int64` — the NUMERIC(18,s) overflow class, gone (G3)

Firebird's int64-backed `NUMERIC(18,s)` carries scaled values up to
±9.2e18, but DuckDB's `DECIMAL(18,s)` tops out at 1e18−1 scaled: large
values round-trip for display and then break innocent arithmetic
(`Out of Range Error: Overflow in multiplication of DECIMAL(18)` — the
classic sentinel is `NUMERIC(18,6) = -9223372036854.775808`).
`numeric_widen_int64=true` (scan or ATTACH) projects those columns as
`DECIMAL(38,s)` — lossless. `firebird_type_audit` gained the
`int64_numeric_widenable` finding so tooling can react without the flag.

### Session stability: keepalive + fetch-failure context (G4)

`SET firebird_dummy_packet_interval = 60` (seconds, 0 = off) sends the
Firebird keepalive DPB per connection for long sessions over NAT/WAN.
When a fetch does die (`-504` cursor lost / `-902` shutdown), the error
now carries the table, rows fetched so far, the keepalive hint, and the
`firebird_last_query()` reference. Honest caveat documented: recent
Firebird versions may parse but not act on the DPB item
(firebird#8266); the error context is the reliable half.

### `bytes_read_estimate` — network budgeting (G5)

`firebird_last_query()` and `firebird_query_log()` gained
`bytes_read_estimate` (rows × summed XSQLDA widths + BLOB segment bytes
actually read; a documented ESTIMATE — fbclient does not expose wire
bytes), and `firebird_pool_stats()` gained the per-catalog lifetime
total (ATTACH-path scans only). Built for WAN budgeting: "this scan will
cost 47k bytes" before running it.

### CI/test debt closed (F-series follow-through)

The G coverage is wired into the canonical suites: five new test files
(`firebird_unpushed_mode`, `firebird_none_pushdown`,
`firebird_numeric_widen`, `firebird_session_stability`,
`firebird_bytes_estimate`) plus the F-series fixtures, with
`numerics.fdb` provisioning added to `setup_test_firebird.sh` and both
Linux workflows.

### Keyset-resume experiment (G6 — recorded, implementation deferred)

On a real ~66GB ERP base (967k-row table, sparse 20M PK span,
aggregates only): `ORDER BY pk` is index-driven at ~zero cost (4.94s vs
5.02s) and a resume-style `WHERE pk > X ORDER BY pk` pushes down and
runs in the same time — keyset resume after `-504`/`-902` is viable and
cheap. Implementation becomes a post-v1.2.0 follow-up with the design
de-blocked.

## Validation

- Full sqllogictest suite green in CI wiring (26 test files) and locally
  via `scripts/build_matrix.ps1`: **26/26 test files, 1,078 assertions,
  PASS identical on DuckDB v1.5.3 and v1.5.6**.
- Read-only battery on a real ~66GB Firebird 5.0.3 ERP restore
  (metadata and aggregate counts only): `none_pushdown` pushed
  `ANOMES IN (_WIN1252 ...)` end to end with equal counts;
  `firebird_unpushed_mode='error'` failed a NONE `NOT IN` with the full
  remedy; `numeric_widen_int64` projected `NUMERIC(18,2)` as
  `DECIMAL(38,2)` with a clean `SUM` over 5,902 rows;
  `firebird_dummy_packet_interval` armed with health intact;
  `bytes_read_estimate` deterministic (47,216 = 5,902 × 8) and
  accumulating per catalog with coherent pool state.

## Compatibility notes

- Fully additive: two new named parameters (`none_pushdown`,
  `numeric_widen_int64`), one new session setting
  (`firebird_unpushed_mode`), one new ATTACH option
  (`firebird_dummy_packet_interval` is a session setting consumed at
  connection creation), five new telemetry/statistics columns
  (`bytes_read_estimate` ×3 surfaces, `active_connections` and
  `last_error` arrived in v1.1.0), one new stable finding
  (`int64_numeric_widenable`). Nothing removed or renamed; default
  behavior is unchanged everywhere (every new lever is opt-in).
- `firebird_scan` now exposes 13 parameters (2 positional + 11 named).
