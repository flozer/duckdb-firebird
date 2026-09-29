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

"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" -i scripts/fixture_common.sql

# Overwrite TBLOB_MULTISEG's placeholder BLOBs with genuinely
# multi-segment content (issue #35) -- see scripts/mkblob_fixture.cpp's
# header comment for why a SQL literal can't do this.
MKBLOB_SRC="$(dirname "$0")/mkblob_fixture.cpp"
MKBLOB_BIN="$(dirname "$FIREBIRD_TEST_DB")/mkblob_fixture"
if command -v g++ >/dev/null 2>&1; then
    FB_INC="${FB_SDK_ROOT:-/usr}/include"
    [ -d "/usr/include/firebird" ] && FB_INC="/usr/include/firebird"
    g++ -std=c++17 -I"$FB_INC" -o "$MKBLOB_BIN" "$MKBLOB_SRC" -lfbclient
elif command -v cl.exe >/dev/null 2>&1 && [ -n "${FB_SDK_ROOT:-}" ]; then
    cl.exe /nologo /EHsc /I"${FB_SDK_ROOT}/include" "$MKBLOB_SRC" \
        /link "/LIBPATH:${FB_SDK_ROOT}/lib" fbclient_ms.lib "/OUT:${MKBLOB_BIN}.exe"
    MKBLOB_BIN="${MKBLOB_BIN}.exe"
else
    echo "mkblob_fixture: no C++ compiler found (need g++ or cl.exe+FB_SDK_ROOT)" >&2
    exit 1
fi
"$MKBLOB_BIN" "$FIREBIRD_TEST_DB" "$ISC_USER" "$ISC_PASSWORD" 1

# Partitions fixture (roadmap F4) -- version-neutral, own database so the
# pinned relation lists of the main fixture stay stable.
PARTITIONS_DB="$(dirname "$FIREBIRD_TEST_DB")/partitions.fdb"
rm -f "$PARTITIONS_DB"
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" <<EOF
CREATE DATABASE '$PARTITIONS_DB' DEFAULT CHARACTER SET UTF8;
EOF
"$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$PARTITIONS_DB" -i scripts/fixture_partitions.sql

# DECFLOAT fixture on Firebird 4+ (roadmap F3): detect the engine version
# of the database we just created and, when >= 4, provision DECVALS in its
# own database. On Firebird 3 nothing is exported, and
# firebird_decfloat.test self-skips via its require-env gate.
FB_ENGINE_VERSION="$(printf "SELECT RDB\$GET_CONTEXT('SYSTEM','ENGINE_VERSION') FROM RDB\$DATABASE;\n" \
    | "$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$FIREBIRD_TEST_DB" 2>/dev/null \
    | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"
FB_MAJOR="${FB_ENGINE_VERSION%%.*}"
DECFLOAT_EXPORT=""
if [[ "$FB_MAJOR" =~ ^[0-9]+$ ]] && [ "$FB_MAJOR" -ge 4 ]; then
    DECFLOAT_DB="$(dirname "$FIREBIRD_TEST_DB")/decfloat.fdb"
    rm -f "$DECFLOAT_DB"
    "$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" <<EOF
CREATE DATABASE '$DECFLOAT_DB' DEFAULT CHARACTER SET UTF8;
EOF
    "$ISQL" -u "$ISC_USER" -p "$ISC_PASSWORD" "$DECFLOAT_DB" -i scripts/fixture_decfloat.sql
    DECFLOAT_EXPORT="FIREBIRD_DECFLOAT_DB=$DECFLOAT_DB"
fi

# Make the file world-readable so the test harness (running as a different
# user than the firebird daemon) can open it via the embedded engine.
chmod 0666 "$FIREBIRD_TEST_DB"

# Emit the env block GitHub Actions can source via $GITHUB_ENV.
cat <<EOF
FIREBIRD_TEST_DB=$FIREBIRD_TEST_DB
FIREBIRD_PARTITIONS_DB=$PARTITIONS_DB
$DECFLOAT_EXPORT
ISC_USER=$ISC_USER
ISC_PASSWORD=$ISC_PASSWORD
EOF
