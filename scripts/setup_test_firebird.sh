#!/usr/bin/env bash
# Provisions the EMPLOYEE fixture that test/sql/firebird_scan.test expects.
#
# Usage:
#   sudo apt-get install -y firebird3.0-server  # installs libfbclient too
#   ./scripts/setup_test_firebird.sh
#
# After running, the following env vars are exported for the build/test
# scripts to consume:
#   FIREBIRD_TEST_DB   absolute path to the .fdb file
#   ISC_USER           SYSDBA
#   ISC_PASSWORD       <generated or fixed test password>

set -euo pipefail

# On Ubuntu the firebird3.0-server postinst writes the password into
# /etc/firebird/3.0/SYSDBA.password. Honor that if present so the file
# matches the running server's expectations; otherwise default to a known
# value for CI use.
if [[ -r /etc/firebird/3.0/SYSDBA.password ]]; then
    # shellcheck disable=SC1091
    source /etc/firebird/3.0/SYSDBA.password
fi

: "${ISC_USER:=SYSDBA}"
: "${ISC_PASSWORD:=masterkey}"
: "${FIREBIRD_TEST_DB:=/tmp/fbdata/test.fdb}"

mkdir -p "$(dirname "$FIREBIRD_TEST_DB")"
# Firebird's server runs as the 'firebird' user — give it write access.
if id -u firebird >/dev/null 2>&1; then
    if command -v sudo >/dev/null 2>&1; then
        sudo chown firebird:firebird "$(dirname "$FIREBIRD_TEST_DB")" || true
    else
        chown firebird:firebird "$(dirname "$FIREBIRD_TEST_DB")" || true
    fi
fi
if command -v sudo >/dev/null 2>&1; then
    sudo chmod 0777 "$(dirname "$FIREBIRD_TEST_DB")"
else
    chmod 0777 "$(dirname "$FIREBIRD_TEST_DB")"
fi

rm -f "$FIREBIRD_TEST_DB"

# Bootstrap the database + fixture.
ISQL=$(command -v isql-fb || command -v isql)
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" <<EOF
CREATE DATABASE '$FIREBIRD_TEST_DB' DEFAULT CHARACTER SET UTF8;
EOF

"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" <<'EOF'
CREATE TABLE EMPLOYEE (
  EMP_ID    INTEGER NOT NULL PRIMARY KEY,
  EMP_NAME  VARCHAR(60) NOT NULL,
  DEPT_NO   VARCHAR(3),
  SALARY    NUMERIC(10,2),
  HIRE_DATE DATE,
  ACTIVE    BOOLEAN,
  NOTE      BLOB SUB_TYPE 1
);
INSERT INTO EMPLOYEE VALUES (1, 'Alice Andersson', '600', 75000.50, DATE '2020-03-15', TRUE,  'Senior developer');
INSERT INTO EMPLOYEE VALUES (2, 'Bob Brown',       '600', 82500.00, DATE '2019-07-01', TRUE,  NULL);
INSERT INTO EMPLOYEE VALUES (3, 'Carlos Chen',     '900', 65000.75, DATE '2022-11-20', FALSE, 'On leave');
INSERT INTO EMPLOYEE VALUES (4, 'Diana Davis',     '600', 98000.25, DATE '2018-01-10', TRUE,  'Team lead');
INSERT INTO EMPLOYEE VALUES (5, 'Erik Eklund',     '700', 71200.00, DATE '2021-06-30', TRUE,  NULL);
COMMIT;

-- Binary BLOB fixture (SUB_TYPE 0) — exercises the BLOB read path that
-- DOESN'T degrade to VARCHAR. The payloads are short ASCII so the test
-- can assert on octet_length without binary string literals.
CREATE TABLE FILE_STORAGE (
  FILE_ID  INTEGER NOT NULL PRIMARY KEY,
  NAME     VARCHAR(40) NOT NULL,
  SIZE_B   INTEGER NOT NULL,
  PAYLOAD  BLOB SUB_TYPE 0
);
INSERT INTO FILE_STORAGE VALUES (1, 'logo.png',     6, CAST(_ASCII'binary' AS BLOB SUB_TYPE 0));
INSERT INTO FILE_STORAGE VALUES (2, 'empty.dat',    0, NULL);
INSERT INTO FILE_STORAGE VALUES (3, 'document.bin', 5, CAST(_ASCII'hello'  AS BLOB SUB_TYPE 0));
COMMIT;

