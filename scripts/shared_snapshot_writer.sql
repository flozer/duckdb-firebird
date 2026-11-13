/* Concurrent writer for scripts/check_shared_snapshot.sh.
 *
 * Moves one unit of VAL from a random row in the low half of ACC
 * (ID 1..1000000) to a random row in the high half (ID 1000001..2000000),
 * so the two rows land in different partitions of any partitioned scan.
 * Each transfer runs in its own autonomous transaction and is committed
 * on its own, so SUM(VAL) stays 200,000,000 in every committed state
 * while the table changes continuously.
 *
 * Runs until WRITER_STOP gets a row (checked inside each autonomous
 * transaction, which starts with a fresh snapshot) or 10 minutes pass.
 */
SET TERM ^;
EXECUTE BLOCK AS
  DECLARE A INTEGER;
  DECLARE B INTEGER;
  DECLARE STOP_NOW SMALLINT = 0;
  DECLARE DEADLINE TIMESTAMP;
BEGIN
  DEADLINE = DATEADD(600 SECOND TO CAST('NOW' AS TIMESTAMP));
  WHILE (STOP_NOW = 0) DO BEGIN
    A = 1 + CAST(RAND() * 999999 AS INTEGER);
    B = 1000001 + CAST(RAND() * 999999 AS INTEGER);
    IN AUTONOMOUS TRANSACTION DO BEGIN
      UPDATE ACC SET VAL = VAL - 1 WHERE ID = :A;
      UPDATE ACC SET VAL = VAL + 1 WHERE ID = :B;
      UPDATE WRITER_STATE SET TRANSFERS = TRANSFERS + 1;
      IF (EXISTS (SELECT 1 FROM WRITER_STOP)) THEN STOP_NOW = 1;
    END
    IF (CAST('NOW' AS TIMESTAMP) > DEADLINE) THEN STOP_NOW = 1;
  END
END^
SET TERM ;^
COMMIT;
