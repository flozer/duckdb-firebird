# duckdb-firebird — Windows quick-start

Verified on Windows 11 Pro 26200 + Firebird 5.0.4 SuperServer + DuckDB
v1.5.3 (built from source) + MSVC 19.44 (Visual Studio 2022 Build
Tools). Times below are from a fresh setup on a modern laptop.

## Step 0 — Requirements

| Component | Tested version | Where to get it |
|---|---|---|
| Firebird server | 5.0.4 (FB3 / FB4 also work) | <https://firebirdsql.org/en/firebird-5-0/> |
| Visual Studio 2022 Build Tools (Workload: "Desktop development with C++") | MSVC 19.44 | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| CMake | 4.x | `winget install Kitware.CMake` |
| Ninja | 1.13+ | `winget install Ninja-build.Ninja` |
| Git | 2.40+ | `winget install Git.Git` |
| DuckDB CLI (optional, can use the one we build) | 1.5.x | `winget install DuckDB.cli` |

Firebird 5 install ships **both** the runtime (`fbclient.dll`, server
service) **and** the SDK (headers under `include\`, import library
`lib\fbclient_ms.lib`), so there's no separate "client SDK" download.
Default install path: `C:\Program Files\Firebird\Firebird_5_0`.

## Step 1 — Clone and pin

```powershell
git clone https://github.com/flozer/duckdb-firebird.git
cd duckdb-firebird

# Pull the duckdb + extension-ci-tools submodules (shallow keeps it fast).
git submodule update --init --recursive --depth=1

# Pin DuckDB to v1.5.3 — matches our extension ABI.
cd duckdb
git fetch --tags --depth=1 origin v1.5.3
git checkout v1.5.3
cd ..
```

## Step 2 — Build

The repo ships [`scripts/build_windows_local.bat`](../scripts/build_windows_local.bat),
which loads `vcvars64.bat`, points CMake at the FB5 SDK paths, and
calls Ninja. From a regular `cmd.exe` (or PowerShell that lets you run
.bat):

```cmd
cd D:\path\to\duckdb-firebird
scripts\build_windows_local.bat
```

Expected output (≈10 min on a 4-core laptop):

```
[1/2] Building CXX object extension\firebird\CMakeFiles\firebird_loadable_extension.dir\src\firebird_scanner.cpp.obj
[2/2] Linking CXX shared library extension\firebird\firebird.duckdb_extension
…
--- artifacts ---
D:\path\to\duckdb-firebird\build\release\extension\firebird\firebird.duckdb_extension
```

Three things land under `build\release\`:

- `duckdb.exe` — a DuckDB v1.5.3 build with the firebird extension
  **statically linked in** (no `LOAD` needed). This is the
  authoritative binary for testing.
- `extension\firebird\firebird.duckdb_extension` — the loadable
  variant. Use this to `LOAD` into a stock DuckDB v1.5.3 CLI.
- `repository\v1.5.3\windows_amd64\firebird.duckdb_extension` — the
  same loadable, laid out like the upstream extension repository.

## Step 3 — Make sure the right `fbclient.dll` loads

This bit catches people. Windows ships a system `C:\Windows\System32\
FBCLIENT.DLL` on machines that have any Firebird version installed.
That copy is whatever the *first* installer wrote — which on a
FB3+FB5 machine is the FB3 client, **not** FB5. A FB3 client cannot
decode FB4+ types (INT128, DECFLOAT, TIMESTAMP_TZ), and the symptom is
`isc_dsql_prepare: Data type unknown [sqlcode -204]`.

Two reliable fixes:

```powershell
# Either copy the FB5 client next to duckdb.exe:
copy "C:\Program Files\Firebird\Firebird_5_0\fbclient.dll" build\release\

# OR prepend Firebird_5_0 to PATH for the current shell:
$env:Path = 'C:\Program Files\Firebird\Firebird_5_0;' + $env:Path
```

`build_windows_local.bat` already copies the FB5 DLL into
`build\release\` as part of the post-build step.

## Step 4 — Create a sandbox database

```sql
-- Save as C:\fbtest\create.sql then run:
--   isql -i C:\fbtest\create.sql
SET SQL DIALECT 3;
CREATE DATABASE 'C:\fbtest\biz4.fdb'
    USER 'SYSDBA' PASSWORD 'masterkey'
    DEFAULT CHARACTER SET UTF8;
QUIT;
```

```cmd
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -i C:\fbtest\create.sql
```

A canned fixture with FB4 types is at
[`scripts/fixture_biz4.sql`](../scripts/fixture_biz4.sql) — apply it
with:

```cmd
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" ^
    -user SYSDBA -password masterkey ^
    -i scripts\fixture_biz4.sql C:\fbtest\biz4.fdb
```

## Step 5 — Smoke test

From `build\release\`:

```sql
-- The static build already has firebird registered; no LOAD needed.
SELECT * FROM firebird_tables('C:/fbtest/biz4.fdb');

-- Type mapping live check:
SELECT typeof(BIG_NUM), typeof(BIG_DEC), typeof(TS_TZ)
  FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES', partitions=1)
 LIMIT 1;
-- HUGEINT | DECIMAL(38,5) | TIMESTAMP WITH TIME ZONE
```

### Federated workflows

```sql
-- Materialize into DuckDB native storage:
CREATE TABLE local_fb4 AS
  SELECT * FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES');

-- Export to Parquet (HUGEINT degrades to DOUBLE in Parquet output —
-- DECIMAL(38, s) stays exact):
COPY (SELECT * FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES'))
  TO 'C:/fbtest/fb4.parquet' (FORMAT 'parquet');

-- Firebird ⋈ Parquet:
SELECT  f.ID, f.LABEL, p.BIG_DEC
  FROM  firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES') f
  JOIN  read_parquet('C:/fbtest/fb4.parquet') p ON f.ID = p.ID;

-- Native ATTACH — exposes the whole database as a DuckDB catalog:
ATTACH 'C:/fbtest/biz4.fdb' AS fb (TYPE firebird,
                                   user     'SYSDBA',
                                   password 'masterkey');
SELECT * FROM fb.main.DEPARTMENT;
DETACH fb;
```

## Step 6 — Loading the extension into a stock DuckDB CLI

If you already have a separately installed DuckDB v1.5.3 CLI, you can
load the `.duckdb_extension` we just built without rebuilding the CLI:

```cmd
duckdb -unsigned
```

```sql
LOAD 'D:/path/to/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
SELECT * FROM firebird_tables('C:/fbtest/biz4.fdb');
```

The `-unsigned` flag (or `SET allow_unsigned_extensions=true;` in
`duckdbrc`) is required because the extension binary isn't signed by
the DuckDB extension authority. Once we publish via
`community-extensions`, this step goes away.

## Troubleshooting

### `Data type unknown [sqlcode -204]` on every prepare
Wrong `fbclient.dll`. See Step 3.

### `isc_attach_database: unavailable database`
URL parser sends the path verbatim to libfbclient. On Windows, use
a **bare path** (`C:/fbtest/biz4.fdb`) or the key=value form
(`database=C:\fbtest\biz4.fdb;user=SYSDBA;password=masterkey`).
The `firebird://USER:PASS@HOST/PATH` URL form is intended for
remote servers; pointing it at a Windows local-disk path produces
the wrong libfbclient string (`HOST:/C:/fbtest/biz4.fdb`).

### `INTERNAL Error: Expected vector of type INT128, but found vector of type VARCHAR`
You built earlier than the v0.4 RDB$FIELD_TYPE fix (commit
introducing case 24..31 in `LoadTableSchema`). Pull, rebuild *both*
the static (`firebird_extension.lib`) and loadable
(`firebird.duckdb_extension`) variants, and re-run `duckdb.exe`.

### `loaded=true installed=true` but my LOAD doesn't take effect
Our `build\release\duckdb.exe` ships the extension statically linked.
`LOAD` is a no-op there (the function table is already populated at
process start). Either rebuild after editing the extension, or test
against a stock DuckDB CLI that loads the `.duckdb_extension` from
disk.

### Anti-virus quarantine on `firebird.duckdb_extension`
Some AV vendors flag fresh, unsigned `.dll`-equivalents. Whitelist
`build\release\extension\firebird\` and the FB5 install dir.
