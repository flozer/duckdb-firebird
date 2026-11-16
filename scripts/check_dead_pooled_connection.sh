#!/usr/bin/env bash
# Check that a dead pooled connection is never handed out again (all Firebird
# versions).
#
# When a pooled connection's attachment is killed on the server, the next
# lease of it fails. That lease must discard it: before the fix, the
# metadata lease (firebird_health() and the other catalog functions), the
# PK probe and the scan's own cleanup all parked it back in the pool, so
# every later query on the ATTACH failed the same way, and the PK probe also
# memoised "no usable PK" for the table, so it was never partitioned again.
#
# Three phases, each in its own DuckDB CLI session and ATTACH:
#   lease: firebird_health() parks a pooled connection; its attachment is
#          killed from isql via `.shell` (DELETE FROM MON$ATTACHMENTS); the
#          next firebird_health() may fail, but the one after it must work;
#   probe: the table list is loaded (information_schema) and a connection
#          parked by firebird_health(), then killed, so the PK probe of the
#          first scan of PART_RANGE runs on the dead connection. Every later
#          scan must return all 300 rows, at the latest from the second one
#          on, and once the probe has run on a live connection the scan must
#          be partitioned again (partitions > 1);
#   scan:  a first scan of PART_NO_LEVER (no PK: one partition, no shared
#          snapshot) leaves its connection parked; it is killed, so the next
#          scan fails on it; the scan after that must return both rows.
# At the end of each phase firebird_pool_stats() must show nothing leased and
# at least one connection discarded.
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

SCANS=4

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

kill_cmd=$(kill_helper)

# run_phase <name>: runs one phase in its own CLI session and ATTACH, reading
# its SQL from stdin; the output goes to $work/<name>.out (CRs stripped).
run_phase() {
    local out="$work/$1.out"
    # No -bail: the failing queries are expected; the output is checked below.
    { echo "${DUCKDB_PRELUDE}"; echo "ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbd (TYPE firebird);"; cat; } |
        "$DUCKDB" -unsigned -list -noheader > "$out.raw" 2>&1 || true
    tr -d '\r' < "$out.raw" > "$out"
    echo "=== phase $1"
    cat "$out"
}

run_phase lease <<SQL
SELECT 'lease health0 rows=' || count(*) FROM firebird_health('fbd');
SELECT 'pool before active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
.shell $kill_cmd
SELECT 'lease health1 rows=' || count(*) FROM firebird_health('fbd');
SELECT 'lease health2 rows=' || count(*) FROM firebird_health('fbd');
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
SQL

{
    echo "SELECT 'probe tables=' || count(*) FROM information_schema.tables WHERE table_catalog = 'fbd';"
    echo "SELECT 'probe health rows=' || count(*) FROM firebird_health('fbd');"
    echo "SELECT 'pool before active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');"
    echo ".shell $kill_cmd"
    for i in $(seq 1 "$SCANS"); do
        echo "SELECT 'probe scan$i rows=' || count(*) FROM fbd.main.PART_RANGE;"
        echo "SELECT 'probe scan$i partitions=' || partitions FROM firebird_last_query();"
    done
    echo "SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');"
} | run_phase probe

run_phase scan <<SQL
SELECT 'scan scan1 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'pool before active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
.shell $kill_cmd
SELECT 'scan scan2 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'scan scan3 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
SQL

fail() { echo "check_dead_pooled_connection: FAILED: $*"; exit 1; }
# value <phase> <regex with one group>: the group of the first matching line.
value() { sed -nE "s/^$2$/\1/p" "$work/$1.out" | head -n 1; }
killed() { grep -cE '^ *targeting [1-9][0-9]* attachments *$' "$work/$1.out" || true; }

# lease phase
[ "$(killed lease)" = 1 ] || fail "lease: the kill found no DuckDB attachment"
d0=$(value lease 'pool before active=0 discarded=([0-9]+)')
[ -n "$d0" ] || fail "lease: a connection was already leased before the kill"
grep -qx 'lease health2 rows=1' "$work/lease.out" ||
    fail "lease: firebird_health() still failed on the call after the failed one (the dead connection was parked again)"
d1=$(value lease 'pool after active=0 discarded=([0-9]+)')
[ -n "$d1" ] || fail "lease: a connection is still counted as leased at the end"
[ "$d1" -gt "$d0" ] || fail "lease: no connection was discarded ($d0 -> $d1)"

# probe phase
[ "$(killed probe)" = 1 ] || fail "probe: the kill found no DuckDB attachment"
grep -qE '^probe tables=[1-9]' "$work/probe.out" || fail "probe: the table list did not load before the kill"
p0=$(value probe 'pool before active=0 discarded=([0-9]+)')
[ -n "$p0" ] || fail "probe: a connection was already leased before the kill"
ok_from=""
for i in $(seq 1 "$SCANS"); do
    if grep -qx "probe scan$i rows=300" "$work/probe.out"; then
        [ -n "$ok_from" ] || ok_from=$i
    elif [ -n "$ok_from" ]; then
        fail "probe: scan $i failed after scan $ok_from had succeeded"
    fi
done
[ -n "$ok_from" ] && [ "$ok_from" -le 2 ] ||
    fail "probe: no scan of PART_RANGE returned 300 rows by the second one (first good scan: ${ok_from:-none})"
last=$(value probe "probe scan$SCANS partitions=([0-9]+)")
[ "${last:-0}" -gt 1 ] ||
    fail "probe: PART_RANGE is no longer partitioned (partitions=${last:-?}); the PK probe memoised its failure"
p1=$(value probe 'pool after active=0 discarded=([0-9]+)')
[ -n "$p1" ] || fail "probe: a connection is still counted as leased at the end"
[ "$p1" -gt "$p0" ] || fail "probe: no connection was discarded ($p0 -> $p1)"

# scan phase
[ "$(killed scan)" = 1 ] || fail "scan: the kill found no DuckDB attachment"
grep -qx 'scan scan1 rows=2' "$work/scan.out" || fail "scan: the first scan of PART_NO_LEVER failed"
s0=$(value scan 'pool before active=0 discarded=([0-9]+)')
[ -n "$s0" ] || fail "scan: a connection was already leased before the kill"
grep -qx 'scan scan3 rows=2' "$work/scan.out" ||
    fail "scan: the scan after the failed one failed too (the dead connection was parked again)"
s1=$(value scan 'pool after active=0 discarded=([0-9]+)')
[ -n "$s1" ] || fail "scan: a connection is still counted as leased at the end"
[ "$s1" -gt "$s0" ] || fail "scan: no connection was discarded ($s0 -> $s1)"

echo "check_dead_pooled_connection: ok (lease: the call after the failed one works, discarded $((d1 - d0)); probe: scans ok from scan $ok_from, partitions=$last, discarded $((p1 - p0)); scan: the scan after the failed one works, discarded $((s1 - s0)); nothing left leased)"
