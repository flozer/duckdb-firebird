-- v0.6.1 benchmark — Phase 1c: isolated round-trip calibration (example remote DB, ~144ms RTT)
-- Goal: cost per minimal remote query/round-trip, to validate cold-load projection
--       WITHOUT running the ~20min full cold catalog load.
-- Note: standalone firebird_scan() RECONNECTS each call (no pool) -> these timings
--       are an UPPER bound vs EnsureTablesLoaded's warm pooled per-table loop.
-- Run: duckdb.exe -unsigned < bench/v0.6.1_phase1c_rtt_calib.sql
.timer on
LOAD 'C:/Users/fernando.souza/Downloads/duckdb-firebird-0.6.0-windows-x64/firebird.duckdb_extension';

-- A) minimal 1-row system table, 5 reps -> connect + tiny query cost + variance
SELECT count(*) AS r1 FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$DATABASE');
SELECT count(*) AS r2 FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$DATABASE');
SELECT count(*) AS r3 FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$DATABASE');
SELECT count(*) AS r4 FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$DATABASE');
SELECT count(*) AS r5 FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$DATABASE');

-- B) volume fetch: ~2871 rows in one query -> batched-fetch cost (contrast with per-table loop)
SELECT count(*) AS relcount FROM firebird_scan('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb', 'RDB$RELATIONS');
