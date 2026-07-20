# ReconcileViewColumnTypes fallback hardening (#36) — design spec

## Problem

`ReconcileViewColumnTypes` (`src/firebird_view_analysis.cpp:188-236`) calls
`LookupObjectType` (`:31-45`) directly, with no try/catch of its own. If
`LookupObjectType`'s `conn.OpenCursor(...)`/`Fetch()` throws — a genuine
connection-level failure, not a benign "object not found" — the exception
propagates out of `ReconcileViewColumnTypes` uncaught.

Two call sites are affected differently:

- `firebird_scanner.cpp:585` (`firebird_scan(...)` bind path): the exception
  propagates out of the bind function itself — a hard query failure, not a
  silent fallback.
- `firebird_storage.cpp:391-394` (`EnsureTablesLoaded`'s batch ATTACH path,
  via `add_entry`): the exception is caught by the batch loop's own outer
  `try { ... } catch (std::exception &e) { tables_.clear(); ... }`
  (`:480-492`), which cannot distinguish "one view's type-lookup hit a
  transient connection hiccup" from "the whole batch discovery query
  failed" — both demote the *entire* ATTACH to the slower per-table
  fallback path, instead of isolating just the one problematic view (the
  fallback's own per-entry try/catch, `:540-542`, already does this
  correctly when it gets the chance).

Confirmed still reproducing on current `main` (2026-07-20, post-v1.0.0);
not a correctness bug — the fallback still builds every table/view
correctly — a robustness/performance nuance: one transient hiccup during
one view's classification can force an entire large schema onto slow
per-table discovery.

## Critical correction to the issue's own suggested fix

The issue proposes: "wrap the `LookupObjectType` call in the same
best-effort try/catch as the probe, so a connection hiccup during view
detection degrades to 'treat as not-reconciled' rather than throwing" —
i.e., on failure, return `false` (skip reconciliation), the same as the
existing "confirmed not found" path.

**This is unsafe.** `column_types`/`column_descs` — the values
`ReconcileViewColumnTypes` corrects from a live, execute-free `Prepare` —
are exactly the fields that prevent the #33 crash: a view's frozen
catalog metadata can claim a wider type (e.g. INT128-backed
`DECIMAL(38,2)`) than what the live query actually produces (e.g.
`SUM()` over `NUMERIC(10,2)` promoting to a narrower BIGINT-backed
`NUMERIC(18,2)` at runtime) — allocating the DuckDB `Vector` from the
stale wide type crashes `Vector::VerifyVectorType` once the fetch writes
the narrower real type into it (see the comment at
`firebird_scanner.cpp:570-584`). If a transient connection hiccup during
`LookupObjectType` makes `ReconcileViewColumnTypes` skip reconciliation
for an *actual* view, the #33 crash is silently reintroduced — gated
behind an even rarer trigger (a connection hiccup landing on exactly this
one query) instead of fixed.

**Corrected rule**, approved as this front's actual design:

- `LookupObjectType` confirms the object is **not** a view (query
  succeeds, `object_type != "VIEW"`) → skip reconciliation, unchanged
  from today. Safe: plain tables don't have the frozen-vs-live type drift
  views do.
- `LookupObjectType` confirms the object **is** a view → reconcile,
  unchanged from today.
- `LookupObjectType` **fails to answer** (throws) → do **not** assume
  "not a view." Proceed into the existing live-describe probe
  defensively, the same as if it had been confirmed a view. Correctness
  outweighs the small extra round-trip this costs in the rare failure
  case; the probe is a `PrepareOnlyTag` (execute-free) describe, safe and
  cheap to run against a plain table too (it just reconfirms the same
  types the catalog already had — a no-op in effect, not a correctness
  risk).

## Design

Change `ReconcileViewColumnTypes` (`firebird_view_analysis.cpp:198-205`)
from:

```cpp
std::string object_type;
if (!LookupObjectType(conn, upper, object_type)) {
    return false;
}
const bool is_view = (object_type == "VIEW");
if (!is_view) {
    return false;
}
```

to:

```cpp
std::string object_type;
bool object_type_confirmed = false;
try {
    object_type_confirmed = LookupObjectType(conn, upper, object_type);
} catch (...) {
    // Connection-level failure classifying the object -- do NOT assume
    // "not a view" and skip reconciliation. That's exactly the crash
    // ReconcileViewColumnTypes exists to prevent (see the aggregate-view
    // INT128/NUMERIC comment at this function's call site in
    // firebird_scanner.cpp). Fall through to the live-describe probe
    // below defensively instead; object_type_confirmed stays false so
    // the check below doesn't treat this as a confirmed non-view.
}

// Only skip reconciliation when the object is CONFIRMED non-view --
// never on mere uncertainty (lookup failure or "not found").
if (object_type_confirmed && object_type != "VIEW") {
    return false;
}
```

The existing probe block (`:207-233`) is unchanged. The function's final
`return true;` (`:235`) is also unchanged — it already fires whenever
this point is reached, which now correctly includes both "confirmed
view" and "classification failed, reconciled defensively" cases.

No signature change to `LookupObjectType` itself — the fix is scoped
entirely to its call site inside `ReconcileViewColumnTypes`, the least
invasive change that closes the gap.

## Validation

No C++ unit-test framework exists in this repo (`test/` is
sqllogictest-only, against a real Firebird server) and no test-only fault
injection hook will be added to the production `LookupObjectType`/
`ReconcileViewColumnTypes` call path (rejected as unnecessary invasiveness
for this narrow case). Validation instead:

- Full existing local suite green, unchanged assertion counts — proves no
  regression to the two already-tested paths: confirmed-view reconciliation
  (the #33 regression fixture, `V_DEPT_HEADCOUNT`) and confirmed-table
  skip.
- Cross-version DuckDB matrix (v1.5.2/v1.5.3/v1.5.4) green.
- Linux FB3/4/5 CI matrix green, Windows CI green.
- Strong adversarial code review specifically on: the try/catch scope
  (confirm it wraps only the `LookupObjectType` call, not the whole
  function), the `object_type_confirmed` boolean's three-way logic
  (confirmed-view / confirmed-non-view / unknown), and that the unknown
  case correctly falls through to the probe rather than an early return.
- A short code comment stating the rule inline ("only skip reconciliation
  when the object is confirmed non-view — never on mere uncertainty") so
  a future reader can't reintroduce the issue's original (unsafe)
  suggestion without seeing why it was rejected.

## Out of scope

- No change to `LookupObjectType`'s own signature or the probe's existing
  `catch (...)` block.
- No test-only fault-injection hook.
- No changes to `duckdb/community-extensions` or any upstream repo.
- Not a v1.0.0 blocker in itself (already shipped) — this closes the item
  as a prerequisite Fernando set before considering a Community Update,
  and is a candidate for a v1.0.1 patch release.
