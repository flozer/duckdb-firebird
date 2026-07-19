# Anti-drift Guard Hardening (#47) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden `scripts/check_no_inline_fixture_drift.sh` (the #39 anti-drift guard) against its three known gaps — false positives, false negatives, and a silent missing-file case — without touching any fixture content or extension behavior.

**Architecture:** Single-file rewrite of the guard script's keyword list and matching logic, plus a file-existence check. No new files, no workflow-step restructuring.

**Tech Stack:** Bash, `grep -E`.

## Global Constraints

- Classification: CI hygiene / maintenance chore, not a product or roadmap front. The extension stays strictly read-only; every keyword this guard matches is blocked *workflow YAML text*, not extension runtime behavior.
- No change to `scripts/fixture_common.sql`, `scripts/fixture_biz4.sql`, or `scripts/fixture_none_charset.sql` content.
- No change to fixture provisioning logic, test wiring, or any product code (`src/`).
- No DECFLOAT CI work (issue #48, separate front) — do not touch it here.
- No changes to `duckdb/community-extensions` or any upstream repo.
- Small PR: only `scripts/check_no_inline_fixture_drift.sh` changes in this plan.

---

### Task 1: Rewrite the guard's keyword matching, pre-filter, and missing-file check

**Files:**
- Modify: `scripts/check_no_inline_fixture_drift.sh` (full rewrite of the body; the shebang and header comment lines 1-6 stay)

**Interfaces:**
- Consumes: nothing (standalone script, no other task in this plan).
- Produces: nothing consumed elsewhere — this is the only task in this plan. The script's exit-code contract (0 = clean, 1 = drift/error) is unchanged, so the existing CI step (`.github/workflows/build-linux-fb-matrix.yml`'s "Guard against inline fixture DDL drift (#39)" step, `run: bash scripts/check_no_inline_fixture_drift.sh`) needs no edit.

- [ ] **Step 1: Read the current script and confirm line numbers match this plan's assumptions**

```bash
cat -n scripts/check_no_inline_fixture_drift.sh
```

Expected (confirmed while writing this plan): 28 lines total. Line 9: `WORKFLOW=".github/workflows/build-linux-fb-matrix.yml"`. Line 10: `KEYWORDS='CREATE TABLE|CREATE OR ALTER VIEW|CREATE VIEW|CREATE INDEX|ALTER TABLE|COMMENT ON|CREATE DOMAIN|CREATE GENERATOR'`. Line 16: `matches=$(grep -inE "$KEYWORDS" "$WORKFLOW" | grep -v "fixture_common\.sql\|fixture_biz4\.sql" || true)`. Lines 18-25: the `if [ -n "$matches" ]` block with the error message. If the file differs from this, stop and re-read before editing — don't assume line numbers.

- [ ] **Step 2: Replace the script body**

Replace the entire content of `scripts/check_no_inline_fixture_drift.sh` with:

