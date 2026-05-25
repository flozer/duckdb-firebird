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
    chown firebird:firebird "$(dirname "$FIREBIRD_TEST_DB")" || true
fi
chmod 0777 "$(dirname "$FIREBIRD_TEST_DB")"

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

-- View fixture — confirms catalog surfaces RDB$RELATION_TYPE=1 alongside
-- regular tables, and that the binder/scanner can scan a view exactly
-- like a base table.
CREATE OR ALTER VIEW V_ACTIVE_EMP (EMP_ID, EMP_NAME, DEPT_NO) AS
    SELECT EMP_ID, EMP_NAME, DEPT_NO
      FROM EMPLOYEE WHERE ACTIVE = TRUE;
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
