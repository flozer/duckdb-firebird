# DECFLOAT CI Fixture (#48) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load `scripts/fixture_decfloat.sql` (table `DECVALS`) into the existing FB4+ `biz4.fdb` database in the Linux FB matrix workflow, export `FIREBIRD_DECFLOAT_DB` pointing at that same database, and run `test/sql/firebird_decfloat.test` in the existing "FB4+ live type smoke" step so it executes for real on FB4/FB5, and is absent from FB3 because of the step's version condition, not a missing env var.

**Architecture:** Two small additions to `.github/workflows/build-linux-fb-matrix.yml`: one `docker cp` + `isql -i` line in the existing FB4+-gated fixture block (loading a second table into the already-provisioned `biz4.fdb`), and one env-var + one test-invocation line in the existing "FB4+ live type smoke" step. No new database, no new workflow step, no fixture-content change.

**Tech Stack:** GitHub Actions (`ubuntu-24.04` runner), Docker (`firebirdsql/firebird:{4,5}-noble`), Firebird `isql`, DuckDB's `unittest` sqllogictest runner.

## Global Constraints

- Classification: CI type/compatibility coverage within the Production Stability Gate, not a new product feature. `firebird_decfloat.test` exercises existing, already-shipped DECFLOAT fallback behavior — no change to `src/` is in scope unless the real CI run surfaces a genuine, previously-hidden bug, and if it does, that becomes its own separately scoped fix, not folded into this task.
- No new `CREATE DATABASE` — `DECVALS` loads into the existing `biz4.fdb`.
- No inline SQL — `scripts/fixture_decfloat.sql` is copied in and applied via `isql -i`, exactly like every other canonical fixture file.
- `FIREBIRD_DECFLOAT_DB` must be the exact same connection URL as `FIREBIRD_BIZ4_DB` (same host, port, path, charset) — both point at `biz4.fdb`.
- `firebird_decfloat.test` runs only in the "FB4+ live type smoke" step (`if: matrix.fb_major != '3'`) — never added to the FB3-inclusive "Run SQL test suite" step.
- No change to `scripts/fixture_biz4.sql`, `scripts/fixture_decfloat.sql`, `scripts/fixture_common.sql`, or `scripts/fixture_none_charset.sql` content.
- No change to `scripts/check_no_inline_fixture_drift.sh` — the new `docker cp`/`isql -i` lines contain no DDL/DML keyword themselves, so the guard needs no update; confirmed by running it after the edit (Step 3 below).
- No changes to `duckdb/community-extensions` or any upstream repo.

---

### Task 1: Load DECVALS into biz4.fdb and wire firebird_decfloat.test into the FB4+ step

