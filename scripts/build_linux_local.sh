#!/usr/bin/env bash
# Local Linux release build, equivalent to scripts/build_windows_local.bat.
# Run from anywhere inside the repository.

set -Eeuo pipefail

DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.3}"
BUILD_TYPE="${BUILD_TYPE:-release}"
GENERATOR="${GEN:-ninja}"
SKIP_SUBMODULES="${SKIP_SUBMODULES:-0}"
SKIP_DUCKDB_PIN="${SKIP_DUCKDB_PIN:-0}"
AUTO_CLEAN_CMAKE_CACHE="${AUTO_CLEAN_CMAKE_CACHE:-1}"
DO_CLEAN="${CLEAN:-0}"
FB_SDK_ROOT="${FB_SDK_ROOT:-}"

usage() {
    cat <<'EOF'
Usage: scripts/build_linux_local.sh [options]

Options:
  --debug              Build debug instead of release
  --clean              Remove build/<type> before building
  --skip-submodules    Do not run git submodule update
  --skip-duckdb-pin    Do not run make set_duckdb_version
  --fb-sdk-root PATH   Pass -DFB_SDK_ROOT=PATH to CMake
  -h, --help           Show this help

Environment:
  DUCKDB_VERSION=v1.5.3  DuckDB tag/commit to pin before build
  GEN=ninja              Generator used by the DuckDB extension Makefile
  SKIP_DUCKDB_PIN=1      Same as --skip-duckdb-pin
  AUTO_CLEAN_CMAKE_CACHE=0  Do not auto-remove mismatched CMake cache
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --debug) BUILD_TYPE="debug" ;;
        --clean) DO_CLEAN="1" ;;
        --skip-submodules) SKIP_SUBMODULES="1" ;;
        --skip-duckdb-pin) SKIP_DUCKDB_PIN="1" ;;
        --fb-sdk-root)
            shift
            FB_SDK_ROOT="${1:-}"
            if [ -z "$FB_SDK_ROOT" ]; then
                echo "ERROR: --fb-sdk-root requires a path" >&2
                exit 2
            fi
            ;;
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

for tool in git cmake python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    fi
done

if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: no C++ compiler found.

Install the Linux build toolchain, for example:
  sudo apt-get update
  sudo apt-get install -y build-essential g++ ninja-build cmake pkg-config python3
EOF
    exit 1
fi

if [ "$GENERATOR" = "ninja" ] && ! command -v ninja >/dev/null 2>&1; then
    echo "ERROR: ninja not found. Install ninja-build or set GEN to another generator." >&2
    exit 1
fi

if [ -z "$FB_SDK_ROOT" ]; then
    HAVE_FBCLIENT=0
    if command -v pkg-config >/dev/null 2>&1; then
        if pkg-config --exists fbclient 2>/dev/null || pkg-config --exists firebird 2>/dev/null; then
            HAVE_FBCLIENT=1
        fi
    fi
    if [ "$HAVE_FBCLIENT" = "0" ] && command -v ldconfig >/dev/null 2>&1; then
        if ldconfig -p 2>/dev/null | grep -q 'libfbclient\.so'; then
            HAVE_FBCLIENT=1
        fi
    fi
    if [ "$HAVE_FBCLIENT" = "0" ]; then
        for header in /usr/include/firebird/ibase.h /usr/include/ibase.h /usr/local/include/firebird/ibase.h /usr/local/include/ibase.h; do
            if [ -f "$header" ]; then
                HAVE_FBCLIENT=1
                break
            fi
        done
    fi

    if [ "$HAVE_FBCLIENT" = "0" ]; then
        cat >&2 <<'EOF'
ERROR: Firebird client development files were not found.

Install them, for example:
  sudo apt-get update
  sudo apt-get install -y firebird-dev libfbclient2

On Ubuntu/WSL, firebird-dev may not provide a fbclient pkg-config file.
That is OK: this script also accepts /usr/include/firebird/ibase.h plus
libfbclient.so in the system linker cache.

Or pass:
  scripts/build_linux_local.sh --fb-sdk-root /opt/firebird
EOF
        exit 1
    fi
fi

