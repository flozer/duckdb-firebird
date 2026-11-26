"""Writes must commit even when the client reads one row and closes.

A DuckDB statement whose result is streamed commits only once the result
is fully read, so a client that read a write's single row with fetchone()
and closed the connection would lose the write without an error. On DuckDB
1.5 firebird_execute has DuckDB materialize its result, as DuckDB does for
INSERT; on DuckDB 2.0, in autocommit, it commits its Firebird write before
returning the row (it must then be the whole statement).

Between BEGIN and COMMIT the write belongs to the transaction: a COMMIT
after fetchone() keeps it, a ROLLBACK drops it.

sqllogictest reads every result fully, so this needs a real client.

Usage: python check_execute_fetchone.py <firebird.duckdb_extension> <connection string>
Needs the Python duckdb package of the same version as the extension, and
scripts/fixture_rw.sql loaded in the database.
"""
import os
import sys
import threading

import duckdb

ext, cs = sys.argv[1], sys.argv[2]


def connect():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{cs}' AS rw (TYPE firebird, READ_WRITE)")
    return con


def count(marker):
    con = connect()
    n = con.execute(f"SELECT count(*) FROM rw.main.RW_LOG WHERE ID = {marker}").fetchone()[0]
    con.close()
    return n


con = connect()
con.execute("SELECT * FROM firebird_execute('rw', 'DELETE FROM RW_LOG')").fetchall()
con.close()

failures = 0
cases = {
    9001: "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_LOG (ID, NOTE) VALUES (9001, ''fetchone'')')",
    9002: "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_LOG (ID, NOTE) VALUES (?, ?)', "
          "source := 'SELECT 9002, ''fetchone''')",
}
for marker, sql in cases.items():
    con = connect()
    con.execute(sql).fetchone()
    con.close()
    n = count(marker)
    status = "ok" if n == 1 else "FAILED"
    if n != 1:
        failures += 1
    print(f"{status}: {sql[:70]}... -> {n} row(s) after close")

# Between BEGIN and COMMIT: the transaction decides, after fetchone().
for marker, end, expected in ((9101, "COMMIT", 1), (9102, "ROLLBACK", 0)):
    con = connect()
    con.execute("BEGIN")
    con.execute(
        f"SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_LOG (ID, NOTE) VALUES ({marker}, ''{end.lower()}'')')"
    ).fetchone()
    con.execute(end)
    con.close()
    n = count(marker)
    status = "ok" if n == expected else "FAILED"
    if n != expected:
        failures += 1
    print(f"{status}: BEGIN, fetchone(), {end} -> {n} row(s) after close (expected {expected})")

# Three calls with a source in one statement, on fewer threads than calls:
# they wait for each other on the transaction's write connection while each
# runs its source query, which must still make progress (no hang). Between
# BEGIN and COMMIT: on DuckDB 2.0 an autocommit call must be the whole
# statement.
con = connect()
con.execute("SET threads = 2")
union = " UNION ALL ".join(
    f"SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_LOG (ID, NOTE) VALUES (?, ?)', "
    f"source := 'SELECT {base} + i, ''union'' FROM range(3000) t(i)')"
    for base in (100000, 200000, 300000))
outcome = {}


def run_union():
    try:
        con.execute("BEGIN")
        outcome["rows"] = con.execute(union).fetchall()
        con.execute("COMMIT")
    except Exception as e:  # noqa: BLE001 - reported below
        outcome["error"] = str(e).splitlines()[0][:200]


worker = threading.Thread(target=run_union, daemon=True)
worker.start()
worker.join(120)
if worker.is_alive():
    print("FAILED: three source calls under UNION ALL with 2 threads still run after 120 s")
    sys.stdout.flush()
    os._exit(1)
if outcome.get("rows") != [(3000, 3000)] * 3:
    failures += 1
    print(f"FAILED: three source calls under UNION ALL -> {outcome}")
else:
    print("ok: three source calls under UNION ALL with 2 threads")
con.close()

con = connect()
con.execute("SELECT * FROM firebird_execute('rw', 'DELETE FROM RW_LOG')").fetchall()
con.close()

if failures:
    sys.exit(f"check_execute_fetchone: {failures} case(s) lost their writes")
print("check_execute_fetchone: ok")
