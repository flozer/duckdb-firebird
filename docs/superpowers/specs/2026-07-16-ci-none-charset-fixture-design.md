# CI NONE-charset fixture (#26) — design spec

## Problem

`test/sql/firebird_none_charset.test` — which carries the `CHARACTER SET
NONE` / `none_encoding` feature's assertions — is not invoked by any GitHub
Actions workflow. The `FIREBIRD_NONE_DB` fixture (a Firebird database whose
default charset is `NONE`, loaded from `scripts/fixture_none_charset.sql`)
is not provisioned in the Linux FB matrix job
(`.github/workflows/build-linux-fb-matrix.yml`), so this file runs only on
the local Windows build matrix (`scripts/build_matrix.ps1`).

Two other test files are collateral damage of this same gap, found during
#31's triage: `test/sql/firebird_explain_pushdown.test` and
`test/sql/firebird_type_audit.test` each carry a *mid-file*
`require-env FIREBIRD_NONE_DB` directive. Confirmed empirically (run
locally with the var unset): DuckDB's sqllogictest runner treats an unmet
`require-env` anywhere in the file as gating the **entire** file, not just
the block after it — both files currently report "All tests were skipped"
in CI, with 0 assertions, not partial coverage. #31 deliberately excluded
both from its test-file additions for exactly this reason and left them as
blocked follow-up.

## Goal

Provision a `CHARACTER SET NONE` Firebird database in the Linux FB3/4/5
matrix workflow, export it as `FIREBIRD_NONE_DB`, and invoke the three
files above in the "Run SQL test suite" step — turning all three from
CI-invisible (fully local-only, or fully skipped) into real, asserting CI
coverage on every push/PR.

## Design

### 1. Provision `none.fdb` (new step content, no new step)

Add to the existing "Provision FB${{ matrix.fb_major }} fixtures" step, as
a new block alongside the existing `test.fdb` (`fixture_common.sql`) and
`biz4.fdb` (`fixture_biz4.sql`) blocks:

```bash
printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/none.fdb' DEFAULT CHARACTER SET NONE;" \
  | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
docker cp scripts/fixture_none_charset.sql fb:/tmp/fixture_none_charset.sql
docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_none_charset.sql /var/lib/firebird/data/none.fdb

echo "FIREBIRD_NONE_DB=firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/none.fdb?charset=NONE" >> $GITHUB_ENV
```

Unlike `fixture_biz4.sql` (gated on `matrix.fb_major != '3'`, since
`DECFLOAT`/`INT128` are FB4+-only types), this block runs **unconditionally
on all three legs** — `CHARACTER SET NONE` and the `none_encoding` feature
are version-independent; there is no reason to skip FB3.

`scripts/fixture_none_charset.sql` is already committed
(from the local-only Windows workflow) and needs no changes: it builds its
non-ASCII test bytes via `ASCII_CHAR(0x..)` rather than embedding raw bytes
in the SQL file itself, so it is safe to run through the container's `isql`
regardless of the runner's shell/locale encoding — the same property that
lets `fixture_common.sql`/`fixture_biz4.sql` run unmodified in this
environment.

`scripts/fixture_none_charset.sql` joins `fixture_common.sql` and
`fixture_biz4.sql` as a third canonical, `docker cp`-sourced fixture file —
conceptually covered by the same "one canonical file per database" rule
#39 established, even though the file itself needs no content change here.

### 2. `?charset=NONE`, never a transliterating charset

