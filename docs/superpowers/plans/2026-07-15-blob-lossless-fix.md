# BLOB Lossless Fix (#35) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix `FirebirdStatement::ReadBlob`'s segment-read loop, which silently drops every BLOB segment after the first, and add a regression test that actually exercises a genuinely multi-segment BLOB (text and binary) plus the pre-existing DEPT long-comment fixture.

**Architecture:** One-line-removal fix in `src/firebird_client.cpp`. A new fixture table (`TBLOB_MULTISEG`) is populated via a small standalone C++ helper (`scripts/mkblob_fixture.cpp`) that writes real, separate `isc_put_segment` calls — a plain SQL `INSERT` literal does **not** create multiple physical segments regardless of the column's declared `SEGMENT SIZE` (confirmed empirically before writing this plan), so a helper using the low-level blob-write API is the only way to reproduce the bug at all.

**Tech Stack:** C++17, Firebird `ibase.h`/`fbclient` API (already a build dependency for headers), DuckDB SQL test format (`test/sql/*.test`), bash (`scripts/setup_test_firebird.sh`), PowerShell (`scripts/build_matrix.ps1`).

## Global Constraints

- Root cause: `isc_get_segment` returning `rc == 0` means "this call's segment completed without truncation," not "no more segments." Only `isc_segstr_eof` signals true end-of-blob. (Spec: `docs/superpowers/specs/2026-07-15-blob-lossless-fix-design.md`.)
- Fix is limited to the one loop in `ReadBlob` (`src/firebird_client.cpp`) — no other function changes. It is shared, unmodified, by text BLOB, binary BLOB, and `RDB$DESCRIPTION` comment reads.
- A plain SQL literal INSERT does not create multiple physical Firebird segments regardless of declared `SEGMENT SIZE` — confirmed empirically. Only `isc_put_segment`, called directly and repeatedly, does. This is why `scripts/mkblob_fixture.cpp` exists.
- Tests assert exact length + `md5()` checksum + start/end marker presence — never "didn't crash" alone.
- Maturity validation against the real production DB must emit only lengths/checksums/counts — never BLOB content.
- Cross-version matrix: DuckDB v1.5.2 / v1.5.3 / v1.5.4 (`scripts/build_matrix.ps1`) is mandatory before PR — and its hardcoded `$testFixtureVar` map must include the new test file, or the matrix silently skips it.
- No changes to `duckdb/community-extensions` or any upstream repo.

---

### Task 1: Add the multi-segment BLOB fixture (helper + table) and confirm it reproduces the bug

**Files:**
- Create: `scripts/mkblob_fixture.cpp`
- Modify: `scripts/setup_test_firebird.sh`

**Interfaces:**
- Produces: a `TBLOB_MULTISEG` table (`ID INTEGER PK`, `NOTE BLOB SUB_TYPE 1 SEGMENT SIZE 80`, `DATA BLOB SUB_TYPE 0 SEGMENT SIZE 80`) with one row (`ID = 1`) whose `NOTE`/`DATA` values are written as 51 real 80-byte segments each via `isc_put_segment`. Later tasks (2, 3) read this row.

- [ ] **Step 1: Write `scripts/mkblob_fixture.cpp`**

