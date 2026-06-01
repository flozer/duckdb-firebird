-- v0.6.1 benchmark — Phase 1b: high-latency probe (example remote DB, ~144ms RTT)
-- Light probe only: get table count + ATTACH cost. Does NOT trigger cold catalog load.
-- Run: duckdb.exe -unsigned < bench/v0.6.1_phase1b_high_latency_probe.sql
.timer on
LOAD 'C:/Users/fernando.souza/Downloads/duckdb-firebird-0.6.0-windows-x64/firebird.duckdb_extension';

-- Step 1: standalone discovery (one query) -> table count N + a real remote round-trip cost
SELECT count(*) AS firebird_tables_count
FROM firebird_tables('firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb');

-- Step 2: ATTACH handshake only (expect cheap even at high latency)
ATTACH 'firebird://APP_READONLY:secret@wan-db.example.com:3050/path/to/database.fdb'
  AS pcp (TYPE firebird);
