# Spike — planner plan-extraction feasibility (`firebird_explain_pushdown`)

- Date: 2026-06-22
- Branch: `feat/v0.7-explain-pushdown`
- DuckDB submodule: **v1.5.3** (`git submodule status duckdb` → `14eca11bd9d4a0de2ea0f078be588a9c1c5b279c (v1.5.3)`)
- Investigation only. No prototype code committed; working tree functional source unchanged.

---

## VERDICT: **GO — with a mandatory implementation constraint**

The optimized logical plan of an inner `SELECT` against an ATTACHed Firebird
catalog **can** be extracted, and every datum the design needs (`FirebirdBindData`,
`column_ids`, `TableFilterSet`, `extra_predicates`, `gated_complex_reasons`) is
reachable from the bound `LogicalGet` in that plan, **without opening a data
cursor**.

**BUT** the literal phrasing of the gate question — "from within its own
bind/execute *reentrantly*, on the outer query's `ClientContext`" — is a
**NO-GO** for the `bind` phase and for reusing the *outer* `ClientContext`.
`ClientContext::ExtractPlan` takes the context's non-recursive `std::mutex`,
which the outer query already holds during binding → guaranteed self-deadlock.

The feature is feasible **only** by this pattern, which the design already
half-anticipates (step 4: "fora de qualquer callback de scan"):

> In the table function's **execute** phase (NOT bind, NOT any scan callback),
> open a **fresh `duckdb::Connection(DatabaseInstance&)`** and call
> `ExtractPlan` on **that** connection's own `ClientContext`. The new context
> has its own `context_lock` and shares the same `DatabaseInstance`, so it sees
> the ATTACHed Firebird catalogs. The outer context is never re-entered.

If the implementer instead tries to call `ExtractPlan` (or `Optimizer`/`Build`)
on the running context from within bind/init, it will hang/deadlock. That path
is forbidden, not "discouraged".

---

## 1. Extraction API (exact)

| | |
|---|---|
| Method | `ClientContext::ExtractPlan(const string &query)` |
| Header | `duckdb/main/client_context.hpp:188` |
| Signature | `DUCKDB_API unique_ptr<LogicalOperator> ExtractPlan(const string &query);` |
| Public wrapper | `Connection::ExtractPlan(const string &query)` — `duckdb/main/connection.hpp:140`, impl `connection.cpp:203-205` (delegates to `context->ExtractPlan`) |
| Impl | `duckdb/src/main/client_context.cpp:728-756` |

**Returns the OPTIMIZED plan — confirmed by the implementation:**

```cpp
unique_ptr<LogicalOperator> ClientContext::ExtractPlan(const string &query) {
    auto lock = LockContext();                                  // <-- takes context_lock
    auto statements = ParseStatementsInternal(*lock, query);
    if (statements.size() != 1)
        throw InvalidInputException("ExtractPlan can only prepare a single statement");
    unique_ptr<LogicalOperator> plan;
    RunFunctionInTransactionInternal(*lock, [&]() {
        Planner planner(*this);
        planner.CreatePlan(std::move(statements[0]));
        plan = std::move(planner.plan);
        if (config.enable_optimizer) {                          // <-- optimizer runs
            Optimizer optimizer(*planner.binder, *this);
            plan = optimizer.Optimize(std::move(plan));         // <-- filter/projection pushdown applied here
        }
        ColumnBindingResolver resolver;
        resolver.Verify(*plan);
        resolver.VisitOperator(*plan);
        plan->ResolveOperatorTypes();
    });
    return plan;
}
```

So filter pushdown, projection pushdown, and the table function's
`pushdown_complex_filter` callback all run before the plan is returned — exactly
what the design needs. (Note: `enable_optimizer` is default-on; the explain
function must not run under a session that disabled it, or the result would be
the un-optimized plan. Worth a defensive check.)

It also raises `InvalidInputException` for multi-statement input — useful, but
the design's allow-list (single SELECT/CTE, reject DML/DDL/COPY/EXPLAIN) must
still be enforced *before* this call, since `ExtractPlan` itself would happily
plan a `DELETE`.

---

## 2. Reentrancy — the decisive finding

- `LockContext()` (`client_context.cpp:194-195`) returns
  `make_uniq<ClientContextLock>(context_lock)`.
