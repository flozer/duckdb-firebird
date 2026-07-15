# BLOB Lossless Fix (#35) — design spec

## Problem

`FirebirdStatement::ReadBlob` (`src/firebird_client.cpp`) silently drops
segments after the first one when reading a multi-segment BLOB. Any BLOB
whose content needs more than one Firebird BLOB segment to store — text,
binary, or the engine's own `RDB$DESCRIPTION` comment metadata — comes
back truncated, with no error, no warning, and no visible signal that
anything went wrong. This is exactly the class of bug the v1.0 Type
Lossless Hardening initiative exists to eliminate: a silently wrong
result instead of a lossless value or an explicit failure.

The bug has been present, untriggered by any existing test, for the
entire lifetime of `ReadBlob`. A fixture already exists that was
specifically built to catch it (`scripts/setup_test_firebird.sh`'s
`COMMENT ON TABLE DEPT`, ~4KB, with a code comment saying "to prove
ReadBlob does not truncate") — but no test ever asserted against it, so
the bug shipped and stayed invisible.

## Root cause

```cpp
std::string out;
char segment[8192];
unsigned short seg_len = 0;
while (true) {
    ISC_STATUS rc = isc_get_segment(status, &blob, &seg_len, sizeof(segment), segment);
    if (rc == isc_segstr_eof) break;
    if (rc != 0 && rc != isc_segment) {
        isc_close_blob(status, &blob);
        FirebirdConnection::Check(status, "isc_get_segment");
    }
    out.append(segment, seg_len);
    if (rc == 0) break;  // BUG
}
```

`isc_get_segment`'s three relevant outcomes:

- `isc_segstr_eof` — there is no more BLOB data. This is the **only**
  correct loop-termination condition.
- `isc_segment` — the current logical segment was larger than the
  client's read buffer and got truncated; the remaining bytes of that
  *same* segment follow on the next call. Already handled correctly
  (falls through to `out.append` and loops again).
- `0` — the call completed with no error: the segment fit entirely in
  the buffer. This says nothing about whether more segments remain — a
  BLOB with 50 segments returns `0` fifty times in a row, then
  `isc_segstr_eof` on call fifty-one. The existing `if (rc == 0) break;`
  treats the first successful segment read as if it were the last,
  discarding every segment after it.

**Documented rule for the fix**: `isc_segstr_eof` is the only condition
that ends the loop. `rc == 0` means "this call's segment is complete,"
never "the BLOB is complete." The fix is the removal of the `if (rc ==
0) break;` line — nothing else in the loop changes.

## Scope

`ReadBlob` is a single, subtype-agnostic helper. Confirmed call sites
(all three read the *same* function, so one fix covers all of them):

- `src/firebird_types.cpp:356` — the normal `SQL_BLOB` fetch path, used
  for both text (`sqlsubtype == 1`) and binary (`sqlsubtype == 0`) BLOB
  columns.
- `src/firebird_metadata_functions.cpp` — `RDB$DESCRIPTION` comment
  reads (table/view/column comments).
- `src/firebird_view_analysis.cpp:58` — view-source text reads.

No other function needs to change. This is not a refactor — one
function, one incorrect early-exit condition removed.

## Fixture

A new table, `TBLOB_MULTISEG`, added to `scripts/setup_test_firebird.sh`:

```sql
CREATE TABLE TBLOB_MULTISEG (
    ID    INTEGER NOT NULL PRIMARY KEY,
    NOTE  BLOB SUB_TYPE 1 SEGMENT SIZE 80,
    DATA  BLOB SUB_TYPE 0 SEGMENT SIZE 80
);
```

`SEGMENT SIZE 80` is set explicitly rather than relying on Firebird's
unspecified default — the default has actually varied across engine
versions/builds, and this bug must reproduce deterministically on every
supported Firebird version, not "whenever the default happens to be
small." At 80 bytes/segment, a content string of a few KB forces dozens
of segments through the exact `isc_get_segment` loop under test.

Content, for both the text and binary column, is built from three
distinguishable markers so a test can prove the *whole* BLOB survived,
not just its front:

- A recognizable **start** marker (e.g. `START-` + a short tag).
- A recognizable, long **middle** run whose exact byte length is known
  and checkable (e.g. a repeating pattern, so a checksum over just the
  middle run is meaningful — not merely "some bytes are here").
- A recognizable **end** marker (e.g. `-END` + a short tag), positioned
  so it falls in one of the *later* segments (i.e., the bug — losing
  everything after segment 1 — would make the end marker vanish and
  the length come up short, while a length-only check on a
  coincidentally-short truncation could theoretically still pass).

The exact total length and an `md5()` checksum (DuckDB's built-in
`md5(value)` scalar function, applied directly to the fetched
VARCHAR/BLOB value) are computed once when the fixture is authored and
hardcoded into the test as the expected values — so the test asserts
byte-for-byte correctness, not merely "didn't crash" or "length looks
plausible."

Concretely, both columns are populated with a literal string built the
same way the existing DEPT comment fixture already does it — a fixed
marker, followed by a many-times-repeated literal chunk written directly
in the SQL text (no server-side string-repeat function, for portability
across FB3/4/5), followed by a distinct closing marker:

```
NOTE: 'START-NOTE-' || 'MID' repeated ~1300x (>4000 chars total) || '-END-NOTE'
DATA: 'START-DATA-' || 'MID' repeated ~1300x (>4000 chars total) || '-END-DATA'
```

(`DATA` is still a `BLOB SUB_TYPE 0` column — using a printable literal
for its content is only about what's easy to author and diff in the SQL
fixture file; the TEST validates it as opaque bytes via `md5()`/length,
never by string-comparing it as text, which is the actual point of
having a separate binary case.) The exact literal, its length, and its
`md5()` value are fixed at fixture-authoring time and hardcoded into the
test file alongside it.

## Tests

1. **`TBLOB_MULTISEG.NOTE` (text, multi-segment)** — assert exact
   length, exact checksum over the full value, and presence of both the
   start and end markers (end marker is the one that actually
   distinguishes "fixed" from "silently truncated after segment 1").
2. **`TBLOB_MULTISEG.DATA` (binary, multi-segment)** — same shape,
   checksum/length only; binary content is validated by checksum and a
   partial hex/byte comparison (e.g. first N and last N bytes), never by
   treating it as text.
3. **`DEPT`'s existing long comment** (already-present fixture,
   `scripts/setup_test_firebird.sh`, 178 × `DEPT_LONG_COMMENT_TEST_` =
   4094 chars) — a regression test asserting `octet_length(comment) =
   4094` **and** that the comment ends with the exact trailing
   substring it's supposed to end with (proving the tail — the part
   that would be lost by the bug — survived, not just that *some*
   4094-length value came back). This closes the exact gap the
   fixture's own comment says it was meant to close, and needed zero new
   fixture work.