```bash
#!/usr/bin/env bash
# Fails if .github/workflows/build-linux-fb-matrix.yml contains fixture
# DDL/DML of its own instead of sourcing one of the canonical fixture
# files (scripts/fixture_common.sql, scripts/fixture_biz4.sql,
# scripts/fixture_none_charset.sql -- issues #39/#26). Run locally before
# pushing a workflow change, and as an early, cheap (no build) CI step.
#
# This is a CI-hygiene guard, not an extension-behavior check: every
# keyword below is a pattern blocked from workflow YAML text so fixture
# DDL/DML stays isolated in test-setup fixture files. The extension
# itself remains strictly read-only regardless of what this script does.
set -euo pipefail

WORKFLOW=".github/workflows/build-linux-fb-matrix.yml"

[ -f "$WORKFLOW" ] || { echo "guard: $WORKFLOW not found" >&2; exit 1; }

# Extended-regex alternation covering fixture DDL/DML keywords, using
# [[:space:]]+ instead of a literal space so multi-space-separated
# keywords still match. Built incrementally (rather than one long
# literal) so each addition's purpose stays legible:
#   - CREATE TABLE, including "CREATE GLOBAL TEMPORARY TABLE"
#   - RECREATE TABLE/VIEW/PROCEDURE/TRIGGER/PACKAGE
#   - CREATE [OR ALTER] VIEW
#   - CREATE [UNIQUE] [ASC[ENDING]|DESC[ENDING]] INDEX -- "UNIQUE"
#     previously broke the old literal "CREATE INDEX" substring match
#   - ALTER TABLE
#   - COMMENT ON
#   - CREATE DOMAIN
#   - CREATE SEQUENCE / CREATE GENERATOR
#   - CREATE [OR ALTER] PROCEDURE/TRIGGER/PACKAGE
#   - INSERT INTO
KEYWORDS='CREATE[[:space:]]+(GLOBAL[[:space:]]+TEMPORARY[[:space:]]+)?TABLE'
KEYWORDS="$KEYWORDS"'|RECREATE[[:space:]]+(TABLE|VIEW|PROCEDURE|TRIGGER|PACKAGE)'
KEYWORDS="$KEYWORDS"'|CREATE[[:space:]]+(OR[[:space:]]+ALTER[[:space:]]+)?VIEW'
KEYWORDS="$KEYWORDS"'|CREATE[[:space:]]+(UNIQUE[[:space:]]+)?(ASC(ENDING)?[[:space:]]+|DESC(ENDING)?[[:space:]]+)?INDEX'
KEYWORDS="$KEYWORDS"'|ALTER[[:space:]]+TABLE'
KEYWORDS="$KEYWORDS"'|COMMENT[[:space:]]+ON'
KEYWORDS="$KEYWORDS"'|CREATE[[:space:]]+DOMAIN'
KEYWORDS="$KEYWORDS"'|CREATE[[:space:]]+(SEQUENCE|GENERATOR)'
KEYWORDS="$KEYWORDS"'|CREATE[[:space:]]+(OR[[:space:]]+ALTER[[:space:]]+)?(PROCEDURE|TRIGGER|PACKAGE)'
KEYWORDS="$KEYWORDS"'|INSERT[[:space:]]+INTO'

# Lines that reference a canonical fixture file by name are the intended,
# allowed way to bring DDL/DML into the workflow -- exclude only those
# (grep -v), so any OTHER matching line still fails the check. Also
# pre-filter lines that can never be real fixture DDL/DML in the first
# place: bash comments, YAML step names, and blank lines -- this is what
# fixes the false-positive risk (e.g. a future step named
# "Create table of results" no longer trips the guard), since real
# fixture DDL/DML only ever appears inside a step's run: block content
# (printf/echo/heredoc/isql -i), never in a name: line or a comment.
matches=$(grep -inE "$KEYWORDS" "$WORKFLOW" \
  | grep -vE '^[0-9]+:[[:space:]]*#|^[0-9]+:[[:space:]]*-[[:space:]]*name:|^[0-9]+:[[:space:]]*$' \
  | grep -v "fixture_common\.sql\|fixture_biz4\.sql\|fixture_none_charset\.sql" || true)

if [ -n "$matches" ]; then
    echo "Fixture SQL/DML must live in scripts/fixture_common.sql," >&2
    echo "scripts/fixture_biz4.sql, or scripts/fixture_none_charset.sql," >&2
    echo "not inline workflow YAML." >&2
    echo "" >&2
    echo "Found in $WORKFLOW:" >&2
    echo "$matches" >&2
    exit 1
fi

echo "OK: no inline fixture DDL/DML found in $WORKFLOW"
```

- [ ] **Step 3: Confirm the guard still passes clean against the current workflow file**

```bash
bash scripts/check_no_inline_fixture_drift.sh; echo "exit=$?"
```

Expected: `OK: no inline fixture DDL/DML found in .github/workflows/build-linux-fb-matrix.yml`, `exit=0`. If this fails, the new `KEYWORDS` regex or pre-filter is matching something in the current, already-clean workflow file — read the printed `matches` output to find which line, and adjust the regex before continuing (do not special-case the specific line; fix the pattern).

- [ ] **Step 4: Prove each new keyword type is caught for real**

Run this once per probe line, confirming `exit code: 1` and that the probe line itself appears in the output each time, then confirming `git diff .github/workflows/build-linux-fb-matrix.yml` is empty (file restored) before moving to the next probe:

```bash
for probe in \
  "CREATE SEQUENCE ZZZ_DRIFT_PROBE;" \
  "CREATE PROCEDURE ZZZ_DRIFT_PROBE AS BEGIN END;" \
  "RECREATE TABLE ZZZ_DRIFT_PROBE (ID INTEGER);" \
  "INSERT INTO ZZZ_DRIFT_PROBE (ID) VALUES (1);" \
  "CREATE UNIQUE INDEX ZZZ_DRIFT_PROBE ON FOO (ID);" \
; do
  cp .github/workflows/build-linux-fb-matrix.yml /tmp/workflow_backup.yml
  echo "          $probe" >> .github/workflows/build-linux-fb-matrix.yml
  echo "=== probe: $probe ==="
  bash scripts/check_no_inline_fixture_drift.sh; echo "exit code: $?"
  cp /tmp/workflow_backup.yml .github/workflows/build-linux-fb-matrix.yml
  rm /tmp/workflow_backup.yml
  git diff --stat .github/workflows/build-linux-fb-matrix.yml
done
```

