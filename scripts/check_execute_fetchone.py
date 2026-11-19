"""Writes must commit even when the client reads one row and closes.

A DuckDB statement whose result is streamed commits only once the result
is fully read, so a client that read a write's single row with fetchone()
and closed the connection would lose the write without an error. On DuckDB
1.5 firebird_execute has DuckDB materialize its result, as DuckDB does for
INSERT; on DuckDB 2.0, in autocommit, it commits its Firebird write before
returning the row.

sqllogictest reads every result fully, so this needs a real client.

Usage: python check_execute_fetchone.py <firebird.duckdb_extension> <connection string>
Needs the Python duckdb package of the same version as the extension, and
scripts/fixture_rw.sql loaded in the database.
"""
import sys

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

con = connect()
con.execute("SELECT * FROM firebird_execute('rw', 'DELETE FROM RW_LOG')").fetchall()
con.close()

if failures:
    sys.exit(f"check_execute_fetchone: {failures} case(s) lost their writes")
print("check_execute_fetchone: ok")