-- Expression / computed index — has NO RDB$INDEX_SEGMENTS rows, so
-- firebird_profile_table() must render it as "(expression)" rather than
-- with empty parentheses. Guards the heavy-view / index-formatting path.
CREATE INDEX EMP_UPPER_NAME_IDX ON EMPLOYEE COMPUTED BY (UPPER(EMP_NAME));
COMMIT;

-- View fixture — confirms catalog surfaces RDB$RELATION_TYPE=1 alongside
-- regular tables, and that the binder/scanner can scan a view exactly
-- like a base table.
CREATE OR ALTER VIEW V_ACTIVE_EMP (EMP_ID, EMP_NAME, DEPT_NO) AS
    SELECT EMP_ID, EMP_NAME, DEPT_NO
      FROM EMPLOYEE WHERE ACTIVE = TRUE;
COMMIT;

-- Heavy-view fixtures (Phase 4 #2) — exercise firebird_profile_table()'s
-- conservative view-shape diagnostics.
--   V_ALL_EMP        : no WHERE, no join, no aggregate  -> "no WHERE" warning
--   V_DEPT_HEADCOUNT : self-JOIN + GROUP BY + COUNT/SUM -> join + aggregation
--                      + no-WHERE warnings
-- The self-join avoids adding a second base table, keeping the relation
-- list (and dbt-sources ordering) stable.
CREATE OR ALTER VIEW V_ALL_EMP (EMP_ID, EMP_NAME) AS
    SELECT EMP_ID, EMP_NAME FROM EMPLOYEE;
COMMIT;

CREATE OR ALTER VIEW V_DEPT_HEADCOUNT (DEPT_NO, HEADCOUNT, TOTAL_SALARY) AS
    SELECT e.DEPT_NO, COUNT(*), SUM(e.SALARY)
      FROM EMPLOYEE e
      JOIN EMPLOYEE m ON m.DEPT_NO = e.DEPT_NO
     GROUP BY e.DEPT_NO;
COMMIT;

-- Literal-noise view — the SELECT carries keyword-shaped text INSIDE a
-- string literal, including an SQL-escaped doubled quote (''). The
-- heavy-view token scan must blank the literal (handling the '') and so
-- must NOT flag JOIN/aggregation, and must NOT treat the literal WHERE as
-- a real filter. There is no real WHERE, so the no-WHERE warning is
-- expected. Guards AnalyzeViewSource() against escaped-quote leakage.
CREATE OR ALTER VIEW V_LITERAL_NOISE (TXT) AS
    SELECT 'fake JOIN, GROUP BY and WHERE inside O''Brien literal'
      FROM EMPLOYEE;
COMMIT;

-- Whitespace-split keyword view — the stored RDB$VIEW_SOURCE keeps the
-- author's formatting, so keywords land split across newlines/tabs
-- (GROUP<newline>BY, an INNER<newline>JOIN, WHERE after a tab). The token
-- scan must collapse whitespace before matching, otherwise it misses the
-- join / aggregation / filter shape. This view has a real WHERE, a real
-- JOIN, and a real GROUP BY, all formatted multi-line.
CREATE OR ALTER VIEW V_MULTILINE_KW (DEPT_NO, N) AS
    SELECT e.DEPT_NO,
           COUNT(*)
      FROM EMPLOYEE e
      INNER
      JOIN EMPLOYEE m
        ON m.DEPT_NO = e.DEPT_NO
     WHERE e.ACTIVE = TRUE
     GROUP
        BY e.DEPT_NO;
COMMIT;

-- Apostrophe fixture — TQUOTES exercises the prepared-statement bind path
-- (firebird_bind_params.test): string filters must parametrise so embedded
-- apostrophes can't break the SQL. Mirrors scripts/smoke_fixture.sql; also
-- counted by firebird_metadata.test (11 tables / 33 columns) and ordered by
-- firebird_dbt_sources.test (DEPT -> EMPLOYEE -> FILE_STORAGE -> TCHILD ->
-- TPK_COMPOSITE -> TQUOTES -> V_ACTIVE_EMP).
CREATE TABLE TQUOTES (
    ID    INTEGER NOT NULL PRIMARY KEY,
    LABEL VARCHAR(60) NOT NULL
);
INSERT INTO TQUOTES VALUES (1, 'D''Agua');
INSERT INTO TQUOTES VALUES (2, 'O''Brien & Co');
COMMIT;

-- Composite-PK fixture — guards firebird_generate_dbt_sources() against
-- emitting "tests: [not_null, unique]" for any single column of a
-- multi-segment PK. The dbt sources walker resolves this table's PK
-- column to NULL, so its YAML block must carry no tests.
CREATE TABLE TPK_COMPOSITE (
    A     INTEGER NOT NULL,
    B     INTEGER NOT NULL,
    LABEL VARCHAR(20),
    CONSTRAINT PK_TPK_COMPOSITE PRIMARY KEY (A, B)
);
COMMIT;

-- Parent table for a single-column FK from EMPLOYEE.DEPT_NO.
CREATE TABLE DEPT (
    DEPT_NO   VARCHAR(3) NOT NULL PRIMARY KEY,
    DEPT_NAME VARCHAR(40)
);
INSERT INTO DEPT VALUES ('600', 'Engineering');
INSERT INTO DEPT VALUES ('700', 'Sales');
INSERT INTO DEPT VALUES ('900', 'Support');
COMMIT;

-- Single-column FK with explicit referential actions.
ALTER TABLE EMPLOYEE
    ADD CONSTRAINT FK_EMP_DEPT FOREIGN KEY (DEPT_NO)
    REFERENCES DEPT (DEPT_NO) ON UPDATE CASCADE ON DELETE SET NULL;
COMMIT;

-- Domain (named field) for firebird_domains coverage.
CREATE DOMAIN D_SALARY AS NUMERIC(10,2) CHECK (VALUE > 0);
COMMIT;

-- Generator with a known initial value for firebird_generators.
CREATE SEQUENCE GEN_EMP_ID START WITH 100;
COMMIT;

-- Computed column for firebird_computed_columns.
ALTER TABLE EMPLOYEE ADD NAME_LEN COMPUTED BY (CHAR_LENGTH(EMP_NAME));
COMMIT;

-- Comments for firebird_comments (TABLE + COLUMN).
COMMENT ON TABLE EMPLOYEE IS 'Staff records fixture';
COMMENT ON COLUMN EMPLOYEE.EMP_NAME IS 'Full name';
COMMIT;

-- Composite FK child: exercises multi-column FK, ordinal position,
-- referenced composite PK, and ON UPDATE/ON DELETE rules.
CREATE TABLE TCHILD (
    PARENT_A INTEGER NOT NULL,
    PARENT_B INTEGER NOT NULL,
    NOTE     VARCHAR(20),
    CONSTRAINT FK_TCHILD_TPK FOREIGN KEY (PARENT_A, PARENT_B)
        REFERENCES TPK_COMPOSITE (A, B) ON UPDATE NO ACTION ON DELETE CASCADE
);
COMMIT;
EOF

# Make the file world-readable so the test harness (running as a different
# user than the firebird daemon) can open it via the embedded engine.
chmod 0666 "$FIREBIRD_TEST_DB"

# Emit the env block GitHub Actions can source via $GITHUB_ENV.
cat <<EOF
FIREBIRD_TEST_DB=$FIREBIRD_TEST_DB
ISC_USER=$ISC_USER
ISC_PASSWORD=$ISC_PASSWORD
EOF