if [ "$SKIP_SUBMODULES" != "1" ]; then
    git submodule update --init --recursive --depth=1
fi

if [ "$SKIP_DUCKDB_PIN" = "1" ]; then
    echo "==> Skipping DuckDB pin (SKIP_DUCKDB_PIN=1)"
else
    CURRENT_DUCKDB_TAG=""
    if [ -d duckdb/.git ] || [ -f duckdb/.git ]; then
        CURRENT_DUCKDB_TAG="$(git -C duckdb describe --tags --exact-match HEAD 2>/dev/null || true)"
    fi
    if [ "$CURRENT_DUCKDB_TAG" = "$DUCKDB_VERSION" ]; then
        echo "==> DuckDB already pinned at $DUCKDB_VERSION"
    else
        case "$REPO_ROOT" in
            /mnt/*)
                echo "==> Pinning DuckDB to $DUCKDB_VERSION"
                echo "    Note: under /mnt/* on WSL this git checkout can take several minutes."
                ;;
            *)
                echo "==> Pinning DuckDB to $DUCKDB_VERSION"
                ;;
        esac
        DUCKDB_GIT_VERSION="$DUCKDB_VERSION" make set_duckdb_version
    fi
fi

if [ "$DO_CLEAN" = "1" ]; then
    rm -rf "build/$BUILD_TYPE"
fi

CMAKE_CACHE="build/$BUILD_TYPE/CMakeCache.txt"
if [ -f "$CMAKE_CACHE" ]; then
    CACHE_SOURCE_DIR="$(awk -F= '/^CMAKE_HOME_DIRECTORY:INTERNAL=/ {print $2; exit}' "$CMAKE_CACHE" || true)"
    EXPECTED_SOURCE_DIR="$REPO_ROOT/duckdb"
    if [ -n "$CACHE_SOURCE_DIR" ] && [ "$CACHE_SOURCE_DIR" != "$EXPECTED_SOURCE_DIR" ]; then
        if [ "$AUTO_CLEAN_CMAKE_CACHE" = "1" ]; then
            echo "==> Removing stale CMake cache from a different source path"
            echo "    cache:    $CACHE_SOURCE_DIR"
            echo "    expected: $EXPECTED_SOURCE_DIR"
            rm -rf "build/$BUILD_TYPE"
        else
            cat >&2 <<EOF
ERROR: build/$BUILD_TYPE was configured from a different source path.

Cache source:    $CACHE_SOURCE_DIR
Expected source: $EXPECTED_SOURCE_DIR

Run:
  scripts/build_linux_local.sh --clean
EOF
            exit 1
        fi
    fi
fi

EXT_FLAGS_VALUE="${EXT_FLAGS:-}"
if [ -n "$FB_SDK_ROOT" ]; then
    EXT_FLAGS_VALUE="${EXT_FLAGS_VALUE:+$EXT_FLAGS_VALUE }-DFB_SDK_ROOT=$FB_SDK_ROOT"
fi

echo "==> Building duckdb-firebird"
echo "    build type:      $BUILD_TYPE"
echo "    DuckDB version:  $DUCKDB_VERSION"
echo "    generator:       $GENERATOR"
if [ -n "$EXT_FLAGS_VALUE" ]; then
    echo "    EXT_FLAGS:       $EXT_FLAGS_VALUE"
fi

if [ "$BUILD_TYPE" = "debug" ]; then
    GEN="$GENERATOR" EXT_FLAGS="$EXT_FLAGS_VALUE" make debug
else
    GEN="$GENERATOR" EXT_FLAGS="$EXT_FLAGS_VALUE" make release
fi

EXT_PATH="build/$BUILD_TYPE/extension/firebird/firebird.duckdb_extension"
DUCKDB_PATH="build/$BUILD_TYPE/duckdb"

echo
echo "--- artifacts ---"
if [ -f "$DUCKDB_PATH" ]; then
    ls -lh "$DUCKDB_PATH"
fi
if [ -f "$EXT_PATH" ]; then
    ls -lh "$EXT_PATH"
else
    echo "ERROR: extension artifact not found: $EXT_PATH" >&2
    exit 1
fi
