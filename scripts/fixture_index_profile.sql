/* firebird_index_profile fixtures — additive against the EXISTING
 * FIREBIRD_TEST_DB (test.fdb). Does NOT create a new database.
 *
 * Run with (Windows):
 *   "C:\Program Files\Firebird\Firebird_5_0\isql.exe" -user SYSDBA ^
 *     -password masterkey -i scripts\fixture_index_profile.sql C:\fbtest\test.fdb
 */

-- Zero-index fixture: no PK, no unique constraint, no FK, no explicit
-- index. Exercises firebird_index_profile()'s synthetic "no indexes" row,
-- the no_indexes_on_table alert, and unindexed_filter_candidates.
CREATE TABLE TNO_INDEX (
    CODE  VARCHAR(10),
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TNO_INDEX VALUES ('A1', 10, 'first');
INSERT INTO TNO_INDEX VALUES ('B2', 20, 'second');
COMMIT;

-- Inactive-index fixture: a PK (so this table is NOT a zero-index case)
-- plus one secondary index later disabled. Exercises the index_inactive
-- alert, and confirms unindexed_filter_candidates excludes a column
-- covered by an index even when that index is inactive.
CREATE TABLE TIDX_INACTIVE (
    ID    INTEGER NOT NULL PRIMARY KEY,
    QTY   INTEGER,
    NOTE  VARCHAR(20)
);
INSERT INTO TIDX_INACTIVE VALUES (1, 5, 'x');
COMMIT;
CREATE INDEX IDX_TIDX_INACTIVE_QTY ON TIDX_INACTIVE (QTY);
ALTER INDEX IDX_TIDX_INACTIVE_QTY INACTIVE;
COMMIT;
