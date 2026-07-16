# CI NONE-charset fixture (#26) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provision a `CHARACTER SET NONE` Firebird database in the Linux FB3/4/5 matrix workflow, export it as `FIREBIRD_NONE_DB`, and wire `firebird_none_charset.test`, `firebird_explain_pushdown.test`, and `firebird_type_audit.test` into the "Run SQL test suite" step so all three run for real in CI instead of local-only (or, for the latter two, fully skipped).

**Architecture:** One new provisioning block in the existing "Provision FB${{ matrix.fb_major }} fixtures" step, mirroring the already-proven `fixture_biz4.sql` pattern (`CREATE DATABASE` via a piped `isql` call, then `docker cp` + `isql -i` for the fixture content) but unconditional across all three matrix legs, since `CHARACTER SET NONE` is not version-gated. Three new `unittest` invocations added to the existing "Run SQL test suite" step.

**Tech Stack:** GitHub Actions (`ubuntu-24.04` runner), Docker (`firebirdsql/firebird:{3,4,5}-noble`), Firebird `isql`, DuckDB's `unittest` sqllogictest runner.

## Global Constraints

- No change to `scripts/fixture_common.sql`, `scripts/fixture_biz4.sql`, `test.fdb`, or `biz4.fdb`.
- No change to `scripts/check_no_inline_fixture_drift.sh`'s own logic (its two known coverage gaps are tracked as issue #47, out of scope here).
- The CI connection URL for the new database MUST use `?charset=NONE` — never `WIN1252`/`ISO8859_1`/any other value; `ValidateClientCharset` (`src/firebird_client.cpp:159`) only accepts `UTF8`, `UTF-8`, `NONE`, or `OCTETS`, and only `NONE` preserves the raw, untransliterated bytes `none_encoding` needs to decode correctly.
- `firebird_decfloat.test` / a DECFLOAT CI fixture is out of scope — tracked separately as issue #48.
- No changes to `duckdb/community-extensions` or any upstream repo.
- No local Docker used to validate this workflow change — every claim about the workflow's behavior must be confirmed via a real GitHub Actions run (`gh run watch <id> --exit-status`), read directly from the log, never assumed from a subagent's self-report.
- The acceptance bar for "this test now runs in CI" is a **nonzero, "All tests passed" assertion count** in the CI log for each of the three newly-wired files — not merely a green step. A file reporting "All tests were skipped" must be treated as a failed validation, not a pass (this is exactly the failure mode #31 found and excluded these files for).

---

### Task 1: Provision `FIREBIRD_NONE_DB` and wire the three NONE-dependent test files

**Files:**
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (two edits: the "Provision FB${{ matrix.fb_major }} fixtures" step, and the "Run SQL test suite" step)

**Interfaces:**
- Consumes: `scripts/fixture_none_charset.sql` (already exists, unmodified — verified in the design spec to build all non-ASCII bytes via `ASCII_CHAR(0x..)`, safe to run through `isql` in any locale). Consumes the existing `$GITHUB_ENV` mechanism the `fixture_biz4.sql` block already uses to export a database URL (`FIREBIRD_TEST_DB`, `FIREBIRD_BIZ4_DB` are the precedents — this task adds `FIREBIRD_NONE_DB` the same way).
- Produces: nothing consumed by a later task — this is the only task in this plan.

- [ ] **Step 1: Confirm the workflow file's current state matches this plan's assumptions**

```bash
grep -n "Provision FB\|Run SQL test suite\|fixture_biz4\|FIREBIRD_TEST_DB=\|firebird_index_profile.test\|firebird_profile_table.test\|NOT listed here" .github/workflows/build-linux-fb-matrix.yml
```

Expected (confirmed while writing this plan, reading the file directly): the "Provision FB${{ matrix.fb_major }} fixtures" step runs from line 90 to line 109, with the `fixture_biz4.sql` block at lines 101-106 (gated `if [ "${{ matrix.fb_major }}" != "3" ]`) and the `FIREBIRD_TEST_DB` `$GITHUB_ENV` export as the step's last line (109). The "Run SQL test suite" step runs from line 202 to line 229, ending with a comment block (lines 222-229) explaining why `firebird_explain_pushdown.test`/`firebird_type_audit.test` are excluded pending issue #26. If the grep output differs from this (extra/missing lines, different line numbers), the file has changed since this plan was written — stop and re-read the full file before proceeding rather than guessing at edit locations.

- [ ] **Step 2: Add the NONE-charset provisioning block**

In `.github/workflows/build-linux-fb-matrix.yml`, within the "Provision FB${{ matrix.fb_major }} fixtures" step, insert a new block after the existing `fixture_biz4.sql` conditional block (after line 106's `fi`) and before the `FIREBIRD_TEST_DB` export (line 108's comment). The step's `run:` block changes from:

```yaml
          if [ "${{ matrix.fb_major }}" != "3" ]; then
            docker cp scripts/fixture_biz4.sql fb:/tmp/fixture_biz4.sql
            printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/biz4.fdb' DEFAULT CHARACTER SET UTF8;" \
              | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb
          fi

          # Suite tests connect over TCP — expose the URL form.
          echo "FIREBIRD_TEST_DB=firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/test.fdb?charset=UTF8" >> $GITHUB_ENV
```

to:

```yaml
          if [ "${{ matrix.fb_major }}" != "3" ]; then
            docker cp scripts/fixture_biz4.sql fb:/tmp/fixture_biz4.sql
            printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/biz4.fdb' DEFAULT CHARACTER SET UTF8;" \
              | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb
          fi

          # CHARACTER SET NONE fixture (issue #26) -- exercises the
          # none_encoding feature. Unlike fixture_biz4.sql above, this runs
          # on all three matrix legs (CHARACTER SET NONE is not FB4+-only).
          docker cp scripts/fixture_none_charset.sql fb:/tmp/fixture_none_charset.sql
          printf "%s\n" "CREATE DATABASE '/var/lib/firebird/data/none.fdb' DEFAULT CHARACTER SET NONE;" \
            | docker exec -i fb bash -c "isql -u SYSDBA -p masterkey"
          docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_none_charset.sql /var/lib/firebird/data/none.fdb

          # Suite tests connect over TCP — expose the URL form.
          echo "FIREBIRD_TEST_DB=firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/test.fdb?charset=UTF8" >> $GITHUB_ENV
          # charset=NONE (not UTF8/WIN1252/etc) -- ValidateClientCharset
          # (src/firebird_client.cpp) only accepts UTF8/NONE/OCTETS, and
          # only NONE preserves the raw, untransliterated bytes
          # none_encoding decodes client-side. This mirrors the local
          # setup, where FIREBIRD_NONE_DB is a bare path with no charset
          # param at all -- that leaves info.charset empty, so the
          # extension never sends isc_dpb_lc_ctype and Firebird defaults
          # the connection to NONE. ?charset=NONE here makes that same
          # behavior explicit.
          echo "FIREBIRD_NONE_DB=firebird://SYSDBA:masterkey@localhost:3050/var/lib/firebird/data/none.fdb?charset=NONE" >> $GITHUB_ENV
```

- [ ] **Step 3: Wire the three test files into "Run SQL test suite"**

In the same workflow file, the "Run SQL test suite" step's `run:` block changes from:

```yaml
      - name: Run SQL test suite
        env:
          ISC_USER: SYSDBA
          ISC_PASSWORD: masterkey
        run: |
          ./build/release/test/unittest test/sql/firebird_scan.test
          ./build/release/test/unittest test/sql/firebird_attach.test
          ./build/release/test/unittest test/sql/firebird_health.test
          ./build/release/test/unittest test/sql/firebird_index_profile.test
          ./build/release/test/unittest test/sql/firebird_bind_params.test
          ./build/release/test/unittest test/sql/firebird_blob_lossless.test
          ./build/release/test/unittest test/sql/firebird_dbt_sources.test
          ./build/release/test/unittest test/sql/firebird_metadata.test
          ./build/release/test/unittest test/sql/firebird_metadata_bridge.test
          ./build/release/test/unittest test/sql/firebird_observability.test
          ./build/release/test/unittest test/sql/firebird_paging.test
          ./build/release/test/unittest test/sql/firebird_pool.test
          ./build/release/test/unittest test/sql/firebird_pool_stats.test
          ./build/release/test/unittest test/sql/firebird_predicates.test
          ./build/release/test/unittest test/sql/firebird_profile_table.test
          # firebird_explain_pushdown.test and firebird_type_audit.test are
          # NOT listed here: both gate their entire file on
          # `require-env FIREBIRD_NONE_DB` (confirmed via a local run with
          # that var unset -- DuckDB's sqllogictest runner skips the WHOLE
          # file, not just the NONE-specific block), which this workflow
          # does not provision (issue #26). Adding them here would just
          # report "0 assertions, skipped" -- not real coverage. Revisit
          # once #26 lands.
```

to:

```yaml
      - name: Run SQL test suite
        env:
          ISC_USER: SYSDBA
          ISC_PASSWORD: masterkey
        run: |
          ./build/release/test/unittest test/sql/firebird_scan.test
          ./build/release/test/unittest test/sql/firebird_attach.test
          ./build/release/test/unittest test/sql/firebird_health.test
          ./build/release/test/unittest test/sql/firebird_index_profile.test
          ./build/release/test/unittest test/sql/firebird_bind_params.test
          ./build/release/test/unittest test/sql/firebird_blob_lossless.test
          ./build/release/test/unittest test/sql/firebird_dbt_sources.test
          ./build/release/test/unittest test/sql/firebird_metadata.test
          ./build/release/test/unittest test/sql/firebird_metadata_bridge.test
          ./build/release/test/unittest test/sql/firebird_observability.test
          ./build/release/test/unittest test/sql/firebird_paging.test
          ./build/release/test/unittest test/sql/firebird_pool.test
          ./build/release/test/unittest test/sql/firebird_pool_stats.test
          ./build/release/test/unittest test/sql/firebird_predicates.test
          ./build/release/test/unittest test/sql/firebird_profile_table.test
          # firebird_explain_pushdown.test and firebird_type_audit.test
          # gate their entire file on `require-env FIREBIRD_NONE_DB` --
          # now provisioned above (issue #26), so both run for real here.
          ./build/release/test/unittest test/sql/firebird_none_charset.test
          ./build/release/test/unittest test/sql/firebird_explain_pushdown.test
          ./build/release/test/unittest test/sql/firebird_type_audit.test
```

- [ ] **Step 4: Guard sanity check (no code change expected)**

```bash
bash scripts/check_no_inline_fixture_drift.sh
```

Expected: `OK: no inline fixture DDL found in .github/workflows/build-linux-fb-matrix.yml`, exit code 0. The new `CREATE DATABASE '/var/lib/firebird/data/none.fdb' DEFAULT CHARACTER SET NONE;` line must NOT trip the guard — `CREATE DATABASE` is deliberately absent from the guard's `KEYWORDS` list (confirmed in the design spec: it's bootstrap, not fixture DDL), and this line is a single quoted string inside a `printf` invocation, not a `CREATE TABLE`/`CREATE OR ALTER VIEW`/etc. line. If this step reports drift, STOP — something about the new block doesn't match the plan's exact text above; do not edit the guard script to make it pass.

