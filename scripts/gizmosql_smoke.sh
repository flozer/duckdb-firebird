#!/usr/bin/env bash
# End-to-end smoke test for the firebird extension under GizmoSQL.
#
# Brings up:
#   - A Firebird 5 container with the biz4 fixture (INT128 etc.).
#   - A GizmoSQL container with the firebird extension bind-mounted
#     and an init.sql that LOADs the extension + ATTACHes biz4.fdb.
# Then runs a Python ADBC Flight SQL client that issues a DAX-shaped
# query, asserts the row count, and exits non-zero on mismatch.
#
# Requires:
#   - docker
#   - python3 with adbc-driver-flightsql installed
#   - The pre-built firebird.duckdb_extension at
#     build/release/extension/firebird/firebird.duckdb_extension
#   - A pre-populated DuckDB extension cache at ~/.duckdb (run
#     gizmosql_aircache.sh first).

set -euo pipefail

EXT_PATH="${EXT_PATH:-$(pwd)/build/release/extension/firebird/firebird.duckdb_extension}"
GIZMO_TAG="${GIZMO_TAG:-1.26.2}"
FB_TAG="${FB_TAG:-5-noble}"
GIZMO_PORT="${GIZMO_PORT:-31337}"
GIZMO_PWD="${GIZMO_PWD:-strong-pw}"

if [[ ! -s "$EXT_PATH" ]]; then
    echo "ERROR: extension not found at $EXT_PATH"
    echo "       run 'make release' or scripts/build_windows_local.bat first."
    exit 1
fi

cleanup() {
    docker rm -f fbsmoke gizmosmoke >/dev/null 2>&1 || true
}
trap cleanup EXIT

cleanup

# 1) Firebird 5 server.
docker run -d --name fbsmoke \
    -p 3050:3050 \
    -e FIREBIRD_ROOT_PASSWORD=masterkey \
    -e FIREBIRD_USE_LEGACY_AUTH=true \
    "firebirdsql/firebird:${FB_TAG}"

for _ in $(seq 1 30); do
    if nc -z localhost 3050; then break; fi
    sleep 1
done

docker exec -i fbsmoke bash -c "isql -u SYSDBA -p masterkey" <<'SQL'
CREATE DATABASE '/var/lib/firebird/data/biz4.fdb' DEFAULT CHARACTER SET UTF8;
SQL

docker cp scripts/fixture_biz4.sql fbsmoke:/tmp/fixture_biz4.sql
docker exec fbsmoke isql -u SYSDBA -p masterkey \
    -i /tmp/fixture_biz4.sql /var/lib/firebird/data/biz4.fdb

# 2) GizmoSQL with the extension bind-mounted.
INIT_SQL=$(mktemp)
cat > "$INIT_SQL" <<EOF
SET allow_unsigned_extensions = true;
LOAD '/extensions/firebird.duckdb_extension';
ATTACH 'firebird://SYSDBA:masterkey@host.docker.internal:3050//var/lib/firebird/data/biz4.fdb?charset=UTF8'
    AS fb (TYPE firebird);
EOF

docker run -d --name gizmosmoke \
    --add-host=host.docker.internal:host-gateway \
    -p "${GIZMO_PORT}:${GIZMO_PORT}" \
    -e GIZMOSQL_PASSWORD="$GIZMO_PWD" \
    -v "$EXT_PATH:/extensions/firebird.duckdb_extension:ro" \
    -v "${HOME}/.duckdb:/root/.duckdb:ro" \
    -v "$INIT_SQL:/init.sql:ro" \
    "gizmodata/gizmosql:${GIZMO_TAG}" \
    gizmosql_server --port "${GIZMO_PORT}" \
                    --init-sql-commands-file /init.sql

# Wait for Flight SQL port.
for _ in $(seq 1 30); do
    if nc -z localhost "${GIZMO_PORT}"; then break; fi
    sleep 1
done
docker logs gizmosmoke 2>&1 | tail -20

# 3) Hit it from Python ADBC.
python3 - <<PY
import sys
import adbc_driver_flightsql.dbapi as flightsql

con = flightsql.connect(
    "grpc+tcp://localhost:${GIZMO_PORT}",
    db_kwargs={"username": "gizmosql_user", "password": "${GIZMO_PWD}"})
cur = con.cursor()
cur.execute("SELECT COUNT(*) FROM fb.main.FB4_TYPES")
(n,) = cur.fetchone()
print(f"FB4_TYPES rows via Flight SQL: {n}")
if n != 4:
    print("ERROR: expected 4 rows, got", n)
    sys.exit(1)

cur.execute("SELECT typeof(BIG_NUM) FROM fb.main.FB4_TYPES LIMIT 1")
(t,) = cur.fetchone()
print(f"BIG_NUM dtype: {t}")
if t.upper() != "HUGEINT":
    print("ERROR: expected HUGEINT, got", t)
    sys.exit(1)

print("OK")
PY
