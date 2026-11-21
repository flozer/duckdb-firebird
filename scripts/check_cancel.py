"""An interrupted DuckDB query cancels the Firebird request it waits on.

DuckDB checks its interrupt flag between chunks; a single Firebird call can
block for minutes (here: the one row of a four-way cross join of
RDB$FIELDS, hours of work). The extension's cancel watcher calls
fb_cancel_operation once the query is interrupted, so the query must end
with DuckDB's InterruptException within seconds, and the session must work
afterwards.

sqllogictest can't interrupt a running statement, so this needs a client.

Usage: python check_cancel.py <firebird.duckdb_extension> <connection string>
Needs the Python duckdb package of the same version as the extension, and
scripts/fixture_query.sql loaded in the database (view V_SLOW).
"""
import os
import sys
import threading
import time

import duckdb

ext, cs = sys.argv[1], sys.argv[2]
SLOW = "SELECT COUNT(*) FROM RDB$FIELDS A, RDB$FIELDS B, RDB$FIELDS C, RDB$FIELDS D"
CASES = [
    ("firebird_query", f"SELECT * FROM firebird_query('fb', '{SLOW}')"),
    ("scan of a view", "SELECT * FROM fb.main.V_SLOW"),
]
INTERRUPT_AFTER_S = 2.0
MAX_S = 15.0


def connect():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{cs}' AS fb (TYPE firebird)")
    return con


failed = False
for name, sql in CASES:
    con = connect()
    result = {}

    def run():
        t0 = time.monotonic()
        try:
            con.execute(sql).fetchall()
            result["outcome"] = "finished"
        except duckdb.InterruptException:
            result["outcome"] = "interrupted"
        except Exception as e:  # noqa: BLE001 - every other outcome is a failure, reported
            result["outcome"] = f"{type(e).__name__}: {str(e).splitlines()[0][:200]}"
        result["secs"] = time.monotonic() - t0

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    time.sleep(INTERRUPT_AFTER_S)
    con.interrupt()
    worker.join(MAX_S)
    if worker.is_alive():
        print(f"FAIL {name}: still running {MAX_S:.0f} s after the interrupt", flush=True)
        failed = True
        continue
    ok = result["outcome"] == "interrupted" and result["secs"] < INTERRUPT_AFTER_S + MAX_S
    print(f"{'ok' if ok else 'FAIL'} {name}: {result['outcome']} after {result['secs']:.1f} s", flush=True)
    failed |= not ok
    after = con.execute("SELECT * FROM firebird_query('fb', 'SELECT 1 FROM RDB$DATABASE')").fetchall()
    ok = after == [(1,)]
    print(f"{'ok' if ok else 'FAIL'} {name}: the session works afterwards ({after})", flush=True)
    failed |= not ok
    con.close()

# A stuck worker thread would keep the process alive: exit hard.
os._exit(1 if failed else 0)