- [ ] **Step 5: Push and watch the real CI run**

```bash
git add .github/workflows/build-linux-fb-matrix.yml
git commit -m "fix(ci): provision FIREBIRD_NONE_DB in Linux FB matrix (#26)

Adds a CHARACTER SET NONE database (scripts/fixture_none_charset.sql,
unchanged) to the Provision fixtures step, unconditional across all
three FB3/4/5 legs (unlike fixture_biz4.sql, this feature is not
FB4+-only). Connection URL uses ?charset=NONE -- ValidateClientCharset
(src/firebird_client.cpp) only accepts UTF8/NONE/OCTETS, and only NONE
preserves the raw bytes none_encoding decodes client-side.

Wires firebird_none_charset.test into the Run SQL test suite step, and
also firebird_explain_pushdown.test/firebird_type_audit.test: both were
excluded in #31 because each gates its ENTIRE file on
require-env FIREBIRD_NONE_DB, not just a block -- confirmed empirically
(both report 'All tests were skipped' with the var unset). Provisioning
it here closes that debt at zero extra fixture cost.

firebird_decfloat.test / a DECFLOAT CI fixture stays out of scope --
tracked separately as issue #48."
git push -u origin fix/26-ci-none-charset-fixture
gh pr create --repo flozer/duckdb-firebird --draft --base main --head fix/26-ci-none-charset-fixture \
  --title "fix(ci): #26 -- provision FIREBIRD_NONE_DB in Linux FB matrix" \
  --body "Closes #26. See docs/superpowers/specs/2026-07-16-ci-none-charset-fixture-design.md."
gh run list --repo flozer/duckdb-firebird --branch fix/26-ci-none-charset-fixture --limit 5
```