Expected for every iteration: the guard prints the "Fixture SQL/DML must live in..." message with that iteration's probe line shown, `exit code: 1`, and the final `git diff --stat` line is empty (no output) confirming the file was fully restored before the next probe ran.

- [ ] **Step 5: Prove the false-positive fix — a non-DDL line containing a keyword phrase must NOT trip the guard**

```bash
cp .github/workflows/build-linux-fb-matrix.yml /tmp/workflow_backup.yml
echo "      - name: Create table of results" >> .github/workflows/build-linux-fb-matrix.yml
bash scripts/check_no_inline_fixture_drift.sh; echo "exit code: $?"
cp /tmp/workflow_backup.yml .github/workflows/build-linux-fb-matrix.yml
rm /tmp/workflow_backup.yml
git diff --stat .github/workflows/build-linux-fb-matrix.yml
```

Expected: `OK: no inline fixture DDL/DML found in ...`, `exit code: 0` (the guard does NOT trip on this line, unlike before this task's change), and the final `git diff --stat` is empty.

- [ ] **Step 6: Prove the missing-file case fails loudly**

```bash
cp scripts/check_no_inline_fixture_drift.sh /tmp/guard_missing_test.sh
sed -i 's#WORKFLOW=".github/workflows/build-linux-fb-matrix.yml"#WORKFLOW="/tmp/does-not-exist.yml"#' /tmp/guard_missing_test.sh
bash /tmp/guard_missing_test.sh; echo "exit code: $?"
rm /tmp/guard_missing_test.sh
```

Expected: `guard: /tmp/does-not-exist.yml not found`, `exit code: 1`. This proves the file-existence check fires before the `grep`, rather than the old behavior where a missing file silently produced "OK".

- [ ] **Step 7: Commit**

```bash
git add scripts/check_no_inline_fixture_drift.sh
git commit -m "$(cat <<'EOF'
chore(ci): harden anti-drift guard keyword coverage + false-positive fix (#47)

CI-hygiene fix, not a product change: hardens the #39 guard
(scripts/check_no_inline_fixture_drift.sh) against its three known
gaps, found during #39's own strong review.

- False positive: pre-filters bash comments, YAML step names, and
  blank lines before keyword matching, so a future line like
  `- name: Create table of results` no longer trips the guard. Real
  fixture DDL/DML only ever appears inside a run: block's actual
  content, never a name: line or comment.
- False negative: keyword list now also covers CREATE SEQUENCE,
  CREATE PROCEDURE/TRIGGER/PACKAGE, RECREATE TABLE/VIEW/etc., bare
  INSERT INTO, CREATE GLOBAL TEMPORARY TABLE, and CREATE UNIQUE INDEX
  (previously missed -- "UNIQUE" broke the old literal "CREATE INDEX"
  substring match).
- Missing-file case: the workflow file's existence is checked before
  the grep, instead of relying on `|| true` to swallow both the
  expected "no matches" exit and a genuine "file not found" error the
  same way.
- Recognizes scripts/fixture_none_charset.sql (introduced by #26) as
  a third canonical fixture file in the allowlist and error message.

Every keyword here is workflow-YAML text this guard refuses to let
back in -- not a statement about extension runtime behavior, which
stays strictly read-only.
EOF
)"
git push -u origin chore/47-guard-hardening
```

- [ ] **Step 8: Push and watch the real CI run**

```bash
gh pr create --repo flozer/duckdb-firebird --draft --base main --head chore/47-guard-hardening \
  --title "chore(ci): #47 -- harden anti-drift guard" \
  --body "Closes #47. CI-hygiene fix only -- see docs/superpowers/specs/2026-07-16-guard-hardening-design.md. No extension-behavior change; no fixture content change."
gh run list --repo flozer/duckdb-firebird --branch chore/47-guard-hardening --limit 5
```

Wait for the "Build + Test Linux x64 (FB 3/4/5 matrix)" run (`gh run watch <run-id> --repo flozer/duckdb-firebird --exit-status`). Expected: all 3 legs green, including the "Guard against inline fixture DDL drift (#39)" step passing quickly (single grep, no build) on every leg — this is the load-bearing proof that the rewritten script doesn't false-positive against the real, current workflow file in the real CI environment (bash version, grep implementation), not just in this local shell.

---

## After Task 1

Single-task plan — no further tasks. No cross-version DuckDB matrix run needed for this branch (per the spec: no product code touched, no fixture content touched — the standing v1.5.2/v1.5.3/v1.5.4 gate exists to catch DuckDB-version-sensitive regressions, and a pure-bash CI guard script has no DuckDB-version dependency). The Linux FB3/4/5 matrix's own green run (this task's Step 7) is the complete validation for this branch.
