#!/usr/bin/env bash
# Failure check for the shared snapshot of partitioned scans (Firebird 4+).
#
# Once a partitioned scan has pinned its snapshot, a worker that cannot join
# it must fail the scan, never read its partitions outside the snapshot, and
# its pooled connection must not go back to the pool. A pooled connection
# whose attachment was killed on the server is the natural way to get there:
# the worker leases it, cannot join, and the scan must stop.
#
# One DuckDB CLI session on the ATTACH path, in two phases:
#   * one:  kill ONE of the idle pooled attachments, so the next scan has
#           workers that join the snapshot and one worker that cannot;
#   * all:  kill ALL of them, so no worker can join.
# In each phase the script
#   1. scans PART_RANGE (300 rows, snapshot_mode = 'shared'), which leaves
#      the workers' connections idle in the pool;
#   2. kills attachments from isql via `.shell` (DELETE FROM MON$ATTACHMENTS)
#      and checks how many it targets (1, or every idle pooled connection);
#   3. scans again: it must fail with the "could not join the scan's shared
#      snapshot" error naming the table, and right after it
#      firebird_pool_stats() must show no connection still leased and at
#      least one more discarded;
#      If a worker that did join recorded a cursor of the failed scan,
#      firebird_last_query() must carry the join error; if none did, it
#      must still show the warm-up scan's record, untouched;
#   4. scans up to RETRIES more times: any scan that still gets a dead
#      connection must fail with that same error, a later one must return
#      all 300 rows with snapshot_mode = 'shared', and so must every scan
#      after it.
# Any other error fails the check. Firebird 3 has no shared snapshot, so the
# check is skipped there.
#
# Uses PART_RANGE from scripts/fixture_partitions.sql (read only). Needs a
# machine with at least 2 cores, so that the ATTACH scan is partitioned.
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

RETRIES=5
JOIN_ERROR="Firebird partitioned scan of table 'PART_RANGE' stopped: a worker could not join the scan's shared snapshot"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

major=$("$DUCKDB" -unsigned -csv -noheader <<EOF | tr -d '\r' | tail -n 1
${DUCKDB_PRELUDE}
ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbv (TYPE firebird);
SELECT split_part(engine_version, '.', 1) FROM firebird_health('fbv');
EOF
)
if [ "$major" -lt 4 ]; then
    echo "check_snapshot_join_failure: skipped (Firebird $major has no shared snapshot)"
    exit 0
fi

# kill_helper <name> <ROWS clause or empty>: writes a script that kills the
# attachments of DuckDB processes to this database (never isql's own; at
# most one with "ROWS 1"), printing how many, and prints the command to pass
# to `.shell`.
kill_helper() {
    local sh="$work/kill_$1.sh"
    {
        echo '#!/usr/bin/env bash'
        echo 'set -euo pipefail'
        printf '%s -q -bail -u %q -p %q %q <<'"'"'SQL'"'"'\n' "$FB_ISQL" "$FB_USER" "$FB_PASS" "$FB_ISQL_DB"
        echo 'SET HEADING OFF;'
        echo "SELECT 'targeting ' || MINVALUE(COUNT(*), ${3}) || ' attachments' FROM MON\$ATTACHMENTS"
        echo " WHERE MON\$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON\$SYSTEM_FLAG = 0"
        echo "   AND MON\$REMOTE_PROCESS CONTAINING 'duckdb';"
        echo "DELETE FROM MON\$ATTACHMENTS"
        echo " WHERE MON\$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON\$SYSTEM_FLAG = 0"
        echo "   AND MON\$REMOTE_PROCESS CONTAINING 'duckdb' $2;"
        echo 'COMMIT;'
        echo 'SQL'
        # isql leaves its last prompt without a newline; end the line here so
        # the next scan's error starts on its own line.
        echo 'echo'
    } > "$sh"
    chmod +x "$sh"
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            # Local runs from Git Bash on Windows: `.shell` goes through
            # cmd.exe, and the CLI drops backslashes, so use forward slashes.
            # One argument only: the CLI quotes each one, which cmd.exe
            # mangles.
            printf '@bash "%s"\r\n' "$(cygpath -m "$sh")" > "$work/kill_$1.bat"
            cygpath -m "$work/kill_$1.bat"
            ;;
        *) echo "$sh" ;;
    esac
}