4. **Regression guard for the single-segment case**: an existing/short
   BLOB fixture (already covered by `FILE_STORAGE.PAYLOAD` et al.) stays
   asserted as-is — the fix must not change behavior for BLOBs that fit
   in one segment.
5. **`firebird_type_audit`**: existing `blob_text` finding must still
   fire where it already does — the fix does not touch type-mapping
   policy, only the byte-accumulation loop, so this is a straight
   no-regression check.

The multi-segment tests are written to the CORRECT (fixed) expectation.
Proving they actually catch the bug is a one-time verification step
during implementation: run the new tests against the pre-fix code and
confirm they fail (wrong length/checksum/missing end marker), then apply
the fix and confirm they pass. The committed test file only ever encodes
the fixed, correct behavior — there is no permanent xfail/skip.

## Validation

- Cross-version matrix: DuckDB v1.5.2 / v1.5.3 / v1.5.4, same pattern as
  prior v1.0 branches.
- Read-only maturity battery against the real production DB
  (`$env:FIREBIRD_MATURITY_DB`). BLOB content itself is never logged or
  compared to a hardcoded value from this environment (real column
  contents from someone's actual database are not test fixtures) — the
  battery emits only lengths, checksums, and counts (e.g. "N BLOB
  columns found, M of them have length > single-segment-threshold,
  checksum of fetched value equals checksum of the same value fetched a
  second time" — a self-consistency check, not a content assertion).

## Out of scope

- `TIME_TZ` offset handling and any other item already tracked
  separately — untouched.
- No changes to `duckdb/community-extensions` or any upstream repo.
- No refactor of `FirebirdStatement`, `AllocateBuffers`, or any other
  fetch path beyond the one loop in `ReadBlob`.
