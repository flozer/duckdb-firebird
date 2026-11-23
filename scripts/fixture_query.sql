/* firebird_query fixture (Firebird 3, 4 and 5).
 *
 * Synthetic data only, in its own database (query.fdb, UTF8).
 *
 *   QTYPES : one column per Firebird 3 type family, with a row of
 *              typical values, a row of extremes and an all-NULL row.
 *              Text is written as hex literals so the stored bytes do not
 *              depend on isql's connection character set.
 *   SP_SEQ   : selectable procedure returning N rows.
 *   QLOG / SP_WRITE : a selectable procedure that writes (firebird_query
 *                     must refuse it: its transaction is read-only).
 *   SP_AUTO, SP_EDS, SP_EDS_AUTO : selectable procedures that write to
 *                     QLOG from an autonomous transaction and through
 *                     EXECUTE STATEMENT ON EXTERNAL DATA SOURCE (this same
 *                     database); the read-only transaction refuses them too.
 *   SP_EDS_READ : reads through the same external data source, to show
 *                 the external connection itself works.
 *   QSEQ : a sequence; GEN_ID / NEXT VALUE FOR in a SELECT advance it.
 */
SET SQL DIALECT 3;

CREATE TABLE QTYPES (
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
INSERT INTO QTYPES VALUES (
    1, 12, 123456, 1234567890123, 12.34, -1234567.89, 1.500, 12345.6789,
    1.5, 3.141592653589793, '2026-11-18', '13:45:30.1234', '2026-11-18 13:45:30.1234',
    'abc', _UTF8 x'41C3A7C3A36F20C3B120E282AC', x'41E7E36F', TRUE,
    _UTF8 x'41C3A7C3A36F20C3B120E282AC', x'00FF10');

/* Extremes: NUMERIC(18,4) and BIGINT limits, negative scaled values,
 * dates before 1900, empty strings. */
INSERT INTO QTYPES VALUES (
    2, -32768, -2147483648, -9223372036854775808, -99.99, 9999999.99, -0.001,
    -922337203685477.5808, -2.5, -1.0E300, '1850-06-01', '00:00:00', '1899-12-31 23:59:59.9999',
    '', '', '', FALSE, '', x'');

INSERT INTO QTYPES (ID) VALUES (3);

INSERT INTO QTYPES (ID, C_BIGINT, C_NUM18_4) VALUES (4, 9223372036854775807, 922337203685477.5807);

CREATE TABLE QLOG (
    ID INTEGER NOT NULL,
    NOTE VARCHAR(20)
);

CREATE SEQUENCE QSEQ;

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
    INSERT INTO QLOG (ID, NOTE) VALUES (1, 'written');
    ID = 1;
    SUSPEND;
END^

CREATE PROCEDURE SP_AUTO
RETURNS (ID INTEGER)
AS
BEGIN
    IN AUTONOMOUS TRANSACTION DO
        INSERT INTO QLOG (ID, NOTE) VALUES (2, 'autonomous');
    ID = 2;
    SUSPEND;
END^

CREATE PROCEDURE SP_EDS
RETURNS (ID INTEGER)
AS
BEGIN
    EXECUTE STATEMENT 'INSERT INTO QLOG (ID, NOTE) VALUES (3, ''external'')'
        ON EXTERNAL DATA SOURCE RDB$GET_CONTEXT('SYSTEM', 'DB_NAME');
    ID = 3;
    SUSPEND;
END^

CREATE PROCEDURE SP_EDS_AUTO
RETURNS (ID INTEGER)
AS
BEGIN
    EXECUTE STATEMENT 'INSERT INTO QLOG (ID, NOTE) VALUES (4, ''external'')'
        ON EXTERNAL DATA SOURCE RDB$GET_CONTEXT('SYSTEM', 'DB_NAME')
        WITH AUTONOMOUS TRANSACTION;
    ID = 4;
    SUSPEND;
END^

CREATE PROCEDURE SP_EDS_READ
RETURNS (N INTEGER)
AS
BEGIN
    EXECUTE STATEMENT 'SELECT COUNT(*) FROM QTYPES'
        ON EXTERNAL DATA SOURCE RDB$GET_CONTEXT('SYSTEM', 'DB_NAME')
        INTO :N;
    SUSPEND;
END^

SET TERM ; ^

COMMIT;
