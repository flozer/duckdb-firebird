duckdb-firebird @@VERSION@@ - Windows x64

Files in this archive:
  firebird.duckdb_extension  - the DuckDB extension binary
  fbclient.dll               - Firebird 5 client library
  fixture_create.sql         - optional: creates an empty sample DB
  fixture_biz4.sql           - optional: tables exercising FB4 types

Requirements:
  1. DuckDB CLI v1.5.3 (other v1.5.x may work; ABI is v1.5.x)
  2. Firebird server reachable, or a local .fdb file

If Firebird is not installed locally, drop fbclient.dll next to
your duckdb.exe (same directory). Without it, Windows pulls
the system FBCLIENT.DLL which on multi-Firebird machines is
usually the oldest installed major and breaks INT128 / TZ types.

Quick start:

  duckdb.exe -unsigned

  > LOAD 'C:/path/to/firebird.duckdb_extension';
  > SELECT * FROM firebird_tables('firebird://SYSDBA:masterkey@host/path/db.fdb');
  > ATTACH 'firebird://SYSDBA:masterkey@host/path/db.fdb' AS fb (TYPE firebird);
  > SELECT * FROM fb.main.YOUR_TABLE;

The -unsigned flag is required because this build is not signed
by the DuckDB extension authority. Once duckdb-firebird is
published via community-extensions, INSTALL firebird FROM
community; LOAD firebird; works without -unsigned.

More: https://github.com/flozer/duckdb-firebird/tree/main/docs
