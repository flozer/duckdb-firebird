#!/usr/bin/env bash
# Regression test for #93: rows committed after the first scan of an ATTACHed
# table, with keys outside the primary-key bounds cached by that scan, must be
# read by later scans in the same session, partitioned or not.
#
# The extension only opens read-only transactions, so a sqllogictest file can't
# insert rows mid-session. This script drives one DuckDB CLI session and uses
# `.shell` to insert the rows through isql between two scans.
#
# Environment:
#   DUCKDB                  DuckDB CLI with the extension (default ./build/release/duckdb)
#   FIREBIRD_PARTITIONS_DB  connection string of the partitions fixture database
#   FB_ISQL                 command that runs isql against that same database and
#                           reads SQL from stdin, e.g.
#                           "docker exec -i fb isql -q -bail -u SYSDBA -p masterkey localhost:/var/lib/firebird/data/partitions.fdb"
#
# Two tables, created at the start and dropped at the end (the test inserts
# into them, so they are not part of the shared fixture):
# - PART_GROWTH: rows 0, 40000, ..., 11960000 (300 rows) span ~12M keys, so the
#   ATTACH scan picks 2..5 partitions (fewer only on a single-core machine,
#   which the test rejects). Rows inserted mid-session: -40000 (below the
#   cached MIN) and 20000000 (above the cached MAX).
# - PART_GROWTH_SMALL: rows 1, 2, 3, scanned as a single partition. Rows
#   inserted mid-session: 0 and 4.

set -euo pipefail

DUCKDB="${DUCKDB:-./build/release/duckdb}"
: "${FIREBIRD_PARTITIONS_DB:?FIREBIRD_PARTITIONS_DB is not set}"
: "${FB_ISQL:?FB_ISQL is not set}"

work="$(mktemp -d)"
cleanup() {
    local t
    for t in PART_GROWTH PART_GROWTH_SMALL; do
        printf 'DROP TABLE %s;
COMMIT;
' "$t" | $FB_ISQL >/dev/null 2>&1 || true
    done
    rm -rf "$work"
}
trap cleanup EXIT

$FB_ISQL >/dev/null <<'SQL'
SET SQL DIALECT 3;
RECREATE TABLE PART_GROWTH (
    ID  INTEGER NOT NULL PRIMARY KEY,
    VAL VARCHAR(16) NOT NULL
);
RECREATE TABLE PART_GROWTH_SMALL (
    ID  INTEGER NOT NULL PRIMARY KEY,
    VAL VARCHAR(16) NOT NULL
);
COMMIT;
INSERT INTO PART_GROWTH_SMALL (ID, VAL) VALUES (1, 'row1');
INSERT INTO PART_GROWTH_SMALL (ID, VAL) VALUES (2, 'row2');
INSERT INTO PART_GROWTH_SMALL (ID, VAL) VALUES (3, 'row3');
COMMIT;
SET TERM !! ;
EXECUTE BLOCK AS
DECLARE I INTEGER = 0;
BEGIN
  WHILE (I < 300) DO
  BEGIN
    INSERT INTO PART_GROWTH (ID, VAL) VALUES (:I * 40000, 'row' || :I);
    I = I + 1;
  END
END!!
SET TERM ; !!
COMMIT;
SQL

# The helper run by `.shell`. It is passed as a single argument: the CLI quotes
# each argument, which cmd.exe mangles when there is more than one.
cat > "$work/insert.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
$FB_ISQL >/dev/null <<'SQL'
INSERT INTO PART_GROWTH (ID, VAL) VALUES (20000000, 'above');
INSERT INTO PART_GROWTH (ID, VAL) VALUES (-40000, 'below');
INSERT INTO PART_GROWTH_SMALL (ID, VAL) VALUES (4, 'above');
INSERT INTO PART_GROWTH_SMALL (ID, VAL) VALUES (0, 'below');
COMMIT;
SQL
echo "inserted rows outside the cached bounds"
EOF
chmod +x "$work/insert.sh"
insert_cmd="$work/insert.sh"
case "${OSTYPE:-}" in
    msys* | cygwin*)
        # Local runs from Git Bash on Windows: `.shell` goes through cmd.exe, and the
        # CLI drops backslashes, so the path uses forward slashes.
        printf '@bash "%s"\r\n' "$(cygpath -m "$work/insert.sh")" > "$work/insert.bat"
        insert_cmd="$(cygpath -m "$work/insert.bat")"
        ;;
esac

# Each check raises an error on failure; -bail makes the CLI exit non-zero.
"$DUCKDB" -unsigned -bail -list -noheader <<EOF
ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbg (TYPE firebird);

-- First scans: cache the primary-key bounds [0, 11960000] and [1, 3] for
-- this ATTACH.
SELECT CASE WHEN n = 300 THEN 'first scan: ok'
            ELSE error('first scan: expected 300 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH);
SELECT CASE WHEN n = 3 THEN 'first scan (small): ok'
            ELSE error('first scan (small): expected 3 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH_SMALL);

.shell ${insert_cmd}

-- Same session, same catalog entry: the cached bounds are now stale.
SET firebird_query_log_size = 16;
SELECT CASE WHEN n = 302 THEN 'count after insert: ok'
            ELSE error('count after insert: expected 302 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH);

-- PART_GROWTH must take the multi-partition path, or this part proves nothing.
SELECT CASE WHEN n >= 2 THEN 'partitioned: ok (' || n || ' cursors)'
            ELSE error('expected a partitioned scan, got ' || n || ' cursor(s)') END
FROM (SELECT count(*) AS n FROM firebird_query_log() WHERE remote_sql LIKE '%PART_GROWTH%');
SET firebird_query_log_size = 0;

SELECT CASE WHEN n = 1 THEN 'key above cached MAX: ok'
            ELSE error('WHERE ID = 20000000: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH WHERE ID = 20000000);

SELECT CASE WHEN n = 1 THEN 'key below cached MIN: ok'
            ELSE error('WHERE ID = -40000: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH WHERE ID = -40000);

-- Single-partition path.
SET firebird_query_log_size = 16;
SELECT CASE WHEN n = 5 THEN 'count after insert (small): ok'
            ELSE error('count after insert (small): expected 5 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH_SMALL);

SELECT CASE WHEN n = 1 THEN 'single partition (small): ok'
            ELSE error('expected a single-partition scan, got ' || n || ' cursor(s)') END
FROM (SELECT count(*) AS n FROM firebird_query_log() WHERE remote_sql LIKE '%PART_GROWTH_SMALL%');
SET firebird_query_log_size = 0;

SELECT CASE WHEN n = 1 THEN 'key above cached MAX (small): ok'
            ELSE error('small, WHERE ID = 4: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH_SMALL WHERE ID = 4);

SELECT CASE WHEN n = 1 THEN 'key below cached MIN (small): ok'
            ELSE error('small, WHERE ID = 0: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH_SMALL WHERE ID = 0);
EOF

echo "attach_cached_bounds: ok"
