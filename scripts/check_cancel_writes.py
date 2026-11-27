"""An interrupted write cancels the Firebird request it waits on, and keeps nothing.

Writes that would run for hours, each interrupted after 2 s:
1. one firebird_execute statement (an EXECUTE BLOCK that writes a row, then
   computes a four-way cross join of RDB$FIELDS);
2. firebird_execute with a source of 100,000,000 rows;
3. INSERT INTO an ATTACH table from 100,000,000 rows (DuckDB alone stops it
   between chunks; the case checks the rollback, and is the one that showed
   an unanswered cancel, see FirebirdCancelGuard);
4. INSERT INTO an ATTACH table of one row, whose trigger computes that cross
   join (RW_SLOW), one row at a time: the Firebird call in the INSERT's Sink
   blocks;
5. (Firebird 4+) the same INSERT with SET firebird_write_batch_size: the
   call that blocks is the batch the INSERT's Finalize sends;
6. (Firebird 4+) firebird_execute of that INSERT with batch_size := and a
   source of one row: the call that blocks is its batch;
7. firebird_execute with a source of 100,000,000 rows and commit_every;
8. case 1 inside BEGIN ... ROLLBACK.
Cases 2, 3 and 7 write to RW_COUNTED, whose trigger advances a sequence a
rollback doesn't undo: they must have sent rows before the interrupt.

Each must end with DuckDB's InterruptException within 15 s and keep none of
its rows (case 7: exactly the rows its commit_every commits kept, a
positive multiple of 100), and the session must write again afterwards,
through firebird_execute and INSERT INTO. firebird_last_query().error_message
must name how the write ended:
- where the Firebird call itself blocks (1, 4, 5, 6, 8), the cancel:
  "... cancelled (fb_cancel_raise)", within 1.9 s of the interrupt (a note
  when it took more than 0.9 s); cases 5 and 6 must also show
  write_mode = 'batch';
- cases 2 and 7: an interrupt message (DuckDB's own "Interrupted!" when the
  interrupt came between source chunks, or the cancel's), never a write
  error;
- case 3: "did not complete" when DuckDB stopped the INSERT between chunks,
  or the cancel's message when the interrupt came during one of its calls.
Case 8 must leave the DuckDB transaction aborted ("Current transaction is
aborted"), and its Firebird write transaction open until ROLLBACK, which
then keeps nothing.

A second pass, in a child process with DUCKDB_FIREBIRD_TEST_CANCEL_ABORT=1,
runs the blocking cases again: the first cancel is then the abort, "...
closed (fb_cancel_abort)", within 1.9 s, and in case 8 the server has rolled
the transaction back already. (A raise the server doesn't answer within 1 s
is aborted in the first pass too: reported, and checked as an abort.)

Usage: python check_cancel_writes.py <firebird.duckdb_extension> <connection string>
Needs the Python duckdb package of the same version as the extension, and
scripts/fixture_rw.sql loaded in the database.
"""
import os
import subprocess
import sys
import threading
import time

import duckdb

ext, cs = sys.argv[1], sys.argv[2]
MODE = "abort" if sys.argv[3:] == ["--abort"] else "raise"
SLOW = "SELECT COUNT(*) FROM RDB$FIELDS A, RDB$FIELDS B, RDB$FIELDS C, RDB$FIELDS D"
LONG_BLOCK = ("SELECT * FROM firebird_execute('rw', 'EXECUTE BLOCK AS DECLARE N BIGINT; BEGIN "
              "INSERT INTO RW_LOG (ID, NOTE) VALUES ({m}, ''cancel''); " + SLOW.replace("'", "''") + " INTO :N; END')")
INTERRUPT_AFTER_S = 2.0
MAX_S = 15.0
RAISE_S = 0.9  # the watcher aborts 1 s after an unanswered raise
ABORT_S = 1.9  # an unanswered raise, then the abort
RAISE_MSG = "Interrupted: the Firebird request was cancelled (fb_cancel_raise)"
ABORT_MSG = "Interrupted: the Firebird connection was closed (fb_cancel_abort)"
INTERRUPT_MSGS = ("Interrupted!", RAISE_MSG, ABORT_MSG)
COMMIT_EVERY = 100


class Case:
    def __init__(self, name, table, marker, sql, blocks=False, setup=(), fb4=False, counted=False,
                 batch=False, commit_every=False, in_tx=False, messages=INTERRUPT_MSGS):
        self.name, self.table, self.marker, self.sql = name, table, marker, sql
        self.blocks, self.setup, self.fb4, self.counted = blocks, setup, fb4, counted
        self.batch, self.commit_every, self.in_tx, self.messages = batch, commit_every, in_tx, messages


