#!/usr/bin/env bash
# Pre-populate the DuckDB extension cache under ~/.duckdb/extensions/
# for environments that can't reach https://extensions.duckdb.org at
# runtime. GizmoSQL's startup runs `INSTALL icu; INSTALL spatial;`
# before any user `init.sql` and will fail closed on air-gapped hosts
# without these files in place.
#
# Run once on a network-connected machine, then bind-mount or copy
# ~/.duckdb/extensions/ to the air-gapped target.
#
# Usage:
#   ./scripts/gizmosql_aircache.sh                # default: v1.5.3, linux_amd64
#   DUCKDB_VERSION=v1.5.3 PLATFORM=linux_amd64 ./scripts/gizmosql_aircache.sh

set -euo pipefail

: "${DUCKDB_VERSION:=v1.5.3}"
: "${PLATFORM:=linux_amd64}"

CACHE_DIR="${HOME}/.duckdb/extensions/${DUCKDB_VERSION}/${PLATFORM}"
BASE_URL="https://extensions.duckdb.org/${DUCKDB_VERSION}/${PLATFORM}"

EXTENSIONS=(icu spatial)

mkdir -p "$CACHE_DIR"

for ext in "${EXTENSIONS[@]}"; do
    url="${BASE_URL}/${ext}.duckdb_extension.gz"
    out="${CACHE_DIR}/${ext}.duckdb_extension"
    if [[ -s "$out" ]]; then
        echo "[ok]    ${ext} already cached at ${out}"
        continue
    fi
    echo "[fetch] ${url} -> ${out}"
    curl -fsSL --retry 3 --retry-delay 2 "$url" | gunzip > "$out"
    chmod 0644 "$out"
done

echo
echo "Cache ready at ${CACHE_DIR}:"
ls -lh "$CACHE_DIR"
echo
echo "To use this cache from GizmoSQL, mount it into the container:"
echo "  docker run -v ${HOME}/.duckdb:/root/.duckdb gizmodata/gizmosql:1.26.2 ..."
