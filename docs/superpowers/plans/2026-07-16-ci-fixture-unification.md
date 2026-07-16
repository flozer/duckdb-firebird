# CI Fixture Unification (#39) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the hand-duplicated Firebird fixture DDL between `scripts/setup_test_firebird.sh` and `.github/workflows/build-linux-fb-matrix.yml` by extracting it to one shared SQL file both consume, compile the BLOB-fixture helper on the CI runner instead of inside the minimal Firebird container, and add a permanent grep-based guard against future re-drift.

**Architecture:** One new canonical asset (`scripts/fixture_common.sql`) replaces two independently-maintained copies of the same DDL. The Linux CI workflow gains one new step (compile+run `mkblob_fixture` on the runner, reusing the already-extracted version-matched `libfbclient` from PR #41) and one new cheap guard step (grep for reintroduced inline DDL).

**Tech Stack:** bash, Firebird `isql`, GitHub Actions YAML, C++ (`mkblob_fixture.cpp`, already exists), `grep`.

## Global Constraints

- No fixture *content* changes — this is a mechanical de-duplication, not a fixture redesign.
- `scripts/fixture_biz4.sql` stays separate and untouched, exactly as it is today.
- No change to which test files the Linux workflow's "Run SQL test suite" step runs (that's issue #31, separate).
- `mkblob_fixture` must link against the version-matched `libfbclient` already extracted per-job by the existing "Extract matching libfbclient from the fb${{ matrix.fb_major }} container" workflow step (PR #41) — via `-l:<exact filename>`, never the apt-provided generic `-lfbclient`, which would reintroduce the exact client/server wire-protocol mismatch PR #41 fixed.
- No changes to `duckdb/community-extensions` or any upstream repo.
- No local Docker use for validation — every workflow-file change in this plan is verified by pushing to the branch and reading the real GitHub Actions run (`gh run watch` / `gh api .../logs`), matching how every prior CI-touching branch this cycle was validated.

---

### Task 1: Extract `scripts/fixture_common.sql`, point `setup_test_firebird.sh` at it

**Files:**
- Create: `scripts/fixture_common.sql`
- Modify: `scripts/setup_test_firebird.sh:52-258`

**Interfaces:**
- Produces: `scripts/fixture_common.sql` — a plain Firebird DSQL script (no bash, no `isql` invocation wrapper), consumed by both this task (local script) and Task 2 (CI workflow) via `isql ... -i <file>`.

- [ ] **Step 1: Confirm the exact current heredoc boundaries**

`scripts/setup_test_firebird.sh`'s fixture heredoc currently runs from the line right after `"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" <<'EOF'` (line 52) through the line right before the matching closing `EOF` (line 258) — i.e. content lines 53-257. Verify this hasn't shifted before extracting:

```bash
grep -n "<<'EOF'\|^EOF$\|^\"\$ISQL\"" scripts/setup_test_firebird.sh
```

Expected output includes exactly these four lines (line numbers may have shifted slightly if this file changed since this plan was written — if so, use the CURRENT line numbers for every step below instead of 52/258):
```
48:"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" <<EOF
50:EOF
52:"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" <<'EOF'
258:EOF
```
(A third, unrelated `EOF` further down, around line 288, closes a `GITHUB_ENV`-emitting heredoc — leave that alone entirely, it isn't part of the fixture.)

- [ ] **Step 2: Extract the fixture DDL verbatim**

```bash
sed -n '53,257p' scripts/setup_test_firebird.sh > scripts/fixture_common.sql
```

Verify the extraction captured exactly the fixture content, nothing from the surrounding bash:

```bash
head -3 scripts/fixture_common.sql
tail -3 scripts/fixture_common.sql
wc -l scripts/fixture_common.sql
```

Expected: first line is `CREATE TABLE EMPLOYEE (`, last line is `COMMIT;`, line count matches `257 - 53 + 1 = 205`.

- [ ] **Step 3: Replace the heredoc invocation in `setup_test_firebird.sh`**

Replace this (currently lines 52-258 — the opening invocation line, the entire extracted body, and the closing `EOF`):
```bash
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" <<'EOF'
[... 205 lines of DDL, now living in scripts/fixture_common.sql ...]
EOF
```
with:
```bash
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" -i scripts/fixture_common.sql
```

- [ ] **Step 4: Run the updated script against the real local test DB**

```bash
export ISC_USER=SYSDBA ISC_PASSWORD=masterkey FIREBIRD_TEST_DB="C:\fbtest\test.fdb" FB_SDK_ROOT="C:/Program Files/Firebird/Firebird_5_0"
bash scripts/setup_test_firebird.sh
```
Expected: completes with no error, same tail output as before this change (`NOTE: wrote 51 segments...` / `DATA: wrote 51 segments...` / `OK`), since `fixture_common.sql`'s content is byte-identical to what the heredoc used to contain.

- [ ] **Step 5: Run the full local test suite to confirm zero fixture-content drift**

```bash
export ISC_USER=SYSDBA ISC_PASSWORD=masterkey FIREBIRD_TEST_DB="C:\fbtest\test.fdb" FIREBIRD_NONE_DB="C:\fbtest\none.fdb" FIREBIRD_DECFLOAT_DB="C:\fbtest\decfloat.fdb"
for f in test/sql/firebird_*.test; do
  build/release/test/unittest.exe "$(pwd)/$f" 2>&1 | tail -3
done
```
Expected: every file reports `All tests passed`, with the SAME assertion counts as the pre-change baseline (this is a pure mechanical extraction — if any count differs, the extraction dropped or duplicated a line; stop and fix before proceeding, do not adjust a test's expected count to match).

- [ ] **Step 6: Commit**

```bash
git add scripts/fixture_common.sql scripts/setup_test_firebird.sh
git commit -m "refactor(ci-fixture): extract scripts/fixture_common.sql (#39)

scripts/setup_test_firebird.sh's fixture DDL now lives in its own file
instead of an inline heredoc, so the Linux CI workflow (next commit) can
consume the exact same content instead of maintaining a hand-duplicated
copy that has already drifted twice this cycle. Pure extraction -- no
fixture content change; full local suite confirms identical assertion
counts."
```

---

### Task 2: Point the Linux workflow at `scripts/fixture_common.sql`

**Files:**
- Modify: `.github/workflows/build-linux-fb-matrix.yml:95-184`

**Interfaces:**
- Consumes: `scripts/fixture_common.sql` (Task 1).

- [ ] **Step 1: Confirm the exact current inline fixture boundaries**

```bash
grep -n "<<'SQL'\|^          SQL$\|fixture_biz4" .github/workflows/build-linux-fb-matrix.yml
```
Expected (line numbers may have shifted if this file changed since this plan was written — use current numbers if so):
```
92:          docker exec -i fb bash -c "isql -u SYSDBA -p masterkey" <<'SQL'
94:          SQL
95:          docker exec -i fb bash -c "isql -u SYSDBA -p masterkey /var/lib/firebird/data/test.fdb" <<'SQL'
184:          SQL
187:            docker cp scripts/fixture_biz4.sql fb:/tmp/fixture_biz4.sql
190:            docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb
```
The first `docker exec ... <<'SQL' ... SQL` pair (92-94, just the `CREATE DATABASE` statement) stays untouched — only the second one (95-184, the actual fixture DDL) is being replaced. The `fixture_biz4.sql` handling (187-190) stays untouched — it's the pattern this task's replacement copies.

- [ ] **Step 2: Diff the workflow's current inline fixture against the canonical content it's about to be replaced with**

```bash
sed -n '96,183p' .github/workflows/build-linux-fb-matrix.yml | sed 's/^          //' > /tmp/workflow_fixture_current.sql
diff /tmp/workflow_fixture_current.sql scripts/fixture_common.sql
```
Expected: the diff shows the workflow's copy is a *subset* of `fixture_common.sql` (missing `TBLOB_MULTISEG`, `TQUOTES`, `TPK_COMPOSITE`, `TCHILD`, `V_LITERAL_NOISE`, `V_MULTILINE_KW`, the domain, and the generator/comments, per the design spec) — no lines present in the workflow's copy that are ABSENT from `fixture_common.sql` (that would indicate CI-specific content this replacement would silently discard; if you see any such line, stop and report it rather than proceeding).

- [ ] **Step 3: Replace the inline heredoc with a `docker cp` + `docker exec -i` of the shared file**

Replace this (currently lines 95-184 — the opening invocation, the entire inline DDL body, and the closing `SQL`):
```bash
docker exec -i fb bash -c "isql -u SYSDBA -p masterkey /var/lib/firebird/data/test.fdb" <<'SQL'
[... inline DDL, now redundant with scripts/fixture_common.sql ...]
SQL
```
with:
```bash
docker cp scripts/fixture_common.sql fb:/tmp/fixture_common.sql
docker exec fb isql -u SYSDBA -p masterkey -i /tmp/fixture_common.sql /var/lib/firebird/data/test.fdb
```

- [ ] **Step 4: Push and watch the real CI run (no local Docker validation)**

```bash
git add .github/workflows/build-linux-fb-matrix.yml
git commit -m "fix(ci): Linux FB matrix uses shared scripts/fixture_common.sql (#39)

Replaces the hand-duplicated inline fixture heredoc with the same
docker cp + docker exec -i pattern already used for fixture_biz4.sql,
against the single canonical asset extracted in the prior commit. This
is what actually closes the drift: the workflow now provisions every
table/view/index scripts/setup_test_firebird.sh does, not a
hand-maintained subset."
git push
gh pr create --repo flozer/duckdb-firebird --draft --base main --head <this-branch> \
  --title "fix(ci): #39 -- unify Linux CI fixture with canonical script" \
  --body "WIP: unifying scripts/setup_test_firebird.sh and the Linux FB3/4/5 matrix workflow's fixture. See docs/superpowers/specs/2026-07-16-ci-fixture-unification-design.md."
gh run list --repo flozer/duckdb-firebird --branch <this-branch> --limit 3
```
Wait for the "Build + Test Linux x64 (FB 3/4/5 matrix)" run to complete (`gh run watch <run-id> --repo flozer/duckdb-firebird --exit-status`, ~15-20 minutes). Expected: all 3 jobs (`fb3`, `fb4`, `fb5`) pass their "Provision FB${{ matrix.fb_major }} fixtures" and "Run SQL test suite" steps — this task does NOT yet add `mkblob_fixture` (Task 3), so `TBLOB_MULTISEG` still won't exist in the container yet; that's fine, because (per the design spec) none of the 4 tests this workflow runs reference it. If any of the 4 existing test files now fail, something in the shared SQL differs from what those tests need — stop and diagnose before proceeding to Task 3.

---

### Task 3: Compile and run `mkblob_fixture` on the CI runner

**Files:**
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (insert a new step between the existing "Extract matching libfbclient from the fb${{ matrix.fb_major }} container" step and the existing "Pin DuckDB version" step)

**Interfaces:**
- Consumes: `scripts/mkblob_fixture.cpp` (already exists, unmodified), and the `$LOCAL_DIR`/`$LOCAL_LIB` shell variables set by the "Extract matching libfbclient" step (same job, same shell — GitHub Actions steps in one job share environment via `$GITHUB_ENV`, but `$LOCAL_DIR`/`$LOCAL_LIB` are plain shell variables set with `=`, not exported to `$GITHUB_ENV` — confirm in Step 1 below whether they need to be re-derived in this new step or are still in scope).

- [ ] **Step 1: Confirm the workflow file's current state matches this plan's assumptions**

```bash
grep -n "LOCAL_DIR\|LOCAL_LIB\|Extract matching libfbclient\|Pin DuckDB version\|DUCKDB_FIREBIRD_CLIENT_LIBRARY" .github/workflows/build-linux-fb-matrix.yml
```
Confirmed (verified directly in this file while writing this plan): `LOCAL_DIR`/`LOCAL_LIB` in the "Extract matching libfbclient from the fb${{ matrix.fb_major }} container" step are plain shell variables (`VAR=value`), set inside that step's own `run:` block — each GitHub Actions step is its own shell invocation, so plain shell variables never persist to the next step. That step DOES persist the resolved absolute library path via `echo "DUCKDB_FIREBIRD_CLIENT_LIBRARY=$LOCAL_LIB" >> $GITHUB_ENV` — the new step in this task reads `$DUCKDB_FIREBIRD_CLIENT_LIBRARY` (already in its environment, no re-derivation needed) rather than trying to reconstruct `$LOCAL_DIR`/`$LOCAL_LIB` by hand. If the grep output above shows this env var under a different name, or shows the "Extract matching libfbclient" step no longer ends with these two `>> $GITHUB_ENV` lines, the file has changed since this plan was written — stop and re-verify before proceeding rather than assuming.

- [ ] **Step 2: Insert the new step**

Insert this new step immediately after the "Extract matching libfbclient from the fb${{ matrix.fb_major }} container" step and before the "Pin DuckDB version" step:

```yaml
      - name: Build and run mkblob_fixture (issue #35 / #39)
        run: |
          # mkblob_fixture.cpp writes TBLOB_MULTISEG's genuinely
          # multi-segment BLOB content via real isc_put_segment calls --
          # a plain SQL literal can't do this (see the file's own header
          # comment), so it can't be folded into fixture_common.sql. The
          # firebirdsql/firebird:*-noble images have no C++ toolchain, so
          # this compiles on the runner instead, linking against the
          # SAME version-matched libfbclient the previous step already
          # extracted from this job's own container -- never the
          # apt-installed firebird-dev package's generic -lfbclient,
          # which would silently relink against a mismatched client
          # version and reintroduce the exact bug PR #41 fixed.
          LOCAL_LIB="$DUCKDB_FIREBIRD_CLIENT_LIBRARY"
          LOCAL_DIR="$(dirname "$LOCAL_LIB")"
          g++ -std=c++17 -I/usr/include/firebird \
              -o mkblob_fixture scripts/mkblob_fixture.cpp \
              -L"$LOCAL_DIR" -l:"$(basename "$LOCAL_LIB")"
          ./mkblob_fixture localhost/3050:/var/lib/firebird/data/test.fdb SYSDBA masterkey 1
```

- [ ] **Step 3: Push and watch the real CI run**

```bash
git add .github/workflows/build-linux-fb-matrix.yml
git commit -m "fix(ci): compile+run mkblob_fixture on the runner (#39)

Closes the last piece of the fixture drift: TBLOB_MULTISEG's
genuinely-multi-segment BLOB content, written via real isc_put_segment
calls (a plain SQL literal can't produce this -- see
scripts/mkblob_fixture.cpp's header comment), compiled on the runner
(the Firebird container images have no C++ toolchain) and linked
against the same version-matched libfbclient the prior step already
extracted, never the apt-installed generic one."
git push
gh run list --repo flozer/duckdb-firebird --branch <this-branch> --limit 3
```
Wait for the "Build + Test Linux x64 (FB 3/4/5 matrix)" run (`gh run watch <run-id> --repo flozer/duckdb-firebird --exit-status`). Expected: all 3 jobs pass, including the new "Build and run mkblob_fixture" step — `mkblob_fixture`'s own `check()` function calls `exit(1)` on any Firebird API error (including "table not found" or a connection failure), so a passing step is itself sufficient confirmation `TBLOB_MULTISEG` was populated correctly; no separate diagnostic query is needed. If the step fails, read the job log for `mkblob_fixture`'s own stderr output (it prints `<column>: wrote <N> segments, <M> bytes` per column on success, or a Firebird error message on failure) before assuming it's a linker/library issue versus a genuine connection/schema issue.

---

### Task 4: Drift guard — `scripts/check_no_inline_fixture_drift.sh`

**Files:**
- Create: `scripts/check_no_inline_fixture_drift.sh`
- Modify: `.github/workflows/build-linux-fb-matrix.yml` (insert a new, early step)

**Interfaces:**
- Produces: an executable script exit-coding 0 (clean) or 1 (drift detected, with a message on stderr) — consumed by the new CI step in this same task.

- [ ] **Step 1: Write the guard script**

```bash
cat > scripts/check_no_inline_fixture_drift.sh <<'SCRIPT'
#!/usr/bin/env bash
# Fails if .github/workflows/build-linux-fb-matrix.yml contains fixture
# DDL of its own instead of sourcing the canonical
# scripts/fixture_common.sql / scripts/fixture_biz4.sql (issue #39).
# Run locally before pushing a workflow change, and as an early,
# cheap (no build) CI step.
set -euo pipefail

WORKFLOW=".github/workflows/build-linux-fb-matrix.yml"
KEYWORDS='CREATE TABLE|CREATE OR ALTER VIEW|CREATE VIEW|CREATE INDEX|ALTER TABLE|COMMENT ON|CREATE DOMAIN|CREATE GENERATOR'

# Lines that reference the canonical fixture files by name are the
# intended, allowed way to bring DDL into the workflow -- exclude only
# those from the scan (grep -v), so any OTHER DDL-keyword line still
# fails the check.
matches=$(grep -inE "$KEYWORDS" "$WORKFLOW" | grep -v "fixture_common\.sql\|fixture_biz4\.sql" || true)

if [ -n "$matches" ]; then
    echo "Fixture SQL must live in scripts/fixture_common.sql or" >&2
    echo "scripts/fixture_biz4.sql, not inline workflow YAML." >&2
    echo "" >&2
    echo "Found in $WORKFLOW:" >&2
    echo "$matches" >&2
    exit 1
fi

echo "OK: no inline fixture DDL found in $WORKFLOW"
SCRIPT
chmod +x scripts/check_no_inline_fixture_drift.sh
```

- [ ] **Step 2: Run it against the current (already-fixed) workflow and confirm it passes**

```bash
bash scripts/check_no_inline_fixture_drift.sh
```
Expected: `OK: no inline fixture DDL found in .github/workflows/build-linux-fb-matrix.yml`, exit code 0.

- [ ] **Step 3: Prove it actually catches drift**

```bash
cp .github/workflows/build-linux-fb-matrix.yml /tmp/workflow_backup.yml
echo "          CREATE TABLE ZZZ_DRIFT_PROBE (ID INTEGER);" >> .github/workflows/build-linux-fb-matrix.yml
bash scripts/check_no_inline_fixture_drift.sh; echo "exit code: $?"
cp /tmp/workflow_backup.yml .github/workflows/build-linux-fb-matrix.yml
rm /tmp/workflow_backup.yml
```
Expected: the first run prints the "Fixture SQL must live in..." message with the `ZZZ_DRIFT_PROBE` line shown, `exit code: 1`. Confirm the workflow file is restored to its exact prior state afterward (`git diff .github/workflows/build-linux-fb-matrix.yml` shows no changes) before continuing.

- [ ] **Step 4: Wire it into the workflow as an early step**

Insert this as the FIRST step in the `build-test` job, before "Checkout" (so it runs even before checkout is needed for anything else — actually it needs the checked-out repo to grep the workflow file itself, so it must run AFTER "Checkout" but BEFORE any of the slower steps like "Install build deps" or starting the Firebird container):

```yaml
      - name: Guard against inline fixture DDL drift (#39)
        run: bash scripts/check_no_inline_fixture_drift.sh
```
placed immediately after the existing "Checkout" step.

- [ ] **Step 5: Push and watch the real CI run**

```bash
git add scripts/check_no_inline_fixture_drift.sh .github/workflows/build-linux-fb-matrix.yml
git commit -m "test(ci): add drift guard against inline fixture DDL (#39)

Cheap, no-build grep check: fails immediately if
.github/workflows/build-linux-fb-matrix.yml ever contains fixture DDL
of its own again, instead of sourcing scripts/fixture_common.sql or
scripts/fixture_biz4.sql. Verified it actually catches a deliberately-
reintroduced stray CREATE TABLE before relying on it."
git push
gh run list --repo flozer/duckdb-firebird --branch <this-branch> --limit 3
```
Wait for the run. Expected: the new "Guard against inline fixture DDL drift" step passes near-instantly (seconds, not minutes -- it's a single `grep`), and the rest of the job proceeds and passes exactly as it did at the end of Task 3.

---

## After Task 4

Per the branch's standing gates (matching every prior v1.0/production-stability branch): run the cross-version matrix (`scripts/build_matrix.ps1`, sed-copied to point at this worktree) before opening the PR for real review. The Linux FB3/4/5 matrix's own green run (confirmed task-by-task above) is this branch's primary validation, since the change is CI-infrastructure-shaped rather than product-code-shaped — there is no separate "maturity battery against the real production DB" angle for this branch (nothing here touches data-fetching/type-reconciliation code paths), so that gate does not apply.
