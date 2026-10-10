# Release notes — v1.2.1

Patch release: one ergonomics fix reported by a production data-lake
consumer. No new functions; fully additive.

## Fix

### `none_encoding` / `none_pushdown` accepted inside the connection string

The v1.2.0 `none_pushdown` opt-in was only reachable as a
`firebird_scan` named parameter or an ATTACH option — putting it in the
connection string was silently ignored (the parser skipped the unknown
key, so the configuration looked applied while staying off). Both are now
parsed from the connection string itself, in both supported forms:

- URI query: `firebird://user:pass@host:3050/path?charset=UTF8&none_encoding=win1252&none_pushdown=true`
- key=value DSN: `database=C:/data/erp.fdb;user=APP_READONLY;password=secret;none_encoding=win1252;none_pushdown=true`

Booleans accept `true`/`false`/`1`/`0`; a malformed boolean fails with an
actionable error instead of being silently ignored (`connection-string
option 'none_pushdown' expects true/false (or 1/0), got '...'`).

Precedence: explicit ATTACH option / `firebird_scan` named parameter >
connection string > default. `numeric_widen_int64` remains an ATTACH
option / named parameter only (unchanged).

The ATTACH path propagates the connection-string values to the catalog
(every scan behind the alias pushes NONE `=`/`IN`), and the direct
`firebird_scan` path honors the string with the named parameter keeping
precedence.

## Validation

- Full local matrix: 26/26 test files, PASS identical on DuckDB v1.5.3
  and v1.5.6.
- New connection-string coverage in `firebird_none_pushdown.test` (DSN
  pushdown end to end, malformed-boolean error; the URI query form is
  exercised by the CI legs where the `firebird://` env form is canonical).
- Read-only spot battery on a real ~66GB Firebird 5.0.3 ERP restore
  (aggregate counts only): connection-string-only configuration pushes
  `ANOMES IN (_WIN1252 ...)` end to end with counts identical to the
  named-parameter form.
