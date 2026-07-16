# Anti-drift guard hardening (#47) — design spec

**Classification:** CI hygiene / maintenance chore, not a product or roadmap
front. `scripts/check_no_inline_fixture_drift.sh` (introduced in #39) is a
CI-only guard against inline Firebird DDL/DML reappearing in
`.github/workflows/build-linux-fb-matrix.yml`. The extension itself stays
strictly read-only; every CREATE/INSERT/ALTER keyword this guard matches is
blocked *workflow YAML text*, not extension behavior. Nothing here touches
`src/`, extension runtime, or any fixture's actual content.

## Problem

Task 4's review (during #39) found the guard's current keyword list and
matching approach has two real gaps, tracked as issue #47:

1. **False positive:** case-insensitive substring matching trips on any
   future non-DDL line containing a keyword phrase — e.g. a step named
   `- name: Create table of results`, or a bash comment mentioning one.
2. **False negative:** the keyword list misses `CREATE SEQUENCE`, `CREATE
   PROCEDURE`, `CREATE TRIGGER`, `RECREATE TABLE`/`VIEW`/etc., bare
   `INSERT INTO`, and `CREATE UNIQUE INDEX` (currently missed because
   `UNIQUE` splits the literal `CREATE INDEX` substring the current regex
   requires).
3. **Missing-file edge case:** `matches=$(grep ... | grep -v ... || true)`
   combined with `set -euo pipefail` — if `$WORKFLOW` doesn't exist, `grep`
   exits 2, which `|| true` swallows the same way it swallows the expected
   "no matches" exit 1, so the script silently reports "OK" instead of
   erroring.

Also found while triaging #26: `scripts/fixture_none_charset.sql` is a
third canonical fixture file (per #26's design) but isn't yet named in the
guard's exclusion list or its error message — purely cosmetic today (no
line needs excluding because of it), but worth fixing for consistency
before it's forgotten.

## Design

### 1. Pre-filter non-executable lines (false-positive fix)

Before keyword matching, exclude lines that never execute as SQL: bash
comments and YAML step names. Real fixture DDL only ever appears inside a
step's `run:` block content (`printf`/`echo`/heredoc/`isql -i`) — never in
a step's `name:` line or a `#`-prefixed comment — so this filter cannot
hide a genuine drift reintroduction, only lines that were never at risk of
being one:

```bash
grep -inE "$KEYWORDS" "$WORKFLOW" \
  | grep -vE '^[0-9]+:[[:space:]]*#|^[0-9]+:[[:space:]]*-[[:space:]]*name:'
```

(`grep -n` prefixes each match with `N:`, hence anchoring past the line
number rather than the raw line start.)

### 2. Wider keyword coverage (false-negative fix)

Replace the flat `KEYWORDS` string with an extended-regex alternation
covering the gaps above, using `[[:space:]]+` instead of a literal space
so multi-space-separated keywords still match:

```bash
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
```

These are workflow-YAML text patterns the guard refuses to let back in —
not a statement about what the extension supports or executes at runtime.

### 3. Fail loudly on a missing workflow file

```bash
[ -f "$WORKFLOW" ] || { echo "guard: $WORKFLOW not found" >&2; exit 1; }
```

placed immediately after `WORKFLOW=` is assigned, before the `grep` call.

### 4. Recognize the third canonical fixture file

Extend the exclusion pattern and the error message to name
`fixture_none_charset.sql` alongside the existing two:

```bash
grep -v "fixture_common\.sql\|fixture_biz4\.sql\|fixture_none_charset\.sql"
```

and update the printed message to list all three file names.

## Validation

- Guard passes clean against the current (post-#26) workflow file.
- Each new keyword type is caught for real: temporarily inject one probe
  line at a time (`CREATE SEQUENCE`, `CREATE PROCEDURE`, `RECREATE TABLE`,
  `INSERT INTO`, `CREATE UNIQUE INDEX`), confirm exit 1, restore the file
  byte-identical (`git diff` empty) before moving to the next probe.
- False-positive fix proven for real: temporarily add a step named
  `- name: Create table of results` (no actual DDL), confirm the guard
  still exits 0 (does not trip), restore the file.
- Missing-file case proven for real: run the script against a temporarily
  renamed/nonexistent workflow path, confirm exit 1 with the new error
  message.
- Linux FB3/4/5 matrix workflow still green (this script runs as an early
  CI step; a regression here would fail the very first step of every job).

## Out of scope

- No change to `scripts/fixture_common.sql`, `scripts/fixture_biz4.sql`,
  or `scripts/fixture_none_charset.sql` content.
- No change to fixture provisioning logic, test wiring, or any product
  code (`src/`).
- No DECFLOAT CI work (issue #48, separate front).
- Not a new SDD front — small enough for direct inline implementation.
- No changes to `duckdb/community-extensions` or any upstream repo.