# phase_sql <phase> <kill command>: the SQL of one phase. Scans are numbered
# 1 (warm-up), 2 (after the kill) and 3.. (retries).
phase_sql() {
    local p=$1 i
    echo ".print === phase $p"
    echo "SELECT '$p scan1 rows=' || count(*) FROM fbk.main.PART_RANGE;"
    echo "SELECT '$p scan1 mode=' || snapshot_mode FROM firebird_last_query();"
    echo "SELECT '$p telemetry after scan1: captured_at=' || captured_at || ' log_rows=' || (SELECT count(*) FROM firebird_query_log()) || ' join_error=' || (coalesce(error_message, '') LIKE '%could not join the scan''s shared snapshot%') FROM firebird_last_query();"
    echo "SELECT '$p pool before idle=' || idle_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbk');"
    echo ".shell $2"
    echo "SELECT '$p scan2 rows=' || count(*) FROM fbk.main.PART_RANGE;"
    echo "SELECT '$p pool after failed scan active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbk');"
    echo "SELECT '$p telemetry after failed scan: captured_at=' || captured_at || ' log_rows=' || (SELECT count(*) FROM firebird_query_log()) || ' join_error=' || (coalesce(error_message, '') LIKE '%could not join the scan''s shared snapshot%') FROM firebird_last_query();"
    for i in $(seq 3 $((RETRIES + 2))); do
        echo "SELECT '$p scan$i rows=' || count(*) FROM fbk.main.PART_RANGE;"
        echo "SELECT '$p scan$i mode=' || snapshot_mode FROM firebird_last_query();"
    done
    echo "SELECT '$p pool after active=' || active_connections || ' discarded=' || total_discarded || ' idle=' || idle_connections FROM firebird_pool_stats('fbk');"
}

out="$work/session.out"
# No -bail: the failing scans are expected; the output is checked below.
{
    echo "${DUCKDB_PRELUDE}"
    echo "ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbk (TYPE firebird);"
    echo "SET firebird_query_log_size = 1000;"
    phase_sql one "$(kill_helper one 'ROWS 1' 1)"
    phase_sql all "$(kill_helper all '' 1000000)"
} | "$DUCKDB" -unsigned -list -noheader > "$out" 2>&1 || true
tr -d '\r' < "$out" > "$out.lf"
cat "$out.lf"

fail() { echo "check_snapshot_join_failure: FAILED: $*"; exit 1; }
has() { grep -qxF -- "$1" "$out.lf"; }
# count <grep option> <pattern> <file>: occurrences, 0 when none.
count() { grep -o "$1" -- "$2" "$3" | wc -l || true; }

