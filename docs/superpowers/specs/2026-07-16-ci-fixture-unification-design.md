# CI Fixture Unification (#39) — design spec

## Problem

`scripts/setup_test_firebird.sh` (the canonical local/matrix fixture) and
`.github/workflows/build-linux-fb-matrix.yml` (the Linux FB3/4/5 matrix
workflow's own inline fixture) have drifted twice already this cycle
(PR #38): a new table/view/index added to the canonical script silently
does not exist in the workflow's hand-duplicated copy until someone
notices a red CI run and patches the workflow by hand.

Current drift, confirmed by direct comparison:

- Canonical script: 8 tables (`EMPLOYEE`, `FILE_STORAGE`,
  `TBLOB_MULTISEG`, `TQUOTES`, `TPK_COMPOSITE`, `DEPT`, `TCHILD`,
  `TNO_INDEX`, `TIDX_INACTIVE`), 5 views, 3 indexes, a domain, a
  generator, table/column comments.
- Workflow's inline copy: 5 tables (`EMPLOYEE`, `DEPT`, `FILE_STORAGE`,
  `TNO_INDEX`, `TIDX_INACTIVE`), 3 views (`V_ACTIVE_EMP`, `V_ALL_EMP`,
  `V_DEPT_HEADCOUNT`), 2 indexes. Missing: `TBLOB_MULTISEG`, `TQUOTES`,
  `TPK_COMPOSITE`, `TCHILD`, `V_LITERAL_NOISE`, `V_MULTILINE_KW`.

This drift is currently **latent** — none of the 4 test files the
workflow's "Run SQL test suite" step actually runs
(`firebird_scan.test`, `firebird_attach.test`, `firebird_health.test`,
`firebird_index_profile.test`) reference any of the missing
tables/views today. It has already caused two real red-CI incidents
when new tests were added (PR #38) and will do so again the next time a
test that needs a not-yet-duplicated fixture piece is added to that
step list.

## Goal

Eliminate the duplication at its root — one canonical SQL source, both
the local/matrix flow and the Linux CI workflow execute the *same*
file — rather than continuing to hand-sync two copies. Add a cheap,
permanent guard so a future re-introduction of inline fixture DDL in the
workflow fails immediately and loudly, instead of waiting for another
silent-drift incident.

## Design

### 1. Shared DDL asset (`scripts/fixture_common.sql`)

Extract the full heredoc body currently inline in
`scripts/setup_test_firebird.sh` (everything between the two `isql`
invocations — `EMPLOYEE` through the last `TIDX_INACTIVE` index
statement, including the domain, generator, and comment statements) into
a new file, `scripts/fixture_common.sql`, preserving statement order,
`COMMIT`s, and existing comments verbatim (this file is the new single
source of truth — comments explaining *why* a fixture piece exists stay
valuable there).

`scripts/setup_test_firebird.sh` changes its second `isql` invocation
from a `<<'EOF' ... EOF` heredoc to:

```bash
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" -i scripts/fixture_common.sql
```

`.github/workflows/build-linux-fb-matrix.yml`'s "Provision FB${{
matrix.fb_major }} fixtures" step drops its inline `CREATE TABLE`/`CREATE
OR ALTER VIEW`/`CREATE INDEX` heredoc entirely and instead:

```bash
docker cp scripts/fixture_common.sql fb:/tmp/fixture_common.sql
docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_common.sql /var/lib/firebird/data/test.fdb
```
— the same `docker cp` + `docker exec ... -i` pattern the workflow
already uses for `fixture_biz4.sql` (the FB4+-only fixture), which stays
a separate file, untouched, exactly as it is today.

The workflow keeps its own `CREATE DATABASE` step (server-local path,
`/var/lib/firebird/data/test.fdb`) — only the fixture *content* is
shared, not the database-creation step, which is legitimately
environment-specific (local file path vs. container-local path).

### 2. `mkblob_fixture` compiled on the runner, not in the container

The `firebirdsql/firebird:*-noble` images are minimal server runtimes —
no C++ toolchain, no Firebird SDK headers — so `mkblob_fixture.cpp`
cannot be compiled inside the container. It's compiled on the GitHub
Actions runner instead, right after the existing "Extract matching
libfbclient from the fb${{ matrix.fb_major }} container" step (PR #41),
reusing that step's output:

```bash
g++ -std=c++17 -I/usr/include/firebird \
    -o mkblob_fixture scripts/mkblob_fixture.cpp \
    -L"$LOCAL_DIR" -l:"$(basename "$LOCAL_LIB")"
