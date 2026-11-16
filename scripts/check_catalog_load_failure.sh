#!/usr/bin/env bash
# Failure check for the catalog load of an ATTACH (all Firebird versions).
#
# The first query that touches a table of an attached database loads the
# table list over one pooled connection. If that load fails, its connection
# must go back to the pool as discarded: before the fix the
# exception dropped it without telling the pool, and
# firebird_pool_stats().active_connections stayed one too high for the life
# of the ATTACH.
#
# A pooled connection whose attachment was killed on the server makes the
# load fail. One DuckDB CLI session on the ATTACH path:
#   1. firebird_health() leases a connection and parks it in the pool,
#      without loading the table list;
#   2. that attachment is killed from isql via `.shell`
#      (DELETE FROM MON$ATTACHMENTS);
#   3. the first query on PART_RANGE must fail with "Firebird catalog
#      discovery failed", and right after it firebird_pool_stats() must
#      show no connection still leased and one more discarded;
#   4. the same query again must return all 300 rows, and leave no
#      connection leased.
#
# Uses PART_RANGE from scripts/fixture_partitions.sql (read only).
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
#   DUCKDB_PRELUDE          SQL run at the start of the session, e.g. a LOAD (empty)
set -euo pipefail

FB_ISQL=${FB_ISQL:-"docker exec -i fb isql"}
FB_DB_PATH=${FB_DB_PATH:-/var/lib/firebird/data/partitions.fdb}
FB_ISQL_DB=${FB_ISQL_DB:-"localhost:${FB_DB_PATH}"}
FIREBIRD_PARTITIONS_DB=${FIREBIRD_PARTITIONS_DB:-"firebird://SYSDBA:masterkey@localhost:3050${FB_DB_PATH}?charset=UTF8"}
DUCKDB=${DUCKDB:-./build/release/duckdb}
DUCKDB_PRELUDE=${DUCKDB_PRELUDE:-}
FB_USER=${ISC_USER:-SYSDBA}
FB_PASS=${ISC_PASSWORD:-masterkey}

LOAD_ERROR="Firebird catalog discovery failed"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Kills the attachments of DuckDB processes to this database (never isql's
# own), printing how many, and prints the command to pass to `.shell`.
kill_helper() {
    local sh="$work/kill.sh"
    {
        echo '#!/usr/bin/env bash'
        echo 'set -euo pipefail'
        printf '%s -q -bail -u %q -p %q %q <<'"'"'SQL'"'"'\n' "$FB_ISQL" "$FB_USER" "$FB_PASS" "$FB_ISQL_DB"
        echo 'SET HEADING OFF;'
        echo "SELECT 'targeting ' || COUNT(*) || ' attachments' FROM MON\$ATTACHMENTS"
        echo " WHERE MON\$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON\$SYSTEM_FLAG = 0"
        echo "   AND MON\$REMOTE_PROCESS CONTAINING 'duckdb';"
        echo "DELETE FROM MON\$ATTACHMENTS"
        echo " WHERE MON\$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON\$SYSTEM_FLAG = 0"
        echo "   AND MON\$REMOTE_PROCESS CONTAINING 'duckdb';"
        echo 'COMMIT;'
        echo 'SQL'
        # isql leaves its last prompt without a newline; end the line here so
        # the next query's error starts on its own line.
        echo 'echo'
    } > "$sh"
    chmod +x "$sh"
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            # Local runs from Git Bash on Windows: `.shell` goes through
            # cmd.exe, and the CLI drops backslashes, so use forward slashes.
            # One argument only: the CLI quotes each one, which cmd.exe
            # mangles.
            printf '@bash "%s"\r\n' "$(cygpath -m "$sh")" > "$work/kill.bat"
            cygpath -m "$work/kill.bat"
            ;;
        *) echo "$sh" ;;
    esac
}

out="$work/session.out"
# No -bail: the failing query is expected; the output is checked below.
{
    echo "${DUCKDB_PRELUDE}"
    echo "ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbl (TYPE firebird);"
    echo "SELECT 'health rows=' || count(*) FROM firebird_health('fbl');"
    echo "SELECT 'pool parked idle=' || idle_connections || ' active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbl');"
    echo ".shell $(kill_helper)"
    echo "SELECT 'load1 rows=' || count(*) FROM fbl.main.PART_RANGE;"
    echo "SELECT 'pool after failed load active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbl');"
    echo "SELECT 'load2 rows=' || count(*) FROM fbl.main.PART_RANGE;"
    echo "SELECT 'pool after retry active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbl');"
} | "$DUCKDB" -unsigned -list -noheader > "$out" 2>&1 || true
tr -d '\r' < "$out" > "$out.lf"
cat "$out.lf"

fail() { echo "check_catalog_load_failure: FAILED: $*"; exit 1; }
value() { sed -nE "s/^$1$/\\1/p" "$out.lf" | head -n 1; }

idle=$(value 'pool parked idle=([0-9]+) active=[0-9]+ discarded=[0-9]+')
active0=$(value 'pool parked idle=[0-9]+ active=([0-9]+) discarded=[0-9]+')
disc0=$(value 'pool parked idle=[0-9]+ active=[0-9]+ discarded=([0-9]+)')
[ "${idle:-0}" -ge 1 ] || fail "firebird_health() left no idle connection in the pool to kill"
[ "$active0" = 0 ] || fail "a connection was already counted as leased before the kill (active=$active0)"
killed=$(sed -nE 's/^ *targeting ([0-9]+) attachments *$/\1/p' "$out.lf" | head -n 1)
[ "${killed:-0}" -ge 1 ] || fail "no DuckDB attachment was found to kill"
grep -q '^load1 rows=' "$out.lf" && fail "the first query after the kill did not fail"
grep -qF "$LOAD_ERROR" "$out.lf" || fail "the failed query did not report '$LOAD_ERROR'"
# Exactly one error in the session: the failed load. (The two
# "failed to load ... constraints" lines are warnings, not errors.)
errors=$(grep -cE '(^|[A-Za-z]+ )Error: ' "$out.lf" || true)
[ "$errors" = 1 ] || fail "expected exactly one error (the failed load), got $errors"
disc1=$(value 'pool after failed load active=0 discarded=([0-9]+)')
[ -n "$disc1" ] || fail "a connection is still counted as leased after the failed load (want active=0)"
[ "$disc1" -gt "$disc0" ] || fail "the failed load discarded no connection (discarded $disc0 -> $disc1)"
grep -qx 'load2 rows=300' "$out.lf" || fail "the query after the failed load did not return 300 rows"
grep -qE '^pool after retry active=0 ' "$out.lf" || fail "a connection is still counted as leased after the retry (want active=0)"

echo "check_catalog_load_failure: ok (killed $killed, failed load discarded $((disc1 - disc0)), retry read 300 rows, nothing left leased)"
