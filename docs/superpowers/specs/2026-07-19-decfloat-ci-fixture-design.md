# DECFLOAT CI fixture (#48) — design spec

**Classification:** CI type/compatibility coverage within the Production
Stability Gate, not a new product feature. `test/sql/firebird_decfloat.test`
exercises existing, already-shipped behavior (the extension's DECFLOAT(16/34)
fallback, which projects those columns as `CAST(... AS VARCHAR(64))` so they
surface losslessly instead of the old silent NULL). This front only wires
that existing behavior into CI — no change to `src/` is expected unless the
CI run surfaces a genuine, previously-hidden bug, which is out of scope to
predict or design around here.

## Problem

`test/sql/firebird_decfloat.test` is not invoked by any GitHub Actions
workflow. Its fixture (`scripts/fixture_decfloat.sql`, table `DECVALS`) is
never loaded into any CI database, so `FIREBIRD_DECFLOAT_DB` is unset in CI
and the whole file is skipped there — found during #31's triage, tracked
as issue #48, deliberately kept out of #31/#26 scope. DECFLOAT is a
Firebird 4+ type (does not exist in FB3), so this only applies to the
`fb_major != '3'` matrix legs — the same conditional `fixture_biz4.sql`
and the "FB4+ live type smoke" step already use.

## Design

### 1. Load `DECVALS` into the existing `biz4.fdb`, not a new database

`scripts/fixture_biz4.sql` (tables `FB4_TYPES`, `DEPARTMENT`, `CUSTOMER`)
and `scripts/fixture_decfloat.sql` (table `DECVALS`) share no table names —
confirmed by reading both files in full. `biz4.fdb` is already provisioned,
FB4+-gated, in the "Provision FB${{ matrix.fb_major }} fixtures" step. Add
one line right after the existing `fixture_biz4.sql` load, inside the same
`if [ "${{ matrix.fb_major }}" != "3" ]` block:

```bash
docker cp scripts/fixture_decfloat.sql fb:/tmp/fixture_decfloat.sql
docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_decfloat.sql /var/lib/firebird/data/biz4.fdb
```

No new `CREATE DATABASE`, no inline SQL, no new database file — `DECVALS`
just becomes a second table in the database `FIREBIRD_BIZ4_DB` already
points at.

### 2. `FIREBIRD_DECFLOAT_DB` = the same URL as `FIREBIRD_BIZ4_DB`

The "FB4+ live type smoke (INT128 / DECIMAL(38) / TIMESTAMP_TZ)" step
already: (a) is gated `if: matrix.fb_major != '3'`, (b) sets
`FIREBIRD_BIZ4_DB` in its own `env:` block (step-scoped, not global — it
isn't exported via `$GITHUB_ENV` because nothing outside this one step
needs it). Add `FIREBIRD_DECFLOAT_DB` to that same `env:` block, same URL:

```yaml
        env:
          ISC_USER: SYSDBA
          ISC_PASSWORD: masterkey
          FIREBIRD_BIZ4_DB: firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8
          FIREBIRD_DECFLOAT_DB: firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8
```

`firebird_decfloat.test` requires only `FIREBIRD_DECFLOAT_DB` (confirmed:
no `FIREBIRD_TEST_DB`/other requirement), so no other env wiring is
needed.

### 3. Test wiring

Add to the same step's `run:` block, after the existing
`firebird_type_audit_fb4.test` invocation:

```bash
./build/release/test/unittest test/sql/firebird_decfloat.test
```

## Validation

- FB4/FB5 jobs run `firebird_decfloat.test` for real — the CI log must show
  a nonzero "All tests passed" assertion count for this file on both jobs,
  not "All tests were skipped".
- FB3's job shows this step (and therefore this file) absent/skipped
  because of the job-level `if: matrix.fb_major != '3'` condition — the
  same version-based skip already visible for `firebird_type_audit_fb4.test`
  today — not an env-var-based skip. This is a version gate, not a fixture
  gap.
- Linux FB3/FB4/FB5 matrix workflow green.
- No behavior change to the extension (`src/`) — unless the real CI run
  surfaces a genuine bug, in which case that becomes its own, separately
  scoped fix, not silently folded into this front.

## Out of scope

- No change to `scripts/fixture_biz4.sql`, `scripts/fixture_decfloat.sql`,
  `scripts/fixture_common.sql`, or `scripts/fixture_none_charset.sql`
  content.
- No change to `scripts/check_no_inline_fixture_drift.sh` (already
  recognizes the 3 canonical fixture files as of #47; `fixture_decfloat.sql`
  is loaded the same `docker cp` + `isql -i` way, so it needs no guard
  change either — it introduces no inline DDL).
- No changes to `duckdb/community-extensions` or any upstream repo.
