#!/usr/bin/env bash
# Shared-snapshot consistency check for partitioned scans.
#
# sqllogictest cannot interleave a concurrent writer with a scan, so this
# script proves the guarantee directly. A writer keeps moving value between
# rows that live in DIFFERENT partitions, committing every transfer:
#
#   ACC(ID, VAL): 2,000,000 rows with VAL = 100 (ID 1..2000000), plus one
#   row with ID 10000000 and VAL = 0 (widens the PK span to 10M so the
#   ATTACH path partitions on its own)        -- fixture_shared_snapshot.sql
#   writer: UPDATE ACC SET VAL = VAL - 1 WHERE ID = <low half>;
#           UPDATE ACC SET VAL = VAL + 1 WHERE ID = <high half>;  COMMIT;
#                                             -- shared_snapshot_writer.sql
#
# SUM(VAL) is therefore 200,000,000 in every committed state. A partitioned
# scan whose partitions all read one snapshot always returns exactly that;
# partitions reading at different moments can catch a transfer half-way
# (debit seen, credit not, or the reverse) and return something else.
#
# Per Firebird version:
#   * 4+ : every scan reports snapshot_mode = 'shared' and returns exactly
#          200,000,000 / 2,000,001, while the writer committed transfers
#          during the scan. Any deviation fails the script.
#   * 3  : the fallback path. Every scan reports 'independent' and must
#          complete; its sums are only reported (they are expected to
#          drift, which also shows the writer really interleaves).
# The firebird_scan(..., partitions=4) path (PK ranges and, with
# partition_by='dbkey', DB_KEY ranges) and the ATTACH path are checked.
#
# Defaults match the Linux FB matrix (server in the `fb` container, port
# 3050). Override for local runs:
#   FB_ISQL            command running isql      (docker exec -i fb isql)
#   FB_DB_PATH         database file on the server (/var/lib/firebird/data/snapshot.fdb)
#   FB_ISQL_DB         database as isql opens it (localhost:<FB_DB_PATH>: through the
#                      server, never embedded, so it can share the file with it)
#   FIREBIRD_SNAPSHOT_DB  connection string for the extension
#                      (firebird://SYSDBA:masterkey@localhost:3050<FB_DB_PATH>?charset=UTF8)
#   DUCKDB             DuckDB CLI with the extension linked in (./build/release/duckdb)
#   DUCKDB_PRELUDE     SQL run before each scan, e.g. a LOAD    (empty)
#   SCANS              scans per path (4)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

FB_ISQL=${FB_ISQL:-"docker exec -i fb isql"}
FB_DB_PATH=${FB_DB_PATH:-/var/lib/firebird/data/snapshot.fdb}
FB_ISQL_DB=${FB_ISQL_DB:-"localhost:${FB_DB_PATH}"}
FIREBIRD_SNAPSHOT_DB=${FIREBIRD_SNAPSHOT_DB:-"firebird://SYSDBA:masterkey@localhost:3050${FB_DB_PATH}?charset=UTF8"}
DUCKDB=${DUCKDB:-./build/release/duckdb}
DUCKDB_PRELUDE=${DUCKDB_PRELUDE:-}
SCANS=${SCANS:-4}
FB_USER=${ISC_USER:-SYSDBA}
FB_PASS=${ISC_PASSWORD:-masterkey}

EXPECTED_SUM=200000000
EXPECTED_COUNT=2000001

isql_db() { $FB_ISQL -q -u "$FB_USER" -p "$FB_PASS" "$FB_ISQL_DB"; }
isql_value() {  # single value from a single-row query
    # isql reading a pipe prints a "Database: ..." banner and SQL>/CON>
    # prompts; strip them and keep the first remaining token.
    printf 'SET HEADING OFF;\n%s\n' "$1" | isql_db | tr -d '\r' |
        sed -e 's/\(SQL\|CON\)> //g' | awk '$1 != "Database:" && NF { print $1; exit }'
}

