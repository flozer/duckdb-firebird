/* duckdb-firebird FB4/FB5 fixture.
 *
 * Creates a "biz4" database used by docs/guide_windows.md to exercise
 * every type added in Firebird 4 (INT128, DECIMAL(38, s), DECFLOAT 16/34,
 * TIMESTAMP/TIME WITH TIME ZONE) plus a handful of plain types for
 * federated-join examples.
 *
 * Run with:
 *   1) Create the database first:
 *        isql -i create_db.sql      (one-liner: CREATE DATABASE 'C:\fbtest\biz4.fdb' ...)
 *   2) Then load this schema:
 *        isql -user SYSDBA -password masterkey -i fixture_biz4.sql C:\fbtest\biz4.fdb
 */

SET SQL DIALECT 3;

CREATE TABLE FB4_TYPES (
    ID         INTEGER NOT NULL PRIMARY KEY,
    LABEL      VARCHAR(80),
    BIG_NUM    INT128,
    BIG_DEC    DECIMAL(38, 5),
    DEC16      DECFLOAT(16),
    DEC34      DECFLOAT(34),
    TS_TZ      TIMESTAMP WITH TIME ZONE,
    T_TZ       TIME WITH TIME ZONE,
    TS_PLAIN   TIMESTAMP,
    T_PLAIN    TIME
);

INSERT INTO FB4_TYPES VALUES
    (1, 'positive max',
        170141183460469231731687303715884105727,
        12345678901234567890.12345,
        1.23456789012345E16,
        1.234567890123456789012345678901234E34,
        TIMESTAMP '2026-05-25 14:30:00.123 America/Sao_Paulo',
        TIME '14:30:00.123 America/Sao_Paulo',
        TIMESTAMP '2026-05-25 14:30:00.123',
        TIME '14:30:00.123');

INSERT INTO FB4_TYPES VALUES
    (2, 'negative extremes',
        -170141183460469231731687303715884105728,
        -999999999999999999999999999999999.99999,   -- DECIMAL(38,5) lower bound
        -1.23456789012345E16,
        -9.99999999999999999999999999999999E34,
        TIMESTAMP '1990-01-15 03:00:00 UTC',
        TIME '03:00:00 UTC',
        TIMESTAMP '1990-01-15 03:00:00',
        TIME '03:00:00');

INSERT INTO FB4_TYPES VALUES
    (3, 'zero',
        0, 0, 0, 0,
        TIMESTAMP '2000-01-01 00:00:00 UTC',
        TIME '00:00:00 UTC',
        TIMESTAMP '2000-01-01 00:00:00',
        TIME '00:00:00');

INSERT INTO FB4_TYPES (ID, LABEL) VALUES (4, 'all nulls');
COMMIT;

CREATE TABLE DEPARTMENT (
    DEPT_ID    INTEGER NOT NULL PRIMARY KEY,
    DEPT_CODE  CHAR(4),
    DEPT_NAME  VARCHAR(40),
    BUDGET     NUMERIC(15, 2)
);

INSERT INTO DEPARTMENT VALUES (1, 'ENG ', 'Engineering', 5000000.00);
INSERT INTO DEPARTMENT VALUES (2, 'SAL ', 'Sales',       2500000.50);
INSERT INTO DEPARTMENT VALUES (3, 'OPS ', 'Operations',  1750000.00);

CREATE TABLE CUSTOMER (
    CUST_ID        INTEGER NOT NULL PRIMARY KEY,
    NAME           VARCHAR(80),
    SIGNUP_DATE    DATE,
    SIGNUP_TIME    TIME,
    IS_VIP         BOOLEAN,
    CREDIT_LIMIT   NUMERIC(18, 2),
    LOYALTY_SCORE  FLOAT
);

INSERT INTO CUSTOMER VALUES (1, 'Acucar Uniao Ltda', DATE '2020-03-14', TIME '08:15:00', TRUE,  15000.50, 4.7);
INSERT INTO CUSTOMER VALUES (2, 'Sao Paulo Textil',  DATE '2021-07-22', TIME '09:30:00', FALSE,  9800.00, 3.9);
INSERT INTO CUSTOMER VALUES (3, 'Coracao Forte ME',  DATE '2019-11-05', TIME '11:00:00', TRUE,  22000.75, 4.95);
COMMIT;

QUIT;