- `context_lock` is a **plain `mutex`** — `client_context.hpp:318`:
  `mutex context_lock;` (DuckDB's `mutex` = `std::mutex`, **non-recursive**).
- `ClientContextLock` holds `lock_guard<mutex> client_guard;`
  (`client_context.hpp:329, 336`).
- During the outer query, binding happens inside
  `CreatePreparedStatement(ClientContextLock &lock, ...)`
  (`client_context.cpp:454`), i.e. the table function's **bind callback runs on
  the thread that already holds `context_lock`**.

Therefore: calling `ExtractPlan` (which calls `LockContext()` again) from inside
our bind/init **re-locks a non-recursive mutex on the same thread = undefined
behaviour / deadlock.** There is no try-lock, no recursive variant, no public
"already-locked" overload of `ExtractPlan`. This is stable, intentional DuckDB
design, not a version quirk.

**Workaround that is sound (and required):**
- A `duckdb::Connection(DatabaseInstance &)` (`connection.hpp:43`) owns a
  **separate `ClientContext`** with its **own** `context_lock`, while sharing
  the same `DatabaseInstance` → same catalog set, including
  `ATTACH … (TYPE firebird)` aliases.
- The table function **execute** (and global-init) phase does **not** hold the
  outer `context_lock` (binding is done; execution drives operators, it is not
  inside `CreatePreparedStatement`). From there, constructing a fresh
  `Connection` and calling `ExtractPlan` on it is safe.
- `DatabaseInstance` is already reachable in this codebase
  (`firebird_extension.cpp:43` uses `loader.GetDatabaseInstance()`; at runtime
  `DatabaseInstance::GetDatabase(context)`).
- `RunFunctionInTransactionInternal` (`client_context.cpp` ~737) starts its own
  transaction only when the (fresh) context is in autocommit with no active
  txn — fine for a throwaway explain connection.

Net: the design's "bind + optimize the inner SQL" must be done from the
**execute** path on a **fresh connection**, not reentrantly on the outer
context from bind. With that constraint the feature is feasible.

---

## 3. Identifying the Firebird `LogicalGet` and reading its data

`LogicalGet` (`duckdb/planner/operator/logical_get.hpp`) exposes everything as
public members / accessors:

| Need | Access | Source |
|---|---|---|
| node type | `op.type == LogicalOperatorType::LOGICAL_GET` | `logical_operator.hpp:35` |
| children (traversal) | `op.children` = `vector<unique_ptr<LogicalOperator>>` | `logical_operator.hpp:37` |
| bound table function | `get.function` (`TableFunction`) | `logical_get.hpp:32` |
| identify as firebird | `get.function.name == "firebird_scan"` (`Function::name`, `function.hpp:105`) | the ATTACH path reuses the `firebird_scan` table function (`firebird_storage.cpp:20-24`) |
| bind data | `get.bind_data` (`unique_ptr<FunctionData>`) → `get.bind_data->Cast<FirebirdBindData>()` | `logical_get.hpp:34`; cast pattern already used at `firebird_scanner.cpp:596,644,675,838,1342,1358` |
| projected column ids | `get.GetColumnIds()` → `const vector<ColumnIndex>&` | `logical_get.hpp:81` |
| pushed filters | `get.table_filters` (`TableFilterSet`) | `logical_get.hpp:44` |
| catalog entry (PK descriptor) | `get.GetTable()` → `optional_ptr<TableCatalogEntry>` | `logical_get.hpp:69` |
| limit/offset hints | `get.extra_info` (`ExtraOperatorInfo`) | `logical_get.hpp:58` |

Identification is doubly robust: match `function.name == "firebird_scan"` AND
confirm the cast (`bind_data->Cast<FirebirdBindData>()` — DuckDB's `Cast<>`
throws `InternalException` on type mismatch, `function.hpp:67-79`). In practice,
gate on `function.name` first to avoid the throwing cast on non-firebird gets;
the `FirebirdBindData` is the only `FunctionData` subtype the `firebird_scan`
function ever attaches.

---

## 4. `extra_predicates` / `gated_complex_reasons` present post-optimization — YES

The scanner registers, at registration time (`firebird_scanner.cpp:1510-1521`):

```cpp
fn.projection_pushdown     = true;
fn.filter_pushdown         = true;
fn.pushdown_complex_filter = FirebirdScanPushdownComplexFilter;
```

`FirebirdScanPushdownComplexFilter` (`firebird_scanner.cpp:1353`) **mutates the
bind_data during optimization**: it pushes `bind.extra_predicates` (LIKE-prefix,
NOT IN, …) and `bind.gated_complex_reasons` (`"NONE_CHARSET"`) — see lines
1425, 1433, 1447, 1460, 1478. Because `ExtractPlan` runs the optimizer (§1), this
callback has already fired by the time the plan is returned, so both vectors are
populated on the `FirebirdBindData` inside the extracted `LogicalGet`. The
explain function reads them as telemetry exactly as `firebird_last_query()` does
(scanner reads them at lines 690, 747, 763) — no manual re-invocation of hooks
and no reentrant `Build()`.

---

## 5. No data cursor for the ATTACHed-table case — verified by code

- The **only** data cursor entry point is `FirebirdConnection::OpenCursor`
  (`firebird_client.cpp:379,384`).
- ATTACH-path scan binding goes through
  `FirebirdStorage…::GetScanFunction` (`firebird_storage.cpp:109-139`), which
  builds a fully-populated `FirebirdBindData` from schema already in the catalog
  entry — header comment lines 20-24: "we build a fully-populated FirebirdBindData
  up front … The planner uses our pre-built bind data verbatim and **never
  re-invokes the bind callback**." No `OpenCursor` in that path.
- `OpenCursor` call sites are either the **scan execute** path
  (`firebird_scanner.cpp:805-806`, inside the per-chunk function) or catalog
  metadata loads. Plan extraction stops at the optimized `LogicalGet`, before
  any operator executes, so no data cursor is opened on the user's query.
- Optimization (filter/projection pushdown, the complex-filter callback) is
  pure in-memory work on `bind_data` — no I/O to Firebird.

**Important nuance — lazy catalog discovery on a cold catalog:** DuckDB catalog
discovery is lazy: `EnsureTablesLoaded` fires on the first `LookupEntry` for a
table not yet in the in-memory catalog, and `GetScanFunction` (called by the
binder) triggers `ProbePrimaryKey` (including a MIN/MAX probe round-trip) the
first time a given table is resolved. These are ATTACH-path binder behaviors, not
explain-specific: any query that binds `fb.main.EMPLOYEE` for the first time
causes the same catalog loads. `ExtractPlan` on a cold catalog will therefore run
these metadata loads + the PK probe on first reference — memoised immediately
after, so subsequent explain calls or real queries against the same table see no
new round-trips.

**Scoped guarantee:** `firebird_explain_pushdown` never opens a **data cursor**
on the user's query and never sends the user's query SQL to Firebird. Catalog
metadata + the PK probe may lazily load on first table reference exactly as any
ATTACH-path query's bind does, and are memoised. (Recommended test
instrumentation: an atomic `OpenCursor` counter distinguishing metadata vs. data
cursors, or an after-ATTACH invalid-credential fixture — the explain must
succeed after ATTACH has already loaded metadata, as noted in the design's test
section.)