```

`-l:<exact filename>` (GNU ld's exact-name link syntax) links directly
against the real, version-matched `.so` file already extracted into
`$LOCAL_DIR` by the PR #41 step — not the apt-provided
`firebird-dev` package's generic `-lfbclient`, which would silently
relink against the wrong (mismatched) client version and reintroduce
the exact wire-protocol bug PR #41 fixed. At runtime, `LD_LIBRARY_PATH`
(already exported to the whole job via `$GITHUB_ENV` by the same PR #41
step) resolves the binary's dependency to the same version-matched copy
automatically — no new environment wiring needed.

`mkblob_fixture` then runs against the containerized database over TCP
(`localhost:3050/var/lib/firebird/data/test.fdb`), identical to its
local Windows invocation, populating `TBLOB_MULTISEG`'s `NOTE`/`DATA`
columns via real `isc_put_segment` calls.

This closes the one piece of the canonical fixture that plain SQL can't
reproduce (per the BLOB Lossless Fix branch's own finding: a SQL literal
INSERT never creates a genuinely multi-segment BLOB, regardless of
declared `SEGMENT SIZE`).

### 3. Drift guard: `scripts/check_no_inline_fixture_drift.sh`

After (1) and (2), the workflow should contain no fixture DDL text of
its own at all. A new script greps
`.github/workflows/build-linux-fb-matrix.yml` for fixture-DDL keywords
(`CREATE TABLE`, `CREATE OR ALTER VIEW`, `CREATE VIEW`, `CREATE INDEX`,
`ALTER TABLE`, `COMMENT ON`, `CREATE DOMAIN`, `CREATE GENERATOR`) and
fails with a clear message if any are found outside the two explicitly
allowed lines that reference the canonical files by name (`docker cp
scripts/fixture_common.sql ...` / `docker cp scripts/fixture_biz4.sql
...`):

```text
Fixture SQL must live in scripts/fixture_common.sql or
scripts/fixture_biz4.sql, not inline workflow YAML.
```

Run as a cheap, early CI step (pure `grep`, no build) in the Linux
workflow itself, as its own standalone step (not folded into
`scripts/build_matrix.ps1`'s own console output, to keep that script's
existing summary table noise-free) — callable manually and independently
for local verification before pushing. This turns a future silent
reintroduction of inline fixture DDL into an immediate, loud failure
instead of another incident discovered via red CI on an unrelated PR.

## Validation

- `scripts/setup_test_firebird.sh` run locally against the real test DB,
  full existing local test suite green, no change in fixture content
  (same tables/views/rows — this is a mechanical extraction, not a
  fixture content change).
- Cross-version matrix (DuckDB v1.5.2/v1.5.3/v1.5.4) green, same as
  every prior branch.
- Linux FB3/FB4/FB5 matrix workflow green, on the now-unified fixture —
  confirms `mkblob_fixture` compiles and runs correctly against all
  three real server versions over the version-matched client.
- `scripts/check_no_inline_fixture_drift.sh` passes against the final
  workflow file, and is verified to actually fail (by temporarily
  reintroducing a stray `CREATE TABLE` in a throwaway copy) before being
  relied on as a real guard.

## Out of scope

- No change to fixture *content* — this is a mechanical de-duplication,
  not a fixture redesign.
- No change to which test files the Linux workflow's "Run SQL test
  suite" step actually runs (that's issue #31, a separate, already-
  tracked follow-up).
- No change to `fixture_biz4.sql` or its own workflow handling.
- No changes to `duckdb/community-extensions` or any upstream repo.
