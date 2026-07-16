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