CASES = [
    Case("one long statement", "RW_LOG", -91, LONG_BLOCK.format(m=-91), blocks=True),
    Case("a source of 100,000,000 rows", "RW_COUNTED", -92,
         "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_COUNTED (ID, NOTE) VALUES (?, ?)', "
         "source := 'SELECT -92, ''cancel'' FROM range(100000000) t(i)')", counted=True),
    Case("INSERT INTO from 100,000,000 rows", "RW_COUNTED", -93,
         "INSERT INTO rw.main.RW_COUNTED SELECT -93, 'cancel' FROM range(100000000) t(i)", counted=True,
         messages=('INSERT into Firebird table "RW_COUNTED" did not complete; none of its rows were kept',
                   RAISE_MSG, ABORT_MSG)),
    Case("INSERT INTO a table whose trigger takes hours, one row at a time", "RW_SLOW", -94,
         "INSERT INTO rw.main.RW_SLOW VALUES (-94, 'cancel')", blocks=True,
         setup=("SET firebird_write_batch_size = 0",)),
    Case("INSERT INTO that table in a batch (Finalize)", "RW_SLOW", -95,
         "INSERT INTO rw.main.RW_SLOW VALUES (-95, 'cancel')", blocks=True, fb4=True, batch=True,
         setup=("SET firebird_write_batch_size = 100",)),
    Case("firebird_execute into that table in a batch", "RW_SLOW", -96,
         "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_SLOW (ID, NOTE) VALUES (?, ?)', "
         "source := 'SELECT -96, ''cancel''', batch_size := 100)", blocks=True, fb4=True, batch=True),
    Case("a source of 100,000,000 rows with commit_every", "RW_COUNTED", -97,
         "SELECT * FROM firebird_execute('rw', 'INSERT INTO RW_COUNTED (ID, NOTE) VALUES (?, ?)', "
         f"source := 'SELECT -97, ''cancel'' FROM range(100000000) t(i)', commit_every := {COMMIT_EVERY})",
         counted=True, commit_every=True),
    Case("one long statement inside BEGIN", "RW_LOG", -98, LONG_BLOCK.format(m=-98), blocks=True, in_tx=True),
]
MARKERS = {c.table: [] for c in CASES}
for c in CASES:
    MARKERS[c.table].append(c.marker)
AFTER = [c.marker * 10 for c in CASES]  # rows the follow-up INSERT writes into RW_LOG


def connect():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{cs}' AS rw (TYPE firebird, READ_WRITE)")
    return con


def fb_value(con, sql):
    return con.execute(f"SELECT * FROM firebird_query('rw', '{sql}')").fetchall()[0][0]


def kept(con, table, marker):
    return fb_value(con, f"SELECT COUNT(*) FROM {table} WHERE ID = {marker}")


def open_write_transactions(con):
    # Write transactions of other attachments (this session's reads run in
    # read-only ones).
    return fb_value(con, "SELECT COUNT(*) FROM MON$TRANSACTIONS WHERE MON$READ_ONLY = 0 "
                         "AND MON$ATTACHMENT_ID <> CURRENT_CONNECTION")


failed = False


def check(ok, text):
    global failed
    print(f"{'ok' if ok else 'FAIL'} {text}", flush=True)
    failed |= not ok


