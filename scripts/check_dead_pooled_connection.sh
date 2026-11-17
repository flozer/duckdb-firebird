#!/usr/bin/env bash
# Check that a dead pooled connection is never handed out again (all Firebird
# versions), and that the transaction snapshot survives dead pooled
# connections and a lost coordinator safely (phases txsnap, txreuse).
#
# When a pooled connection's attachment is killed on the server, the next
# lease of it fails. That lease must discard it: before the fix, the
# metadata lease (firebird_health() and the other catalog functions), the
# PK probe and the scan's own cleanup all parked it back in the pool, so
# every later query on the ATTACH failed the same way, and the PK probe also
# memoised "no usable PK" for the table, so it was never partitioned again.
#
# Phases, each in its own DuckDB CLI session and ATTACH:
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
#          scan fails on it; the scan after that must return both rows;
#   idle:  only when FIREBIRD_IDLE_DB is set (Firebird 4+): a database whose
#          databases.conf entry sets ConnectionIdleTimeout = 1 (minute).
#          firebird_health() parks a connection, the server's idle timeout
#          kills it (connection shutdown / Idle timeout expired), and the
#          call after the failed one must work;
#   txsnap: the transaction snapshot (firebird_transaction_snapshot): dead
#          parked connections must not turn it off, and a coordinator killed
#          in the middle of a transaction must fail the next scan, pooled and
#          with the pool disabled (Firebird 3: no coordinator, scans work);
#   txreuse: Firebird 4+, in its own database txreuse.fdb, created next to
#          the partitions database (dropped at the end): another session pins the
#          snapshot number of a lost coordinator, and the scan that joins it
#          must be refused.
# At the end of the first phases firebird_pool_stats() must show nothing
# leased and at least one connection discarded.
#
# Uses PART_RANGE and PART_NO_LEVER from scripts/fixture_partitions.sql (read
# only). Needs a machine with at least 2 cores, so that the ATTACH scan is
# partitioned. txreuse derives its database from FB_ISQL_DB and
# FIREBIRD_PARTITIONS_DB, which must name the same file.
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
#   FIREBIRD_IDLE_DB        connection string of the idle-timeout database (unset: skip
#                           the idle phase)
#   IDLE_WAIT               seconds to wait for the idle kill (75)
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
FIREBIRD_IDLE_DB=${FIREBIRD_IDLE_DB:-}
IDLE_WAIT=${IDLE_WAIT:-75}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() { echo "check_dead_pooled_connection: FAILED: $*"; exit 1; }

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

# sleep_helper: prints a `.shell` command that waits IDLE_WAIT seconds.
sleep_helper() {
    local sh="$work/wait.sh"
    {
        echo '#!/usr/bin/env bash'
        echo "sleep $IDLE_WAIT"
    } > "$sh"
    chmod +x "$sh"
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            printf '@bash "%s"\r\n' "$(cygpath -m "$sh")" > "$work/wait.bat"
            cygpath -m "$work/wait.bat"
            ;;
        *) echo "$sh" ;;
    esac
}

# script_cmd <name>: writes stdin to $work/<name>.sh and prints the command
# that runs it from `.shell` (a .bat wrapper on Windows, as above).
script_cmd() {
    local sh="$work/$1.sh"
    { echo '#!/usr/bin/env bash'; cat; } > "$sh"
    chmod +x "$sh"
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            printf '@bash "%s"\r\n' "$(cygpath -m "$sh")" > "$work/$1.bat"
            cygpath -m "$work/$1.bat"
            ;;
        *) echo "$sh" ;;
    esac
}

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

if [ -n "$FIREBIRD_IDLE_DB" ]; then
    # Own ATTACH on the idle-timeout database (run_phase attaches fbd).
    run_phase idle <<SQL
ATTACH '${FIREBIRD_IDLE_DB}' AS fbi (TYPE firebird);
SELECT 'idle health0 rows=' || count(*) FROM firebird_health('fbi');
SELECT 'pool before active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbi');
.shell $(sleep_helper)
SELECT 'idle health1 rows=' || count(*) FROM firebird_health('fbi');
SELECT 'idle health2 rows=' || count(*) FROM firebird_health('fbi');
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbi');
SQL
fi

