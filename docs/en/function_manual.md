# Extension function manual

Public function and setting reference for `duckdb-firebird`.

This is the English counterpart of `docs/pt/function_manual.md`. Keep both
files aligned when public behavior changes.

## How to read this manual

Use this document as a behavior reference, not as a product roadmap. Future
functions mentioned in `docs/en/roadmap.md` are not available until they are
implemented, tested, and documented here.

## Runtime requirement

The extension loads the Firebird client library (`libfbclient`) at runtime.
Install the Firebird client/runtime matching your platform before loading the
extension.

**Interrupting a query.** Interrupting a DuckDB query (Ctrl+C in the CLI,
`interrupt()` in a client) also cancels the Firebird request it is waiting
on: a cursor open that sorts, or a fetch of a row the server is still
computing. Scans and `firebird_query` end within a fraction of a second
(about a second at most, when the server doesn't answer the cancel) with
DuckDB's "Interrupted!" error, instead of waiting for the server to
finish. The connection the request ran on is dropped from the pool, and
`firebird_last_query().error_message` says how the query ended:
`Interrupted: the Firebird request was cancelled (fb_cancel_raise)`, or
`Interrupted: the Firebird connection was closed (fb_cancel_abort)` when
the server didn't answer the cancel. A Firebird error that isn't the
cancel's (including "operation was cancelled" from a DBA's
`DELETE FROM MON$STATEMENTS`) keeps its own message, and so does a query
that failed: DuckDB then interrupts its other parts itself, and their
cancel doesn't replace the error that stopped the query. Writes too: an
interrupted `firebird_execute` (one long statement, a batch, or a `source`
loop) or `INSERT INTO fb.main.T` stops the same way. In autocommit its
Firebird write transaction rolls back with the DuckDB statement; rows an
earlier `commit_every` committed stay. Inside `BEGIN ... COMMIT`, DuckDB
keeps the transaction aborted after the interrupt (every statement fails
with "Current transaction is aborted" until `ROLLBACK`), and the Firebird
write transaction stays open, with its locks, until that `ROLLBACK` ends
it; after an abort the server has rolled it back already. An interrupt
doesn't reach opening a connection or loading an ATTACH's table metadata:
those wait for the server.

## Level 1 - Direct reads

### `firebird_scan(connection_string, table_name)`

Reads a Firebird table directly into DuckDB.

Common use:

```sql
SELECT *
FROM firebird_scan('database=C:/data/erp.fdb user=APP_READONLY password=secret',
                   'CUSTOMER');
```

Remote database:

```sql
SELECT *
FROM firebird_scan(
  'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb?charset=UTF8',
  'CUSTOMER'
);
```

The equivalent libfbclient-style form is:

```sql
SELECT *
FROM firebird_scan(
  'database=db.example.com/3050:/path/to/database.fdb;user=APP_READONLY;password=secret;charset=UTF8',
  'CUSTOMER'
);
```

For remote connections, use `firebird://USER:PASSWORD@HOST:PORT/path`
or `database=HOST/PORT:/path;user=USER;password=PASSWORD`. Do not use
`HOST:PORT://path`.

Supported named parameters include:

- `schema`
- `columns`
- `filter`
- `row_limit`
- `row_offset`
- `partitions` — parallelism: `0` = auto, `1` = serial. With the default
  `partition_by = 'pk'`, the ranges come from a `MIN`/`MAX` probe of the PK;
  the first partition has no lower bound and the last no upper bound, so
  rows outside the probed range (committed after the probe, or after an
  `ATTACH` cached it) are still read.
