#!/usr/bin/env bash
# Regression test for #93: rows committed after the first scan of an ATTACHed
# table, with keys outside the primary-key bounds cached by that scan, must be
# read by later partitioned scans in the same session.
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
#                           "docker exec -i fb isql -u SYSDBA -p masterkey /var/lib/firebird/data/partitions.fdb"
#
# The table PART_GROWTH is created at the start and dropped at the end.
# Rows 0, 40000, ..., 11960000 (300 rows) span ~12M keys, so the ATTACH scan
# picks 2..5 partitions (fewer only on a single-core machine, which the test
# rejects). The rows inserted mid-session are -40000 (below the cached MIN) and
# 20000000 (above the cached MAX).

set -euo pipefail

DUCKDB="${DUCKDB:-./build/release/duckdb}"
: "${FIREBIRD_PARTITIONS_DB:?FIREBIRD_PARTITIONS_DB is not set}"
: "${FB_ISQL:?FB_ISQL is not set}"

work="$(mktemp -d)"
cleanup() {
    $FB_ISQL <<'SQL' >/dev/null 2>&1 || true
DROP TABLE PART_GROWTH;
COMMIT;
SQL
    rm -rf "$work"
}
trap cleanup EXIT

$FB_ISQL >/dev/null <<'SQL'
SET SQL DIALECT 3;
RECREATE TABLE PART_GROWTH (
    ID  INTEGER NOT NULL PRIMARY KEY,
    VAL VARCHAR(16) NOT NULL
);
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
COMMIT;
SQL
echo "inserted 2 rows outside the cached bounds"
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

-- First scan: caches the primary-key bounds [0, 11960000] for this ATTACH.
SELECT CASE WHEN n = 300 THEN 'first scan: ok'
            ELSE error('first scan: expected 300 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH);

.shell ${insert_cmd}

-- Same session, same catalog entry: the cached bounds are now stale.
SET firebird_query_log_size = 16;
SELECT CASE WHEN n = 302 THEN 'count after insert: ok'
            ELSE error('count after insert: expected 302 rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.PART_GROWTH);

-- The scan above must have been partitioned, or this test proves nothing.
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
EOF

echo "attach_cached_bounds: ok"