def run_case(case, major):
    """Runs one case; returns False when its statement is still running (the run must stop)."""
    label = f"{case.name} ({MODE})"
    con = connect()
    for s in case.setup:
        con.execute(s)
    writes_before = fb_value(con, "SELECT GEN_ID(RW_WRITES, 0) FROM RDB$DATABASE") if case.counted else 0
    if case.in_tx:
        con.execute("BEGIN")
    result = {}

    def run():
        try:
            con.execute(case.sql).fetchall()
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
        check(False, f"{label}: still running {MAX_S:.0f} s after the interrupt; stopping the run")
        return False
    secs = result["end"] - t_interrupt
    check(result["outcome"] == "interrupted", f"{label}: {result['outcome']} {secs:.2f} s after the interrupt")

    if case.in_tx:
        try:
            con.execute("SELECT 1").fetchall()
            check(False, f"{label}: the DuckDB transaction still runs statements after the interrupt")
        except duckdb.Error as e:
            check("Current transaction is aborted" in str(e),
                  f"{label}: the DuckDB transaction is aborted ({str(e).splitlines()[0][:100]})")
        probe = connect()
        n_open = open_write_transactions(probe)
        # A raise leaves the Firebird write transaction open until ROLLBACK;
        # an abort closed the attachment, and the server rolled it back.
        expected_open = 0 if MODE == "abort" else 1
        check(n_open == expected_open, f"{label}: {n_open} Firebird write transaction(s) open before ROLLBACK")
        probe.close()
        con.execute("ROLLBACK")
        # A new attachment: Firebird 3 keeps a transaction's monitoring
        # snapshot, so the first probe would still count the old one.
        probe = connect()
        n_open = open_write_transactions(probe)
        check(n_open == 0, f"{label}: {n_open} Firebird write transaction(s) open after ROLLBACK")
        probe.close()

    record = con.execute("SELECT error_message, write_mode, rows_written FROM firebird_last_query()").fetchone()
    message, write_mode, rows_written = record
    if case.blocks:
        aborted = MODE == "abort" or message == ABORT_MSG
        if MODE == "raise" and message == ABORT_MSG:
            print(f"note {label}: the server didn't answer the raise; the abort ended it", flush=True)
        check(message == (ABORT_MSG if aborted else RAISE_MSG), f"{label}: error_message is {message!r}")
        check(secs < ABORT_S, f"{label}: ended within {ABORT_S} s of the interrupt")
        if not aborted and secs >= RAISE_S:
            print(f"note {label}: the raise took {secs:.2f} s (more than {RAISE_S} s)", flush=True)
    else:
        check(message in case.messages, f"{label}: error_message is {message!r}")
    if case.batch:
        check(write_mode == "batch", f"{label}: write_mode is {write_mode!r}")

    if case.counted:
        sent = fb_value(con, "SELECT GEN_ID(RW_WRITES, 0) FROM RDB$DATABASE") - writes_before
        check(sent > 0, f"{label}: {sent} row(s) sent before the interrupt")
    n = kept(con, case.table, case.marker)
    if case.commit_every:
        check(n > 0 and n % COMMIT_EVERY == 0,
              f"{label}: {n} row(s) kept, those commit_every committed (a positive multiple of {COMMIT_EVERY})")
        if case.counted:
            check(n <= sent, f"{label}: kept {n} of the {sent} row(s) sent")
    else:
        check(n == 0, f"{label}: {n} row(s) kept")

    after = con.execute("SELECT * FROM firebird_execute('rw', 'UPDATE RW_LOG SET NOTE = NOTE WHERE 1 = 0')").fetchall()
    check(after == [(0, 1)], f"{label}: firebird_execute writes afterwards ({after})")
    con.execute("SET firebird_write_batch_size = 0")
    con.execute(f"INSERT INTO rw.main.RW_LOG VALUES ({case.marker * 10}, 'after')")
    n_after = kept(con, "RW_LOG", case.marker * 10)
    check(n_after == 1, f"{label}: INSERT INTO writes afterwards ({n_after} row read back)")
    con.close()
    return True


def cleanup():
    con = connect()
    for table, markers in MARKERS.items():
        ids = ", ".join(str(m) for m in markers + (AFTER if table == "RW_LOG" else []))
        try:
            con.execute(f"SELECT * FROM firebird_execute('rw', 'DELETE FROM {table} WHERE ID IN ({ids})')").fetchall()
        except duckdb.Error as e:
            print(f"note cleanup of {table}: {str(e).splitlines()[0][:200]}", flush=True)
    con.close()


con = connect()
major = int(con.execute("SELECT split_part(engine_version, '.', 1) FROM firebird_health('rw')").fetchone()[0])
con.close()
try:
    for case in CASES:
        if MODE == "abort" and not case.blocks:
            continue
        if case.fb4 and major < 4:
            print(f"skip {case.name}: Firebird {major} has no batches", flush=True)
            continue
        if not run_case(case, major):
            break
finally:
    try:
        cleanup()
    except Exception as e:  # noqa: BLE001 - a cleanup failure must not hide the results
        check(False, f"cleanup: {e}")

if MODE == "raise":
    child = subprocess.run([sys.executable, *(["-I"] if sys.flags.isolated else []), __file__, ext, cs, "--abort"],
                           env={**os.environ, "DUCKDB_FIREBIRD_TEST_CANCEL_ABORT": "1"})
    if child.returncode != 0:
        print(f"FAIL the abort pass exited with {child.returncode}", flush=True)
        failed = True
sys.stdout.flush()
os._exit(1 if failed else 0)
