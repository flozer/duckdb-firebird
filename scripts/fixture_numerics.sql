/* Fixture for the G3 `numeric_widen_int64` feature.
 *
 * NUMERIC(18,s) / DECIMAL(18,s) in Firebird (dialect 3) is physically a
 * 64-bit scaled integer: it carries values up to +/-2^63-1 scaled, one
 * order of magnitude beyond DuckDB's DECIMAL(18,s) ceiling of 10^18-1
 * scaled. The default projection (DECIMAL(18,s)) therefore silently
 * corrupts the extreme values; the opt-in numeric_widen_int64=true
 * projects DECIMAL(38,s) (hugeint-backed) and is lossless. This fixture
 * pins the exact boundary values the test asserts:
 *
 *   id=1 : the classic INT64 sentinel —
 *          N18_6 = -9223372036854.775808 (scaled = INT64_MIN exactly),
 *          N18_2 = -922337203685477.58, N18_0 = -9223372036854775808
 *   id=2 : one scaled step below the positive extreme —
 *          N18_6 =  9223372036854.775807 (scaled = INT64_MAX exactly),
 *          N18_2 =  922337203685477.57, N18_0 =  9223372036854775807
 *   id=3 : ordinary value 12345.678901 / 12345.67 / 12345
 *   id=4 : real NULL in all numeric columns (must stay NULL)
 *
 * Bootstrap (Firebird 3/4/5, dialect 3):
 *   isql -user SYSDBA -password masterkey -i scripts/fixture_numerics.sql <DB>
 * where <DB> already exists (create it first, e.g.):
 *   CREATE DATABASE '<path>' DEFAULT CHARACTER SET UTF8;
 *
 * NUM18 deliberately stays in its own database: the main test.fdb
 * relation lists are pinned by static, cross-version expectations
 * (firebird_metadata.test SHOW TABLES, dbt-sources YAML). The test reads
 * it through FIREBIRD_NUMERICS_DB and skips when that env var is unset.
 */
SET SQL DIALECT 3;

CREATE TABLE NUM18 (
    ID    INTEGER NOT NULL PRIMARY KEY,
    N18_6 NUMERIC(18,6),
    N18_2 NUMERIC(18,2),
    N18_0 NUMERIC(18,0)
);
COMMIT;

INSERT INTO NUM18 (ID, N18_6, N18_2, N18_0) VALUES
    (1, -9223372036854.775808, -922337203685477.58, -9223372036854775808);
INSERT INTO NUM18 (ID, N18_6, N18_2, N18_0) VALUES
    (2, 9223372036854.775807, 922337203685477.57, 9223372036854775807);
INSERT INTO NUM18 (ID, N18_6, N18_2, N18_0) VALUES
    (3, 12345.678901, 12345.67, 12345);
INSERT INTO NUM18 (ID, N18_6, N18_2, N18_0) VALUES
    (4, NULL, NULL, NULL);
COMMIT;

-- Pass-through view over the extremes: the scanner's VIEW reconciliation
-- (issue #33) re-derives column types from a live describe, so this proves
-- the widened projection is applied on the SAME mapping on both sides
-- (declared type and fetch vector) for views as well as tables.
CREATE VIEW V_NUM18 AS SELECT ID, N18_6, N18_2 FROM NUM18;
COMMIT;