```cpp
// Standalone helper (not part of the shipped extension): writes
// TBLOB_MULTISEG's NOTE/DATA columns as genuinely separate 80-byte
// isc_put_segment calls. A plain SQL literal INSERT does NOT create
// multiple physical Firebird segments regardless of the column's
// declared SEGMENT SIZE -- confirmed empirically (a 20,020-byte literal
// round-tripped correctly through the unfixed ReadBlob, because the
// engine stored it as one segment). SEGMENT SIZE only advises a
// low-level writer calling isc_put_segment directly; it does not
// retroactively chunk a value the engine received as a single string.
// This is the only way to reproduce -- or verify the fix for -- the
// ReadBlob segment-loop bug (issue #35).
#include <ibase.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static void check(ISC_STATUS *status, const char *what) {
    if (status[0] == 1 && status[1]) {
        ISC_LONG sqlcode = isc_sqlcode(status);
        char buf[512];
        isc_sql_interprete(sqlcode, buf, sizeof(buf));
        fprintf(stderr, "%s failed: %s (sqlcode=%ld)\n", what, buf, (long)sqlcode);
        exit(1);
    }
}

static int write_column(isc_db_handle &db_handle, long id, const char *column,
                         const std::string &content) {
    ISC_STATUS status[20] = {0};
    isc_tr_handle tr_handle = 0;
    isc_start_transaction(status, &tr_handle, 1, &db_handle, 0, nullptr);
    check(status, "isc_start_transaction");

    isc_blob_handle blob_handle = 0;
    ISC_QUAD blob_id = {0, 0};
    isc_create_blob2(status, &db_handle, &tr_handle, &blob_handle, &blob_id, 0, nullptr);
    check(status, "isc_create_blob2");

    size_t off = 0;
    int segments = 0;
    while (off < content.size()) {
        size_t chunk = content.size() - off;
        if (chunk > 80) chunk = 80;
        isc_put_segment(status, &blob_handle, (unsigned short)chunk,
                         const_cast<char *>(content.data() + off));
        check(status, "isc_put_segment");
        off += chunk;
        segments++;
    }
    isc_close_blob(status, &blob_handle);
    check(status, "isc_close_blob");

    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE TBLOB_MULTISEG SET %s = ? WHERE ID = %ld", column, id);

    isc_stmt_handle stmt = 0;
    isc_dsql_allocate_statement(status, &db_handle, &stmt);
    check(status, "isc_dsql_allocate_statement");

    char sqlda_buf[sizeof(XSQLDA) + sizeof(XSQLVAR)];
    XSQLDA *in_sqlda = reinterpret_cast<XSQLDA *>(sqlda_buf);
    memset(in_sqlda, 0, sizeof(sqlda_buf));
    in_sqlda->version = SQLDA_VERSION1;
    in_sqlda->sqln = 1;

    isc_dsql_prepare(status, &tr_handle, &stmt, 0, sql, 3, in_sqlda);
    check(status, "isc_dsql_prepare");

    in_sqlda->sqld = 1;
    in_sqlda->sqlvar[0].sqltype = SQL_BLOB;
    in_sqlda->sqlvar[0].sqllen = sizeof(ISC_QUAD);
    in_sqlda->sqlvar[0].sqldata = reinterpret_cast<char *>(&blob_id);
    in_sqlda->sqlvar[0].sqlind = nullptr;

    isc_dsql_execute(status, &tr_handle, &stmt, 1, in_sqlda);
    check(status, "isc_dsql_execute (update)");

    isc_dsql_free_statement(status, &stmt, DSQL_drop);
    isc_commit_transaction(status, &tr_handle);
    check(status, "isc_commit_transaction");

    fprintf(stderr, "%s: wrote %d segments, %zu bytes\n", column, segments, content.size());
    return segments;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: mkblob_fixture <db> <user> <password> <table_id>\n");
        return 2;
    }
    const char *db = argv[1];
    const char *user = argv[2];
    const char *pass = argv[3];
    long id = atol(argv[4]);

    char dpb[256];
    char *p = dpb;
    *p++ = isc_dpb_version1;
    *p++ = isc_dpb_user_name; *p++ = (char)strlen(user); memcpy(p, user, strlen(user)); p += strlen(user);
    *p++ = isc_dpb_password;  *p++ = (char)strlen(pass); memcpy(p, pass, strlen(pass)); p += strlen(pass);
    *p++ = isc_dpb_sql_dialect; *p++ = 1; *p++ = 3;

    isc_db_handle db_handle = 0;
    ISC_STATUS status[20] = {0};
    isc_attach_database(status, (short)strlen(db), (char *)db, &db_handle, (short)(p - dpb), dpb);
    check(status, "isc_attach_database");

    std::string note = "START-NOTE-"; note.append(4000, 'N'); note.append("-END-NOTE");
    std::string data = "START-DATA-"; data.append(4000, 'D'); data.append("-END-DATA");

    write_column(db_handle, id, "NOTE", note);
    write_column(db_handle, id, "DATA", data);

    isc_detach_database(status, &db_handle);
    printf("OK\n");
    return 0;
}
```

