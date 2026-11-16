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

checks="$total scans"
[ "$dbkey_method" = dbkey ] && checks="$checks + the DB_KEY range check"
echo "== $checks: $failures failed, $torn torn reads (mode: $expected_mode)"
if [ "$expected_mode" = independent ]; then
    echo "   (Firebird 3 fallback: torn reads are the pre-existing behavior and only reported)"
fi
[ "$failures" -eq 0 ]
