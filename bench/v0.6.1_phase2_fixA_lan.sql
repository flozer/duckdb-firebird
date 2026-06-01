-- v0.6.1 benchmark — Phase 2: Fix A (CI-built artifact) vs v0.6.0 baseline, LAN
-- Same steps as bench/v0.6.1_phase1_catalog.sql for a direct before/after.
-- Run: duckdb.exe -unsigned < bench/v0.6.1_phase2_fixA_lan.sql
.timer on
LOAD 'd:/Dados/duckdb-firebird/bench/ci_artifact/firebird.duckdb_extension-windows-x64/firebird.duckdb_extension';

-- Step 1: standalone discovery function
SELECT count(*) AS firebird_tables_count
FROM firebird_tables('firebird://APP_READONLY:secret@lan-db.example.com:3050/path/to/database.fdb');

-- Step 2: ATTACH handshake
ATTACH 'firebird://APP_READONLY:secret@lan-db.example.com:3050/path/to/database.fdb'
  AS fb (TYPE firebird);

-- Step 3: FIRST catalog touch -> EnsureTablesLoaded (was 25.26s on v0.6.0)
SELECT count(*) AS ntables_cold FROM duckdb_tables() WHERE database_name = 'fb';

-- Step 4: SECOND catalog touch (warm)
SELECT count(*) AS ntables_warm FROM duckdb_tables() WHERE database_name = 'fb';

-- Step 5: total columns materialised (must match baseline 46030)
SELECT count(*) AS ncolumns FROM duckdb_columns() WHERE database_name = 'fb';
