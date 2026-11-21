"""An interrupted write cancels the Firebird request it waits on, and keeps nothing.

Four writes that would run for hours, each interrupted after 2 s:
- one firebird_execute statement (an EXECUTE BLOCK that writes a row, then
  computes a four-way cross join of RDB$FIELDS);
- firebird_execute with a source of 100,000,000 rows;
- INSERT INTO an ATTACH table from 100,000,000 rows (DuckDB alone stops it
  between chunks; the case checks the rollback, and is the one that showed
  an unanswered cancel, see FirebirdCancelGuard);
- INSERT INTO an ATTACH table of one row, whose trigger computes that cross
  join (RW_SLOW): the Firebird call itself blocks, in the INSERT's Sink one
  row at a time (Firebird 3) or in its Finalize batch (Firebird 4+).
Each must end with DuckDB's InterruptException within 15 s, its write must
be rolled back, and the session must work afterwards. The two whose Firebird
call itself blocks (the long statement, the trigger) must also end within
0.9 s of the interrupt, with firebird_last_query().error_message naming the
cancel: "... cancelled (fb_cancel_raise)". A second pass runs them with
DUCKDB_FIREBIRD_TEST_CANCEL_ABORT=1, which makes the first cancel the abort:
"... closed (fb_cancel_abort)", within 1.9 s, on the write connection the
DuckDB transaction then can't use again. (A raise the server doesn't answer
within 1 s is aborted in the first pass too: reported, and allowed 1.9 s.)

Usage: python check_cancel_writes.py <firebird.duckdb_extension> <connection string>
Needs the Python duckdb package of the same version as the extension, and
scripts/fixture_rw.sql loaded in the database.
"""
import os
import sys
import threading
import time

import duckdb

ext, cs = sys.argv[1], sys.argv[2]
SLOW = "SELECT COUNT(*) FROM RDB$FIELDS A, RDB$FIELDS B, RDB$FIELDS C, RDB$FIELDS D"
# name, table, marker, SQL, whether the Firebird call itself blocks
CASES = [
    ("one long statement", "RW_LOG", -91,
     "SELECT * FROM firebird_execute('rw', 'EXECUTE BLOCK AS DECLARE N BIGINT; BEGIN "
     "INSERT INTO RW_LOG (ID, NOTE) VALUES (-91, ''cancel''); " + SLOW.replace("'", "''") + " INTO :N; END')", True),
    ("a source of 100,000,000 rows", "RW_LOG", -92,
     "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_LOG (ID, NOTE) VALUES (?, ?)', "
     "source := 'SELECT -92, ''cancel'' FROM range(100000000) t(i)')", False),
    ("INSERT INTO from 100,000,000 rows", "RW_LOG", -93,
     "INSERT INTO rw.main.RW_LOG SELECT -93, 'cancel' FROM range(100000000) t(i)", False),
    ("INSERT INTO a table whose trigger takes hours", "RW_SLOW", -94,
     "INSERT INTO rw.main.RW_SLOW VALUES (-94, 'cancel')", True),
]
INTERRUPT_AFTER_S = 2.0
MAX_S = 15.0
RAISE_S = 0.9  # below the watcher's 1 s abort
ABORT_S = 1.9  # an unanswered raise, then the abort
RAISE_MSG = "Interrupted: the Firebird request was cancelled (fb_cancel_raise)"
ABORT_MSG = "Interrupted: the Firebird connection was closed (fb_cancel_abort)"


def connect():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{cs}' AS rw (TYPE firebird, READ_WRITE)")
    return con


def kept(con, table, marker):
    return con.execute(
        f"SELECT * FROM firebird_query('rw', 'SELECT COUNT(*) FROM {table} WHERE ID = {marker}')").fetchall()[0][0]


failed = False


def check(ok, text):
    global failed
    print(f"{'ok' if ok else 'FAIL'} {text}", flush=True)
    failed |= not ok


for mode in ("raise", "abort"):
    if mode == "abort":
        os.environ["DUCKDB_FIREBIRD_TEST_CANCEL_ABORT"] = "1"
    for name, table, marker, sql, blocks in CASES:
        if mode == "abort" and not blocks:
            continue
        label = f"{name} ({mode})"
        con = connect()
        result = {}

        def run():
            try:
                con.execute(sql).fetchall()
                result["outcome"] = "finished"
            except duckdb.InterruptException:
                result["outcome"] = "interrupted"
            except Exception as e:  # noqa: BLE001 - every other outcome is a failure, reported
                result["outcome"] = f"{type(e).__name__}: {str(e).splitlines()[0][:200]}"
            result["end"] = time.monotonic()

        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        time.sleep(INTERRUPT_AFTER_S)
        t_interrupt = time.monotonic()
        con.interrupt()
        worker.join(MAX_S)
        if worker.is_alive():
            check(False, f"{label}: still running {MAX_S:.0f} s after the interrupt")
            continue
        secs = result["end"] - t_interrupt
        check(result["outcome"] == "interrupted", f"{label}: {result['outcome']} {secs:.2f} s after the interrupt")
        if blocks:
            message = con.execute("SELECT error_message FROM firebird_last_query()").fetchone()[0]
            if mode == "raise" and message == ABORT_MSG:
                print(f"note {label}: the server didn't answer the raise; the abort ended it", flush=True)
            expected = RAISE_MSG if mode == "raise" and message != ABORT_MSG else ABORT_MSG
            limit = RAISE_S if expected == RAISE_MSG else ABORT_S
            check(message == expected, f"{label}: error_message is {message!r}")
            check(secs < limit, f"{label}: ended within {limit} s of the interrupt")
        n = kept(con, table, marker)
        check(n == 0, f"{label}: {n} row(s) kept")
        after = con.execute("SELECT * FROM firebird_execute('rw', 'UPDATE RW_LOG SET NOTE = NOTE WHERE 1 = 0')").fetchall()
        check(after == [(0, 1)], f"{label}: the session writes afterwards ({after})")
        con.close()

con = connect()
con.execute("SELECT * FROM firebird_execute('rw', 'DELETE FROM RW_LOG WHERE ID IN (-91, -92, -93)')").fetchall()
con.close()
os._exit(1 if failed else 0)