summary=""
telemetry=""
# check_phase <phase> <expected kill count, or "all">
check_phase() {
    local p=$1 want=$2 sec="$work/$1.out" line idle disc0 disc killed i first_ok=""
    # This phase's part of the output: from its marker to the next one.
    awk -v m="=== phase $p" '$0 == m {f = 1; next} /^=== phase / {f = 0} f' "$out.lf" > "$sec"

    has "$p scan1 rows=300" || fail "$p: first scan did not return 300 rows"
    has "$p scan1 mode=shared" || fail "$p: first scan was not 'shared'"
    line=$(grep "^$p pool before idle=" "$sec" || true)
    [[ "$line" =~ idle=([0-9]+)\ discarded=([0-9]+)$ ]] || fail "$p: unexpected pool stats: '$line'"
    idle=${BASH_REMATCH[1]}; disc0=${BASH_REMATCH[2]}
    [ "$idle" -ge 2 ] || fail "$p: only $idle idle pooled connection(s); the scan was not partitioned"
    [ "$want" = all ] && want=$idle
    # Counted just before the DELETE; that the kill took effect is shown by
    # the failed scan and the discard checks below.
    killed=$(sed -nE 's/^ *targeting ([0-9]+) attachments *$/\1/p' "$sec")
    [ "$killed" = "$want" ] || fail "$p: targeted '$killed' attachments, expected $want (idle pooled: $idle)"
    grep -q "^$p scan2 rows=" "$sec" && fail "$p: the scan after the kill did not fail"

    local join_errors all_errors
    join_errors=$(count -F "$JOIN_ERROR" "$sec")
    all_errors=$(count -E '(^|[A-Za-z]+ )Error: ' "$sec")
    [ "$join_errors" -ge 1 ] || fail "$p: no 'could not join the scan's shared snapshot' error"
    [ "$all_errors" -eq "$join_errors" ] || fail "$p: $((all_errors - join_errors)) error(s) of another kind"

    # The worker that failed must have discarded its connection, not
    # returned it to the pool (a killed attachment fails the rollback and
    # would otherwise be leased again by the next scan).
    line=$(grep "^$p pool after failed scan " "$sec" || true)
    [[ "$line" =~ active=0\ discarded=([0-9]+)$ ]] || fail "$p: unexpected pool stats: '$line'"
    disc=${BASH_REMATCH[1]}
    [ "$disc" -gt "$disc0" ] || fail "$p: the failed scan discarded no connection: '$line'"

    # Telemetry: if a worker that did join recorded a cursor of the failed
    # scan, firebird_last_query() must carry the join error; if none did,
    # it must still be scan 1's record, untouched.
    local t1 t2 c1 c2 n1 n2 e2
    t1=$(grep "^$p telemetry after scan1: " "$sec" || true)
    t2=$(grep "^$p telemetry after failed scan: " "$sec" || true)
    [[ "$t1" =~ captured_at=(.*)\ log_rows=([0-9]+)\ join_error=false$ ]] || fail "$p: unexpected telemetry: '$t1'"
    c1=${BASH_REMATCH[1]}; n1=${BASH_REMATCH[2]}
    [[ "$t2" =~ captured_at=(.*)\ log_rows=([0-9]+)\ join_error=(true|false)$ ]] || fail "$p: unexpected telemetry: '$t2'"
    c2=${BASH_REMATCH[1]}; n2=${BASH_REMATCH[2]}; e2=${BASH_REMATCH[3]}
    if [ "$n2" -gt "$n1" ]; then
        [ "$e2" = true ] || fail "$p: the failed scan logged $((n2 - n1)) cursor(s) but firebird_last_query() has no join error"
        telemetry="the failed scan logged $((n2 - n1)) cursor(s), last_query has the join error"
    else
        [ "$e2" = false ] && [ "$c2" = "$c1" ] || fail "$p: nothing logged, but firebird_last_query() changed: '$t1' -> '$t2'"
        telemetry="nothing recorded, previous record untouched"
    fi

    for i in $(seq 3 $((RETRIES + 2))); do
        if has "$p scan$i rows=300"; then
            [ -n "$first_ok" ] || first_ok=$i
            has "$p scan$i mode=shared" || fail "$p: scan $i returned 300 rows but was not 'shared'"
        elif grep -q "^$p scan$i rows=" "$sec"; then
            fail "$p: scan $i returned the wrong number of rows"
        elif [ -n "$first_ok" ]; then
            fail "$p: scan $i failed after scan $first_ok had recovered"
        fi
    done
    [ -n "$first_ok" ] || fail "$p: no scan recovered within $RETRIES retries"
    # Every scan before the recovery (scan 2 included) failed with the join
    # error, and no other scan did.
    [ "$join_errors" -eq $((first_ok - 2)) ] || fail "$p: $join_errors join error(s) for $((first_ok - 2)) failed scan(s)"

    line=$(grep "^$p pool after active=" "$sec" || true)
    [[ "$line" =~ ^$p\ pool\ after\ active=0\ discarded=([0-9]+)\ idle= ]] || fail "$p: unexpected final pool stats: '$line'"
    summary+=" $p: targeted $killed of $idle, $join_errors failed scan(s), $((disc - disc0)) discarded by the first, recovered at scan $first_ok, telemetry: $telemetry;"
}

check_phase one 1
check_phase all all
summary=${summary# }
echo "check_snapshot_join_failure: ok (${summary%;})"