run_phase scan <<SQL
SELECT 'scan scan1 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'pool before active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
.shell $kill_cmd
SELECT 'scan scan2 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'scan scan3 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
SQL

# Transaction snapshot (opt-in): dead parked connections must not turn the
# option off for a transaction (the coordinator skips them), and a
# coordinator lost in the middle of a transaction fails the next scan
# instead of reading outside the snapshot. Firebird 3 has no snapshot
# numbers: its scans report 'serial' with the option on.
run_phase txsnap <<SQL
SELECT 'tx fb4=' || (CAST(split_part(engine_version, '.', 1) AS INTEGER) >= 4) FROM firebird_health('fbd');
SELECT 'tx scan0 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
.shell $kill_cmd
SET firebird_transaction_snapshot = true;
BEGIN;
SELECT 'tx scan1 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'tx scan1 mode=' || snapshot_mode FROM firebird_last_query();
SELECT 'tx scan2 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'tx scan2 mode=' || snapshot_mode FROM firebird_last_query();
COMMIT;
BEGIN;
SELECT 'tx scan3 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
.shell $kill_cmd
SELECT 'tx scan4 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
ROLLBACK;
BEGIN;
SELECT 'tx scan5 rows=' || count(*) FROM fbd.main.PART_NO_LEVER;
SELECT 'tx scan5 mode=' || snapshot_mode FROM firebird_last_query();
COMMIT;
SELECT 'pool after active=' || active_connections || ' discarded=' || total_discarded FROM firebird_pool_stats('fbd');
SET firebird_pool_enabled = false;
SET firebird_transaction_snapshot_idle_timeout = 0;
ATTACH '${FIREBIRD_PARTITIONS_DB}' AS fbn (TYPE firebird);
BEGIN;
SELECT 'tx scan6 rows=' || count(*) FROM fbn.main.PART_NO_LEVER;
.shell $kill_cmd
SELECT 'tx scan7 rows=' || count(*) FROM fbn.main.PART_NO_LEVER;
ROLLBACK;
SQL

