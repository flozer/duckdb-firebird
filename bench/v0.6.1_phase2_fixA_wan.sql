-- v0.6.1 benchmark — Phase 2: Fix A on WAN (example remote DB, ~144ms RTT)
-- v0.6.0 cold load was projected ~16-21min (deliberately never run). Fix A
-- makes the cold catalog load cheap enough to measure directly.
-- Run: duckdb.exe -unsigned < bench/v0.6.1_phase2_fixA_wan.sql
.timer on
LOAD 'd:/Dados/duckdb-firebird/bench/ci_artifact/firebird.duckdb_extension-windows-x64/firebird.duckdb_extension';

SELECT count(*) AS firebird_tables_count
FROM firebird_tables('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb');

ATTACH 'firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb'
  AS pcp (TYPE firebird);

-- FIRST catalog touch (v0.6.0: projected ~16-21min for 2871 tables)
SELECT count(*) AS ntables_cold FROM duckdb_tables() WHERE database_name = 'pcp';

SELECT count(*) AS ntables_warm FROM duckdb_tables() WHERE database_name = 'pcp';

SELECT count(*) AS ncolumns FROM duckdb_columns() WHERE database_name = 'pcp';
