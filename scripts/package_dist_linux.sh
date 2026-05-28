#!/usr/bin/env bash
# Package a Linux x64 build for local distribution.
#
# Produces dist/duckdb-firebird-<version>-linux-x64/ plus a .tar.gz archive.
# Run scripts/build_linux_local.sh first.

set -Eeuo pipefail

INCLUDE_FBCLIENT="${INCLUDE_FBCLIENT:-0}"

usage() {
    cat <<'EOF'
Usage: scripts/package_dist_linux.sh [options]

Options:
  --include-fbclient   Also copy the system libfbclient.so into the package
  -h, --help           Show this help

By default the archive does not vendor libfbclient. Install the Firebird
client runtime on the target machine instead, for example:
  sudo apt-get install -y libfbclient2
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --include-fbclient) INCLUDE_FBCLIENT="1" ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

EXT="build/release/extension/firebird/firebird.duckdb_extension"
if [ ! -f "$EXT" ]; then
    echo "ERROR: extension not found at $EXT" >&2
    echo "Run scripts/build_linux_local.sh first." >&2
    exit 1
fi

# Version source priority:
#   1. GITHUB_REF_NAME when it looks like a tag (`v<semver>`) - the
#      release-assets.yml workflow runs on tag push, so this is the
#      authoritative source for published artifacts.
#   2. community-extensions/description.yml - the developer-edited
#      version field. Useful for local builds outside CI.
#   3. "unknown" - last resort so the archive still gets produced.
VERSION=""
if [ -n "${GITHUB_REF_NAME:-}" ]; then
    case "$GITHUB_REF_NAME" in
        v*) VERSION="${GITHUB_REF_NAME#v}" ;;
    esac
fi
if [ -z "$VERSION" ]; then
    VERSION="$(awk -F: '/^[[:space:]]+version:/ {gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}' community-extensions/description.yml)"
fi
if [ -z "$VERSION" ]; then
    VERSION="unknown"
fi

STAGE="dist/duckdb-firebird-$VERSION-linux-x64"
ARCHIVE="$STAGE.tar.gz"

rm -rf "$STAGE" "$ARCHIVE"
mkdir -p "$STAGE"

cp "$EXT" "$STAGE/firebird.duckdb_extension"

for fixture in scripts/fixture_create.sql scripts/fixture_biz4.sql scripts/fixture_none_charset.sql; do
    if [ -f "$fixture" ]; then
        cp "$fixture" "$STAGE/"
    fi
done

if [ "$INCLUDE_FBCLIENT" = "1" ]; then
    FBCLIENT_PATH=""
    if command -v ldconfig >/dev/null 2>&1; then
        FBCLIENT_PATH="$(ldconfig -p | awk '/libfbclient\.so/ {print $NF; exit}')"
    fi
    if [ -z "$FBCLIENT_PATH" ] && command -v pkg-config >/dev/null 2>&1; then
        for pc_name in fbclient firebird; do
            if pkg-config --exists "$pc_name" 2>/dev/null; then
                LIBDIR="$(pkg-config --variable=libdir "$pc_name" 2>/dev/null || true)"
                if [ -n "$LIBDIR" ]; then
                    FBCLIENT_PATH="$(find "$LIBDIR" -maxdepth 1 -name 'libfbclient.so*' -type f | sort | tail -n 1)"
                    if [ -n "$FBCLIENT_PATH" ]; then
                        break
                    fi
                fi
            fi
        done
    fi
    if [ -z "$FBCLIENT_PATH" ] || [ ! -f "$FBCLIENT_PATH" ]; then
        echo "ERROR: --include-fbclient requested but libfbclient.so was not found" >&2
        exit 1
    fi
    cp "$FBCLIENT_PATH" "$STAGE/"
fi

cat > "$STAGE/README.txt" <<EOF
duckdb-firebird $VERSION - Linux x64

Files in this archive:
  firebird.duckdb_extension  - the DuckDB extension binary
  fixture_create.sql         - optional sample database scaffold
  fixture_biz4.sql           - optional tables exercising FB4 types
  fixture_none_charset.sql   - optional CHARACTER SET NONE fixture

Requirements:
  1. DuckDB CLI v1.5.3 or another ABI-compatible v1.5.x build
  2. Firebird client runtime installed on the target machine

Install the Firebird client runtime on Debian/Ubuntu with:
  sudo apt-get install -y libfbclient2

Quick start:
  duckdb -unsigned
  LOAD '/path/to/firebird.duckdb_extension';
  SELECT * FROM firebird_tables('firebird://SYSDBA:masterkey@host:3050/path/db.fdb');

The -unsigned flag is required because this local archive is not signed
by the DuckDB extension authority. Once duckdb-firebird is published via
community-extensions, use:

  INSTALL firebird FROM community;
  LOAD firebird;

More: https://github.com/flozer/duckdb-firebird/tree/main/docs
EOF

tar -czf "$ARCHIVE" -C "$(dirname "$STAGE")" "$(basename "$STAGE")"

echo
echo "--- packaged ---"
echo "Stage dir: $STAGE"
echo "Archive:   $ARCHIVE"
ls -lh "$ARCHIVE"
echo
echo "SHA-256:"
sha256sum "$ARCHIVE"