# txreuse (Firebird 4+): a worker that joins a snapshot number must check
# that the coordinator still holds it. Firebird restarts snapshot numbers
# when a database is reopened, so after the coordinator is lost and the
# database closes, the same number can belong to another session: here
# session B pins it (both read RDB$GET_CONTEXT('SYSTEM', 'SNAPSHOT_NUMBER')
# through a view), so session A's join succeeds and only the check after
# the join can stop A from reading B's snapshot. Own database, pool off and
# no idle limit, so nothing else keeps it open.
reuse_note="txreuse: skipped (Firebird 3: no snapshot numbers)"
if grep -qx 'tx fb4=true' "$work/txsnap.out"; then
    base=${FB_ISQL_DB##*/}
    RE_ISQL_DB="${FB_ISQL_DB%/*}/txreuse.fdb"
    RE_DB="${FIREBIRD_PARTITIONS_DB/$base/txreuse.fdb}"
    [ "$RE_DB" != "$FIREBIRD_PARTITIONS_DB" ] ||
        fail "txreuse: FIREBIRD_PARTITIONS_DB does not contain '$base' (the file name in FB_ISQL_DB); can't derive the txreuse database"
    $FB_ISQL -q -u "$FB_USER" -p "$FB_PASS" "$RE_ISQL_DB" > /dev/null 2>&1 <<'DROPSQL' || true
DROP DATABASE;
DROPSQL
    $FB_ISQL -q > /dev/null <<CREATESQL
CREATE DATABASE '$RE_ISQL_DB' USER '$FB_USER' PASSWORD '$FB_PASS';
CREATE TABLE T (A INTEGER);
INSERT INTO T VALUES (1);
CREATE VIEW SN (N) AS SELECT RDB\$GET_CONTEXT('SYSTEM', 'SNAPSHOT_NUMBER') FROM RDB\$DATABASE;
COMMIT;
CREATESQL
    hold_cmd=$(echo 'sleep 10' | script_cmd reuse_hold)
    cat > "$work/reuse_b.sql" <<BSQL
${DUCKDB_PRELUDE}
SET firebird_transaction_snapshot = true;
ATTACH '${RE_DB}' AS fbr (TYPE firebird);
BEGIN;
SELECT 'reuse b sn=' || N FROM fbr.main.SN;
.shell $hold_cmd
COMMIT;
SELECT 'reuse b done';
BSQL
    step_cmd=$(script_cmd reuse_step <<STEP
$FB_ISQL -q -u "$FB_USER" -p "$FB_PASS" "$RE_ISQL_DB" > /dev/null <<'SQL'
DELETE FROM MON\$ATTACHMENTS WHERE MON\$ATTACHMENT_ID <> CURRENT_CONNECTION AND MON\$SYSTEM_FLAG = 0;
COMMIT;
SQL
sleep 2
$FB_ISQL -q -u "$FB_USER" -p "$FB_PASS" "$RE_ISQL_DB" > /dev/null <<'SQL'
UPDATE T SET A = 2;
COMMIT;
SQL
sleep 2
"$DUCKDB" -unsigned -list -noheader < "$work/reuse_b.sql" > "$work/reuse_b.raw" 2>&1 &
for _ in \$(seq 1 300); do
    grep -q '^reuse b sn=' "$work/reuse_b.raw" 2>/dev/null && break
    sleep 0.1
done
grep -q '^reuse b sn=' "$work/reuse_b.raw" 2>/dev/null || echo 'reuse b pin timeout'
STEP
)
    # Printed after A's scan only if B still holds its snapshot by then.
    holding_cmd=$(script_cmd reuse_holding <<HOLDING
grep -q 'reuse b done' "$work/reuse_b.raw" 2>/dev/null || echo 'reuse b still holding'
HOLDING
)
    run_phase txreuse <<SQL
SET firebird_pool_enabled = false;
SET firebird_transaction_snapshot_idle_timeout = 0;
SET firebird_transaction_snapshot = true;
ATTACH '${RE_DB}' AS fbr (TYPE firebird);
BEGIN;
SELECT 'reuse a sn=' || N FROM fbr.main.SN;
SELECT 'reuse a1=' || A FROM fbr.main.T;
.shell $step_cmd
SELECT 'reuse a2=' || A FROM fbr.main.T;
.shell $holding_cmd
ROLLBACK;
SQL
    for _ in $(seq 1 60); do
        grep -q 'reuse b done' "$work/reuse_b.raw" 2>/dev/null && break
        sleep 1
    done
    [ -f "$work/reuse_b.raw" ] || fail "txreuse: session B never started"
    tr -d '\r' < "$work/reuse_b.raw" > "$work/reuse_b.out"
    echo "=== session B"
    cat "$work/reuse_b.out"
    $FB_ISQL -q -u "$FB_USER" -p "$FB_PASS" "$RE_ISQL_DB" > /dev/null 2>&1 <<'DROPSQL' || true
DROP DATABASE;
DROPSQL
fi

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

# txsnap phase
[ "$(killed txsnap)" = 3 ] || fail "txsnap: the three kills did not all find DuckDB attachments"
tx_mode=serial
grep -qx 'tx fb4=true' "$work/txsnap.out" && tx_mode=transaction
for n in 1 2 5; do
    grep -qx "tx scan$n rows=2" "$work/txsnap.out" || fail "txsnap: scan $n of PART_NO_LEVER failed"
    grep -qx "tx scan$n mode=$tx_mode" "$work/txsnap.out" ||
        fail "txsnap: scan $n reported $(value txsnap "tx scan$n mode=(.*)"), expected $tx_mode"
done
! grep -q '^tx scan4 rows=' "$work/txsnap.out" ||
    fail "txsnap: the scan after the coordinator was killed did not fail"
# On Firebird 4+ it must fail because the snapshot is gone, not for any
# other reason (Firebird 3 has no coordinator: a dead parked worker fails it).
if [ "$tx_mode" = transaction ]; then
    grep -q "Firebird ATTACH 'fbd': the snapshot of this DuckDB transaction is gone" "$work/txsnap.out" ||
        fail "txsnap: the scan after the coordinator was killed did not report the lost snapshot"
fi
[ -n "$(value txsnap 'pool after active=0 discarded=([0-9]+)')" ] ||
    fail "txsnap: a connection is still counted as leased at the end"
# Pool disabled, no idle limit: the scan after the coordinator was killed
# must notice it itself (a new worker connection would otherwise join
# whatever snapshot holds that number after the database reopens). On
# Firebird 3 there is no coordinator, and the scan simply works.
grep -qx 'tx scan6 rows=2' "$work/txsnap.out" || fail "txsnap: scan 6 (pool disabled) failed"
if [ "$tx_mode" = transaction ]; then
    ! grep -q '^tx scan7 rows=' "$work/txsnap.out" &&
        grep -q "Firebird ATTACH 'fbn': the snapshot of this DuckDB transaction is gone" "$work/txsnap.out" ||
        fail "txsnap: with the pool disabled, the scan after the coordinator was killed did not report the lost snapshot"
else
    grep -qx 'tx scan7 rows=2' "$work/txsnap.out" || fail "txsnap: scan 7 (pool disabled) failed on Firebird 3"
fi
tx_note="txsnap: dead parked connections skipped (mode $tx_mode), lost coordinator fails the next scan (pooled and unpooled)"
[ "$tx_mode" = serial ] && tx_note="txsnap: Firebird 3, no coordinator; scans report serial and work after the kills"

# txreuse phase
if grep -qx 'tx fb4=true' "$work/txsnap.out"; then
    a_sn=$(value txreuse 'reuse a sn=([0-9]+)')
    b_sn=$(sed -nE 's/^reuse b sn=([0-9]+)$/\1/p' "$work/reuse_b.out" | head -n 1)
    grep -qx 'reuse a1=1' "$work/txreuse.out" || fail "txreuse: the first scan did not read A = 1"
    # B must have pinned before A's second scan and held on after it, or the
    # refusal below would not prove the check after the join.
    ! grep -q 'reuse b pin timeout' "$work/txreuse.out" ||
        fail "txreuse: session B did not pin its snapshot within 30 s"
    grep -q 'reuse b still holding' "$work/txreuse.out" ||
        fail "txreuse: session B released its snapshot before session A's second scan finished"
    [ -n "$a_sn" ] && [ "$a_sn" = "$b_sn" ] ||
        fail "txreuse: session B did not pin the same snapshot number (A ${a_sn:-?}, B ${b_sn:-?}); the check proves nothing"
    ! grep -q '^reuse a2=' "$work/txreuse.out" ||
        fail "txreuse: after the coordinator was lost, the scan read $(value txreuse 'reuse a2=(.*)') from another session's snapshot"
    grep -q "Firebird ATTACH 'fbr': the snapshot of this DuckDB transaction is gone" "$work/txreuse.out" ||
        fail "txreuse: the scan after the coordinator was lost did not report the lost snapshot"
    reuse_note="txreuse: snapshot number $a_sn reused by another session, the join was refused"
fi

idle_note="idle: skipped (FIREBIRD_IDLE_DB unset)"
if [ -n "$FIREBIRD_IDLE_DB" ]; then
    i0=$(value idle 'pool before active=0 discarded=([0-9]+)')
    [ -n "$i0" ] || fail "idle: a connection was already leased before the wait"
    grep -q 'Idle timeout expired' "$work/idle.out" ||
        fail "idle: the server's idle timeout did not kill the parked connection (no 'Idle timeout expired')"
    grep -qx 'idle health2 rows=1' "$work/idle.out" ||
        fail "idle: firebird_health() still failed on the call after the failed one (the dead connection was parked again)"
    i1=$(value idle 'pool after active=0 discarded=([0-9]+)')
    [ -n "$i1" ] || fail "idle: a connection is still counted as leased at the end"
    [ "$i1" -gt "$i0" ] || fail "idle: no connection was discarded ($i0 -> $i1)"
    idle_note="idle: the call after the idle kill works, discarded $((i1 - i0))"
fi

echo "check_dead_pooled_connection: ok (lease: the call after the failed one works, discarded $((d1 - d0)); probe: scans ok from scan $ok_from, partitions=$last, discarded $((p1 - p0)); scan: the scan after the failed one works, discarded $((s1 - s0)); $idle_note; $tx_note; $reuse_note; nothing left leased)"
