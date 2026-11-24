"""An interrupted DuckDB query cancels the Firebird request it waits on.

DuckDB checks its interrupt flag between chunks; a single Firebird call can
block for minutes (here: the one row of a four-way cross join of
RDB$FIELDS, hours of work). The extension's cancel watcher calls
fb_cancel_operation once the query is interrupted, so the query must end
with DuckDB's InterruptException within a second, and the session must work
afterwards.

Each case first waits until the server runs the statement (MON$STATEMENTS,
read from another attachment), so the interrupt can't land before the
Firebird call. Then it checks how the query ended:
- the raise: within 0.9 s of the interrupt, with error_message
  "... cancelled (fb_cancel_raise)" in firebird_last_query();
- the abort: "... closed (fb_cancel_abort)". The watcher aborts a raise the
  server doesn't answer within 1 s (rare; reported, and allowed 1.9 s), and
  DUCKDB_FIREBIRD_TEST_CANCEL_ABORT=1 makes the first cancel the abort, so
  the second pass always takes that path.
A last case fails one pipeline while another waits on the slow call: the
query must end with that error, and error_message must keep it.

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
# name, query, text that identifies the Firebird statement in MON$STATEMENTS
CASES = [
    ("firebird_query", f"SELECT * FROM firebird_query('fb', '{SLOW}')", "RDB$FIELDS D"),
    ("scan of a view", "SELECT * FROM fb.main.V_SLOW", "V_SLOW"),
]
WAIT_IN_FLIGHT_S = 15.0
MAX_S = 15.0
RAISE_S = 0.9  # below the watcher's 1 s abort
ABORT_S = 1.9  # an unanswered raise, then the abort
RAISE_MSG = "Interrupted: the Firebird request was cancelled (fb_cancel_raise)"
ABORT_MSG = "Interrupted: the Firebird connection was closed (fb_cancel_abort)"


def connect(alias):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{cs}' AS {alias} (TYPE firebird)")
    return con


def in_flight(marker):
    # A new attachment each time: MON$ tables are a snapshot per transaction.
    mon = connect("mon")
    try:
        sql = (
            "SELECT COUNT(*) FROM MON$STATEMENTS WHERE MON$STATE = 1 "
            "AND MON$ATTACHMENT_ID <> CURRENT_CONNECTION "
            f"AND MON$SQL_TEXT CONTAINING ''{marker}''"
        )
        return mon.execute(f"SELECT * FROM firebird_query('mon', '{sql}')").fetchone()[0] > 0
    finally:
        mon.close()


failed = False


def check(ok, text):
    global failed
    print(f"{'ok' if ok else 'FAIL'} {text}", flush=True)
    failed |= not ok


for mode in ("raise", "abort"):
    if mode == "abort":
        os.environ["DUCKDB_FIREBIRD_TEST_CANCEL_ABORT"] = "1"
    for name, sql, marker in CASES:
        label = f"{name} ({mode})"
        con = connect("fb")
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
        deadline = time.monotonic() + WAIT_IN_FLIGHT_S
        seen = False
        while not seen and time.monotonic() < deadline and worker.is_alive():
            seen = in_flight(marker)
            if not seen:
                time.sleep(0.1)
        check(seen, f"{label}: the statement runs on the server before the interrupt")
        t_interrupt = time.monotonic()
        con.interrupt()
        worker.join(MAX_S)
        if worker.is_alive():
            check(False, f"{label}: still running {MAX_S:.0f} s after the interrupt")
            continue
        secs = result["end"] - t_interrupt
        message = con.execute("SELECT error_message FROM firebird_last_query()").fetchone()[0]
        check(result["outcome"] == "interrupted", f"{label}: {result['outcome']} {secs:.2f} s after the interrupt")
        if mode == "raise" and message == ABORT_MSG:
            print(f"note {label}: the server didn't answer the raise; the abort ended it", flush=True)
        expected = RAISE_MSG if mode == "raise" and message != ABORT_MSG else ABORT_MSG
        limit = RAISE_S if expected == RAISE_MSG else ABORT_S
        check(message == expected, f"{label}: error_message is {message!r}")
        check(secs < limit, f"{label}: ended within {limit} s of the interrupt")
        after = con.execute("SELECT * FROM firebird_query('fb', 'SELECT 1 FROM RDB$DATABASE')").fetchall()
        check(after == [(1,)], f"{label}: the session works afterwards ({after})")
        con.close()

# An interrupt must not replace the error that stopped the query. DuckDB
# interrupts a query itself when one of its pipelines fails, so the watcher
# also cancels the Firebird calls still running in the other pipelines. Here
# the slow call runs in one pipeline and a call that fails with a division by
# zero (after counting a few hundred thousand rows) in another. The UNION ALL
# feeds a COUNT(*), so both run at once: DuckDB 1.5 runs the sides of a
# UNION ALL that feeds the query result one after the other. firebird_last_query()
# must keep the division error, not the slow call's "Interrupted: ...".
os.environ.pop("DUCKDB_FIREBIRD_TEST_CANCEL_ABORT", None)
label = "an error in another pipeline"
FAILING = (
    "SELECT 1 / (COUNT(*) - COUNT(*)) FROM RDB$FIELDS A, RDB$FIELDS B, "
    "(SELECT FIRST 3 RDB$FIELD_NAME FROM RDB$FIELDS) C"
)
con = connect("fb")
con.execute("SET threads = 4")
result = {}


def run_both():
    try:
        con.execute(
            f"SELECT COUNT(*) FROM (SELECT * FROM firebird_query('fb', '{SLOW}') "
            f"UNION ALL SELECT * FROM firebird_query('fb', '{FAILING}'))"
        ).fetchall()
        result["outcome"] = "finished"
    except duckdb.InterruptException:
        result["outcome"] = "interrupted"
    except Exception as e:  # noqa: BLE001 - the expected outcome; checked below
        result["outcome"] = f"{type(e).__name__}: {str(e).splitlines()[0][:200]}"
        result["text"] = str(e)


worker = threading.Thread(target=run_both, daemon=True)
t0 = time.monotonic()
worker.start()
worker.join(MAX_S)
if worker.is_alive():
    check(False, f"{label}: still running {MAX_S:.0f} s after it started (the slow call wasn't cancelled)")
else:
    secs = time.monotonic() - t0
    check("divide by zero" in result.get("text", "").lower(),
          f"{label}: the query fails with the division error ({result['outcome']}, {secs:.2f} s)")
    message = con.execute("SELECT error_message FROM firebird_last_query()").fetchone()[0] or ""
    check("divide by zero" in message.lower(), f"{label}: error_message is {message[:200]!r}")
    after = con.execute("SELECT * FROM firebird_query('fb', 'SELECT 1 FROM RDB$DATABASE')").fetchall()
    check(after == [(1,)], f"{label}: the session works afterwards ({after})")
    con.close()

# A stuck worker thread would keep the process alive: exit hard.
os._exit(1 if failed else 0)
