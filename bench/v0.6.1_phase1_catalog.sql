-- v0.6.1 benchmark — Phase 1: catalog-load hypothesis
-- Run: duckdb.exe -unsigned < bench/v0.6.1_phase1_catalog.sql
.timer on
LOAD 'C:/Users/fernando.souza/Downloads/duckdb-firebird-0.6.0-windows-x64/firebird.duckdb_extension';

-- Step 1: standalone discovery function (own connection + full table scan)
SELECT count(*) AS firebird_tables_count
FROM firebird_tables('firebird://APP_READONLY:secret@lan-db.example.com:3050/path/to/database.fdb');

-- Step 2: ATTACH handshake only (expect cheap; should NOT load all columns yet)
ATTACH 'firebird://APP_READONLY:secret@lan-db.example.com:3050/path/to/database.fdb'
  AS fb (TYPE firebird);

-- Step 3: FIRST catalog touch -> triggers EnsureTablesLoaded (expect the spike)
SELECT count(*) AS ntables_cold FROM duckdb_tables() WHERE database_name = 'fb';

-- Step 4: SECOND catalog touch -> catalog warm (expect fast)
SELECT count(*) AS ntables_warm FROM duckdb_tables() WHERE database_name = 'fb';

-- Step 5: how many columns total were materialised (context for N round-trips)
SELECT count(*) AS ncolumns FROM duckdb_columns() WHERE database_name = 'fb';