work=$(mktemp -d)
writer_pid=
cleanup() {
    if [ -n "$writer_pid" ] && kill -0 "$writer_pid" 2>/dev/null; then
        printf 'INSERT INTO WRITER_STOP VALUES (1);\nCOMMIT;\n' | isql_db >/dev/null 2>&1 || true
        wait "$writer_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

echo "== creating $FB_ISQL_DB"
printf "CREATE DATABASE '%s' USER '%s' PASSWORD '%s' DEFAULT CHARACTER SET UTF8;\n" \
    "$FB_ISQL_DB" "$FB_USER" "$FB_PASS" | $FB_ISQL -q >/dev/null
isql_db >/dev/null <"$here/fixture_shared_snapshot.sql"
rows=$(isql_value 'SELECT COUNT(*) FROM ACC;')
if [ "$rows" != "$EXPECTED_COUNT" ]; then
    echo "FAIL: fixture has '$rows' rows, expected $EXPECTED_COUNT"; exit 1
fi

major=$(isql_value "SELECT RDB\$GET_CONTEXT('SYSTEM', 'ENGINE_VERSION') FROM RDB\$DATABASE;" | cut -d. -f1)
echo "== Firebird engine major version: $major"
if [ "$major" -ge 4 ]; then expected_mode=shared; else expected_mode=independent; fi
# partition_by='dbkey' uses DB_KEY ranges on FB4/FB5 and PK ranges otherwise.
case $major in 4|5) dbkey_method=dbkey ;; *) dbkey_method=pk ;; esac

echo "== starting writer"
isql_db <"$here/shared_snapshot_writer.sql" >"$work/writer.log" 2>&1 &
writer_pid=$!
for _ in $(seq 1 30); do
    [ "$(isql_value 'SELECT TRANSFERS FROM WRITER_STATE;')" -gt 0 ] && break
    sleep 1
done
if [ "$(isql_value 'SELECT TRANSFERS FROM WRITER_STATE;')" -eq 0 ]; then
    echo "FAIL: writer did not start"; cat "$work/writer.log"; exit 1
fi

failures=0
torn=0
total=0

# run_scan <label> <scan SQL> <expected partition_method>: runs one
# partitioned scan, checks its mode, method and sum.
run_scan() {
    local label=$1 scan_sql=$2 expected_method=$3 before after out sum count mode parallel method
    before=$(isql_value 'SELECT TRANSFERS FROM WRITER_STATE;')
    out=$(printf '%s\n%s\nSELECT snapshot_mode, parallel_scan, partition_method FROM firebird_last_query();\n' \
            "$DUCKDB_PRELUDE" "$scan_sql" | "$DUCKDB" -csv -noheader 2>&1 | tr -d '\r') || true
    after=$(isql_value 'SELECT TRANSFERS FROM WRITER_STATE;')
    sum=$(echo "$out" | sed -n 1p | cut -d, -f1)
    count=$(echo "$out" | sed -n 1p | cut -d, -f2)
    mode=$(echo "$out" | sed -n 2p | cut -d, -f1)
    parallel=$(echo "$out" | sed -n 2p | cut -d, -f2)
    method=$(echo "$out" | sed -n 2p | cut -d, -f3)
    total=$((total + 1))
    local verdict=ok
    if [ "$mode" != "$expected_mode" ] || [ "$parallel" != "true" ]; then
        verdict="FAIL (snapshot_mode=$mode parallel_scan=$parallel, expected $expected_mode/true)"
    elif [ "$method" != "$expected_method" ]; then
        verdict="FAIL (partition_method=$method, expected $expected_method)"
    elif [ "$count" != "$EXPECTED_COUNT" ]; then
        verdict="FAIL (count $count, expected $EXPECTED_COUNT)"
    elif [ "$sum" != "$EXPECTED_SUM" ]; then
        torn=$((torn + 1))
        if [ "$expected_mode" = shared ]; then
            verdict="FAIL (sum $sum, expected $EXPECTED_SUM)"
        else
            verdict="torn read (expected without a shared snapshot)"
        fi
    elif [ "$after" -le "$before" ]; then
        verdict="FAIL (no transfer committed during the scan; writer stalled?)"
    fi
    echo "  $label: sum=$sum count=$count mode=$mode method=$method transfers_during_scan=$((after - before)) -> $verdict"
    case $verdict in FAIL*) failures=$((failures + 1)); echo "$out" | sed 's/^/    | /' ;; esac
}

