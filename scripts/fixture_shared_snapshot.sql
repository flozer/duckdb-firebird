/* Fixture for scripts/check_shared_snapshot.sh (shared snapshot for
 * partitioned scans), loaded into its own throw-away database by that
 * script on every Firebird 3/4/5 matrix leg.
 *
 * ACC: 2,000,000 rows (ID 1..2000000, VAL = 100) plus one row with
 * ID = 10,000,000 and VAL = 0. The extra row widens the PK span to 10M,
 * above PickPartitionCount's 4M threshold, so the ATTACH path partitions
 * the table on its own. Invariants that scripts/shared_snapshot_writer.sql
 * keeps in every committed state: SUM(VAL) = 200,000,000 and
 * COUNT(*) = 2,000,001.
 *
 * WRITER_STATE / WRITER_STOP: progress counter and stop flag for the
 * concurrent writer.
 */
SET SQL DIALECT 3;

CREATE TABLE ACC (
    ID  INTEGER NOT NULL PRIMARY KEY,
    VAL BIGINT  NOT NULL
);
CREATE TABLE WRITER_STATE (TRANSFERS BIGINT NOT NULL);
CREATE TABLE WRITER_STOP (X INTEGER);
COMMIT;

INSERT INTO WRITER_STATE VALUES (0);

SET TERM ^;
EXECUTE BLOCK AS
  DECLARE I INTEGER = 1;
BEGIN
  WHILE (I <= 2000000) DO BEGIN
    INSERT INTO ACC VALUES (:I, 100);
    I = I + 1;
  END
  INSERT INTO ACC VALUES (10000000, 0);
END^
SET TERM ;^
COMMIT;
