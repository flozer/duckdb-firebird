# Contributing

Thanks for helping improve `duckdb-firebird`.

This project is a DuckDB extension that talks to Firebird through
`libfbclient`, so correctness, reproducible tests, and conservative
pushdown rules matter more than clever shortcuts.

## Before Opening a Pull Request

1. Open an issue or draft PR for behavior changes, new pushdown rules, type
   mapping changes, or public API changes.
2. Keep pull requests focused. Avoid mixing refactors with feature changes.
3. Do not include real Firebird databases, customer data, production paths,
   private keys, tokens, or passwords.
4. Use synthetic fixtures under `scripts/` and `test/sql/`.

## Development Setup

Clone with submodules:

```bash
git clone https://github.com/flozer/duckdb-firebird.git
cd duckdb-firebird
git submodule update --init --recursive --depth=1
```

Build with the DuckDB extension harness:

```bash
GEN=ninja make release
```

See the platform guides for details:

- `docs/guide_linux.md`
- `docs/guide_windows.md`

## Testing

Run the sqllogictest suites against real Firebird servers when changing
scanner behavior, type mapping, predicate pushdown, ATTACH metadata, or
charset handling.

At minimum, changes should cover the affected area:

- `test/sql/firebird_scan.test`
- `test/sql/firebird_attach.test`
- `test/sql/firebird_none_charset.test`
- `test/sql/firebird_paging.test`
- `test/sql/firebird_bind_params.test`
- `test/sql/firebird_predicates.test`
- `test/sql/firebird_metadata.test`

For release candidates, verify both Firebird 4 and Firebird 5 when possible.

## Pushdown Rules

Only push predicates to Firebird when the result is semantically equivalent
to DuckDB evaluation. If there is any doubt, leave the predicate residual so
DuckDB re-checks it.

Special care is required for:

- `CHARACTER SET NONE` text columns;
- bind parameters and Firebird XSQLDA types;
- timestamp/time-zone literals;
- unsigned integers, huge integers, and high-precision decimals;
- partitioned scans combined with paging.

## Security

Follow `SECURITY.md` for vulnerability reporting. If a change affects SQL
generation, credential handling, file access, or GitHub Actions, call that
out explicitly in the PR description.