echo "== firebird_scan(..., partitions=4)"
for i in $(seq 1 "$SCANS"); do
    run_scan "scan $i" \
        "SELECT SUM(VAL), COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'ACC', partitions=4);" pk
done

echo "== firebird_scan(..., partitions=4, partition_by='dbkey')  (PK ranges on FB3)"
for i in $(seq 1 "$SCANS"); do
    run_scan "scan $i" \
        "SELECT SUM(VAL), COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'ACC', partitions=4, partition_by='dbkey');" "$dbkey_method"
done

if [ "$dbkey_method" = dbkey ]; then
    # Exact sums don't show that the table was really split: a wrong record
    # space would put every row in one DB_KEY range. Count each partition's
    # range with isql: all four must hold rows, and together all of them.
    echo "== DB_KEY ranges of one partitions=4 scan"
    preds=$(cat <<SQL | "$DUCKDB" -list -noheader 2>&1 | tr -d '\r'
$DUCKDB_PRELUDE
SET firebird_query_log_size = 8;
SELECT COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'ACC', partitions=4, partition_by='dbkey');
SELECT regexp_extract(remote_sql, 'WHERE \((.*)\)\$', 1) FROM firebird_query_log();
SQL
)
    nranges=0 nempty=0 nrows=0
    while IFS= read -r pred; do
        case $pred in *MAKE_DBKEY*) ;; *) continue ;; esac
        n=$(isql_value "SELECT COUNT(*) FROM ACC WHERE $pred;")
        echo "  $pred -> $n rows"
        nranges=$((nranges + 1)); nrows=$((nrows + n))
        [ "$n" -gt 0 ] || nempty=$((nempty + 1))
    done <<<"$preds"
    if [ "$nranges" -ne 4 ] || [ "$nempty" -ne 0 ] || [ "$nrows" -ne "$EXPECTED_COUNT" ]; then
        echo "  FAIL ($nranges ranges, $nempty empty, $nrows rows; expected 4, 0, $EXPECTED_COUNT)"
        echo "$preds" | sed 's/^/    | /'
        failures=$((failures + 1))
    fi
fi

echo "== ATTACH (automatic partitioning, pooled connections)"
for i in $(seq 1 "$SCANS"); do
    run_scan "scan $i" \
        "ATTACH '$FIREBIRD_SNAPSHOT_DB' AS fbs (TYPE firebird); SELECT SUM(VAL), COUNT(*) FROM fbs.main.ACC;" pk
done

