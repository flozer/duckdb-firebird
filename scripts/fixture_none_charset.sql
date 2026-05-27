/* Fixture for the CHARACTER SET NONE / none_encoding feature.
 *
 * Creates C:\fbtest\none.fdb with three rows that exercise the
 * three branches the extension cares about:
 *
 *   id=1 : pure ASCII — round-trips byte-identical under any
 *          none_encoding (including strict).
 *
 *   id=2 : WIN1252-encoded Brazilian Portuguese — round-trips
 *          correctly under none_encoding='win1252' (and
 *          'iso8859_1' for letters at codepoints <= U+00FF).
 *          Strict mode must REJECT the row.
 *
 *   id=3 : CP1252-exclusive bytes (0x93 0x94 = curly quotes “”).
 *          These only round-trip under 'win1252'. ISO-8859-1
 *          maps those bytes to control chars; the extension
 *          preserves them as U+0093 / U+0094.
 *
 * Run with:
 *   isql -i scripts\fixture_create_none.sql
 *   isql -user SYSDBA -password masterkey -i scripts\fixture_none_charset.sql C:\fbtest\none.fdb
 */
SET SQL DIALECT 3;

CREATE TABLE TXT (
    ID    INTEGER NOT NULL PRIMARY KEY,
    LABEL VARCHAR(80) CHARACTER SET NONE,
    NOTE  BLOB SUB_TYPE 1 CHARACTER SET NONE
);

INSERT INTO TXT (ID, LABEL, NOTE) VALUES
    (1, 'pure ascii', 'no high bytes here');

-- Row 2: WIN1252 / Latin-1 — bytes 0xC3 0xA7 ('ç' UTF-8) won't appear,
-- but a real WIN1252 client storing 'São Paulo' would write
-- 0x53 0xE3 0x6F 0x20 0x50 0x61 0x75 0x6C 0x6F. We build that byte
-- sequence using ASCII_CHAR so the fixture is reproducible regardless
-- of the writer client encoding.
INSERT INTO TXT (ID, LABEL, NOTE) VALUES
    (2,
     'S' || ASCII_CHAR(0xE3) || 'o Paulo',                         -- São Paulo
     'A' || ASCII_CHAR(0xE7) || 'u' || ASCII_CHAR(0xCC) || 'car'); -- AçuÌcar (Ì = 0xCC, just for variety)

-- Row 3: CP1252-only bytes — curly quotes 0x93 (left), 0x94 (right).
-- In ISO-8859-1 those land in C1 control space; in Windows-1252 they
-- decode to U+201C / U+201D.
INSERT INTO TXT (ID, LABEL, NOTE) VALUES
    (3,
     ASCII_CHAR(0x93) || 'hi' || ASCII_CHAR(0x94),                 -- "hi"
     ASCII_CHAR(0x80));                                            -- € sign

COMMIT;
QUIT;
