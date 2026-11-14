#!/usr/bin/env bash
# Regression check for #93: on the ATTACH path the primary-key MIN/MAX is
# probed once, the first time a query references the table, and cached for
# the life of the ATTACH. Rows committed afterwards with a key below the cached
# MIN or above the cached MAX must still be read by later scans in the same
# session, partitioned or not.
#
# sqllogictest can't insert rows mid-session (the extension only opens
# read-only transactions), so this script drives DuckDB CLI sessions and uses
# `.shell` to insert the rows through isql, over a separate connection, between
# two scans.
#
# Two tables, created at the start and dropped at the end (the script inserts
# into them, so they are not part of the shared fixture), each checked in its
# own CLI session so that either can fail on its own:
#   * PART_GROWTH: ID 0, 40000, ..., 11960000 (300 rows, ~12M span), so the
#     ATTACH scan picks 2..5 partitions (fewer only on a single-core machine,
#     which the check rejects). Inserted mid-session: -40000 and 20000000.
#   * PART_GROWTH_SMALL: ID 1, 2, 3, scanned as a single partition. Inserted
#     mid-session: 0 and 4.
#
# Defaults match the Linux FB matrix (server in the `fb` container, port
# 3050). Override for local runs:
#   FB_ISQL                 command running isql        (docker exec -i fb isql)
#   FB_DB_PATH              database file on the server (/var/lib/firebird/data/partitions.fdb)
#   FB_ISQL_DB              database as isql opens it   (localhost:<FB_DB_PATH>: through
#                           the server, never embedded)
#   FIREBIRD_PARTITIONS_DB  connection string for the extension
#                           (firebird://SYSDBA:masterkey@localhost:3050<FB_DB_PATH>?charset=UTF8)
#   DUCKDB                  DuckDB CLI with the extension linked in (./build/release/duckdb)
#   DUCKDB_PRELUDE          SQL run at the start of each session, e.g. a LOAD (empty)
set -euo pipefail

FB_ISQL=${FB_ISQL:-"docker exec -i fb isql"}
FB_DB_PATH=${FB_DB_PATH:-/var/lib/firebird/data/partitions.fdb}
FB_ISQL_DB=${FB_ISQL_DB:-"localhost:${FB_DB_PATH}"}
FIREBIRD_PARTITIONS_DB=${FIREBIRD_PARTITIONS_DB:-"firebird://SYSDBA:masterkey@localhost:3050${FB_DB_PATH}?charset=UTF8"}
DUCKDB=${DUCKDB:-./build/release/duckdb}
DUCKDB_PRELUDE=${DUCKDB_PRELUDE:-}
FB_USER=${ISC_USER:-SYSDBA}
FB_PASS=${ISC_PASSWORD:-masterkey}

# -bail: a failing statement makes isql exit non-zero.
isql_db() { $FB_ISQL -q -bail -u "$FB_USER" -p "$FB_PASS" "$FB_ISQL_DB"; }

work=$(mktemp -d)
cleanup() {
    local t
    for t in PART_GROWTH PART_GROWTH_SMALL; do
        printf 'DROP TABLE %s;\nCOMMIT;\n' "$t" | isql_db >/dev/null 2>&1 || true
    done
    rm -rf "$work"
}
trap cleanup EXIT

echo "== creating PART_GROWTH and PART_GROWTH_SMALL in $FB_ISQL_DB"
isql_db >/dev/null <<'SQL'
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

# helper <name> <low key> <high key>: writes a script that `.shell` runs to
# insert both keys into <name>, and prints the command to pass to `.shell`.
# The command is a single argument: the CLI quotes each argument, which
# cmd.exe mangles when there is more than one.
helper() {
    local f="$work/insert_$1.sh"
    {
        echo '#!/usr/bin/env bash'
        echo 'set -euo pipefail'
        printf '%s -q -bail -u %q -p %q %q >/dev/null <<SQL\n' "$FB_ISQL" "$FB_USER" "$FB_PASS" "$FB_ISQL_DB"
        echo "INSERT INTO $1 (ID, VAL) VALUES ($2, 'below');"
        echo "INSERT INTO $1 (ID, VAL) VALUES ($3, 'above');"
        echo 'COMMIT;'
        echo 'SQL'
        echo "echo \"inserted $2 and $3 into $1\""
    } > "$f"
    chmod +x "$f"
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            # Local runs from Git Bash on Windows: `.shell` goes through
            # cmd.exe, and the CLI drops backslashes, so use forward slashes.
            printf '@bash "%s"\r\n' "$(cygpath -m "$f")" > "$work/insert_$1.bat"
            cygpath -m "$work/insert_$1.bat"
            ;;
        *) echo "$f" ;;
    esac
}

# check <table> <rows before> <low key> <high key> <cursor check>
# One CLI session: first scan (caches the bounds), insert outside them, then
# check COUNT, both keys, and how many cursors the second scan used. Each check
# raises error() on failure and -bail makes the CLI exit non-zero.
check() {
    local t=$1 before=$2 lo=$3 hi=$4 cursors=$5
    local after=$((before + 2))
    echo "== $t"
    "$DUCKDB" -unsigned -bail -list -noheader <<EOF
${DUCKDB_PRELUDE}
ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbg (TYPE firebird);

SELECT CASE WHEN n = ${before} THEN '${t} first scan: ok'
            ELSE error('${t} first scan: expected ${before} rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.${t});

.shell $(helper "$t" "$lo" "$hi")

SET firebird_query_log_size = 16;
SELECT CASE WHEN n = ${after} THEN '${t} count after insert: ok'
            ELSE error('${t} count after insert: expected ${after} rows, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.${t});
SELECT CASE WHEN ${cursors} THEN '${t} cursors: ok (' || n || ')'
            ELSE error('${t} cursors: expected ${cursors}, got ' || n) END
FROM (SELECT count(*) AS n FROM firebird_query_log() WHERE remote_sql LIKE '%"${t}"%');
SET firebird_query_log_size = 0;

SELECT CASE WHEN n = 1 THEN '${t} key ${hi} above cached MAX: ok'
            ELSE error('${t} WHERE ID = ${hi}: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.${t} WHERE ID = ${hi});
SELECT CASE WHEN n = 1 THEN '${t} key ${lo} below cached MIN: ok'
            ELSE error('${t} WHERE ID = ${lo}: expected 1 row, got ' || n) END
FROM (SELECT count(*) AS n FROM fbg.${t} WHERE ID = ${lo});
EOF
}

failed=0
check PART_GROWTH 300 -40000 20000000 'n >= 2' || failed=1
check PART_GROWTH_SMALL 3 0 4 'n = 1' || failed=1

if [ "$failed" -ne 0 ]; then
    echo "check_attach_cached_bounds: FAILED"
    exit 1
fi
echo "check_attach_cached_bounds: ok"
