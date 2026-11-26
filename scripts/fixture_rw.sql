/* Write-path fixture (Firebird 3, 4 and 5).
 *
 * Synthetic data only. Loaded into its own database (rw.fdb, UTF8) so the
 * write tests can write without touching the other fixtures.
 *
 *   RW_TYPES : one column per Firebird 3 type family, with a row of
 *              typical values, a row of extremes and an all-NULL row.
 *              Text is written as hex literals so the stored bytes do not
 *              depend on isql's connection character set.
 *   RW_TARGET: the same columns as RW_TYPES, empty: parameter round trips.
 *   CUSTOMER : an upsert target (EXECUTE BLOCK ... UPDATE OR INSERT).
 *   RW_CS1252: WIN1252 VARCHAR and text BLOB in the UTF8 database.
 *   RW_DEFAULTS: column defaults, for INSERTs that don't name every column.
 *   RW_COMPUTED: a COMPUTED BY column, which an INSERT must not send.
 *   SP_SEQ   : selectable procedure returning N rows.
 *   RW_LOG / SP_WRITE: a log table, and a selectable procedure that writes.
 *   RW_COMMIT_GUARD: always empty; a transaction that commits with a row in
 *              it is refused by the RW_COMMIT_CHECK database trigger (a
 *              commit that fails, for the INSERT tests).
 */
SET SQL DIALECT 3;

CREATE TABLE RW_TYPES (
    ID             INTEGER NOT NULL PRIMARY KEY,
    C_SMALLINT     SMALLINT,
    C_INTEGER      INTEGER,
    C_BIGINT       BIGINT,
    C_NUM4_2       NUMERIC(4, 2),
    C_DEC9_2       DECIMAL(9, 2),
    C_NUM9_3       NUMERIC(9, 3),
    C_NUM18_4      NUMERIC(18, 4),
    C_FLOAT        FLOAT,
    C_DOUBLE       DOUBLE PRECISION,
    C_DATE         DATE,
    C_TIME         TIME,
    C_TS           TIMESTAMP,
    C_CHAR         CHAR(10),
    C_VARCHAR      VARCHAR(40),
    C_VARCHAR_NONE VARCHAR(20) CHARACTER SET NONE,
    C_BOOL         BOOLEAN,
    C_BLOB_TEXT    BLOB SUB_TYPE TEXT,
    C_BLOB_BIN     BLOB SUB_TYPE BINARY
);

/* 'Ação ñ €' in UTF-8; 'Ação' in WIN1252 for the NONE column. */
INSERT INTO RW_TYPES VALUES (
    1, 12, 123456, 1234567890123, 12.34, -1234567.89, 1.500, 12345.6789,
    1.5, 3.141592653589793, '2026-11-18', '13:45:30.1234', '2026-11-18 13:45:30.1234',
    'abc', _UTF8 x'41C3A7C3A36F20C3B120E282AC', x'41E7E36F', TRUE,
    _UTF8 x'41C3A7C3A36F20C3B120E282AC', x'00FF10');

/* Extremes: NUMERIC(18,4) and BIGINT limits, negative scaled values,
 * dates before 1900, empty strings. */
INSERT INTO RW_TYPES VALUES (
    2, -32768, -2147483648, -9223372036854775808, -99.99, 9999999.99, -0.001,
    -922337203685477.5808, -2.5, -1.0E300, '1850-06-01', '00:00:00', '1899-12-31 23:59:59.9999',
    '', '', '', FALSE, '', x'');

INSERT INTO RW_TYPES (ID) VALUES (3);

INSERT INTO RW_TYPES (ID, C_BIGINT, C_NUM18_4) VALUES (4, 9223372036854775807, 922337203685477.5807);

/* Write targets: the same columns as RW_TYPES, empty. */
CREATE TABLE RW_TARGET (
    ID             INTEGER NOT NULL PRIMARY KEY,
    C_SMALLINT     SMALLINT,
    C_INTEGER      INTEGER,
    C_BIGINT       BIGINT,
    C_NUM4_2       NUMERIC(4, 2),
    C_DEC9_2       DECIMAL(9, 2),
    C_NUM9_3       NUMERIC(9, 3),
    C_NUM18_4      NUMERIC(18, 4),
    C_FLOAT        FLOAT,
    C_DOUBLE       DOUBLE PRECISION,
    C_DATE         DATE,
    C_TIME         TIME,
    C_TS           TIMESTAMP,
    C_CHAR         CHAR(10),
    C_VARCHAR      VARCHAR(40),
    C_VARCHAR_NONE VARCHAR(20) CHARACTER SET NONE,
    C_BOOL         BOOLEAN,
    C_BLOB_TEXT    BLOB SUB_TYPE TEXT,
    C_BLOB_BIN     BLOB SUB_TYPE BINARY
);

CREATE TABLE CUSTOMER (
    ID   INTEGER NOT NULL PRIMARY KEY,
    NAME VARCHAR(40),
    CITY VARCHAR(30)
);

/* WIN1252 columns in this UTF8 database: values are stored as WIN1252 bytes
 * ('Ação' is 4 bytes, not 6). */
CREATE TABLE RW_CS1252 (
    ID INTEGER NOT NULL PRIMARY KEY,
    V  VARCHAR(10) CHARACTER SET WIN1252,
    B  BLOB SUB_TYPE TEXT CHARACTER SET WIN1252
);

/* A COMPUTED BY column: an INSERT that sends it is refused at prepare. */
CREATE TABLE RW_COMPUTED (
    ID    INTEGER NOT NULL,
    TWICE COMPUTED BY (ID * 2)
);

/* Column defaults: an INSERT that doesn't name a column gets Firebird's
 * default, not NULL. */
CREATE TABLE RW_DEFAULTS (
    ID   INTEGER NOT NULL PRIMARY KEY,
    NOTE VARCHAR(10) DEFAULT 'dflt',
    QTY  INTEGER DEFAULT 7
);

CREATE TABLE RW_LOG (
    ID INTEGER NOT NULL,
    NOTE VARCHAR(20)
);

CREATE TABLE RW_COMMIT_GUARD (
    ID INTEGER NOT NULL
);

CREATE EXCEPTION RW_COMMIT_REFUSED 'commit refused: RW_COMMIT_GUARD has a row';

SET TERM ^ ;

CREATE PROCEDURE SP_SEQ (N INTEGER)
RETURNS (I INTEGER, SQ BIGINT)
AS
BEGIN
    I = 1;
    WHILE (I <= N) DO
    BEGIN
        SQ = CAST(I AS BIGINT) * I;
        SUSPEND;
        I = I + 1;
    END
END^

CREATE PROCEDURE SP_WRITE
RETURNS (ID INTEGER)
AS
BEGIN
    INSERT INTO RW_LOG (ID, NOTE) VALUES (1, 'written');
    ID = 1;
    SUSPEND;
END^

CREATE TRIGGER RW_COMMIT_CHECK ON TRANSACTION COMMIT
AS
BEGIN
    IF (EXISTS (SELECT 1 FROM RW_COMMIT_GUARD)) THEN
        EXCEPTION RW_COMMIT_REFUSED;
END^

SET TERM ; ^

COMMIT;