**Files:**
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (two edits: the "Provision FB${{ matrix.fb_major }} fixtures" step's FB4+ block, and the "FB4+ live type smoke" step)

**Interfaces:**
- Consumes: `scripts/fixture_decfloat.sql` (already exists, unmodified — creates table `DECVALS`, no name conflict with `fixture_biz4.sql`'s `FB4_TYPES`/`DEPARTMENT`/`CUSTOMER`, confirmed by reading both files in full during the design spec). Consumes the existing `$GITHUB_ENV`-independent, step-scoped `env:` pattern the "FB4+ live type smoke" step already uses for `FIREBIRD_BIZ4_DB`.
- Produces: nothing consumed by a later task — this is the only task in this plan.

- [ ] **Step 1: Confirm the workflow file's current state matches this plan's assumptions**

```bash
grep -n "fixture_biz4\|FIREBIRD_BIZ4_DB\|FB4+ live type smoke\|firebird_type_audit_fb4.test\|matrix.fb_major != '3'" .github/workflows/build-linux-fb-matrix.yml
```

Expected (confirmed while writing this plan, reading the file directly): the FB4+ fixture block is at lines 101-106 (`if [ "${{ matrix.fb_major }}" != "3" ]; then ... docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb ... fi`). The "FB4+ live type smoke" step starts at line 247, with `if: matrix.fb_major != '3'` (line 248), its `env:` block at lines 249-252 (ending with the `FIREBIRD_BIZ4_DB` line), and `./build/release/test/unittest test/sql/firebird_type_audit_fb4.test` as the step's last `run:` line (line 264, per the file's full length of 268 lines). If the grep output differs from this (different line numbers, missing lines), the file has changed since this plan was written — stop and re-read the full file before editing.

- [ ] **Step 2: Add the DECVALS load and the FIREBIRD_DECFLOAT_DB/test wiring**

In `.github/workflows/build-linux-fb-matrix.yml`, the "Provision FB${{ matrix.fb_major }} fixtures" step's FB4+ block changes from:

```yaml
          if [ "${{ matrix.fb_major }}" != "3" ]; then
            docker cp scripts/fixture_biz4.sql fb:/tmp/fixture_biz4.sql
            printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/biz4.fdb' DEFAULT CHARACTER SET UTF8;" \
              | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb
          fi
```

to:

```yaml
          if [ "${{ matrix.fb_major }}" != "3" ]; then
            docker cp scripts/fixture_biz4.sql fb:/tmp/fixture_biz4.sql
            printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/biz4.fdb' DEFAULT CHARACTER SET UTF8;" \
              | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb

            # DECFLOAT fixture (issue #48) -- loads into the SAME biz4.fdb,
            # no new database, no table-name conflict with fixture_biz4.sql
            # (DECVALS vs FB4_TYPES/DEPARTMENT/CUSTOMER).
            docker cp scripts/fixture_decfloat.sql fb:/tmp/fixture_decfloat.sql
            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_decfloat.sql /var/lib/firebird/data/biz4.fdb
          fi
```

And the "FB4+ live type smoke (INT128 / DECIMAL(38) / TIMESTAMP_TZ)" step changes from:

```yaml
      - name: FB4+ live type smoke (INT128 / DECIMAL(38) / TIMESTAMP_TZ)
        if: matrix.fb_major != '3'
        env:
          ISC_USER: SYSDBA
          ISC_PASSWORD: masterkey
          FIREBIRD_BIZ4_DB: firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8
        run: |
          ./build/release/duckdb -unsigned <<'EOF'
          SELECT
              typeof(BIG_NUM)  AS t_big_num,
              typeof(BIG_DEC)  AS t_big_dec,
              typeof(TS_TZ)    AS t_ts_tz,
              typeof(T_TZ)     AS t_t_tz
            FROM firebird_scan(
                'firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8',
                'FB4_TYPES',
                partitions=1)
           LIMIT 1;
          EOF
          ./build/release/test/unittest test/sql/firebird_type_audit_fb4.test
```

to:

```yaml
      - name: FB4+ live type smoke (INT128 / DECIMAL(38) / TIMESTAMP_TZ)
        if: matrix.fb_major != '3'
        env:
          ISC_USER: SYSDBA
          ISC_PASSWORD: masterkey
          FIREBIRD_BIZ4_DB: firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8
          # Same database as FIREBIRD_BIZ4_DB (issue #48) -- DECVALS was
          # loaded into biz4.fdb above, not a separate database.
          FIREBIRD_DECFLOAT_DB: firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8
        run: |
          ./build/release/duckdb -unsigned <<'EOF'
          SELECT
              typeof(BIG_NUM)  AS t_big_num,
              typeof(BIG_DEC)  AS t_big_dec,
              typeof(TS_TZ)    AS t_ts_tz,
              typeof(T_TZ)     AS t_t_tz
            FROM firebird_scan(
                'firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/biz4.fdb?charset=UTF8',
                'FB4_TYPES',
                partitions=1)
           LIMIT 1;
          EOF
          ./build/release/test/unittest test/sql/firebird_type_audit_fb4.test
          ./build/release/test/unittest test/sql/firebird_decfloat.test
```

- [ ] **Step 3: Guard sanity check (no code change expected)**

```bash
bash scripts/check_no_inline_fixture_drift.sh
```

Expected: `OK: no inline fixture DDL/DML found in .github/workflows/build-linux-fb-matrix.yml`, exit code 0. The new `docker cp`/`isql -i` lines contain no DDL/DML keyword (they're invocation lines, not `CREATE TABLE`/`INSERT INTO`/etc. text), so this should pass unchanged. If it doesn't, STOP and re-read the diff — do not edit the guard to force a pass.

- [ ] **Step 4: Local sanity check with the existing built binary**

```bash
export ISC_USER=SYSDBA ISC_PASSWORD=masterkey FIREBIRD_DECFLOAT_DB='C:\fbtest\decfloat.fdb'
```

Run against the existing local build in `C:\tmp\fbwt-fixdrift` (a prior worktree from this same session with a working `build/release/test/unittest.exe`, and a local `C:\fbtest\decfloat.fdb` already provisioned per the project's standing local test setup) to confirm the test itself still passes against the canonical fixture content, unchanged by this branch:

```bash
cd /c/tmp/fbwt-fixdrift
build/release/test/unittest.exe "$(pwd)/test/sql/firebird_decfloat.test"
```

Expected: `All tests passed (N assertions in 1 test case)` for some N > 0. This does not validate the CI-specific `biz4.fdb`-piggyback wiring (only a real CI run does that, per this branch's constraints) — it only confirms the test file itself is not broken before pushing a workflow change that depends on it.

- [ ] **Step 5: Commit, push, open PR, and watch the real CI run**

```bash
git add .github/workflows/build-linux-fb-matrix.yml
git commit -m "$(cat <<'EOF'
fix(ci): run firebird_decfloat.test on FB4/FB5 via biz4.fdb (#48)

Loads scripts/fixture_decfloat.sql (table DECVALS, no name conflict
with fixture_biz4.sql's FB4_TYPES/DEPARTMENT/CUSTOMER) into the
already-provisioned, FB4+-gated biz4.fdb database, instead of creating
a new database file. FIREBIRD_DECFLOAT_DB points at the same URL as
FIREBIRD_BIZ4_DB. firebird_decfloat.test runs in the existing "FB4+
live type smoke" step (if: matrix.fb_major != '3'), so it's absent
from the FB3 job because of that version condition -- not a missing
env var, the same distinction firebird_type_audit_fb4.test already
has today.

CI type/compatibility coverage for existing, already-shipped DECFLOAT
fallback behavior -- not a product change. No src/ change expected
unless this run surfaces a genuine bug, which would be its own,
separately scoped fix.
EOF
)"
git push -u origin fix/48-decfloat-ci-fixture
gh pr create --repo flozer/duckdb-firebird --draft --base main --head fix/48-decfloat-ci-fixture \
  --title "fix(ci): #48 -- run firebird_decfloat.test on FB4/FB5" \
  --body "Closes #48. See docs/superpowers/specs/2026-07-19-decfloat-ci-fixture-design.md."
gh run list --repo flozer/duckdb-firebird --branch fix/48-decfloat-ci-fixture --limit 5
```

Wait for the "Build + Test Linux x64 (FB 3/4/5 matrix)" run (`gh run watch <run-id> --repo flozer/duckdb-firebird --exit-status`). After it completes, fetch the fb4 and fb5 jobs' "FB4+ live type smoke" step logs (`gh run view --repo flozer/duckdb-firebird --job <job-id> --log`) and confirm both show `All tests passed (N assertions in 1 test case)` for `firebird_decfloat.test`, with `N > 0` — not `All tests were skipped`, and not absent. Separately confirm the fb3 job's job summary shows the "FB4+ live type smoke" step itself skipped (the `-` marker GitHub Actions already uses for `if:`-condition skips, matching how `firebird_type_audit_fb4.test` already appears on fb3 today) — this is the proof that FB3's absence is a version-condition skip, not a silent env-based one.

---

## After Task 1

Single-task plan — no further tasks. No cross-version DuckDB matrix run needed (per the spec: no product code or fixture content touched — this is a CI-workflow-only change with no DuckDB-version sensitivity). The Linux FB3/4/5 matrix's own green run (this task's Step 5) is the complete validation for this branch.