- `partition_by` — `'pk'` (default) or `'dbkey'`; see [Choosing how a
  table is split (`partition_by`)](#choosing-how-a-table-is-split-partition_by)
- `none_pushdown` — opt-in pushdown of `=` / `IN` over `CHARACTER SET NONE`
  CHAR/VARCHAR columns when `none_encoding` is `win1252` or `iso8859_1`
  (default `false`; see [Charset handling of `CHARACTER SET NONE`
  text filters](#charset-handling-of-character-set-none-text-filters))
- `numeric_widen_int64` — opt-in: projects int64-backed `NUMERIC`/`DECIMAL`
  columns with a scale (precision 10–18, i.e. `NUMERIC(18,s)` and friends)
  as `DECIMAL(38,s)` instead of `DECIMAL(18,s)`, so Firebird's full scaled
  int64 range survives (default `false`; see [Type mapping
  notes](#type-mapping-notes))
- charset-related options documented in the usage guide

Use this when you need one direct table scan without attaching the whole
database catalog.

#### Partitioned scans read one snapshot

With `partitions > 1` (or when the ATTACH path partitions a large table on
its own), every partition is read by its own connection. On Firebird 4 and
later all of them read **one snapshot** of the database: a coordinating
connection starts a read-only SNAPSHOT transaction, and each partition
joins it (`isc_tpb_at_snapshot_number`). A partitioned scan therefore
returns exactly the rows of one committed state, even while other
connections commit changes, as a serial scan does.

- The snapshot is held, read-only, until the scan ends, like any Firebird
  snapshot transaction. On Firebird 4+ with the default
  `ReadConsistency = 1`, a running read-committed cursor already pins its
  own statement snapshot for as long as it runs, so garbage collection is
  held back for about the same time as before.
- On Firebird 3 (no snapshot numbers), or if the shared snapshot cannot be
  established, each partition uses its own read-committed transaction, as
  in earlier versions. Partitions can then see different committed states.
- Once the shared snapshot is established, every partition worker must
  join it. If one cannot, the scan fails with an error naming the table and
  the server's reason; no partition is read outside the snapshot. The error
  is also set in `firebird_last_query().error_message` when another worker
  of the scan opens a cursor, before or after the failure.
- Serial scans are unchanged: one connection, one read-committed
  transaction. The opt-in [transaction snapshot](#one-snapshot-per-duckdb-transaction-transaction_snapshot)
  of the ATTACH path extends the snapshot to every scan of a DuckDB
  transaction, serial ones included.
- `firebird_last_query().snapshot_mode` reports which case applied:
  `shared`, `independent` or `serial` (and `transaction` with the
  transaction snapshot).
- A partitioned scan opens one extra attachment (the coordinator) for its
  duration. It does not come from the ATTACH connection pool, so
  `firebird_pool_stats()` counters are unaffected.

**Known server issues.** Two open Firebird bugs weaken this guarantee on
Firebird 4, 5 and master:

- [FirebirdSQL/firebird#9227](https://github.com/FirebirdSQL/firebird/issues/9227):
  a transaction that joins a snapshot with `isc_tpb_at_snapshot_number` can
  see a commit that the snapshot's owner doesn't see. A transaction that
  commits while the partitions join, with no older read-write transaction
  still open, can then be visible to some partitions and not to others,
  and the scan mixes two committed states.
- [FirebirdSQL/firebird#9301](https://github.com/FirebirdSQL/firebird/issues/9301):
  a SNAPSHOT transaction that starts while another transaction commits can
  miss that transaction's rows or see later commits (fix proposed in
  [#9303](https://github.com/FirebirdSQL/firebird/pull/9303)).

Both apply to the snapshot of a partitioned scan, `partition_by = 'dbkey'`
included, and to the [transaction snapshot](#one-snapshot-per-duckdb-transaction-transaction_snapshot),
where the joining window lasts for the whole DuckDB transaction. Scans
only read, so data at rest is never at risk; only the
consistency of what one scan or transaction reads is. This note will be
updated when the fixes are released.

#### Choosing how a table is split (`partition_by`)

`partition_by` selects how `firebird_scan` splits a table when
`partitions > 1`:

- `'pk'` (default): ranges of a single-column integer primary key,
  computed from its `MIN`/`MAX`. Unchanged from earlier versions.
- `'dbkey'`: ranges of `RDB$DB_KEY` over the table's physical record
  space, which comes from `RDB$PAGES` and the page size. The bounds are
  read inside the scan's shared snapshot, so no `MIN`/`MAX` probe is
  needed. The split does not depend on how the primary key is distributed,
  and **tables without a usable primary key can be split too**. The space
  counts whole pointer pages (one covers about 12.8 MiB of data pages with
  8 KiB pages), so partitions are balanced only to within one pointer
  page: most of a table smaller than a few pointer pages lands in the
  first partitions.
  Without a pushed filter that can use an index, each partition is a
  bounded full scan
  (`RDB$DB_KEY >= MAKE_DBKEY(...) AND RDB$DB_KEY < MAKE_DBKEY(...)`,
  plan `Full Scan (lower bound, upper bound)`). When a pushed filter can
  use an index, Firebird reads through that index in every partition and
  applies the DB_KEY bounds only as a filter, so each partition repeats the
  whole index scan: prefer `'pk'` for such queries.

`'dbkey'` applies only with an explicit `partitions` of 2 or more, to
persistent base tables, on Firebird 4.x or 5.x (`MAKE_DBKEY` and bounded
DB_KEY scans are Firebird 4.0+), and only when the scan's shared snapshot
was started. In every other case (Firebird 3, views, `partitions` unset or
`1`, no shared snapshot) the scan behaves as without `partition_by`. With
an explicit `partitions` of 2 or more, the `MIN`/`MAX` probe for PK ranges
then runs when the scan starts, on every execution.
`firebird_last_query().partition_method` reports what was used: `pk`,
`dbkey`, or `none` (one partition).

```sql
-- A table without a primary key, split four ways (Firebird 4/5):
SELECT COUNT(*)
FROM firebird_scan('<conn>', 'AUDIT_LOG', partitions=4, partition_by='dbkey');
SELECT partition_method, snapshot_mode, partitions FROM firebird_last_query();
-- dbkey  shared  4
```

Splitting a scan does not always make it faster. In our measurements
(4-core VM, TCP loopback, Firebird 5.0.3 client) four partitions sped up a
19-column scan by 1.4–1.6×, but made narrow or column-projected scans
about 2× slower than serial. DB_KEY ranges were faster than PK ranges by
about the cost of the `MIN`/`MAX` probe they skip (1.5–2 s on 5–10 M rows).
In our tests most of the slowdown came from the Firebird client library,
which serializes per-row allocations across attachments
([FirebirdSQL/firebird#9297](https://github.com/FirebirdSQL/firebird/issues/9297)).
Measure before relying on `partitions > 1`.

#### Pagination determinism (`RDB$DB_KEY`)

When paginating (`row_limit`/`row_offset`) against a table or view with no
usable single-column numeric primary key, `firebird_scan` orders by
`RDB$DB_KEY` so that multiple pages read from the same query see a stable,
deterministic boundary. (Previously `ROWS` was pushed with no `ORDER BY` at
all, so page boundaries were not deterministic — this was a bug, now fixed.)

`RDB$DB_KEY` is a **physical row-pagination stabilizer valid only for the
duration of one scan/transaction**. It is NOT a business ordering contract,
is NOT stable across transactions or after row updates, and is NEVER used
for incremental/watermark logic or recommended as a key anywhere else in
this extension. The one other use is `partition_by = 'dbkey'`, where the
DB_KEY ranges are planned and read inside one shared snapshot, the only
scope in which they are stable.

When a usable single-column numeric primary key exists, it is used for the
`ORDER BY` instead of `RDB$DB_KEY` — preferred, since it is a real,
meaningful column rather than a physical stabilizer.

For a heavy view (one with a JOIN, `GROUP BY`, or an aggregate function) or
a view whose definition could not be read, `RDB$DB_KEY` is unsafe — Firebird
silently returns it as SQL `NULL` for such views rather than erroring, which
is why this decision is made statically, at bind time from the view's
shape, rather than detected at runtime. In that case `firebird_scan` does
not push `ROWS` to Firebird at all; it applies `row_limit`/`row_offset`
locally instead, so the requested slice is still correctly honored, just
without the server-side pushdown optimization. See `firebird_explain_pushdown`'s
`view_heavy` / `view_heavy_reasons` columns below to check this ahead of
time.

#### View column type reconciliation

When the scan target is a view, column types are reconciled against
Firebird's live, runtime-described column layout rather than trusted from
the view's catalog metadata alone (both the DuckDB column type and the
internal fetch descriptor are corrected together). A view's
computed/aggregate column type (e.g. a `SUM()` over a `NUMERIC` column) is
frozen at `CREATE VIEW` time and can legitimately disagree with what
Firebird's live SQL compiler produces for the identical expression today —
without this reconciliation, that mismatch could crash the scan (a real
bug, fixed as issue #33). Base-table scans are unaffected (a table's
storage width cannot disagree with its own catalog metadata) and pay no
added cost from this reconciliation.

## Level 2 - Catalog discovery

### `firebird_tables(connection_string)`

Lists Firebird tables visible to the connection.

```sql
SELECT *
FROM firebird_tables('database=C:/data/erp.fdb user=APP_READONLY password=secret');
```

### `information_schema` via `ATTACH`

After attaching a database, use DuckDB catalog views to inspect exposed
Firebird tables and columns:

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT *
FROM information_schema.tables
WHERE table_schema = 'main';
```

## Level 3 - Attached database

### `ATTACH ... (TYPE firebird)`

Attaches a Firebird database as a DuckDB catalog.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT *
FROM fb.main.CUSTOMER;
```

Options mirror the `firebird_scan` named parameters — `user`, `password`,
`charset`, `role`, `dialect`, `none_encoding`, `none_pushdown` (opt-in
pushdown of `=` / `IN` over `CHARACTER SET NONE` CHAR/VARCHAR columns), and
`numeric_widen_int64` (opt-in `DECIMAL(38,s)` projection for int64-backed
scaled numerics):

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird, none_encoding 'win1252', none_pushdown true,
         numeric_widen_int64 true);
```

The ATTACH is read-only unless it is opened with DuckDB's `READ_WRITE`
option, which allows writes through [`firebird_execute`](#firebird_executealias-sql--source-commit_every-count_rows)
and [`INSERT INTO`](#insert-into-fbmaint-):

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_WRITER password=secret'
  AS fb (TYPE firebird, READ_WRITE);
```

Since v1.2.1, `none_encoding` and `none_pushdown` are also accepted inside
the connection string itself, in both supported forms — URI query
(`firebird://user:pass@host:3050/path?charset=UTF8&none_encoding=win1252&none_pushdown=true`)
and key=value DSN (`database=C:/data/erp.fdb;user=APP_READONLY;password=secret;none_encoding=win1252;none_pushdown=true`).
Booleans accept `true`/`false`/`1`/`0`; anything else fails with an
actionable error (unknown keys are still ignored, so spell carefully).
Precedence: explicit ATTACH option / `firebird_scan` named parameter >
connection string > default. `numeric_widen_int64` remains an ATTACH
option / named parameter only.

The key=value form may also be separated by spaces instead of `;`
(`database=C:/data/erp.fdb user=APP_READONLY password=secret`), as long as
every token is a known key (`database`, `user`, `password`, `charset`,
`role`, `dialect`, `none_encoding`, `none_pushdown`). Use `;` when a value
contains spaces. Connection errors and the path shown by
`duckdb_databases()` never show the password. DuckDB itself quotes the
start of a failing statement in some errors (`LINE 1: ...`), so a
connection string written as a literal there can still appear; pass it
through a variable (`getvariable('cs')`) to keep it out.

Prefer `ATTACH` for analytics sessions that query multiple Firebird tables.
It enables catalog discovery, normal SQL references, and connection-pool
reuse.

#### One snapshot per DuckDB transaction (`transaction_snapshot`)

By default each scan reads its own state of the database: a partitioned
scan reads one snapshot ([above](#partitioned-scans-read-one-snapshot)),
and a serial scan reads in its connection's read-committed transaction.
So two scans of one statement (a join, a self-join, two subqueries), or
two statements of one `BEGIN … COMMIT`, can each see a different
committed state while other connections write.

With the transaction snapshot on, every scan of the ATTACH in one DuckDB
transaction reads **one snapshot** (Firebird 4+): one per statement in
autocommit mode, one per transaction inside `BEGIN … COMMIT`. It is off by
default; turn it on for a session or for one ATTACH:

```sql
SET firebird_transaction_snapshot = true;            -- every Firebird ATTACH

ATTACH 'firebird://srv/data/erp.fdb' AS erp
  (TYPE firebird, transaction_snapshot true);        -- this ATTACH only
```

The ATTACH option wins over the setting in both directions
(`transaction_snapshot false` keeps an ATTACH out).

- At the first scan of the transaction, a connection from the ATTACH
  pool starts a read-only SNAPSHOT transaction (the coordinator), and
  every scan of the transaction joins it with `isc_tpb_at_snapshot_number`,
  like the partitions of one scan. At `COMMIT` or `ROLLBACK` the
  coordinator goes back to the pool, reset to read committed;
  `firebird_pool_stats().active_connections` counts it until then. With
  `firebird_pool_max_size = 1` or the pool disabled, the coordinator is
  closed at every commit and the next transaction opens a new attachment
  (about 12 ms per transaction on a local server).
- A pooled connection the server already closed (a restart, a network
  drop) is skipped when the coordinator is picked. If no coordinator can be
  started, the scan fails rather than read without the snapshot. Every
  connection that joins the snapshot then checks that the coordinator
  still holds it (one round trip), and the scan fails if it was lost:
  Firebird restarts snapshot numbers when a database is reopened, so a
  lost number could belong to someone else's snapshot. A scan whose own
  pooled connection was lost also fails. In every case, run the query
  again (inside `BEGIN … COMMIT`, after a `ROLLBACK`).
- **Serial scans change too**: they read the transaction's snapshot, not
  read committed, so they don't see commits made after the transaction's
  first Firebird scan.
- `firebird_last_query().snapshot_mode` reports `transaction` for every
  scan that read the transaction's snapshot.
- If a scan cannot join the snapshot, it fails with an error that names
  the table and the server's reason, as a partitioned scan does.
- **Firebird 3** has no snapshot numbers: scans behave as without the
  option and report `serial` or `independent`. The first transaction of an
  ATTACH finds that out, and later ones don't try again until the next
  `ATTACH`.
- A direct `firebird_scan()` call is not part of an ATTACH's transaction
  and is not affected. So `partition_by`, a `firebird_scan()` parameter,
  never meets this option: ATTACH scans don't take it.
- The known server issues listed
  [above](#partitioned-scans-read-one-snapshot) apply here too, for the
  whole transaction.

**Idle limit.** An open DuckDB transaction holds its snapshot on the
server and, like any long SNAPSHOT transaction, holds back garbage
collection. `firebird_transaction_snapshot_idle_timeout` (seconds, default
`60`, `0` = no limit) bounds the time from the end of a statement to the
next scan of the ATTACH in the transaction: at the end of each statement
the extension sets `SET SESSION IDLE TIMEOUT` on the coordinator, and the
next scan clears it. If no scan comes in time, the server itself closes the
coordinator and releases the snapshot, even if the client does nothing.
The next scan of the transaction then fails:

```text
IO Error: Firebird ATTACH 'erp': the snapshot of this DuckDB transaction is
gone. Its coordinating connection was closed by the server, because no scan
of this ATTACH ran in the transaction for
firebird_transaction_snapshot_idle_timeout (60 s), or because the
connection was lost. Run the query again (inside BEGIN ... COMMIT, after a
ROLLBACK).
```

- Once a statement has started a scan of the ATTACH, the rest of that
  statement never counts, however long it runs. Statements that don't read
  the ATTACH do count.
- A statement ends when its result is fully read or closed. **A client
  that keeps a result open** (an API or BI tool holding a cursor) keeps the
  snapshot for as long as it does, like any running statement, with or
  without `BEGIN`: the limit only starts once the result is done.
- Read at the transaction's first Firebird scan; a later `SET` applies to
  the next transaction.
- Firebird counts the timeout in whole seconds, so the extension asks for
  one second more: the snapshot survives at least the configured time. If
  the server refuses the timer, the snapshot is released at once, and the
  next scan of the transaction fails with the server's reason.
- Values outside `0..2147483646` are rejected by `SET` (the server takes
  up to 2³¹ − 1 seconds, and the extension adds one):
  `firebird_transaction_snapshot_idle_timeout must be 0 (no limit) to 2147483646 seconds, got -1`.

### `firebird_query(alias, sql)`

Runs one `SELECT` on an attached Firebird database exactly as written and
returns its rows. Use it for Firebird SQL that DuckDB can't express over
`fb.main.<table>`: selectable stored procedures, `FIRST`/`SKIP`, Firebird
functions, joins and aggregates you want the server to run.

```sql
ATTACH 'database=C:/data/erp.fdb;user=APP_READONLY;password=secret' AS fb (TYPE firebird);

SELECT * FROM firebird_query('fb', 'SELECT DEPT_NO, COUNT(*) AS N FROM EMPLOYEE GROUP BY 1');
SELECT * FROM firebird_query('fb', 'SELECT * FROM SP_ORDERS_OF_DAY(CURRENT_DATE)');
```

| Argument | Type | Description |
|---|---|---|
| `alias` | VARCHAR | Name of a `TYPE firebird` ATTACH |
| `sql` | VARCHAR | One Firebird `SELECT` (a CTE, a selectable procedure and an `EXECUTE BLOCK` with `SUSPEND` are `SELECT`s too) |

Both arguments may be any constant expression, for example
`firebird_query('fb', getvariable('extract_sql'))`.

- **Only SELECTs.** The statement is prepared at bind time and its type
  read (`isc_info_sql_stmt_type`); `UPDATE`, `EXECUTE PROCEDURE`, DDL and
  the rest fail at bind.
- **Read-only transaction.** A `SELECT` can still run code that writes: a
  selectable procedure, or an `EXECUTE BLOCK` with `SUSPEND`. It runs in
  the ATTACH pool's read-only transaction, so the server refuses those
  writes ("attempted update during read-only transaction"). Tested on
  Firebird 3, 4 and 5, the refusal also covers:
  - DDL run through `EXECUTE STATEMENT`;
  - `IN AUTONOMOUS TRANSACTION` blocks (the autonomous transaction is
    read-only too);
  - `EXECUTE STATEMENT ... ON EXTERNAL DATA SOURCE`, sharing the caller's
    transaction or `WITH AUTONOMOUS TRANSACTION` (tested against the same
    database).

  It holds on an ATTACH opened with `READ_WRITE` too: `firebird_query`
  runs on the read pool, never on a write connection.
- **Where read-only stops.**
  - Sequences are not transactional: `GEN_ID(G, 1)` or `NEXT VALUE FOR G`
    in the `SELECT` advances `G`, and the step stays.
  - External functions (UDFs and UDRs) run outside transaction control.
    The read-only transaction doesn't stop what they do outside the
    database, the same as running the statement in `isql` against your
    own database.
- **Types.** Each result column maps to the DuckDB type a scan gives the
  same Firebird type, with the ATTACH's `none_encoding` and
  `numeric_widen_int64`. `DECFLOAT` comes back as text, as in a scan.
  Column names are Firebird's (aliases); duplicates get DuckDB's usual
  suffixes, and an empty name becomes `columnN`.
- **No planning.** The SQL goes to Firebird as written: no projection,
  filter or `ROWS` pushdown, no partitions. A filter written around it in
  DuckDB is applied after the rows arrive. `firebird_explain_pushdown`
  shows it as `scan_strategy = 'passthrough'` with `table_name` NULL.
- **Snapshot.** With `firebird_transaction_snapshot` on, it reads the
  snapshot of the DuckDB transaction, like the scans of the ATTACH.
- **Telemetry.** Each call leaves one `firebird_last_query()` /
  `firebird_query_log()` record: the SQL as sent in `remote_sql`,
  `rows_read`, timings, `partitions` 1, `table_name` empty.
- **Prepared twice.** The statement is prepared at bind (to learn its
  columns) and again when it runs. If its columns changed in between, the
  query fails and asks to be run again.

### `firebird_execute(alias, sql [, source, commit_every, count_rows])`

Runs one Firebird statement (DML, DDL, `EXECUTE PROCEDURE`,
`EXECUTE BLOCK`) on an ATTACH opened with `READ_WRITE`, inside the DuckDB
transaction's Firebird write transaction: once, or once per row of a
DuckDB query (`source`), with that row's values as its parameters.

```sql
ATTACH 'database=C:/data/erp.fdb;user=APP_WRITER;password=secret'
  AS fb (TYPE firebird, READ_WRITE);

SELECT * FROM firebird_execute('fb', 'UPDATE CUSTOMER SET ACTIVE = TRUE WHERE ID = 1');
SELECT * FROM firebird_execute('fb', 'EXECUTE PROCEDURE SP_CLOSE_DAY(CURRENT_DATE)');

-- One execution per row of a DuckDB query: an upsert from a staging table.
SELECT * FROM firebird_execute('fb',
    $$EXECUTE BLOCK (ID INTEGER = ?, NAME VARCHAR(40) = ?, CITY VARCHAR(30) = ?) AS
      BEGIN UPDATE OR INSERT INTO CUSTOMER (ID, NAME, CITY) VALUES (:ID, :NAME, :CITY) MATCHING (ID); END$$,
    source := 'SELECT id, name, city FROM staging_customers',
    commit_every := 10000);
```

| Argument | Type | Description |
|---|---|---|
| `alias` | VARCHAR | Name of a `TYPE firebird` ATTACH opened with `READ_WRITE` |
| `sql` | VARCHAR | One Firebird statement, as Firebird runs it |
| `source` | VARCHAR | Optional. A DuckDB query; the statement runs once per row, with column i as parameter i |
| `commit_every` | BIGINT | Optional, with `source`. Commit every N rows; 0 (default) = with the DuckDB transaction |
| `count_rows` | BOOLEAN | Optional. `false` skips the record counts (`rows_affected` NULL). Default `true` |
| `batch_size` | BIGINT | Optional, with `source`. Rows per batch on Firebird 4+; overrides `firebird_write_batch_size` |

Returns one row:

| Column | Type | Description |
|---|---|---|
| `rows_affected` | BIGINT | Rows inserted + updated + deleted, as Firebird reports them; 0 for DDL; `NULL` for a statement that returns rows, and with `count_rows := false` |
| `executions` | BIGINT | Executions: 1, or the source rows run |

- **Opt-in.** Writing needs DuckDB's own `READ_WRITE` attach option.
  Without it, and on a `READ_ONLY` ATTACH, the call fails at bind and names
  the option. An ATTACH without `READ_WRITE` never opens a write
  connection.
- **Transactions follow DuckDB's.** Writes run on a second connection pool
  of the ATTACH, never on the connections that serve reads, in a
  read-write transaction: `read_committed`, `rec_version`, waiting up to
  `firebird_write_lock_timeout` seconds for a row lock held elsewhere
  (then "update conflicts with concurrent update"). In autocommit each call
  is one Firebird transaction; between `BEGIN` and `COMMIT`, every write of
  the DuckDB transaction is one Firebird transaction, committed or rolled
  back with it. `SET TRANSACTION`, `COMMIT` and `ROLLBACK` in `sql` are
  refused. As for any DuckDB transaction, one transaction writes to one
  attached database. The write pool has the same settings as the read pool
  (`firebird_pool_*`), so it can keep as many idle connections again;
  `firebird_pool_stats()` lists it in a second row (`pool = 'write'`).
- **What a read sees after a write.** Scans and `firebird_query` keep
  their own connections and read-only transactions, so a read sees a
  write once it is committed. In autocommit, that is from the next
  statement on. Between `BEGIN` and `COMMIT`, the transaction's reads
  don't see its own writes until `COMMIT`. With
  [`firebird_transaction_snapshot`](#one-snapshot-per-duckdb-transaction-transaction_snapshot)
  on (Firebird 4+), the reads of one DuckDB transaction keep the snapshot
  of its first read, so they also miss a write that another connection
  commits after it; the next transaction sees both. In autocommit each
  statement is a transaction of its own, so the setting changes nothing
  there.
- **No atomicity across engines.** DuckDB lets one transaction write to
  one attached database, so a transaction can't write both a DuckDB table
  and Firebird: the second write is refused ("a single transaction can
  only write to a single attached database"). Writes in two transactions,
  one per engine, commit separately, with no two-phase commit: if the
  second fails, the first stays committed.
- **Statements that return rows** (a `SELECT`, a selectable procedure, an
  `EXECUTE BLOCK` with `SUSPEND`) run and their rows are read and
  discarded, so a selectable procedure's writes happen, as in isql.
  `rows_affected` is `NULL`. Use `firebird_query` to read rows.
- **Parameters from a DuckDB query (`source`).** With
  `source := '<duckdb query>'`, the statement is prepared once and run
  once per row of the query, with column i of the row as parameter i (the
  `?` placeholders, in text order). The column count must match the
  parameter count; both are checked at bind. The query runs on its own
  DuckDB connection: it sees the database's tables and attached catalogs,
  but not the calling connection's TEMP tables, uncommitted changes or
  session state (`USE`, `search_path`, variables, `SET` values; name
  tables with their catalog, or use `SET GLOBAL`). Its
  rows are read as a stream, a chunk (2,048 rows) at a time, and DuckDB
  computes only a chunk or two ahead of the writes. So memory stays
  small, and if the query fails half-way, the rows `commit_every`
  committed before the failure stay (the error says which). Its column
  types are checked when the call runs: if another connection changed them
  since the bind, the call fails before writing. `source := NULL` is
  refused ("source must not be NULL") on DuckDB 1.5 and 2.0, also when the
  NULL comes from a prepared statement's parameter; a call without
  `source` runs the statement once.
- **Exact values.** Each value travels in a Firebird type that represents
  it exactly, and the server converts it to the parameter's type with its
  own rules, so rounding, overflow and truncation are what the same
  literal in the SQL text gets. Integers go as BIGINT, a `DECIMAL(p,s)`
  as a BIGINT with scale s (1.5 lands in a `NUMERIC(9,3)` as 1.500), a
  128-bit value, or a DECIMAL whose digits don't fit in 64 bits, as
  `INT128` with its scale when the client library and the server are both
  Firebird 4+; otherwise as text, which fails past 18 digits (on a
  Firebird 3 server, like the same literal; through a Firebird 3 client to
  a Firebird 4+ server, the literal still rounds);
  FLOAT, DOUBLE, BOOLEAN, DATE, TIME and TIMESTAMP in their own types;
  time zones as UTC (Firebird keeps 1/10000 s), so a `TIMESTAMPTZ` stored
  in a column without a time zone keeps its UTC time (cast it in the
  source, `ts AT TIME ZONE '...'`, for a local time). Text, and values with no
  native form, go as VARCHAR in the parameter's character set
  (`none_encoding` bytes for `CHARACTER SET NONE`); a value longer than
  the parameter, or a character its encoding can't hold, is an error,
  never silent truncation. Values for BLOB parameters become BLOBs of any
  size. Every row sets every parameter again, so a value after a NULL is
  never sent as NULL. Dates outside 0001-01-01 to 9999-12-31 (BC dates,
  years past 9999) and `TIME '24:00:00'` are an error: Firebird can't
  hold them. (In a scan's filter, such a constant stays a DuckDB filter.)
- **`commit_every := N`** commits every N rows; those rows stay committed
  if a later row fails. 0 (default) commits with the DuckDB transaction.
  It needs autocommit: inside `BEGIN ... COMMIT` the call fails, because
  its commits would also commit the transaction's earlier writes.
- **`count_rows := false`** skips reading the record counts after each
  row (one round trip per row) and returns `rows_affected` NULL.
- **Errors.** The first failing row stops the call. The message names the
  source row and what is kept: nothing (the transaction is rolled back),
  or source rows 1 to K that `commit_every` committed.
- **Batches (Firebird 4+).** With `SET firebird_write_batch_size = N`
  (or `batch_size := N` on the call), the rows go N at a time through
  Firebird's batch interface (`IBatch`) instead of one round trip each.
  `INSERT`, `UPDATE` and `DELETE` batch on Firebird 4+, `EXECUTE
  PROCEDURE` and `EXECUTE BLOCK` on Firebird 5+ (a Firebird 4 server fails
  the second `EXECUTE BLOCK` message of a batch). Other statements,
  Firebird 3, a client library older than Firebird 4 (it lacks
  `fb_get_statement_interface`), statements with BLOB parameters, and
  statements that return values (`INSERT ... RETURNING`, an `EXECUTE
  PROCEDURE` with output parameters: Firebird 5 makes a batch of them and
  writes nothing) run one row at a time. `write_mode` in the query log says
  which mode ran, and when a batch was asked for and not used,
  `not_pushed_reasons` says why, as one entry `batch: <reason>`. A batch is sent every N rows, at every `commit_every` boundary, and
  whenever the next row might not fit its 16 MB buffer, so wide rows get
  smaller batches, never a "batch too big" error. The first failing row
  stops the call and is named as in the per-row mode: a row the client
  can't encode sends the rows before it first. A batch that fails as a
  whole (a lost connection) names its range of source rows. `rows_affected`
  sums the record counts for plain `INSERT`/`UPDATE`/`DELETE`; for
  `EXECUTE BLOCK` and `EXECUTE PROCEDURE` it is `NULL`, because Firebird
  reports 0 for each such message (one row at a time they are counted, so
  `batch_size` changes it).
- With `source`, a statement that returns rows is refused, and a
  statement with `?` placeholders needs a `source`.
- **Session statements are refused:** `SET ROLE`, `SET TRUSTED ROLE`,
  `SET BIND`, `SET DECFLOAT`, `SET TIME ZONE`, `SET SESSION ...`,
  `SET STATEMENT TIMEOUT`, `SET OPTIMIZE`, `ALTER SESSION`. The write
  connection goes back to the ATTACH's pool, and later writes would
  inherit the change. The check reads the statement's first words only:
  the same change made inside `EXECUTE BLOCK` or `EXECUTE STATEMENT`, or a
  context variable set with `RDB$SET_CONTEXT('USER_SESSION', ...)`, is not
  refused (see "Session state" below).
- **Passwords.** When a statement mentions `PASSWORD` (`CREATE USER ...
  PASSWORD '...'`, `EXECUTE STATEMENT ... PASSWORD '...'`, also inside a
  nested statement), this extension's query log and error messages show
  every string literal of it as `'***'`. DuckDB's own logs and the client
  still see the call as typed.
- **Two calls in one statement** (for example under `UNION ALL`) run one
  after the other on the DuckDB transaction's write connection. On DuckDB
  2.0, in autocommit, this form is refused: run it between `BEGIN` and
  `COMMIT` (see below).
- **Session state** that a procedure, a trigger or an `EXECUTE BLOCK` sets
  (a context variable from `RDB$SET_CONTEXT('USER_SESSION', ...)`, a role,
  a time zone, rows of an `ON COMMIT PRESERVE ROWS` temporary table) doesn't
  reach later queries on Firebird 4+: before a connection that ran a write
  transaction or a `firebird_query` goes back to the ATTACH's pool, the
  extension runs `ALTER SESSION RESET` on it (one round trip). The reset
  fires the database's `ON DISCONNECT` and `ON CONNECT` triggers, with the
  `RESETTING` variable true, so a trigger can skip it; `SET
  firebird_pool_session_reset = false` turns the reset off. Firebird 3 has
  no session reset: there, such state stays with the pooled connection,
  context variables set with `RDB$SET_CONTEXT('USER_SESSION', ...)`
  included, and later queries that get that connection see it.
- **DDL.** `CREATE`, `ALTER` and `DROP` run like any statement. Once the
  transaction commits, the ATTACH loads its tables again, so `fb.main`
  shows the change without a new ATTACH; a prepared statement sees it too
  (DuckDB binds it again on every execution). Before DDL, the ATTACH closes its
  idle connections: an idle Firebird attachment keeps the tables it used
  in its metadata cache, and Firebird refuses to drop or alter them while
  it lives ("object ... is in use"). DDL is recognized by the statement's
  own type: DDL run through `EXECUTE BLOCK` or `EXECUTE STATEMENT` is not,
  so neither step happens for it; run `firebird_clear_cache` after it, as
  for DDL made outside this ATTACH.
- **The write is kept when the client stops reading, and a statement is
  all or nothing.** A client that reads the one row with `fetchone()` and
  closes still keeps the write. On DuckDB 1.5 the statement's result is
  materialized, as DuckDB does for `INSERT`, and the write commits with the
  statement. On DuckDB 2.0, in autocommit, the call commits its Firebird
  write before it returns the row, so it must be the whole statement:
  `SELECT * FROM firebird_execute(...)`, or a choice of its columns. Any
  other form (an expression or a filter on its row, an aggregate, `LIMIT`,
  a join, `UNION ALL`, `INSERT ... SELECT`) is refused before it writes,
  because the rest of the statement could fail after that commit. The
  exception is an `INSERT INTO` a table of the same ATTACH that the call's
  rows go straight into: the call doesn't commit, and its write commits with
  the INSERT (see `INSERT INTO`). Between
  `BEGIN` and `COMMIT` every form runs, on both lines, and the write
  commits or rolls back with the transaction. The check reads DuckDB's
  optimized plan, so with the optimizer disabled (`PRAGMA
  disable_optimizer`) DuckDB 2.0 refuses every autocommit call.
- **Telemetry.** Each call leaves one `firebird_last_query()` /
  `firebird_query_log()` record: the statement in `remote_sql`,
  `operation = 'execute'`, `write_mode` (`row` or `batch`), `rows_written`
  (executions) and `rows_affected`; on failure, the error.

### `firebird_clear_cache(alias)`

Forgets the tables an ATTACH has loaded, so the next query loads the table
list and columns again, and closes the ATTACH's idle connections. Use it
after DDL made outside this ATTACH; DDL through `firebird_execute` does it
when its transaction commits.

```sql
SELECT * FROM firebird_clear_cache('fb');
```

Returns one row, `cleared BOOLEAN` (`true`).

### `INSERT INTO fb.main.T ...`

On an ATTACH opened with `READ_WRITE`, DuckDB's `INSERT` writes into a
Firebird table.

```sql
ATTACH 'database=C:/data/erp.fdb;user=APP_WRITER;password=secret'
  AS fb (TYPE firebird, READ_WRITE);

INSERT INTO fb.main.CUSTOMER SELECT id, name, city FROM staging_customers;
INSERT INTO fb.main.CUSTOMER (NAME, ID) VALUES ('Fa', 6);
COPY fb.main.CUSTOMER FROM 'customers.csv' (HEADER);
```

- **How.** The INSERT prepares `INSERT INTO "T" ("C1", ...) VALUES (?, ...)`
  once and runs it once per row, with the values encoded as for
  `firebird_execute(..., source := ...)`, in the DuckDB transaction's
  Firebird write transaction (see `firebird_execute`). It returns the row
  count like any INSERT; `EXPLAIN` shows `FIREBIRD_INSERT`. `COPY ... FROM`
  a file into an ATTACH table inserts the same way. The Firebird write
  transaction starts at the INSERT's first row, so a source that sorts or
  aggregates holds none open while it computes.
- **Columns.** DuckDB casts the values to the ATTACH's column types first.
  With a column list, only those columns are sent, and Firebird applies
  its own defaults, triggers and generators to the others. Without one,
  every column is sent; a `COMPUTED BY` column then makes Firebird refuse
  the INSERT, so name the columns. `DEFAULT` inside a `VALUES` list
  becomes NULL: the ATTACH doesn't declare Firebird's defaults to DuckDB.
- **Refused:** `RETURNING`, `DEFAULT VALUES`, `ON CONFLICT`,
  `INSERT OR REPLACE` and `MERGE` (use `firebird_execute`, with
  `UPDATE OR INSERT ... MATCHING` for an upsert), and an ATTACH without
  `READ_WRITE` or with `READ_ONLY`, all before any row is written.
- **Errors.** The first failing row stops the INSERT with "row N failed:"
  and Firebird's error; the DuckDB transaction rolls back, and with it
  every row of the INSERT.
- **Batches.** With `SET firebird_write_batch_size = N`, a Firebird 4+
  server gets the rows N at a time, as for `firebird_execute`; when they
  go one at a time instead, the record's `not_pushed_reasons` says why.
- **Fed by `firebird_execute`.** An `INSERT ... SELECT ... FROM
  firebird_execute(...)` on the same ATTACH shares one write transaction,
  committed when the statement ends, when the call's rows go straight into
  the INSERT. With an `ORDER BY` or an aggregate in between, DuckDB 2.0 may
  run the call before the INSERT starts; in autocommit it then refuses the
  call (it must be the whole statement, see `firebird_execute`). Between
  `BEGIN` and `COMMIT` both forms run. `commit_every` is refused there.
- **In a transaction.** Between `BEGIN` and `COMMIT`, scans of the ATTACH
  don't see the inserted rows yet (they read on their own connections),
  and, as for any DuckDB transaction, the transaction writes to one
  attached database only.
- **Not through `fb.main`:** `UPDATE`, `DELETE` and DDL. Use
  `firebird_execute`.
- **Telemetry.** One record per INSERT, made when it ends (so it follows
  the record of a Firebird scan in its source): `operation = 'insert'`,
  `table_name`, the prepared statement in `remote_sql`, `write_mode`
  (`row` or `batch`), `rows_written`, `firebird_time_us` (the encoding and
  Firebird calls), `total_time_us` and `error_message`. `rows_affected`
  holds the record counts in batch mode; one row at a time it is `NULL`
  (counting would cost one round trip per row). On an error no row is
  kept: `rows_written` is 0, and so is `rows_affected` in batch mode. A
  statement that fails outside the INSERT (in its source, or interrupted)
  still leaves the INSERT's record, with "did not complete"; a commit that
  fails sets the record's `error_message` ("the Firebird commit failed:
  ...") and its counts to 0.

### `firebird_attach_sql(connection_string [, schema])`

Generates DuckDB SQL views for Firebird tables.

Use this when you want a view-based workflow instead of a storage attach.

### `firebird_generate_dbt_sources(catalog_name)`

Generates dbt `sources.yml` content from an attached Firebird catalog.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT firebird_generate_dbt_sources('fb');
```

The generated YAML is a starting point. Users should still review names,
tests, descriptions, and project conventions.

## Level 3b - Metadata Bridge 2.0

Starting with v0.7, the extension populates standard `information_schema` views
with constraint data and provides additional catalog functions for deep
inspection of Firebird metadata.

### Populated `information_schema` views

After `ATTACH ... (TYPE firebird)`, the following views are populated:

#### `information_schema.table_constraints`

Exposes PK, UNIQUE, and FK constraints for all user tables.

```sql
SELECT constraint_name, constraint_type, table_name
FROM information_schema.table_constraints
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_type;
```

#### `information_schema.key_column_usage`

Maps columns participating in PK, UNIQUE, and FK constraints.

```sql
SELECT constraint_name, table_name, column_name, ordinal_position
FROM information_schema.key_column_usage
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_name, ordinal_position;
```

#### `information_schema.referential_constraints`

Exposes FK constraints with reference information.

**Note**: the `update_rule` and `delete_rule` columns always return
`'NO ACTION'` due to a DuckDB limitation. For the real Firebird rules
(`CASCADE`, `SET NULL`, etc.) use `firebird_foreign_keys` (described below).

```sql
SELECT constraint_name, unique_constraint_name
FROM information_schema.referential_constraints
WHERE constraint_catalog = 'fb';
```

### `firebird_foreign_keys(catalog_name)`

Lists all foreign-key constraints with real Firebird referential rules.

Output columns:

| Column | Type | Description |
|---|---|---|
| `fk_schema` | VARCHAR | Always `'main'` |
| `fk_table` | VARCHAR | Table declaring the FK |
| `fk_constraint` | VARCHAR | FK constraint name |
| `ordinal_position` | INTEGER | Column position in the key (0-based) |
| `fk_column` | VARCHAR | Column in the child table |
| `pk_table` | VARCHAR | Referenced table |
| `pk_constraint` | VARCHAR | Referenced PK/UNIQUE constraint name |
| `update_rule` | VARCHAR | Firebird rule (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |
| `delete_rule` | VARCHAR | Firebird rule (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |

```sql
SELECT * FROM firebird_foreign_keys('fb');
```

**Note**: `firebird_foreign_keys.ordinal_position` is 0-based (the raw Firebird `RDB$FIELD_POSITION`), whereas `information_schema.key_column_usage.ordinal_position` is 1-based. Account for the off-by-one when joining the two surfaces.

### `firebird_indexes(catalog_name)`

Lists all user indexes with their segments.

Output columns:

| Column | Type | Description |
|---|---|---|
| `table_schema` | VARCHAR | Always `'main'` |
| `table_name` | VARCHAR | Table owning the index |
| `index_name` | VARCHAR | Index name |
| `is_unique` | BOOLEAN | `true` if the index is unique |
| `is_active` | BOOLEAN | `true` if the index is active |
| `segment_position` | INTEGER | Column position in the index (0-based) |
| `column_name` | VARCHAR | Segment column name (`NULL` for expression indexes) |
| `expression_source` | VARCHAR | Expression (expression indexes only; `NULL` otherwise) |

```sql
SELECT * FROM firebird_indexes('fb');
```

### `firebird_generators(catalog_name)`

Lists user generators/sequences with initial and current values.

Output columns:

| Column | Type | Description |
|---|---|---|
| `generator_name` | VARCHAR | Generator name |
| `initial_value` | BIGINT | Configured initial value |
| `current_value` | BIGINT | Current value via `GEN_ID(name, 0)`; `NULL` if no privilege. Read per-generator (one round-trip each) to preserve per-generator isolation; on databases with very many generators this is N+1 round-trips. |

```sql
SELECT * FROM firebird_generators('fb');
```

### `firebird_domains(catalog_name)`

Lists user-defined domains with type, nullability, charset, and constraints.

Output columns:

| Column | Type | Description |
|---|---|---|
| `domain_name` | VARCHAR | Domain name |
| `base_type` | VARCHAR | Firebird type string (e.g. `VARCHAR(100)`, `NUMERIC(10,2)`) |
| `length` | INTEGER | Length in bytes (for text types) |
| `scale` | INTEGER | Decimal scale (absolute value) |
| `is_nullable` | BOOLEAN | `true` if the domain allows `NULL` |
| `charset_name` | VARCHAR | Character set (text types only) |
| `check_source` | VARCHAR | Domain `CHECK` clause (`NULL` if none) |
| `default_source` | VARCHAR | Domain `DEFAULT` clause (`NULL` if none) |

```sql
SELECT * FROM firebird_domains('fb');
```

**Note**: for `CHAR`/`VARCHAR` domains, the reported length is the Firebird-declared byte length (`RDB$FIELD_LENGTH`). Under multi-byte charsets (e.g. UTF8) this is larger than the character length (a UTF8 `VARCHAR(10)` reports `VARCHAR(40)`).

### `firebird_computed_columns(catalog_name)`

Lists computed columns (`COMPUTED BY`) for all user tables.

Output columns:

| Column | Type | Description |
|---|---|---|
| `table_schema` | VARCHAR | Always `'main'` |
| `table_name` | VARCHAR | Table containing the column |
| `column_name` | VARCHAR | Computed column name |
| `expression_source` | VARCHAR | `COMPUTED BY` expression |

```sql
SELECT * FROM firebird_computed_columns('fb');
```

### `firebird_dependencies(catalog_name)`

Lists dependencies between database objects (tables, procedures, triggers, etc.).

Output columns:

| Column | Type | Description |
|---|---|---|
| `object_name` | VARCHAR | Dependent object |
| `object_type` | VARCHAR | Human-readable type (e.g. `TABLE`, `TRIGGER`, `PROCEDURE`) |
| `object_type_code` | INTEGER | Raw `RDB$DEPENDENT_TYPE` code |
| `depends_on_name` | VARCHAR | Object depended upon |
| `depends_on_type` | VARCHAR | Human-readable type of the referenced object |
| `depends_on_type_code` | INTEGER | Raw `RDB$DEPENDED_ON_TYPE` code |
| `field_name` | VARCHAR | Specific column referenced (`NULL` for object-level dependencies) |

```sql
SELECT * FROM firebird_dependencies('fb');
```

### `firebird_comments(catalog_name)`

Lists `RDB$DESCRIPTION` comments for user tables, views, and columns.

Output columns:

| Column | Type | Description |
|---|---|---|
| `object_schema` | VARCHAR | Always `'main'` |
| `object_name` | VARCHAR | Table or view name |
| `object_type` | VARCHAR | `'TABLE'`, `'VIEW'`, or `'COLUMN'` |
| `column_name` | VARCHAR | Column name (column rows only; `NULL` for object rows) |
| `comment` | VARCHAR | Comment text |

```sql
SELECT * FROM firebird_comments('fb');
```

## Level 4 - Diagnostics and observability

### `firebird_profile_table(qualified_name [, exact_row_count])`

Returns a single-row factual diagnostic for one Firebird relation reachable
through an attached Firebird catalog. The argument is a qualified name in
`catalog.schema.table` form; the schema part is accepted only as `main`
(the Firebird ATTACH path exposes exactly one schema) and may be omitted.
The optional named parameter `exact_row_count=true` replaces the cheap row
estimate with a real server-side `COUNT(*)` (see "Row estimate" below).

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT *
FROM firebird_profile_table('fb.main.CUSTOMER');
-- equivalent short form:
SELECT *
FROM firebird_profile_table('fb.CUSTOMER');
```

Output columns:

- `table_name`
- `object_type` - `TABLE` or `VIEW`
- `has_primary_key` - boolean
- `primary_key_columns` - list of columns
- `indexes` - list of `NAME (COL, ...) [UNIQUE] [PK]` descriptions
- `watermark_candidates` - columns whose type makes them plausible
  high-water-mark columns (integer-family and date/timestamp)
- `filter_candidates` - indexed columns whose type pushes down cheaply
- `full_scan_risk` - `LOW`, `MEDIUM`, or `HIGH`
- `recommended_partitions` - advisory `partitions=N` value
- `warnings` - list of explicit caveats (human-readable strings)
- `alerts` - structured form of `warnings`: `LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR))`
- `estimated_rows` - best available row count: PK-range upper bound by
  default, exact `COUNT(*)` with `exact_row_count=true`; `NULL` when no
  numeric-PK range was probed and no exact count ran
- `row_estimate_method` - `pk_range_upper_bound` or `exact_count`;
  `NULL` alongside a `NULL` estimate

This is a factual diagnostic, not a cost-based advisor. Heuristics are
simple and explicit: a watermark candidate is judged by type, not by proven
monotonicity; `recommended_partitions` is derived from the primary-key
`MIN`/`MAX` range, not a row count, and is advisory only. The `warnings`
column carries those caveats inline. Use it to decide whether to scan live,
filter harder, partition, or materialize through DuckDB/dbt/Parquet.

#### Row estimate — how it is produced

`estimated_rows` never triggers a full scan on its own:

- **`pk_range_upper_bound`** (default, tables with a single-column numeric
  PK): `MAX(PK) - MIN(PK) + 1` from the same probe that feeds
  `recommended_partitions`. It is an *upper bound that assumes a dense
  key* — sequences/identity columns track it closely; sparse keys (gaps,
  deletes) overcount. It is never presented as a count.
- **`exact_count`** (only with `exact_row_count=true`): a real
  `SELECT COUNT(*)` executed server-side on the source table. This reads
  every row on the server — request it knowingly. When the count cannot
  run (e.g. privilege), the cheap estimate stays and
  `row_estimate_method` shows what was actually produced; the
  `exact_count_executed` alert is emitted only when the count really ran.
- Tables without a usable single-column numeric PK (composite / non-numeric
  / no PK) have no cheap estimate: both columns are `NULL` unless
  `exact_row_count=true` produces one.

### `recommended_partitions` — how it is computed

`recommended_partitions` is an **advisory** number. It does **not** change
how `firebird_scan` runs; nothing is parallelized automatically and there is
no promise of a performance gain. It only tells you what `partitions=N` you
*could* try.

As of this version, `recommended_partitions` is computed by the exact same
`PickPartitionCount` function that `firebird_scan` itself calls at scan
time. Previously these were two separate, differently-calibrated formulas,
so a profile's recommendation and a real scan's chosen partition count
could disagree; they no longer can.

It is derived only from the single-column numeric primary key's `MIN`/`MAX`
range width (no row count, no `COUNT(*)`, no full scan):

- **No single-column numeric PK** (view, no PK, composite PK, non-numeric
  PK): `recommended_partitions = 1`, with a `warnings` entry explaining why
  the PK-range parallel lever is unavailable.
- **Numeric PK but a small range** (`MIN`/`MAX` span < 10000): still
  `recommended_partitions = 1`, with a note that partitioning would add
  overhead without meaningful parallelism. A numeric PK does **not**
  automatically mean parallel is recommended.
- **Numeric PK with a wide range**: a conservative `partitions=N` between 2
  and 8 (scaled by range width, capped at 8). The `warnings` make clear this
  is advisory, derived from range width (so it can be uneven if the PK is
  sparse), and must be validated against the live server.

When `partitions > 1` is recommended, `warnings` also carries a
**server-side parallelism caveat**: if Firebird server-side parallelism is
already enabled/configured (e.g. Firebird 5 `ParallelWorkers`), prefer
starting with `partitions=1` or benchmark before combining server-side and
client-side parallelism. The extension does not probe the server's parallel
setting, so this is a generic caveat, not a detected condition.

For `object_type = VIEW`, the function additionally performs a shallow,
conservative inspection of the stored view definition
(`RDB$VIEW_SOURCE`) and emits `warnings` when it detects:

- a `JOIN` in the definition,
- aggregation (`GROUP BY` or `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/`LIST`),
- no `WHERE` filter in the definition.

The detection is token-level pattern matching, not a SQL parser, and the
view source text itself is never returned — only the shape flags drive the
warnings. String literals (including SQL-escaped doubled quotes) and SQL
comments are blanked before matching, so keyword-shaped text inside a
literal or comment does not produce false positives. Whitespace is
collapsed first, so keywords split across newlines or tabs in the stored
view text (e.g. `GROUP` and `BY` on separate lines) are still detected. When the definition cannot be read, a `view definition not
inspected` warning is emitted instead. These warnings point toward
materializing heavy views through DuckDB/dbt/Parquet rather than scanning
them repeatedly.

The diagnostic reads only the Firebird system tables (`RDB$*`) plus a
best-effort primary-key `MIN`/`MAX` probe; it never reads business rows and
never returns the connection string or credentials.

### `alerts` column — structured warning catalog

`alerts` is the machine-readable counterpart of `warnings`. Both columns are
produced from the same internal alert list, so they are **1:1 and in the same
order**: `alerts[i].message == warnings[i]` for every index `i`.

Each element of `alerts` is a `STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)`:

- `code` — stable identifier (see catalog below); public API contract.
- `severity` — one of `LOW`, `MEDIUM`, or `HIGH`, reusing the `full_scan_risk`
  vocabulary: `LOW` = advisory / informational, `MEDIUM` = degraded but
  functional, `HIGH` = real operational risk.
- `message` — the same human-readable string that appears in `warnings`.

**Stable-code contract:** alert codes are public API. Once a code is shipped it
is never reused for a different condition and its meaning never changes. New
conditions always receive new codes.

Alert code catalog:

| Code | Severity | Meaning |
| --- | --- | --- |
| `view_no_scan_lever` | HIGH | Object is a VIEW: no PK/index/partition lever available |
| `view_definition_not_inspected` | MEDIUM | View source unreadable; shape unknown |
| `view_contains_join` | HIGH | View definition contains a JOIN |
| `view_contains_aggregation` | HIGH | View definition has GROUP BY or an aggregate function |
| `view_no_filter` | MEDIUM | View definition has no WHERE filter |
| `partition_advisory` | LOW | Recommended partitions (derived from PK range) is advisory |
| `server_parallelism_caveat` | LOW | Server-side parallelism (Firebird 5) caveat when `partitions > 1` |
| `pk_range_small_serial` | LOW | PK MIN/MAX span is small — serial scan recommended |
| `no_primary_key` | HIGH | No primary key — full scan, no PK ranges (`partition_by='dbkey'` can split it on Firebird 4/5) |
| `composite_pk_serial` | LOW | Composite PK — serial by default (`partition_by='dbkey'` can split it on Firebird 4/5) |
| `numeric_pk_no_range_serial` | LOW | Single numeric PK but no usable MIN/MAX range — serial scan |
| `non_numeric_pk_serial` | LOW | Single non-numeric PK — serial by default (`partition_by='dbkey'` can split it on Firebird 4/5) |
| `no_indexed_filter_columns` | MEDIUM | No cheap indexed filter columns found |
| `none_charset_text_columns` | MEDIUM | One or more text columns use CHARACTER SET NONE |
| `filter_before_scan` | MEDIUM | HIGH-risk base table with filterable columns — the message names them |
| `materialize_before_scan` | HIGH | HIGH-risk base table with no filter/watermark columns — materialize instead |
| `exact_count_executed` | LOW | `exact_row_count=true` ran a server-side `COUNT(*)` |

### `firebird_index_profile(qualified_name)`

Returns **one row per existing index** for a Firebird table reachable
through an attached catalog. The argument is a qualified name in the same
form as `firebird_profile_table`: `catalog.schema.table` (the schema part is
accepted only as `main`, and may be omitted).

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT *
FROM firebird_index_profile('fb.main.CUSTOMER');
```

**Per-index grain:** a table with zero indexes still emits exactly **one
synthetic row** — never zero rows. On that row, `index_name IS NULL` marks
the absence of any index, `columns` is an empty list `[]`, and the remaining
index-scoped columns (`is_unique`, `is_active`, `is_primary_key`,
`is_foreign_key`, `selectivity`) are `NULL`. The synthetic row always carries
the `no_indexes_on_table` alert (`HIGH` severity) in `alerts`.

Output columns (9):

| Column | Type | Notes |
| --- | --- | --- |
| `index_name` | VARCHAR | Index name; `NULL` on the synthetic "no index" row |
| `columns` | LIST(VARCHAR) | Index columns in segment order; `[]` on the synthetic row |
| `is_unique` | BOOLEAN | Whether the index is unique; `NULL` on the synthetic row |
| `is_active` | BOOLEAN | Whether the index is active (`RDB$INDICES.RDB$INDEX_INACTIVE = 0`); `NULL` on the synthetic row |
| `is_primary_key` | BOOLEAN | Whether the index backs the table's primary key; `NULL` on the synthetic row |
| `is_foreign_key` | BOOLEAN | Whether the index backs a foreign key; `NULL` on the synthetic row |
| `selectivity` | DOUBLE | Raw value from `RDB$STATISTICS`; `NULL` when never computed; `NULL` on the synthetic row |
| `alerts` | LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)) | Alerts for this index (or the "no index" alert on the synthetic row) |
| `unindexed_filter_candidates` | LIST(VARCHAR) | Table columns not covered by any index segment (active or inactive); repeated on every row |

Alert code catalog:

| Code | Severity | Condition |
| --- | --- | --- |
| `no_indexes_on_table` | HIGH | Table has no indexes at all — drives the synthetic row |
| `index_inactive` | MEDIUM | `is_active = false` for that index |
| `missing_statistics` | LOW | `RDB$STATISTICS IS NULL` for that index |

Notes:

- `selectivity` is a **raw**, uninterpreted value: a **lower** value tends to
  indicate a **more selective** index. `NULL` means the statistic was never
  computed — it does **not** mean "stale". There is **no numeric threshold**
  in this version: no low-selectivity alert exists (this avoids turning the
  function into an opaque advisor).
- `unindexed_filter_candidates` excludes any column covered by **any** index
  segment, including an **inactive** index's segments — that inactivity
  signal arrives separately via the `index_inactive` alert on that index's
  own row, not by omission from this list.

Use it to:

- See, index by index, what exists, whether it's active, whether it has
  computed statistics, and whether it backs a PK/FK.
- Quickly confirm whether a table has ended up with no indexes at all.
- Find filter-candidate columns that currently have no index coverage at all.

```sql
SELECT index_name, columns, is_unique, is_active, selectivity
FROM firebird_index_profile('fb.main.CUSTOMER');

-- Filter-candidate columns with no index coverage:
SELECT DISTINCT unindexed_filter_candidates
FROM firebird_index_profile('fb.main.CUSTOMER');
```

### `firebird_last_query()`

Returns telemetry for the most recent Firebird query captured by the
extension.

Useful fields include:

- remote SQL
- pushed filters/projection details where available
- residual (not-pushed) filters and a coarse reason for each
  (`not_pushed_reasons`)
- paging actually pushed to Firebird (`limit_pushed` / `offset_pushed`,
  `NULL` when none)
- elapsed time
- rows read
- estimated bytes read from Firebird (`bytes_read_estimate`)
- parallel scan details, including whether the partitions shared one
  snapshot (`snapshot_mode`) and how the table was split
  (`partition_method`)
- connection reuse details
- what the record is (`operation`: `scan`, `query` for `firebird_query`,
  `execute` for `firebird_execute`, `insert` for an INSERT into an ATTACH
  table) and, for writes, `write_mode` (`row` or `batch`),
  `rows_written` (executions) and `rows_affected` (Firebird's record
  counts; `NULL` when not counted). The three write columns are `NULL` for
  reads.

The schema is 25 columns (shared with `firebird_query_log()`). The
pushdown-explainability columns (`limit_pushed`, `offset_pushed`,
`not_pushed_reasons`) make it explicit what reached Firebird and why a
filter stayed local, and `bytes_read_estimate` (added last, G5) supports
network budgeting before a long scan. See `docs/en/observability.md` for
the full column reference and the reason vocabulary.

#### `bytes_read_estimate` — what it is and what it counts

fbclient does not expose a wire-traffic meter, so the extension
**estimates** the transferred payload:

```
bytes_read_estimate = rows_read × XSQLDA row width + BLOB segment bytes
```

- **XSQLDA row width** is the sum of the per-column fetch descriptor
  sizes, fixed once at prepare time: `CHAR(n)` counts `n` bytes,
  `VARCHAR(n)` counts `n + 2` (length prefix), numerics/temporals count
  their fixed widths, and a BLOB column counts its 8-byte id frame.
- **BLOB content** is counted where it is actually read: the segment
  bytes returned per BLOB are added to the estimate as they cross
  `isc_get_segment`.
- **Deterministic**: two identical runs of the same query report the
  same estimate (same row width, same row count).

It does **not** include: protocol headers and per-row framing, the
prepare/execute/bind round-trips, paging (`ROWS`) metadata, or the
connection/attachment handshake. Treat it as a payload lower-bound-ish
estimate for network budgeting — e.g. "this scan will move roughly X MB"
— not as an exact byte meter.

Network-budget example (WAN at ~0.3 MB/s — is this scan minutes or
hours?):

```sql
SELECT * FROM fb.main.BIG_TABLE LIMIT 0;   -- or any probe scan
SELECT rows_read, bytes_read_estimate,
       bytes_read_estimate / rows_read AS bytes_per_row,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb
FROM firebird_last_query();
```

Remedy per `not_pushed_reasons` code (the codes are a stable API):

| Reason | Meaning | Remedy |
|---|---|---|
| `NONE_CHARSET` | The filter targets a `CHARACTER SET NONE` text column and was not pushed. With `none_pushdown=true` this means the literal had no byte in the target encoding (e.g. `'€'` under `iso8859_1`) or the column is a NONE text BLOB; without the flag, every text filter on NONE columns is gated. | Use `none_encoding='strict'` for known-UTF-8 data, or set `none_pushdown=true` (direct `firebird_scan` parameter or ATTACH option) to push `=` / `IN` over NONE CHAR/VARCHAR; ranges, `LIKE` and `NOT IN` always stay client-side. Confirm with `firebird_explain_pushdown`. |
| `UNSUPPORTED_OP` | The filter operator/shape/constant type has no Firebird SQL translation yet. | Simplify the predicate or accept DuckDB-side filtering. |
| `ROWID_OR_INVALID_COLUMN` | The filter targets the virtual rowid or a column outside the resolved schema. | Filter on real columns instead of `rowid`. |
| `UNSUPPORTED_PROJECTION_MAPPING` | The filter's projected column could not be mapped back to a source column. | Check the query's column list / projection shape. |

To make residual filters act instead of only report, set
`SET firebird_unpushed_mode = 'warn' | 'error'` — see
[Level 5 - Session options](#level-5---session-options).

### `firebird_explain_pushdown(sql)`

Returns an a-priori, plan-only analysis of what a `SELECT` statement would
push down to Firebird — **without executing the query** and **without opening
a data cursor on the user's query**. This is the complement of
`firebird_last_query()`: explain is prospective (before any run), while
`firebird_last_query()` is post-hoc (after the last real scan).

Explain never opens a data cursor on the user's query and never sends the
user's query SQL to Firebird. On a cold catalog (first reference to a table),
the bind via `ExtractPlan` may load catalog metadata (schema) and run the PK
probe (MIN/MAX) — exactly as the bind of any ATTACH-path query does — and
these loads are memoised immediately. No data cursor is opened on the user's
query; no user-query SQL is sent to Firebird.

**Note on `remote_sql`:** the column shows the base remote SELECT the scanner
would send; complex predicates (LIKE-prefix, NOT IN, BETWEEN) appear in the
`pushed_filters` list rather than inline in `remote_sql` — the same telemetry
split used by `firebird_last_query()`.

Because explain never runs the query, it leaves no entry in
`firebird_last_query()` telemetry.

#### Accepted input

- A single `SELECT` statement referencing one or more Firebird tables via an
  `ATTACH` alias (e.g. `fb.main.EMPLOYEE`).
- A `WITH ... SELECT` (CTE) whose body is a Firebird scan.

Rejected input (raises an error):

- DML statements (`UPDATE`, `DELETE`, `INSERT`, `MERGE`).
- Direct `firebird_scan(...)` calls — use the `ATTACH` alias form instead.
- `WITH ... DELETE` or other non-SELECT CTE statements.

#### Output columns (18)

| Column | Type | Notes |
| --- | --- | --- |
| `scan_ordinal` | BIGINT | 1-based ordinal; distinct for each `LogicalGet` node, including self-joins |
| `table_name` | VARCHAR | Firebird table name (never the connection string) |
| `remote_sql` | VARCHAR | SQL that would be sent to Firebird; bind values appear as `?` — no literals, no connection-string fragments |
| `projected_columns` | VARCHAR[] | Columns selected from Firebird (post projection-pruning) |
| `pushed_filters` | VARCHAR[] | Predicates the scanner would push to Firebird |
| `residual_filters` | VARCHAR[] | Predicates DuckDB would revalidate above the scan; always `len(residual_filters) == len(not_pushed_reasons)` |
| `not_pushed_reasons` | VARCHAR[] | One coarse reason per `residual_filters` entry, same order; values: `NONE_CHARSET`, `UNSUPPORTED_OP`, `ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING` |
| `limit_pushed` | BIGINT | `row_limit=` named-parameter value if it would be pushed to a Firebird `ROWS` clause; `NULL` for SQL `LIMIT` (DuckDB applies SQL `LIMIT` above the scan) |
| `offset_pushed` | BIGINT | `row_offset=` named-parameter value if pushed; `NULL` otherwise |
| `rows_clause` | VARCHAR | Firebird `ROWS m TO n` clause that would be emitted, or `NULL` |
| `pk_range_eligible` | BOOLEAN | `true` only for a single-column numeric primary key |
| `pk_range_column` | VARCHAR | Name of that PK column, or `NULL` when not eligible (e.g. composite or non-numeric PK) |
| `pk_range_reason` | VARCHAR | One of four normalized values: `single numeric PK`, `non-numeric PK`, `composite PK`, `no primary key` |
| `scan_strategy` | VARCHAR | `pk-range-partitionable` when `pk_range_eligible` is true; `passthrough` for a `firebird_query()` call (SQL sent as written, `table_name` NULL); `serial` otherwise |
| `view_heavy` | BOOLEAN | `NULL` when the scan target is a base table; for a view, `true` if it has a JOIN, `GROUP BY`, an aggregate function, or an unreadable/uninspectable definition, `false` for a simple pass-through view |
| `view_heavy_reasons` | VARCHAR[] | Factual notes about the view's shape: `view contains a JOIN`, `view contains aggregation (GROUP BY or aggregate functions)`, `view definition not inspected (RDB$VIEW_SOURCE unavailable or unreadable)`, `view has no WHERE filter in its definition`. The last one can appear even when `view_heavy` is `false` (a filterless-but-simple view is not "heavy", but the observation is still useful) — do not assume this list is empty whenever `view_heavy` is `false`. Empty (and `view_heavy` is `NULL`) when the target is a base table. |
| `charset_pushdown_blocked` | BOOLEAN | `true` when any entry in `not_pushed_reasons` is `NONE_CHARSET` — a pure synthesis of that already-existing signal, no new detection |
| `planned_partitions` | BIGINT | The exact partition count `firebird_scan` would use for this table's PK range, computed by the same `PickPartitionCount` function the scanner itself calls; `NULL` when `pk_range_eligible` is `false` |

#### Invariants

- `len(residual_filters) == len(not_pushed_reasons)` always holds.
- A `NOT IN` on a `CHARACTER SET NONE` text column is recorded in
  `residual_filters` as `complex_filter[none_gated]` with the parallel
  `not_pushed_reasons` entry `NONE_CHARSET`.
- `limit_pushed` / `offset_pushed` / `rows_clause` are `NULL` for SQL
  `LIMIT` — only the `row_limit=` / `row_offset=` named parameters of the
  scanner are considered for pushdown.
- `pk_range_column` is `NULL` whenever `pk_range_eligible` is `false`.
- `planned_partitions` is `NULL` whenever `pk_range_eligible` is `false`.

Each `not_pushed_reasons` code maps to a documented remedy — see the table
in the [`firebird_last_query()`](#firebird_last_query) section above.

#### Notes on `view_heavy`, `charset_pushdown_blocked`, and `planned_partitions`

- `planned_partitions` is the exact value `firebird_scan` would use for this
  table's PK range — computed by the same `PickPartitionCount` function, not
  a separate estimate.
- `charset_pushdown_blocked` is a boolean synthesis of `not_pushed_reasons`
  containing `NONE_CHARSET` — a convenience flag over an already-existing
  signal, not new detection.
- `view_heavy` / `view_heavy_reasons`: when the scan target is a view with a
  JOIN, `GROUP BY`, aggregate function, or an unreadable definition,
  `view_heavy = true` and `view_heavy_reasons` explains why. This is also
  why `remote_sql` may show no `ROWS` clause even when `row_limit`/
  `row_offset` were requested against that view — see the `RDB$DB_KEY` note
  in the `firebird_scan` section above. `view_heavy_reasons` can carry a
  `view has no WHERE filter in its definition` note independently of
  `view_heavy` — a simple, filterless view is still reported as `view_heavy
  = false`, but the missing-`WHERE` observation is surfaced anyway.

#### Usage example

```sql
ATTACH 'C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

-- Check what would be pushed before actually running the query:
SELECT table_name, pushed_filters, residual_filters, not_pushed_reasons,
       pk_range_eligible, scan_strategy
FROM firebird_explain_pushdown(
  'SELECT EMP_ID, EMP_NAME FROM fb.main.EMPLOYEE WHERE EMP_ID > 10');
```

### `firebird_query_log()`

Returns the in-memory query log for the current DuckDB session.

Each row carries the same 25-column telemetry schema as
`firebird_last_query()` — including `bytes_read_estimate`, so a rotated
log keeps the per-query transfer estimates of the session.

The log is bounded by `firebird_query_log_size` and is intended for
debugging, diagnostics, and support.

### `firebird_pool_stats(catalog_name)`

Returns factual connection-pool state for a single attached Firebird
catalog, one row per pool: the read pool, and, for an ATTACH opened with
`READ_WRITE`, its write pool. The argument is the **explicit ATTACH
alias** — the function does not enumerate catalogs.

```sql
ATTACH 'database=C:/data/erp.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

SELECT EMP_ID FROM fb.main.EMPLOYEE WHERE EMP_ID = 1;

SELECT * FROM firebird_pool_stats('fb');
```

Output columns (12):

| Column | Type | Notes |
|---|---|---|
| `catalog_name` | VARCHAR | The alias passed in |
| `pool_enabled` | BOOLEAN | Whether the ATTACH pool is enabled |
| `max_idle_size` | BIGINT | Configured idle-queue cap (`firebird_pool_max_size`); `0` = unlimited |
| `idle_timeout_ms` | BIGINT | Configured idle expiry (`firebird_pool_idle_timeout_ms`); `0` = no expiry |
| `idle_connections` | BIGINT | Connections currently parked in the idle queue |
| `total_created` | BIGINT | Lifetime physical connections created |
| `total_reused` | BIGINT | Lifetime connections served from the idle queue |
| `total_discarded` | BIGINT | Lifetime connections destroyed (pool disabled, idle cap/expiry hit, a worker connection that could not join a partitioned scan's snapshot, one whose transaction could not be reset after a partitioned scan, one whose catalog load failed, or one whose attachment was lost) |
| `active_connections` | BIGINT | Connections currently leased out and not yet returned via Release or discarded |
| `last_error` | VARCHAR | Sanitized message of the most recent failed connection creation (`NULL` when none); the password is redacted before storage |
| `bytes_read_estimate` | BIGINT | G5 — lifetime estimate of Firebird payload bytes fetched by scans that ran **inside this catalog** (ATTACH path). Same semantics as the other lifetime counters: process-relative, never reset. Direct `firebird_scan()` calls are **not** included — they belong to no catalog. Estimate definition: see [`firebird_last_query()`](#firebird_last_query). `0` on the `write` row: scans never use that pool |
| `pool` | VARCHAR | `'read'`: the pool of scans, `firebird_query` and catalog loads. `'write'`: the pool of the DuckDB transactions' write connections, listed only for an ATTACH opened with `READ_WRITE` (see [`firebird_execute`](#firebird_executealias-sql--source-commit_every-count_rows)) |

It reads only counters and config the pool already tracks, and it does
**not** lease a connection — calling it never perturbs the pool it reports
on. The configured values mirror the pool settings read at `ATTACH` time;
a later `SET` does not retune an existing pool (re-`ATTACH` to apply).

Use it to confirm pooling is actually reusing connections, to size
`firebird_pool_max_size`, to verify a `firebird_pool_enabled = false`
catalog never parks idle connections — or, with `bytes_read_estimate`,
to watch how much Firebird payload a whole working session has pulled
through one catalog (e.g. before/after a heavy extraction):

```sql
SELECT bytes_read_estimate,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb_total
FROM firebird_pool_stats('fb')
WHERE pool = 'read';
```

`last_error` is populated only for post-`ATTACH` connection-creation
failures (a failure during `ATTACH` itself never registers a catalog) —
for example the Firebird server went away between queries. The stored
message is the exception text with every occurrence of the connection
password replaced by `****` and the length capped at 500 characters.
`active_connections` counts leases handed out minus returns; a query in
flight on another connection holds it above 0.

Current limitations:

- **Per-catalog, alias-only**: you pass one ATTACH alias; there is no
  no-argument form that lists every attached catalog.
- **No error history**: only the *most recent* connection error is kept
  (no timestamped list of past failures).

### `firebird_type_audit(catalog_name)`

Findings-only audit of type/charset conversion fidelity for an attached
Firebird catalog. Only columns that carry a caveat are emitted; columns
whose Firebird type maps to DuckDB cleanly and without semantic caveats are
omitted. The function never reads business data: it queries only Firebird
system tables (`RDB$RELATION_FIELDS`, `RDB$FIELDS`) via a pooled
connection lease, and it never executes the user's SQL.

```sql
SELECT * FROM firebird_type_audit('fb');
```

Output columns (7):

| Column | Type | Notes |
| --- | --- | --- |
| `table_schema` | VARCHAR | Always `'main'` (Firebird ATTACH exposes one schema) |
| `table_name` | VARCHAR | Firebird relation name |
| `column_name` | VARCHAR | Firebird field name |
| `firebird_type` | VARCHAR | Firebird type as reported (e.g. `DECFLOAT(16)`, `CHAR(10) CHARACTER SET NONE`) |
| `duckdb_type` | VARCHAR | DuckDB type the extension projects (e.g. `VARCHAR`, `HUGEINT`) |
| `finding` | VARCHAR | Finding code — see table below |
| `detail` | VARCHAR | One-sentence explanation of the caveat |

Finding codes:

| Code | Meaning |
| --- | --- |
| `decfloat_as_varchar` | DECFLOAT(16/34) projected as VARCHAR via server-side `CAST`; text semantics apply to filter comparisons |
| `int64_numeric_widenable` | int64-backed `NUMERIC`/`DECIMAL` with scale ≠ 0; the physical int64 scaled range exceeds `DECIMAL(18,s)`'s domain — pass `numeric_widen_int64=true` (`firebird_scan` parameter or ATTACH option) to project `DECIMAL(38,s)` losslessly |
| `int128` | Native INT128 or scale-0 NUMERIC/DECIMAL(19–38) projected as HUGEINT (lossless); some BI tools lack INT128 support |
| `time_tz` | TIME WITH TIME ZONE; session offset / zone-id handling caveat |
| `timestamp_tz` | TIMESTAMP WITH TIME ZONE; session offset / zone-id handling caveat |
| `none_charset` | CHAR/VARCHAR or text BLOB with CHARACTER SET NONE; decoding depends on the scan's `none_encoding` (documented default `win1252`); the function does not read that setting |
| `blob_text` | Text BLOB (non-NONE charset) projected as VARCHAR; charset and size caveat |

Notes:

- `decfloat_as_varchar`, `int128`, `time_tz`, and `timestamp_tz` require
  Firebird 4 or later (FB4+ types); they will not appear against a FB3 catalog.
- `none_charset` decoding behavior at scan time is controlled by the
  `none_encoding` session option (default `win1252`). This function reads the
  catalog schema, not that setting, so the finding is emitted whenever the
  character set is NONE regardless of what `none_encoding` is configured to.
- `int128` flags native `INT128` and scale-0 `NUMERIC`/`DECIMAL` with
  precision 19–38 (all projected as HUGEINT). `int64_numeric_widenable`
  covers the scaled int64-backed numerics (precision 10–18, scale ≠ 0);
  narrower scaled numerics (precision 1–9, SMALLINT/INTEGER-backed) always
  fit their DuckDB `DECIMAL` and are not flagged.

### `firebird_health(alias)`

Returns a single row with a health diagnostic for a Firebird database
reachable through an alias already attached via `ATTACH ... (TYPE firebird)`.
Reads from `MON$DATABASE`, `MON$ATTACHMENTS`,
`rdb$get_context('SYSTEM','ENGINE_VERSION')`, and
`RDB$DATABASE.RDB$CHARACTER_SET_NAME`. Read-only operation.

```sql
ATTACH 'database=C:/data/company.fdb user=SYSDBA password=masterkey'
  AS fb (TYPE firebird);

SELECT *
FROM firebird_health('fb');
```

Output columns (18):

| Column | Type | Description |
| --- | --- | --- |
| `engine_version` | VARCHAR | Firebird engine version (e.g. `'5.0.1'`) |
| `ods_version` | VARCHAR | On-disk structure version of the database file (e.g. `'13.1'`) |
| `sql_dialect` | INTEGER | Active SQL dialect of the database (normally `3`) |
| `default_charset` | VARCHAR | Default character set of the database (e.g. `'UTF8'`) |
| `page_size` | INTEGER | Page size in bytes (e.g. `8192`) |
| `forced_writes` | BOOLEAN | `true` when forced writes are enabled |
| `sweep_interval` | INTEGER | Configured sweep interval (transactions) |
| `oldest_transaction` | BIGINT | OIT — oldest interesting transaction |
| `oldest_active` | BIGINT | OAT — oldest active transaction |
| `oldest_snapshot` | BIGINT | OST — oldest snapshot transaction |
| `next_transaction` | BIGINT | Next transaction number |
| `oit_gap` | BIGINT | `next_transaction - oldest_transaction` |
| `oat_gap` | BIGINT | `next_transaction - oldest_active` |
| `attachments` | INTEGER | Number of connections visible to the current user |
| `warnings` | LIST(VARCHAR) | Active warning codes (empty when everything is healthy) |
| `client_version` | VARCHAR | Version of the loaded Firebird client library, e.g. `'5.0.3.1683'`; `NULL` when the library doesn't report one |
| `inline_blob_size` | INTEGER | Inline-blob limit of this attachment in bytes (Firebird counts each segment with a 2-byte length): blobs within it are sent with their row (`0` = inlining disabled); `NULL` when the client or the server is older than 5.0.3, and for local (XNET) and embedded attachments, where the server does not inline blobs. See [`firebird_max_inline_blob_size`](#set-firebird_max_inline_blob_size--bytes) |
| `network_protocol` | VARCHAR | Transport of this attachment: `'TCPv4'`, `'TCPv6'`, `'XNET'` (Windows local), `'WNET'` (Firebird 3/4 named pipes); `NULL` when embedded |

Warning codes:

| Code | Trigger condition |
| --- | --- |
| `oit_gap_high` | `oit_gap > 1,000,000` |
| `oat_gap_high` | `oat_gap > 1,000,000` |
| `sweep_disabled` | `sweep_interval = 0` |
| `forced_writes_off` | `forced_writes = false` |
| `charset_none` | `default_charset = 'NONE'` |
| `mon_unavailable` | Real monitoring-query failure |
| `inline_blobs_unavailable` | A TCP attachment to a 5.0.3+ server through a client library older than 5.0.3, so blobs are not inlined (load a 5.0.3+ `fbclient`) |

The first four codes (`oit_gap_high`, `oat_gap_high`, `sweep_disabled`,
`forced_writes_off`) are evaluated only when the monitoring read succeeds; if
`mon_unavailable` fires, they are suppressed because their inputs are `NULL`.
`charset_none` is always evaluated (it derives from `default_charset`, which is
read independently of `MON$`).

Notes:

- **The `1,000,000` gap threshold is a conservative default signal**, not a
  universal limit. It is a compile-time constant (`FB_HEALTH_GAP_THRESHOLD`);
  adjusting it requires recompiling the extension.
- **`attachments` is the count visible to the current user**: under limited
  privilege the `MON$` views expose only the current user's own connections;
  under monitoring privilege the full server count is returned. The value
  faithfully reflects what the database reports for that credential — it is
  not an error condition.
- **`mon_unavailable` fires only on a real monitoring-query failure**: limited
  visibility (fewer attachments visible) does not trigger this warning. When
  the `MON$DATABASE`/`MON$ATTACHMENTS` query raises an exception,
  `mon_unavailable` appears in `warnings`, the `MON$`-sourced columns are
  `NULL`, and `engine_version` and `default_charset` remain populated.

Usage:

```sql
-- Quick health check
SELECT engine_version, oit_gap, oat_gap, attachments, warnings
FROM firebird_health('fb');

-- Full diagnostic row
SELECT *
FROM firebird_health('fb');
```

## Level 5 - Session options

### `SET firebird_query_log_size = N`

Controls the maximum number of query telemetry entries kept in memory.

### `SET firebird_unpushed_mode = 'silent' | 'warn' | 'error'`

Controls what happens when a Firebird scan keeps filters in DuckDB that
were **not pushed down** to Firebird — the same signal telemetry surfaces
in `not_pushed_reasons` (`NONE_CHARSET`, `UNSUPPORTED_OP`,
`ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING`).

- `silent` (default) — current behaviour: telemetry only, zero extra cost.
- `warn` — one warning per scan through DuckDB's native warning channel
  (the CLI prints it to the console; embedded hosts receive it through the
  logging subsystem, and `warnings_as_errors` promotes it to an exception).
- `error` — the scan fails with an Invalid Input Error that lists the
  residual filter count, the reason codes present, and the per-reason
  remedy. The check runs once per scan, before any row is fetched, so
  arming it before a long load fails fast.

Example — arm the guard before a long load:

```sql
SET firebird_unpushed_mode = 'error';
COPY (SELECT * FROM fb.main.BIG_TABLE) TO 'load.parquet';
-- fails here if any filter stayed residual, with the reason + remedy

SET firebird_unpushed_mode = 'silent';   -- back to default in-session
```

An unknown value (`'warnn'`, ...) is rejected with an Invalid Input Error
as soon as a scan has residual filters to report — a typo never silently
disables a guard you asked for.

### `SET firebird_pool_enabled = true|false`

Enables or disables the ATTACH connection pool for future attaches.

### `SET firebird_pool_max_size = N`

Controls the maximum number of idle connections kept by the pool.

### `SET firebird_pool_idle_timeout_ms = MS`

Controls how long idle pooled connections may remain reusable.

### `SET firebird_transaction_snapshot = true|false`

Off by default. When on, every scan of a Firebird ATTACH in one DuckDB
transaction reads one Firebird snapshot (Firebird 4+). The ATTACH option
`transaction_snapshot` overrides it per catalog. See [One snapshot per
DuckDB transaction](#one-snapshot-per-duckdb-transaction-transaction_snapshot).

### `SET firebird_transaction_snapshot_idle_timeout = SECONDS`

Default `60`, `0` = no limit. With the transaction snapshot on, the server
releases a DuckDB transaction's snapshot when this many seconds pass from
the end of a statement to the next scan of the ATTACH in the transaction;
that scan then fails. See
[One snapshot per DuckDB transaction](#one-snapshot-per-duckdb-transaction-transaction_snapshot).

### `SET firebird_write_batch_size = N`

Rows per batch for Firebird writes (`firebird_execute` with `source`, and
`INSERT INTO` an ATTACH table) on Firebird 4+ servers. Default 0: one
execution per row. `firebird_execute(..., batch_size := N)` overrides it
for one call. See the batches note under `firebird_execute`.

### `SET firebird_write_lock_timeout = SECONDS`

How long a Firebird write (`firebird_execute`, `INSERT`) waits for a row lock held by
another transaction before it fails with "update conflicts with concurrent
update". Default 10; `0` fails at once (no wait); at most 32767. Read when
the write transaction starts.

### `SET firebird_pool_session_reset = BOOLEAN`

Default `true`. On Firebird 4+, a connection that ran a write transaction
or a `firebird_query` gets `ALTER SESSION RESET` before it goes back to the
ATTACH's pool, so later queries don't inherit context variables, a role, a
time zone or temporary-table rows that the caller's SQL set (scans and
catalog loads run SQL the extension writes, and skip it). The reset costs
one round trip and fires the database's `ON DISCONNECT` and `ON CONNECT`
triggers, with `RESETTING` true; `false` turns it off. Read when the write
transaction starts and when a `firebird_query` runs. Firebird 3 has no
session reset.

### `SET firebird_dummy_packet_interval = SECONDS`

Keepalive for long-running fetches. When set above `0`, **every**
connection the extension opens — `firebird_scan()` per-call connections
and `ATTACH` connections, pooled or fresh — sends
`isc_dpb_dummy_packet_interval` in the attach DPB, asking Firebird to
emit a dummy packet on the wire every `SECONDS` so a connection silently
dropped by a NAT gateway / stateful firewall is detected during the
fetch instead of surfacing later as a dead cursor
(`-504 Unable to complete request... cursor lost`) or connection
shutdown (`-902`).

- **Unit: seconds** — the same unit as `DummyPacketInterval` in the
  server's `firebird.conf` and the Firebird wire-protocol docs.
- **Default `0` = disabled**: the DPB item is not sent and the attach
  is byte-for-byte identical to previous versions.
- Read at `firebird_scan()` bind time and at `ATTACH` time. For an
  existing attachment a later `SET` does not apply — `DETACH` + `ATTACH`
  again (the pool templates its connections from the info captured at
  attach).
- Negative values (or values past the 32-bit DPB payload) are rejected
  with a Binder Exception naming the unit:
  `firebird_dummy_packet_interval must be >= 0 (0 = disabled), got -5 (unit: seconds)`.

Example — arm the keepalive before a long WAN extract:

```sql
SET firebird_dummy_packet_interval = 60;   -- seconds
ATTACH 'firebird://srv:/data/erp.fdb' AS erp (TYPE firebird);
COPY (SELECT * FROM erp.main.BIG_TABLE) TO 'load.parquet';

SET firebird_dummy_packet_interval = 0;    -- back to default in-session
```

When a fetch does die mid-scan despite this, the scanner raises an
`IO Error` with actionable context (table, rows already read by the
worker, this setting as the remedy, and a pointer to
`firebird_last_query()`) — see [observability](observability.md).
Honest caveat: the per-attachment DPB value depends on the server
honoring it. Firebird issue
[#8266](https://github.com/FirebirdSQL/firebird/issues/8266) reports
that on some server versions the value is parsed but not acted upon,
and the server-side `DummyPacketInterval` configuration is what
actually schedules dummy packets. Setting it here is still the correct
client-side ask and is harmless on servers that ignore it; the
error-context message (below) is the reliable half of the diagnosis.

### `SET firebird_max_inline_blob_size = BYTES`

Inline blobs (Firebird 5.0.3+). When both the loaded client library and
the server are 5.0.3 or later, Firebird sends blobs up to this size
together with the row that references them. Reading them then needs no
extra round trips, which is several times faster for tables with many
small blobs (about 12× for 200,000 small text blobs over TCP in our
measurement). The extension sends the value as
`isc_dpb_max_inline_blob_size` on every connection it opens.

- **Default `-1`**: nothing is sent and the client's own default applies,
  which is 65,535 bytes for a 5.0.3+ client. **Inlining is therefore
  already on by default** on TCP attachments whenever client and server
  are both 5.0.3+.
- `0` disables inlining; `1..65535` sets the limit. Values outside
  `-1..65535` are rejected (Firebird caps the size at 65,535).
- Read at `firebird_scan()` bind time and at `ATTACH` time, like
  `firebird_dummy_packet_interval`. For an existing attachment a later
  `SET` does not apply.
- Older clients and servers ignore the setting.
- **Check what is in effect** with `firebird_health(alias)`:
  `inline_blob_size` shows the effective limit, or `NULL` when either side
  is older than 5.0.3 or the attachment is local (XNET) or embedded: the
  server inlines blobs only over TCP (see `network_protocol`). The warning
  `inline_blobs_unavailable` flags the fixable case, a 5.0.3+ server
  reached over TCP through an older `fbclient`.

```sql
ATTACH 'firebird://srv:/data/erp.fdb' AS erp (TYPE firebird);
SELECT client_version, engine_version, inline_blob_size, warnings
FROM firebird_health('erp');

SET firebird_max_inline_blob_size = 0;   -- disable for new connections
```

## Type mapping notes

### Type handling policy

The rule this extension follows for every Firebird type it surfaces: if
DuckDB has a lossless equivalent, use it; if not, expose the value as a
lossless `VARCHAR` (never truncate it to fit) or fail explicitly (never
silently substitute a wrong value). Nothing is truncated, rounded, or
returned incorrect without either a visible error or an explicit, already
documented caveat. This is a description of the policy this codebase
already follows for its existing hard-type cases — `DECFLOAT` as `VARCHAR`
below, `CHARACTER SET NONE`'s explicit `none_encoding` modes (see the usage
guide) — not a claim of a new automated check that enforces it everywhere.

One known, tracked exception to this policy exists today:

- `TIME WITH TIME ZONE` currently discards the original zone/offset on
  read; already flagged by `firebird_type_audit`'s `time_tz` finding above.

(Issue #35 — multi-segment `BLOB` truncated by a segment-loop defect in
`ReadBlob` — is fixed; `ReadBlob` now keeps reading segments until
`isc_segstr_eof`, the only real end-of-blob signal, instead of
incorrectly stopping on the first `rc == 0`.)

### Servers with `DataTypeCompatibility`

A Firebird 4+ server can describe newer types to clients in their legacy
form: `DataTypeCompatibility = 3.0` (in `firebird.conf` or per database
in `databases.conf`) sends `INT128` as `BIGINT`, `DECFLOAT` as
`DOUBLE PRECISION` and the `WITH TIME ZONE` types without their zone;
`2.5` also sends `BOOLEAN` as `CHAR(5)`. The extension takes column types
from the declared metadata (`RDB$FIELDS`), so it asks for the native types
on every attachment (`isc_dpb_set_bind` with `... TO NATIVE` rules). This
overrides the server setting for the extension's own attachments only;
other applications keep the legacy form. Firebird 3 has no such setting
and ignores the request.

### `NUMERIC(18,s)` / `DECIMAL(18,s)` — the int64 scaled-integer domain

Firebird stores dialect-3 `NUMERIC`/`DECIMAL` with precision 10–18 as a
**64-bit scaled integer**. That physical int64 carries values up to
±(2^63−1) scaled, while DuckDB's default projection for these columns,
`DECIMAL(18,s)`, tops out at 10^18−1 scaled — a full order of magnitude
narrower. Values past the DuckDB ceiling are written unchanged into the
int64-backed `DECIMAL(18,s)` vector, so:

- the value **displays** with more precision digits than the declared type
  allows (the type's domain is silently violated);
- innocent arithmetic **fails loudly** on the extreme values — e.g. the
  classic sentinel `NUMERIC(18,6) = -9223372036854.775808` (scaled value
  `INT64_MIN`) raises `Out of Range Error: Overflow in multiplication of
  DECIMAL(18)` for `col * 2` and `Overflow in negation` for `abs(col)`;
- downstream consumers that trust the declared `DECIMAL(18,s)` precision
  (dbt models, BI tools, CTAS targets) mis-handle the out-of-domain rows.

`numeric_widen_int64=true` — a `firebird_scan` named parameter or an
ATTACH option — projects these columns as **`DECIMAL(38,s)`** instead,
which is hugeint-backed and lossless for the entire int64 scaled range:

```sql
SELECT N18_6 FROM firebird_scan('C:/data/erp.fdb', 'LEDGER',
                                numeric_widen_int64=true)
WHERE N18_6 <= -9223372036854.775808;

ATTACH 'C:/data/erp.fdb' AS erp (TYPE firebird, numeric_widen_int64 true);
```

Scope and behavior:

- Applies **only** to int64-backed scaled numerics (precision 10–18,
  scale ≠ 0). `NUMERIC(18,0)` stays `BIGINT`; narrower scaled numerics
  (precision 1–9) already fit their `DECIMAL(4,s)`/`DECIMAL(9,s)`;
  INT128-backed numerics (precision 19+) were already `DECIMAL(38,s)`.
- Both the **declared column type** and the **fetch vector** change
  together (the fetch sign-extends the scaled int64 into the hugeint
  vector), including through view type reconciliation.
- Default is `false`: without the flag the `DECIMAL(18,s)` mapping and
  fetch are byte-for-byte the historical behavior.
- `firebird_type_audit` flags every column affected by this option with
  the stable `int64_numeric_widenable` finding (see above), so you can
  size the impact across a real schema before flipping the flag.

### Charset handling of `CHARACTER SET NONE` text filters

By default, text filters that touch a `CHARACTER SET NONE` CHAR/VARCHAR (or
NONE text BLOB) column are **not** pushed to Firebird unless
`none_encoding='strict'`: DuckDB literals are UTF-8 while the server stores
and compares raw bytes, so a pushed UTF-8 literal could silently miss rows
(`'São Paulo'` is `53 E3 6F ...` on a Windows-1252 disk but
`53 C3 A3 6F ...` in UTF-8). The filtered predicate is applied by DuckDB
after transcoding, so results stay correct either way.

`none_pushdown=true` (a `firebird_scan` named parameter or an ATTACH
option) opts into pushing **`=` and `IN (constants)`** over NONE
**CHAR/VARCHAR** columns when `none_encoding` is `win1252` or
`iso8859_1`/`latin1`:

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'CLIENTES',
                   none_pushdown=true)
WHERE CITY = 'São Paulo';

ATTACH 'C:/legacy/erp.fdb' AS erp
    (TYPE firebird, none_encoding 'win1252', none_pushdown true);

SELECT COUNT(*) FROM erp.main.CLIENTES
WHERE CITY IN ('São Paulo', 'Resende');
```

How it stays lossless:

- Firebird compares `CHARACTER SET NONE` values **byte-by-byte** (binary
  semantics, no collation). Each UTF-8 literal is re-encoded to the target
  encoding's raw bytes — byte-identical to what a Windows-1252 / Latin-1
  writer stored — so a row matches exactly when its transcoded value equals
  the literal.
- The remote SQL carries those bytes behind a charset introducer
  (`"CITY" = _WIN1252 '...'` / `_ISO8859_1 '...'`). The introducer is
  required: without it, `isc_dsql_prepare` validates the statement's
  literals against the UTF-8 connection charset and fails with
  "Malformed string".
- WIN1252 encoding maps `codepoint < 0x80` and `0xA0..0xFF` straight to the
  identical byte, plus the fixed Windows-1252 table for the 27 allocated
  codepoints in `0x80–0x9F` (Euro, curly quotes, en/em dash, ...). ISO-8859-1
  maps `codepoint <= 0xFF` straight to the byte. No new dependencies.

Conservative fallback and scope:

- **Any unencodable literal** (e.g. `'€'` under `iso8859_1`, Cyrillic under
  `win1252`) leaves the whole filter in DuckDB — re-applied above the scan —
  and records the stable `NONE_CHARSET` reason, so
  `firebird_unpushed_mode='warn' | 'error'` reacts exactly as for the other
  gated cases.
- **Scope is `=` and `IN` only.** Range comparisons are never pushed this
  way (byte order in a single-byte encoding does not always agree with
  Unicode code-point order), and `NOT IN` / `LIKE` keep the existing
  NONE_CHARSET gating. NONE text **BLOB** columns are excluded too
  (Firebird cannot evaluate `=` / IN on BLOB values at all).
- With `none_encoding='strict'` or `'blob'` the flag has **no effect**:
  strict data is valid UTF-8 and already pushes; blob columns are DuckDB
  `BLOB`s. It is not an error to combine them.

Telemetry note: the pushed fragment (and the `remote_sql` shown by
`firebird_last_query()` / `firebird_query_log()` /
`firebird_explain_pushdown()`) intentionally contains the re-encoded
non-UTF-8 bytes. DuckDB validates `VARCHAR` values, so these surfaces
display each such byte escaped as `\xNN` — e.g.
`"CITY" = _WIN1252 'S\xE3o Paulo'`. The SQL actually sent to Firebird
carries the real byte (`0xE3`).

### `DECFLOAT(16)` / `DECFLOAT(34)`

Firebird 4+ `DECFLOAT(16)` and `DECFLOAT(34)` are IEEE 754 Decimal64 /
Decimal128. DuckDB has no native decimal-floating-point type, and the
legacy client path the extension uses has no decimal-float decoder.

These columns are surfaced as **VARCHAR**, produced by a server-side
`CAST(col AS VARCHAR(64))` in the generated query. Firebird stringifies the
value losslessly, so the column reads as exact text:

- ordinary decimals and exponent forms round-trip exactly
  (e.g. `123.45`, `1.234567890123456789012345678901234E+200`);
- `NaN`, `Infinity`, `-Infinity` come through as those literal strings;
- a real `NULL` stays `NULL` (the cast of NULL is NULL).

This **replaces the previous behavior**, where the column was typed
`DOUBLE` but always fetched `NULL` — a misleading schema. Lossless `VARCHAR`
is honest and queryable. There is no local Decimal64/Decimal128 decoder and
no `DOUBLE` fallback; a lossy numeric fast path would be a future opt-in.

Pushdown stays consistent with the `VARCHAR` schema: pushed filters (simple
comparisons, `IN` / `NOT IN`) compare against the same
`CAST(col AS VARCHAR(64))` expression, so text semantics match — e.g.
`WHERE D16 = '123.450'` does not match a stored `123.45` (numerically
equal, textually different).

Limitation: the DECFLOAT test fixture is dedicated
(`scripts/fixture_decfloat.sql`, referenced by `FIREBIRD_DECFLOAT_DB`) and
is **not yet in the main CI fixture** — the test skips when that variable is
unset. Promoting it into `setup_test_firebird.sh` with the coordinated
`metadata` / `dbt-sources` test updates is tracked as future work.

## Documentation premise

When public behavior changes:

- update this file
- update `docs/pt/function_manual.md`
- update usage guides when examples or workflows change
- update roadmap files when status changes

PT/EN docs must stay aligned in meaning, status, caveats, and examples.
