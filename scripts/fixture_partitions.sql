/* Fixture for the recommended_partitions > 1 CI coverage (roadmap F4).
 *
 * PART_RANGE: a single-column INTEGER primary key with a deliberately
 * sparse, wide range. PickPartitionCount (firebird_scanner.cpp) starts
 * recommending real parallelism at a PK span of 4M (2 x the 2M
 * minimum-rows-per-partition floor). 300 rows at ID = i * 40000 span
 * 0..11,960,000 (~12M), so the profile recommends 2..5 partitions
 * depending on hardware concurrency, and a real `partitions=4` scan
 * exercises the parallel PK-range path end-to-end. Exactness invariants:
 * COUNT(*) = 300 and SUM(ID) = 40,000 * (299 * 300 / 2) = 1,794,000,000.
 *
 * PART_NO_LEVER: no PK, no index, no numeric/date column at all -- the
 * materialize_before_scan profile case (HIGH risk, no filter or watermark
 * candidate to recommend).
 *
 * Kept in its OWN database (not the main test.fdb fixture) so the pinned
 * relation lists asserted by firebird_metadata.test / dbt-sources stay
 * stable across all three Firebird matrix legs.
 *
 * Bootstrap (any Firebird 3/4/5, dialect 3):
 *   isql -user SYSDBA -password masterkey -i scripts/fixture_partitions.sql <DB>
 */
SET SQL DIALECT 3;

CREATE TABLE PART_RANGE (
    ID  INTEGER NOT NULL PRIMARY KEY,
    VAL VARCHAR(16) NOT NULL
);
COMMIT;

-- EXECUTE BLOCK carries internal semicolons, so isql needs a changed
-- statement terminator around it.
SET TERM !! ;
EXECUTE BLOCK AS
DECLARE I INTEGER = 0;
BEGIN
  WHILE (I < 300) DO
  BEGIN
    INSERT INTO PART_RANGE (ID, VAL) VALUES (:I * 40000, 'row' || :I);
    I = I + 1;
  END
END!!
SET TERM ; !!
COMMIT;

CREATE TABLE PART_NO_LEVER (
    CODE VARCHAR(10) NOT NULL,
    NOTE VARCHAR(40)
);
INSERT INTO PART_NO_LEVER VALUES ('X1', 'no lever at all');
INSERT INTO PART_NO_LEVER VALUES ('X2', NULL);
COMMIT;
