#!/usr/bin/env bash
# Failure check for the shared snapshot of partitioned scans (Firebird 4+).
#
# Once a partitioned scan has pinned its snapshot, a worker that cannot join
# it must fail the scan, never read its partitions outside the snapshot, and
# its pooled connection must not go back to the pool. A pooled connection
# whose attachment was killed on the server is the natural way to get there:
# the worker leases it, cannot join, and the scan must stop.
#
# One DuckDB CLI session on the ATTACH path:
#   1. scan PART_RANGE once (snapshot_mode = 'shared'), which leaves the
#      workers' connections idle in the pool;
#   2. kill those attachments from isql via `.shell`
#      (DELETE FROM MON$ATTACHMENTS);
#   3. scan again: it must fail with the "could not join the scan's shared
#      snapshot" error naming the table;
#   4. scan a few more times: any scan that still gets a dead connection
#      must fail with that same error, and a later one must return all 300
#      rows with snapshot_mode = 'shared' on new connections; every scan
#      after it must succeed too;
#   5. firebird_pool_stats() must show no connection still leased and at
#      least one discarded.
# Any other error fails the check. Firebird 3 has no shared snapshot, so the
# check is skipped there.
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

# Kills every attachment of a DuckDB process to this database except isql's
# own, and prints how many it killed.
kill_sh="$work/kill_attachments.sh"
{
    echo '#!/usr/bin/env bash'
    echo 'set -euo pipefail'
    printf '%s -q -bail -u %q -p %q %q <<'"'"'SQL'"'"'\n' "$FB_ISQL" "$FB_USER" "$FB_PASS" "$FB_ISQL_DB"
    cat <<'SQL'
SET HEADING OFF;
SELECT 'killing ' || COUNT(*) || ' attachments' FROM MON$ATTACHMENTS
 WHERE MON$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON$SYSTEM_FLAG = 0
   AND MON$REMOTE_PROCESS CONTAINING 'duckdb';
DELETE FROM MON$ATTACHMENTS
 WHERE MON$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON$SYSTEM_FLAG = 0
   AND MON$REMOTE_PROCESS CONTAINING 'duckdb';
COMMIT;
SQL
    echo 'SQL'
    # isql leaves its last prompt without a newline; end the line here so
    # the next scan's error starts on its own line.
    echo 'echo'
} > "$kill_sh"
chmod +x "$kill_sh"
kill_cmd=$kill_sh
case "${OSTYPE:-}" in
    msys* | cygwin*)
        # Local runs from Git Bash on Windows: `.shell` goes through cmd.exe,
        # and the CLI drops backslashes, so use forward slashes. One argument
        # only: the CLI quotes each one, which cmd.exe mangles.
        printf '@bash "%s"\r\n' "$(cygpath -m "$kill_sh")" > "$work/kill_attachments.bat"
        kill_cmd=$(cygpath -m "$work/kill_attachments.bat")
        ;;
esac

scans=""
for i in $(seq 3 $((RETRIES + 2))); do
    scans+="SELECT 'scan${i} rows=' || count(*) FROM fbk.main.PART_RANGE;
SELECT 'scan${i} mode=' || snapshot_mode FROM firebird_last_query();
"
done

out="$work/session.out"
# No -bail: the failing scans are expected; the output is checked below.
"$DUCKDB" -unsigned -list -noheader > "$out" 2>&1 <<EOF || true
${DUCKDB_PRELUDE}
ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbk (TYPE firebird);
SELECT 'scan1 rows=' || count(*) FROM fbk.main.PART_RANGE;
SELECT 'scan1 mode=' || snapshot_mode FROM firebird_last_query();
SELECT 'pool before idle=' || idle_connections FROM firebird_pool_stats('fbk');
.shell ${kill_cmd}
SELECT 'scan2 rows=' || count(*) FROM fbk.main.PART_RANGE;
SELECT 'pool after failed scan active=' || active_connections || ' discarded=' || total_discarded
       FROM firebird_pool_stats('fbk');
${scans}
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded
       || ' idle=' || idle_connections FROM firebird_pool_stats('fbk');
EOF
tr -d '\r' < "$out" > "$out.lf"
cat "$out.lf"

fail() { echo "check_snapshot_join_failure: FAILED: $*"; exit 1; }
has() { grep -qxF -- "$1" "$out.lf"; }

has 'scan1 rows=300' || fail "first scan did not return 300 rows"
has 'scan1 mode=shared' || fail "first scan was not 'shared'"
grep -qE '^ *killing [1-9][0-9]* attachments *$' "$out.lf" || fail "no DuckDB attachment was killed"
grep -q '^scan2 rows=' "$out.lf" && fail "the scan after the kill did not fail"

join_errors=$(grep -oF -- "$JOIN_ERROR" "$out.lf" | wc -l)
all_errors=$(grep -oE '[A-Za-z]+ Error: ' "$out.lf" | wc -l)
[ "$join_errors" -ge 1 ] || fail "no 'could not join the scan's shared snapshot' error"
[ "$all_errors" -eq "$join_errors" ] || fail "$((all_errors - join_errors)) error(s) of another kind"

# The worker that failed must have discarded its connection, not returned
# it to the pool (a killed attachment fails the rollback and would otherwise
# be leased again by the next scan).
line=$(grep '^pool after failed scan ' "$out.lf" || true)
[[ "$line" =~ active=0\ discarded=([0-9]+) ]] || fail "unexpected pool stats: '$line'"
[ "${BASH_REMATCH[1]}" -ge 1 ] || fail "the failed scan discarded no connection: '$line'"

first_ok=""
for i in $(seq 3 $((RETRIES + 2))); do
    if has "scan${i} rows=300"; then
        if [ -z "$first_ok" ]; then
            first_ok=$i
            has "scan${i} mode=shared" || fail "scan $i after recovery was not 'shared'"
        fi
    elif [ -n "$first_ok" ]; then
        fail "scan $i failed after scan $first_ok had recovered"
    fi
done
[ -n "$first_ok" ] || fail "no scan recovered within $RETRIES retries"

line=$(grep '^pool after ' "$out.lf" || true)
[[ "$line" =~ active=0\ discarded=([0-9]+) ]] || fail "unexpected pool stats: '$line'"
[ "${BASH_REMATCH[1]}" -ge 1 ] || fail "no connection was discarded: '$line'"

echo "check_snapshot_join_failure: ok ($join_errors failed scan(s), recovered at scan $first_ok)"