The connection URL's `charset` parameter is validated by
`ValidateClientCharset` (`src/firebird_client.cpp:159`), which accepts only
`UTF8`, `UTF-8`, `NONE`, or `OCTETS` — anything else (e.g. `WIN1252`,
`ISO8859_1`) throws, because Firebird would transliterate the raw bytes
before they reach DuckDB, defeating the whole point of `none_encoding`
(which decodes the *raw, untransliterated* bytes client-side). `NONE` is
the only charset value that guarantees the wire protocol hands back the
exact stored bytes for `ValidateClientCharset` to accept and for
`none_encoding='win1252'`/`'iso8859_1'`/`'strict'`/`'blob'` to decode
correctly downstream. This matches local usage: `FIREBIRD_NONE_DB` is set
locally as a bare path with no `charset` parameter at all, which leaves
`info.charset` empty, so the extension never sends `isc_dpb_lc_ctype` and
Firebird defaults the connection to `NONE` — `?charset=NONE` in the CI URL
makes that same behavior explicit rather than relying on an implicit
default.

### 3. Anti-drift guard needs no change

`scripts/check_no_inline_fixture_drift.sh` (#39) matches on DDL keywords
(`CREATE TABLE`, `CREATE OR ALTER VIEW`, `CREATE VIEW`, `CREATE INDEX`,
`ALTER TABLE`, `COMMENT ON`, `CREATE DOMAIN`, `CREATE GENERATOR`) inside
`.github/workflows/build-linux-fb-matrix.yml` itself. The new block adds
one inline `CREATE DATABASE` line to the workflow YAML — deliberately not
in the guard's keyword list, and not a new gap it introduces: `CREATE
DATABASE` is bootstrap (declaring a new, empty database file and its
default charset), not fixture DDL (defining tables/views/indexes/data
inside one). The same distinction already holds for the two existing
`CREATE DATABASE` lines (`test.fdb`, `biz4.fdb`) that predate this fixture
and were confirmed clean against the guard when #39 landed. The actual
table/data DDL for `none.fdb` (`CREATE TABLE TXT`, three `INSERT`s) lives
entirely inside `scripts/fixture_none_charset.sql`, copied in via `docker
cp` and applied via `isql -i` — never inlined into the workflow YAML — so
the guard passes with zero changes.

### 4. Test wiring

Add to the existing "Run SQL test suite" step, unconditionally (all three
now get real `FIREBIRD_NONE_DB`-backed coverage on every leg):

```bash
./build/release/test/unittest test/sql/firebird_none_charset.test
./build/release/test/unittest test/sql/firebird_explain_pushdown.test
./build/release/test/unittest test/sql/firebird_type_audit.test
```

`firebird_explain_pushdown.test` and `firebird_type_audit.test` also
require `FIREBIRD_TEST_DB` (already provisioned) and `ISC_PASSWORD`
(already set in this step's `env:` block) — no additional environment
wiring needed beyond `FIREBIRD_NONE_DB` itself.

## Validation

- `scripts/check_no_inline_fixture_drift.sh` passes unchanged against the
  final workflow file (no new keyword-matching line introduced).
- Linux FB3/FB4/FB5 matrix workflow green, on all three legs (this fixture
  is not FB-version-gated).
- The real, load-bearing check: the CI log for the "Run SQL test suite"
  step must show **all three files reporting a nonzero, "All tests
  passed" assertion count** — not "All tests were skipped" — confirming
  `FIREBIRD_NONE_DB` actually resolved and the `require-env` gates are
  satisfied, not merely that the step didn't error. A skipped file with 0
  assertions must be treated as a failed validation, not a pass.
- Cross-version matrix (DuckDB v1.5.2/v1.5.3/v1.5.4) green, same as every
  prior branch — unaffected by this change (it's Linux-CI-only; the
  Windows local matrix already runs all three files with
  `FIREBIRD_NONE_DB` set, per the existing `build_matrix.ps1`).

## Out of scope

- `firebird_decfloat.test` / a DECFLOAT CI fixture — tracked separately as
  issue #48, not touched here.
- No change to `fixture_common.sql`, `fixture_biz4.sql`, `test.fdb`, or
  `biz4.fdb`.
- No change to `scripts/check_no_inline_fixture_drift.sh`'s own logic (its
  two known coverage gaps are tracked separately as issue #47).
- No changes to `duckdb/community-extensions` or any upstream repo.