- [ ] **Step 2: Add the `TBLOB_MULTISEG` table and helper invocation to `scripts/setup_test_firebird.sh`**

Add this block right after the existing `V_LITERAL_NOISE`/`V_MULTILINE_KW` view fixtures (before the `TQUOTES` section), inside the same heredoc that provisions `EMPLOYEE`/`FILE_STORAGE`/etc.:

```sql
-- Multi-segment BLOB fixture (issue #35) -- NOTE/DATA start as short
-- placeholders here; scripts/mkblob_fixture.cpp overwrites them with
-- genuinely multi-segment content right after this heredoc runs (a
-- plain SQL literal, even with SEGMENT SIZE declared, is stored as one
-- segment -- see that file's header comment).
CREATE TABLE TBLOB_MULTISEG (
    ID    INTEGER NOT NULL PRIMARY KEY,
    NOTE  BLOB SUB_TYPE 1 SEGMENT SIZE 80,
    DATA  BLOB SUB_TYPE 0 SEGMENT SIZE 80
);
INSERT INTO TBLOB_MULTISEG VALUES (1, 'placeholder', CAST('placeholder' AS BLOB SUB_TYPE 0));
COMMIT;
```

This heredoc runs from `"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" <<'EOF'` (currently line 52) through the `EOF` that currently sits right after the `TIDX_INACTIVE`/`IDX_TIDX_INACTIVE_QTY` block (currently line 244) — insert the block above anywhere inside it (e.g. right after `V_MULTILINE_KW`'s `COMMIT;`, before the `TQUOTES` comment). Then, immediately after that same closing `EOF` (line 244) and *before* the `chmod 0666 "$FIREBIRD_TEST_DB"` line that currently follows it, add:

```bash
# Overwrite TBLOB_MULTISEG's placeholder BLOBs with genuinely
# multi-segment content (issue #35) -- see scripts/mkblob_fixture.cpp's
# header comment for why a SQL literal can't do this.
MKBLOB_SRC="$(dirname "$0")/mkblob_fixture.cpp"
MKBLOB_BIN="$(dirname "$FIREBIRD_TEST_DB")/mkblob_fixture"
if command -v g++ >/dev/null 2>&1; then
    FB_INC="${FB_SDK_ROOT:-/usr}/include"
    [ -d "/usr/include/firebird" ] && FB_INC="/usr/include/firebird"
    g++ -std=c++17 -I"$FB_INC" -o "$MKBLOB_BIN" "$MKBLOB_SRC" -lfbclient
elif command -v cl.exe >/dev/null 2>&1 && [ -n "${FB_SDK_ROOT:-}" ]; then
    cl.exe /nologo /EHsc /I"${FB_SDK_ROOT}/include" "$MKBLOB_SRC" \
        /link "/LIBPATH:${FB_SDK_ROOT}/lib" fbclient_ms.lib "/OUT:${MKBLOB_BIN}.exe"
    MKBLOB_BIN="${MKBLOB_BIN}.exe"
else
    echo "mkblob_fixture: no C++ compiler found (need g++ or cl.exe+FB_SDK_ROOT)" >&2
    exit 1
fi
"$MKBLOB_BIN" "$FIREBIRD_TEST_DB" "$ISC_USER" "$ISC_PASSWORD" 1
```

- [ ] **Step 3: Run the updated setup against the real local test DB and verify the placeholder-then-overwrite sequence works**

Run (adjust paths/creds to match your local Firebird install, matching every other task in this branch):
```
export ISC_USER=SYSDBA ISC_PASSWORD=masterkey FIREBIRD_TEST_DB="C:\fbtest\test.fdb" FB_SDK_ROOT="C:/Program Files/Firebird/Firebird_5_0"
bash scripts/setup_test_firebird.sh
```
Expected: script completes with no error, ending in something like:
```
NOTE: wrote 51 segments, 4020 bytes
DATA: wrote 51 segments, 4020 bytes
OK
```

- [ ] **Step 4: Confirm this genuinely reproduces the bug against the CURRENT (unfixed) code**

Build the extension (already-established local build command for this worktree — `scripts/build_windows_local.bat` on Windows, or the platform's equivalent), then run:
```
echo "ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird); SELECT length(NOTE), md5(NOTE) FROM fb.main.TBLOB_MULTISEG WHERE ID=1;" | build/release/duckdb.exe -unsigned -csv
```
Expected (bug still present — this is the FAILING baseline, confirming the fixture actually exercises the bug):
```
length(NOTE),md5(NOTE)
80,92bfe48b3f323b09880b1b5225378ebb
```
(80 = first segment only; the correct value is 4020 bytes, confirmed in Task 3.)

- [ ] **Step 5: Commit**

```bash
git add scripts/mkblob_fixture.cpp scripts/setup_test_firebird.sh
git commit -m "test(blob-lossless): add multi-segment BLOB fixture (#35)

A plain SQL literal INSERT does not create multiple physical Firebird
segments regardless of declared SEGMENT SIZE (confirmed empirically).
mkblob_fixture.cpp writes TBLOB_MULTISEG's NOTE/DATA via genuinely
separate isc_put_segment calls (51 x 80 bytes each) so the fixture
actually exercises ReadBlob's segment-read loop. Confirmed this
reproduces the bug: current code reads back only the first 80-byte
segment (length=80) instead of the full 4020-byte value."
```

---

### Task 2: Write the failing tests (prove the bug, before touching the fix)

**Files:**
- Create: `test/sql/firebird_blob_lossless.test`
- Modify: `test/sql/firebird_metadata_bridge.test`

**Interfaces:**
- Consumes: `TBLOB_MULTISEG` (Task 1). `firebird_comments('fb')`'s existing `DEPT` row (pre-existing fixture, `scripts/setup_test_firebird.sh`'s `COMMENT ON TABLE DEPT`).
- Produces: nothing consumed by later tasks — this is the regression suite the fix (Task 3) must satisfy.

- [ ] **Step 1: Create `test/sql/firebird_blob_lossless.test`**

```
# name: test/sql/firebird_blob_lossless.test
# description: ReadBlob must not drop segments after the first (issue #35)
# group: [firebird]
#
# isc_get_segment's rc == 0 means "this call's segment completed without
# truncation" -- it says nothing about whether more segments remain.
# isc_segstr_eof is the only real end-of-blob signal. TBLOB_MULTISEG
# (scripts/mkblob_fixture.cpp) is written via 51 genuinely separate
# 80-byte isc_put_segment calls per column -- a plain SQL literal INSERT
# does not create multiple physical segments regardless of declared
# SEGMENT SIZE, so this is the only fixture that actually exercises the
# bug. NOTE = 'START-NOTE-' + 4000x'N' + '-END-NOTE' (4020 bytes).
# DATA = 'START-DATA-' + 4000x'D' + '-END-DATA' (4020 bytes, BLOB SUB_TYPE 0).

require firebird

require-env FIREBIRD_TEST_DB

require-env ISC_PASSWORD

statement ok
ATTACH '${FIREBIRD_TEST_DB}' AS fb (TYPE firebird);

# --- text BLOB (SUB_TYPE 1), multi-segment -----------------------------

query I
SELECT length(NOTE) FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
4020

query I
SELECT md5(NOTE) FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
8b0ecc5b1da53e0cd50a096b6c61884f

query I
SELECT starts_with(NOTE, 'START-NOTE-') AND ends_with(NOTE, '-END-NOTE')
  FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
true

# --- binary BLOB (SUB_TYPE 0), multi-segment ---------------------------
# Validated as opaque bytes (checksum + partial hex), never as text.

query I
SELECT octet_length(DATA) FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
4020

query I
SELECT md5(DATA) FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
5a5473ca6ca300831ce18ee4066a6741

query II
SELECT hex(DATA[1:11]), hex(DATA[-9:]) FROM fb.main.TBLOB_MULTISEG WHERE ID = 1;
----
53544152542D444154412D	2D454E442D44415441

# --- single-segment BLOB regression guard ------------------------------
# FILE_STORAGE.PAYLOAD (short, single-segment) must be unaffected by the
# fix -- these values are already asserted in firebird_scan.test; this
# is an explicit same-fixture guard living alongside the multi-segment
# cases so a future reader sees both side by side.

query I
SELECT octet_length(PAYLOAD) FROM fb.main.FILE_STORAGE WHERE FILE_ID = 3;
----
5
```

- [ ] **Step 2: Run it against the CURRENT (unfixed) code and confirm it fails**

```
build/release/test/unittest.exe test/sql/firebird_blob_lossless.test
```
Expected: FAILED on the `length(NOTE)` and `md5(NOTE)` queries (actual `80` / `92bfe48b3f323b09880b1b5225378ebb` instead of `4020` / `8b0ecc5b1da53e0cd50a096b6c61884f`), and on the `octet_length(DATA)`/`md5(DATA)`/hex queries similarly. The `FILE_STORAGE` guard query passes (single-segment, unaffected).

- [ ] **Step 3: Add the DEPT long-comment end-marker assertion to `test/sql/firebird_metadata_bridge.test`**

Find this existing block (already in the file):
```
# --- firebird_comments: long BLOB comment (>4000 chars) round-trips fully ----
# DEPT has a 4100-char comment; ReadBlob must return all 4100 chars.
# Under the old VARCHAR(4000) CAST this would have returned <=4000.
query I
SELECT LENGTH(comment) FROM firebird_comments('fb') WHERE object_name='DEPT' AND object_type='TABLE';
----
4100
```

Add immediately after it:
```
# The LENGTH check above passes even with issue #35's bug present: this
# specific comment happens to be stored as a single Firebird segment
# (a plain COMMENT ON ... IS '...' statement doesn't create multiple
# physical segments -- see scripts/mkblob_fixture.cpp's header comment),
# so it never could have caught the bug despite being written to. This
# assertion doesn't change that (still not a multi-segment repro -- see
# firebird_blob_lossless.test for the real one), but locks down the
# exact tail of the comment so a future accidental truncation to
# anything shorter than the true value is still caught here too.
query I
SELECT ends_with(comment, '_COMMENT_TEST_DEPT_L')
  FROM firebird_comments('fb') WHERE object_name='DEPT' AND object_type='TABLE';
----
true
```

- [ ] **Step 4: Run the updated `firebird_metadata_bridge.test` and confirm it still passes**

```
build/release/test/unittest.exe test/sql/firebird_metadata_bridge.test
```
Expected: all assertions pass, including the new one (this file's existing behavior is unaffected by issue #35 for this specific fixture, as documented in the comment just added — the value of this assertion is future-proofing, not proving today's bug).

- [ ] **Step 5: Commit**

```bash
git add test/sql/firebird_blob_lossless.test test/sql/firebird_metadata_bridge.test
git commit -m "test(blob-lossless): failing regression tests for issue #35

firebird_blob_lossless.test fails against current ReadBlob (reads back
80 bytes instead of 4020 for both the text and binary multi-segment
BLOB columns). firebird_metadata_bridge.test's new end-marker check on
DEPT's long comment already passes today (that fixture is single-
segment, as documented) but guards against a future truncation
regression on the same value."
```

---

### Task 3: Fix `ReadBlob` and confirm both test files now pass

**Files:**
- Modify: `src/firebird_client.cpp:830-844` (the `ReadBlob` method)

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new — this task's deliverable is the fix itself plus green tests.

- [ ] **Step 1: Remove the incorrect early-exit line**

Current code (`src/firebird_client.cpp`):
```cpp
std::string FirebirdStatement::ReadBlob(idx_t col) const {
    ISC_QUAD blob_id;
    std::memcpy(&blob_id, buffers_[col].data(), sizeof(blob_id));

    isc_blob_handle blob = 0;
    ISC_STATUS status[20] = {};
    isc_open_blob2(status, &conn_.db(), &conn_.tr(), &blob, &blob_id, 0, nullptr);
    FirebirdConnection::Check(status, "isc_open_blob2");

    std::string out;
    char segment[8192];
    unsigned short seg_len = 0;
    while (true) {
        std::memset(status, 0, sizeof(status));
        ISC_STATUS rc = isc_get_segment(status, &blob, &seg_len, sizeof(segment), segment);
        if (rc == isc_segstr_eof) break;
        if (rc != 0 && rc != isc_segment) {
            isc_close_blob(status, &blob);
            FirebirdConnection::Check(status, "isc_get_segment");
        }
        out.append(segment, seg_len);
        if (rc == 0) break;  // last partial segment
    }
    std::memset(status, 0, sizeof(status));
    isc_close_blob(status, &blob);
    return out;
}
```

Replace with:
```cpp
std::string FirebirdStatement::ReadBlob(idx_t col) const {
    ISC_QUAD blob_id;
    std::memcpy(&blob_id, buffers_[col].data(), sizeof(blob_id));

    isc_blob_handle blob = 0;
    ISC_STATUS status[20] = {};
    isc_open_blob2(status, &conn_.db(), &conn_.tr(), &blob, &blob_id, 0, nullptr);
    FirebirdConnection::Check(status, "isc_open_blob2");

    // isc_get_segment's return codes: isc_segstr_eof is the ONLY signal
    // that there is no more BLOB data -- this is the sole loop-exit
    // condition. isc_segment means the current logical segment was
    // larger than `segment` and got truncated; the rest of that same
    // segment follows on the next call (handled below by simply looping
    // again). rc == 0 means this call's segment completed with no
    // truncation -- it says nothing about whether MORE segments remain
    // after it. A multi-segment BLOB returns 0 on every call except the
    // last real read before isc_segstr_eof, so treating rc == 0 as
    // end-of-blob (as this loop used to) silently drops every segment
    // after the first. See docs/superpowers/specs/2026-07-15-blob-lossless-fix-design.md.
    std::string out;
    char segment[8192];
    unsigned short seg_len = 0;
    while (true) {
        std::memset(status, 0, sizeof(status));
        ISC_STATUS rc = isc_get_segment(status, &blob, &seg_len, sizeof(segment), segment);
        if (rc == isc_segstr_eof) break;
        if (rc != 0 && rc != isc_segment) {
            isc_close_blob(status, &blob);
            FirebirdConnection::Check(status, "isc_get_segment");
        }
        out.append(segment, seg_len);
    }
    std::memset(status, 0, sizeof(status));
    isc_close_blob(status, &blob);
    return out;
}
```

- [ ] **Step 2: Rebuild**

```
scripts\build_windows_local.bat
```
Expected: builds cleanly (this is a one-line removal plus a comment — no new warnings expected).

- [ ] **Step 3: Run `firebird_blob_lossless.test` and confirm it now passes**

```
build/release/test/unittest.exe test/sql/firebird_blob_lossless.test
```
Expected: `All tests passed (N assertions in 1 test case)` — every query from Task 2 Step 1 now returns the correct value (`length(NOTE)=4020`, correct `md5`s, markers present, `FILE_STORAGE` guard unchanged).

- [ ] **Step 4: Run the full existing suite to confirm no regressions**

Run every `test/sql/firebird_*.test` file individually via `build/release/test/unittest.exe <file>` (same pattern used throughout this branch's predecessors — see `.superpowers/sdd/` reports in prior branches for the exact per-file command list if unsure), with `FIREBIRD_TEST_DB`, `ISC_USER`, `ISC_PASSWORD`, `FIREBIRD_NONE_DB`, `FIREBIRD_DECFLOAT_DB` set. Pay particular attention to:
- `firebird_scan.test` and `firebird_attach.test` (existing `FILE_STORAGE.PAYLOAD` single-segment BLOB assertions).
- `firebird_metadata_bridge.test` (the modified file from Task 2).
- `firebird_type_audit.test` and `firebird_type_audit_fb4.test` (the `blob_text` finding must still fire where it already did — this fix does not touch type-mapping policy).

Expected: every file reports `All tests passed`, with assertion counts matching each file's pre-existing baseline plus the intentional additions from Task 2 (`firebird_metadata_bridge.test`: +1).

- [ ] **Step 5: Commit**

```bash
git add src/firebird_client.cpp
git commit -m "fix(blob-lossless): ReadBlob must not stop on rc == 0 (#35)

isc_get_segment's rc == 0 means the current call's segment completed
without truncation -- it says nothing about whether more segments
remain. isc_segstr_eof is the only real end-of-blob signal. The removed
'if (rc == 0) break' treated every multi-segment BLOB's first
successful segment read as if it were the last, silently dropping
everything after it -- affecting text BLOB, binary BLOB, and
RDB\$DESCRIPTION comment reads alike, since all three go through this
one shared helper.

firebird_blob_lossless.test (added in the prior commit, confirmed
failing against the old code) now passes."
```

---

### Task 4: Register the new test in the cross-version matrix, and update docs

**Files:**
- Modify: `scripts/build_matrix.ps1:52-71` (the `$testFixtureVar` map)
- Modify: `docs/pt/function_manual.md`
- Modify: `docs/en/function_manual.md`

**Interfaces:**
- Consumes: `test/sql/firebird_blob_lossless.test` (Task 2).
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Register the new test file in `scripts/build_matrix.ps1`**

Current (`scripts/build_matrix.ps1:52-71`):
```powershell
$testFixtureVar = @{
    'firebird_attach.test'        = 'FIREBIRD_TEST_DB'
    'firebird_scan.test'          = 'FIREBIRD_TEST_DB'
    'firebird_metadata.test'      = 'FIREBIRD_TEST_DB'
    'firebird_predicates.test'    = 'FIREBIRD_TEST_DB'
    'firebird_bind_params.test'   = 'FIREBIRD_TEST_DB'
    'firebird_paging.test'        = 'FIREBIRD_TEST_DB'
    'firebird_pool.test'          = 'FIREBIRD_TEST_DB'
    'firebird_pool_stats.test'    = 'FIREBIRD_TEST_DB'
    'firebird_observability.test' = 'FIREBIRD_TEST_DB'
    'firebird_profile_table.test' = 'FIREBIRD_TEST_DB'
    'firebird_dbt_sources.test'        = 'FIREBIRD_TEST_DB'
    'firebird_metadata_bridge.test'    = 'FIREBIRD_TEST_DB'
    'firebird_decfloat.test'           = 'FIREBIRD_DECFLOAT_DB'
    'firebird_none_charset.test'       = 'FIREBIRD_NONE_DB'
    'firebird_explain_pushdown.test'   = 'FIREBIRD_TEST_DB'
    'firebird_type_audit.test'         = 'FIREBIRD_TEST_DB'
    'firebird_health.test'             = 'FIREBIRD_TEST_DB'
    'firebird_index_profile.test'      = 'FIREBIRD_TEST_DB'
}
```

Add one line so the map reads:
```powershell
$testFixtureVar = @{
    'firebird_attach.test'        = 'FIREBIRD_TEST_DB'
    'firebird_scan.test'          = 'FIREBIRD_TEST_DB'
    'firebird_metadata.test'      = 'FIREBIRD_TEST_DB'
    'firebird_predicates.test'    = 'FIREBIRD_TEST_DB'
    'firebird_bind_params.test'   = 'FIREBIRD_TEST_DB'
    'firebird_paging.test'        = 'FIREBIRD_TEST_DB'
    'firebird_pool.test'          = 'FIREBIRD_TEST_DB'
    'firebird_pool_stats.test'    = 'FIREBIRD_TEST_DB'
    'firebird_observability.test' = 'FIREBIRD_TEST_DB'
    'firebird_profile_table.test' = 'FIREBIRD_TEST_DB'
    'firebird_dbt_sources.test'        = 'FIREBIRD_TEST_DB'
    'firebird_metadata_bridge.test'    = 'FIREBIRD_TEST_DB'
    'firebird_decfloat.test'           = 'FIREBIRD_DECFLOAT_DB'
    'firebird_none_charset.test'       = 'FIREBIRD_NONE_DB'
    'firebird_explain_pushdown.test'   = 'FIREBIRD_TEST_DB'
    'firebird_type_audit.test'         = 'FIREBIRD_TEST_DB'
    'firebird_health.test'             = 'FIREBIRD_TEST_DB'
    'firebird_index_profile.test'      = 'FIREBIRD_TEST_DB'
    'firebird_blob_lossless.test'      = 'FIREBIRD_TEST_DB'
}
```

(Without this, the matrix script silently never runs the new test file at all — its loop only iterates over this map's keys.)

- [ ] **Step 2: Update `docs/pt/function_manual.md`**

Find (existing text, in the "two known, tracked exceptions" section):
```
Duas excecoes conhecidas e rastreadas a essa politica existem hoje:

- `TIME WITH TIME ZONE` atualmente descarta a zona/offset original na
  leitura; ja sinalizado pelo finding `time_tz` de `firebird_type_audit`
  acima.
- `BLOB` de texto multi-segmento pode ser truncado por um defeito no loop
  de segmentos de `ReadBlob`, rastreado como issue #35 — uma causa raiz
  separada da divergencia de tipo de coluna em view corrigida como issue
  #33 acima.
```

Replace with:
```
Uma excecao conhecida e rastreada a essa politica existe hoje:

- `TIME WITH TIME ZONE` atualmente descarta a zona/offset original na
  leitura; ja sinalizado pelo finding `time_tz` de `firebird_type_audit`
  acima.

(Issue #35 — BLOB multi-segmento truncado por um defeito no loop de
segmentos de `ReadBlob` — foi corrigida; `ReadBlob` agora continua lendo
segmentos ate `isc_segstr_eof`, o unico sinal real de fim de BLOB, em
vez de parar erroneamente no primeiro `rc == 0`.)
```

- [ ] **Step 3: Update `docs/en/function_manual.md`**

Find (existing text, mirroring the PT section):
```
Two known, tracked exceptions to this policy exist today:

- `TIME WITH TIME ZONE` currently discards the original zone/offset on
  read; already flagged by `firebird_type_audit`'s `time_tz` finding above.
- Multi-segment `BLOB` text can be truncated by a segment-loop defect in
  `ReadBlob`, tracked as issue #35 — a separate root cause from the view
  column-type mismatch fixed as issue #33 above.
```

Replace with:
```
One known, tracked exception to this policy exists today:

- `TIME WITH TIME ZONE` currently discards the original zone/offset on
  read; already flagged by `firebird_type_audit`'s `time_tz` finding above.

(Issue #35 — multi-segment `BLOB` truncated by a segment-loop defect in
`ReadBlob` — is fixed; `ReadBlob` now keeps reading segments until
`isc_segstr_eof`, the only real end-of-blob signal, instead of
incorrectly stopping on the first `rc == 0`.)
```

- [ ] **Step 4: Commit**

```bash
git add scripts/build_matrix.ps1 docs/pt/function_manual.md docs/en/function_manual.md
git commit -m "docs(blob-lossless): register new test in matrix, close #35 doc caveat

scripts/build_matrix.ps1's hardcoded test list needed the new
firebird_blob_lossless.test added explicitly, or the DuckDB
v1.5.2/v1.5.3/v1.5.4 matrix would silently skip it. Also removes the
now-resolved #35 caveat from both function manuals' known-exceptions
list, leaving only the still-open TIME_TZ exception."
```

---

## After Task 4

Per the branch's standing gates (matching every prior v1.0 branch): run the cross-version matrix (`scripts/build_matrix.ps1`, sed-copied to point at this worktree, same as every prior branch), then the read-only maturity battery against the real production DB (emitting only lengths/checksums/counts, never BLOB content), before opening a PR. Both are follow-up steps after this plan's four tasks, not separate plan tasks themselves — they validate the branch as a whole, the same way they did for #33/RACE33.