# Transaction snapshot (#104): with SET firebird_transaction_snapshot = true,
# every scan of an ATTACH in one DuckDB transaction reads one snapshot
# (Firebird 4+). Firebird 3 falls back to per-scan behavior: reported only.
echo "== ATTACH, transaction snapshot (SET firebird_transaction_snapshot = true)"
tx_failures=0
tx_session() {  # tx_session <sql>: one CLI session with the option on, output on stdout
    printf '%s\nSET firebird_transaction_snapshot = true;\nATTACH '"'"'%s'"'"' AS fbs (TYPE firebird);\n%s\n' \
        "$DUCKDB_PRELUDE" "$FIREBIRD_SNAPSHOT_DB" "$1" | "$DUCKDB" -list -noheader 2>&1 | tr -d '\r' || true
}
tx_expect() {  # tx_expect <label> <expected> <actual> <output> <Firebird 3 pattern>
    if [ "$3" = "$2" ]; then
        echo "  $1: $3 -> ok"
    elif [ "$expected_mode" != shared ] && echo "$3" | grep -qxE "$5"; then
        # Firebird 3: the statement must work and report the per-scan
        # mode; only the values may drift.
        echo "  $1: $3, expected $2 -> torn (expected without a shared snapshot)"
    else
        echo "  $1: $3, expected $2 -> FAIL"
        echo "$4" | sed 's/^/    | /'
        tx_failures=$((tx_failures + 1))
    fi
}
# Firebird 3 has no snapshot numbers: the scans report what they report
# without the option (partitioned: independent, serial: serial).
tx_mode=transaction tx_serial_mode=transaction
[ "$expected_mode" = shared ] || { tx_mode=independent; tx_serial_mode=serial; }
for i in $(seq 1 "$SCANS"); do
    out=$(tx_session "SELECT 'sum=' || ((SELECT SUM(VAL) FROM fbs.main.ACC WHERE ID <= 1000000) + (SELECT SUM(VAL) FROM fbs.main.ACC WHERE ID > 1000000));
SELECT 'mode=' || snapshot_mode FROM firebird_last_query();")
    tx_expect "one statement, two scans $i" "sum=$EXPECTED_SUM mode=$tx_mode" \
        "$(echo "$out" | grep -E '^(sum|mode)=' | tr '\n' ' ' | sed 's/ $//')" "$out" \
        'sum=[0-9]+ mode=independent'
    out=$(tx_session "BEGIN;
CREATE TEMP TABLE lo AS SELECT SUM(VAL) s FROM fbs.main.ACC WHERE ID <= 1000000;
CREATE TEMP TABLE hi AS SELECT SUM(VAL) s FROM fbs.main.ACC WHERE ID > 1000000;
SELECT 'sum=' || (lo.s + hi.s) FROM lo, hi;
COMMIT;")
    tx_expect "two statements, one transaction $i" "sum=$EXPECTED_SUM" \
        "$(echo "$out" | grep -E '^sum=' | head -1)" "$out" 'sum=[0-9]+'
    # Serial scans join too: the one-row WRITER_STATE read twice in one
    # transaction, with a full scan of ACC in between while the writer
    # commits, must return the same counter.
    out=$(tx_session "BEGIN;
CREATE TEMP TABLE c1 AS SELECT TRANSFERS t FROM fbs.main.WRITER_STATE;
SELECT 'serial mode=' || snapshot_mode || ' partitions=' || partitions FROM firebird_last_query();
SELECT 'in between=' || COUNT(*) FROM fbs.main.ACC;
CREATE TEMP TABLE c2 AS SELECT TRANSFERS t FROM fbs.main.WRITER_STATE;
SELECT 'same=' || (c1.t = c2.t) FROM c1, c2;
COMMIT;")
    tx_expect "serial scan read twice $i" "serial mode=$tx_serial_mode partitions=1 same=true" \
        "$(echo "$out" | grep -E '^(serial mode|same)=' | tr '\n' ' ' | sed 's/ $//')" "$out" \
        'serial mode=serial partitions=1 same=(true|false)'
done

if [ "$expected_mode" = shared ]; then
    # The idle limit: the server closes the coordinator after
    # firebird_transaction_snapshot_idle_timeout seconds between two
    # statements of the transaction; the next scan fails and names the
    # setting. A statement that runs longer than the limit is not idle time.
    wait_sh="$work/wait.sh"
    printf '#!/usr/bin/env bash\nsleep 8\n' > "$wait_sh"; chmod +x "$wait_sh"
    wait_cmd=$wait_sh
    case "${OSTYPE:-}" in
        msys* | cygwin*)
            printf '@bash "%s"\r\n' "$(cygpath -m "$wait_sh")" > "$work/wait.bat"
            wait_cmd=$(cygpath -m "$work/wait.bat") ;;
    esac
    out=$(tx_session "SET firebird_transaction_snapshot_idle_timeout = 2;
BEGIN;
SELECT 'idle scan1=' || TRANSFERS FROM fbs.main.WRITER_STATE;
SELECT 'idle scan2=' || TRANSFERS FROM fbs.main.WRITER_STATE;
.timer on
SELECT 'idle long statement=' || COUNT(*) FROM fbs.main.ACC a JOIN fbs.main.ACC b USING (ID) JOIN fbs.main.ACC c USING (ID);
.timer off
SELECT 'idle scan3=' || TRANSFERS FROM fbs.main.WRITER_STATE;
.shell $wait_cmd
SELECT 'idle scan4=' || TRANSFERS FROM fbs.main.WRITER_STATE;
ROLLBACK;
SELECT 'idle after rollback=' || COUNT(*) FROM fbs.main.WRITER_STATE;
SELECT 'idle pool active=' || active_connections FROM firebird_pool_stats('fbs');")
    t1=$(echo "$out" | sed -n 's/^idle scan1=//p')
    # The long statement must outlast the limit as the server applies it
    # (limit + 1 s), or it proves nothing.
    long_s=$(echo "$out" | sed -n 's/^Run Time (s): real \([0-9.]*\).*/\1/p' | head -1)
    long_ok=$(awk -v t="${long_s:-0}" 'BEGIN { print (t > 3) ? 1 : 0 }')
    if [ -n "$t1" ] && echo "$out" | grep -qx "idle scan2=$t1" && [ "$long_ok" = 1 ] &&
       echo "$out" | grep -qx 'idle long statement=2000001' && echo "$out" | grep -qx "idle scan3=$t1" &&
       ! echo "$out" | grep -q '^idle scan4=' &&
       echo "$out" | grep -q 'firebird_transaction_snapshot_idle_timeout (2 s)' &&
       echo "$out" | grep -qx 'idle after rollback=1' && echo "$out" | grep -qx 'idle pool active=0'; then
        echo "  idle limit (2 s): statements back to back and a ${long_s} s statement keep the snapshot; the scan after 8 s idle failed and named the setting; after ROLLBACK scans work -> ok"
    else
        echo "  idle limit (2 s; long statement ${long_s:-?} s, must exceed 3 s) -> FAIL"
        echo "$out" | sed 's/^/    | /'
        tx_failures=$((tx_failures + 1))
    fi
    # The server really gets the limit: between two statements the
    # coordinator's MON$IDLE_TIMEOUT is limit + 1 (30 s here, so slow
    # connects can't expire it), and it is reset at COMMIT.
    out=$(tx_session "SET firebird_transaction_snapshot_idle_timeout = 30;
BEGIN;
SELECT 'armed scan=' || COUNT(*) FROM fbs.main.WRITER_STATE;
SELECT 'armed=' || COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'MON\$ATTACHMENTS') WHERE \"MON\$IDLE_TIMEOUT\" = 31;
COMMIT;
SELECT 'armed after commit=' || COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'MON\$ATTACHMENTS') WHERE \"MON\$IDLE_TIMEOUT\" = 31;")
    if echo "$out" | grep -qx 'armed scan=1' && echo "$out" | grep -qx 'armed=1' &&
       echo "$out" | grep -qx 'armed after commit=0'; then
        echo "  idle limit 30 s: armed on the server as 31 between statements, reset at COMMIT -> ok"
    else
        echo "  idle limit 30 s armed on the server -> FAIL"
        echo "$out" | sed 's/^/    | /'
        tx_failures=$((tx_failures + 1))
    fi
    # The largest limit (2147483646 s) is armed as 2^31 - 1 on the server and
    # reset at COMMIT.
    out=$(tx_session "SET firebird_transaction_snapshot_idle_timeout = 2147483646;
BEGIN;
SELECT 'max scan=' || COUNT(*) FROM fbs.main.WRITER_STATE;
SELECT 'max armed=' || COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'MON\$ATTACHMENTS') WHERE \"MON\$IDLE_TIMEOUT\" = 2147483647;
COMMIT;
SELECT 'max after commit=' || COUNT(*) FROM firebird_scan('$FIREBIRD_SNAPSHOT_DB', 'MON\$ATTACHMENTS') WHERE \"MON\$IDLE_TIMEOUT\" = 2147483647;")
    if echo "$out" | grep -qx 'max scan=1' && echo "$out" | grep -qx 'max armed=1' &&
       echo "$out" | grep -qx 'max after commit=0'; then
        echo "  idle limit 2147483646 s: armed on the server as 2147483647, reset at COMMIT -> ok"
    else
        echo "  idle limit 2147483646 s -> FAIL"
        echo "$out" | sed 's/^/    | /'
        tx_failures=$((tx_failures + 1))
    fi
fi
failures=$((failures + tx_failures))

checks="$total scans"
[ "$dbkey_method" = dbkey ] && checks="$checks + the DB_KEY range check"
checks="$checks + the transaction snapshot checks"
echo "== $checks: $failures failed, $torn torn reads (mode: $expected_mode)"
if [ "$expected_mode" = independent ]; then
    echo "   (Firebird 3 fallback: torn reads are the pre-existing behavior and only reported)"
fi
[ "$failures" -eq 0 ]