Wait for the "Build + Test Linux x64 (FB 3/4/5 matrix)" run (`gh run watch <run-id> --repo flozer/duckdb-firebird --exit-status`). Do not just check for a green checkmark: after the run completes, fetch the "Run SQL test suite" step's log text (`gh run view --repo flozer/duckdb-firebird --job <job-id> --log`) and confirm, for each of the three newly-added files, an `All tests passed (N assertions in 1 test case)` line with `N > 0` — not `All tests were skipped`. A skipped file with 0 assertions is a failed validation per this plan's Global Constraints, even if the overall step and job report green (an `unittest` invocation that itself errors would fail the step; a silently-skipped file inside a passing step would not, which is exactly the gap this validation step exists to catch).

---

## After Task 1

This is a single-task plan — no further tasks. Per the branch's standing gates (matching every prior Production Stability Gate branch): run the cross-version matrix (`scripts/build_matrix.ps1`, sed-copied to point at this worktree, never at the dirty principal worktree) before converting the PR to ready. The local matrix already exercises all three newly-wired files today (with `FIREBIRD_NONE_DB` set locally) — this run's purpose is confirming the branch as a whole (not just this one workflow file) still builds and passes across DuckDB v1.5.2/v1.5.3/v1.5.4, matching prior branches' practice, not to re-validate the CI-specific fixture wiring (only a real GitHub Actions run does that, per this branch's own Global Constraints).
