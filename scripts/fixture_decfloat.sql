/* Fixture for the Firebird 4+ DECFLOAT(16/34) fallback feature.
 *
 * DECFLOAT is IEEE 754 Decimal64 / Decimal128, with no native DuckDB
 * type. The extension projects these columns as CAST(... AS VARCHAR(64))
 * server-side so the value arrives as a lossless string instead of the
 * old silent NULL. This fixture covers the cases the test asserts:
 *
 *   id=1 : zero
 *   id=2 : ordinary decimal
 *   id=3 : large exponent form (longest string; DECFLOAT(34) ~ 40 chars)
 *   id=4 : NaN
 *   id=5 : +Infinity
 *   id=6 : -Infinity
 *   id=7 : real NULL (must stay NULL, not a string)
 *   id=8 : small negative / boundary
 *
 * Bootstrap (Firebird 4 or 5, dialect 3, any default charset):
 *   isql -user SYSDBA -password masterkey -i scripts/fixture_decfloat.sql <DB>
 * where <DB> already exists (create it first, e.g.):
 *   CREATE DATABASE '<path>' USER 'SYSDBA' PASSWORD 'masterkey';
 *
 * The test reads it through FIREBIRD_DECFLOAT_DB and skips when that env
 * var is unset (the main CI fixture does not yet carry DECFLOAT — see the
 * debt note in docs/roadmap / test report).
 */
SET SQL DIALECT 3;

CREATE TABLE DECVALS (
    ID   INTEGER NOT NULL PRIMARY KEY,
    D16  DECFLOAT(16),
    D34  DECFLOAT(34)
);
COMMIT;

INSERT INTO DECVALS (ID, D16, D34) VALUES (1, 0, 0);
INSERT INTO DECVALS (ID, D16, D34) VALUES (2, 123.45, 123.45);
INSERT INTO DECVALS (ID, D16, D34) VALUES
    (3, 1.2345678901234E+20, 1.234567890123456789012345678901234E+200);
INSERT INTO DECVALS (ID, D16, D34) VALUES
    (4, CAST('NaN' AS DECFLOAT(16)), CAST('NaN' AS DECFLOAT(34)));
INSERT INTO DECVALS (ID, D16, D34) VALUES
    (5, CAST('Infinity' AS DECFLOAT(16)), CAST('Infinity' AS DECFLOAT(34)));
INSERT INTO DECVALS (ID, D16, D34) VALUES
    (6, CAST('-Infinity' AS DECFLOAT(16)), CAST('-Infinity' AS DECFLOAT(34)));
INSERT INTO DECVALS (ID, D16, D34) VALUES (7, NULL, NULL);
INSERT INTO DECVALS (ID, D16, D34) VALUES (8, -0.0001, -9999999999999999.9);
COMMIT;