### Direct `firebird_scan(...)` WOULD connect at bind — confirmed

`FirebirdScanBind` (`firebird_scanner.cpp:411`) constructs
`FirebirdConnection conn(bind->conn_info)` and calls `LoadTableSchema(conn, …)`
(lines 534-536), which runs `OpenCursor` (line 247) to read
`RDB$RELATION_FIELDS`. So binding a direct `firebird_scan('<path>','EMPLOYEE')`
opens a Firebird connection at bind time. This justifies the design's pre-bind
rejection: the parsed tree must be scanned for a table function named
`firebird_scan` and rejected with the actionable `BinderException`
**before** the inner SQL is bound/optimized — otherwise the very act of
extracting the plan would connect.

---

## 6. Traversal / `scan_ordinal`

Walk the returned `LogicalOperator` tree by recursing `op.children` in order,
visiting each node **before** its children (preorder/DFS). Emit one row per node
where `op.type == LOGICAL_GET` and `function.name == "firebird_scan"`, assigning
`scan_ordinal` 1-based in visit order. For a self-join the two firebird gets get
ordinals 1 and 2 with the same `table_name` — matching the design's stable-order
contract. `children` is an ordered `vector`, so the order is deterministic for a
given optimized plan.

---

## Residual risks / notes for the implementer

1. **Execute-phase only.** Do the `ExtractPlan` on a fresh `Connection` from the
   table function's execute (or global init) — never from bind/init of
   `firebird_explain_pushdown` itself, never from inside any scan callback.
2. **Optimizer must be enabled.** Guard against a session with
   `enable_optimizer=false` (would return an un-optimized plan, breaking the
   pushdown report). Either assert or document.
3. **Pre-bind `firebird_scan` rejection** must happen on the parsed tree, before
   the inner SQL reaches `ExtractPlan`, to honor the no-remote-connection
   guarantee.
4. **Allow-list** (single SELECT/CTE) enforced before `ExtractPlan`;
   `ExtractPlan` only rejects multi-statement, not DML/DDL.
5. **Cross-version**: API is identical surface in v1.5.2/1.5.3/1.5.4 family
   (stable `DUCKDB_API`); still run `scripts/build_matrix.ps1` with the new test
   as the design specifies.
