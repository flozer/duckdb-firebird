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
