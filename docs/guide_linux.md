# duckdb-firebird — Linux quick-start

CI runs this every push: `.github/workflows/build-linux.yml`. This
guide is the local-dev equivalent. Numbers below are from Ubuntu 24.04
LTS, 4 vCPU.

## Step 0 — Requirements

| Component | Tested version | Install |
|---|---|---|
| Firebird server | 5.0.x (3.0 / 4.0 also work) | `apt install firebird5.0-server` (Ubuntu 24+) or [firebirdsql.org tarball](https://firebirdsql.org/en/firebird-5-0/) |
| `libfbclient` + headers | matches server | `apt install firebird-dev` (provides `pkg-config` entry) |
| Build tools | gcc 11+ / clang 14+ | `apt install build-essential cmake ninja-build git` |
| Python 3 | 3.10+ | `apt install python3` (used by the CI test harness) |

For the `firebird5.0-server` package on Ubuntu/Debian, the default
`SYSDBA` password lives in `/etc/firebird/SYSDBA.password` — read it
once at install time and stash it.

## Step 1 — Clone and pin

```bash
git clone https://github.com/flozer/duckdb-firebird.git
cd duckdb-firebird
git submodule update --init --recursive --depth=1
DUCKDB_GIT_VERSION=v1.5.3 make set_duckdb_version
```

## Step 2 — Build

The repo ships [`scripts/build_linux_local.sh`](../scripts/build_linux_local.sh),
which pins DuckDB, checks for `libfbclient` headers, and delegates to
the DuckDB extension-ci-tools Makefile:

```bash
scripts/build_linux_local.sh
```

Expected output (≈8 min on 4 vCPU):

```
[469/469] Linking CXX executable duckdb
```

Artifacts:

- `build/release/duckdb` — DuckDB v1.5.3 with `firebird` statically
  linked in.
- `build/release/extension/firebird/firebird.duckdb_extension` —
  loadable into a stock DuckDB CLI.
- `build/release/repository/v1.5.3/linux_amd64/firebird.duckdb_extension`
  — same loadable, packaged for the upstream extension repository.

If `pkg-config --modversion fbclient` fails, point CMake at the SDK
explicitly:

```bash
scripts/build_linux_local.sh --fb-sdk-root /opt/firebird
```

Useful options:

```bash
scripts/build_linux_local.sh --clean
scripts/build_linux_local.sh --debug
SKIP_SUBMODULES=1 scripts/build_linux_local.sh
SKIP_DUCKDB_PIN=1 scripts/build_linux_local.sh
```

On WSL, builds from `/mnt/c` or `/mnt/d` are much slower than builds
inside the Linux filesystem because the DuckDB submodule contains many
small files. If the script appears to pause at
`cd duckdb && git checkout v1.5.3`, it is usually the DuckDB pin step
touching files on the Windows-mounted drive. After the first successful
pin, use `SKIP_DUCKDB_PIN=1 scripts/build_linux_local.sh` for repeat
builds, or clone the repo under `~/src` for much faster Linux builds.

If you alternate between Windows and WSL builds, the script removes a stale
`build/release/CMakeCache.txt` automatically when it detects that CMake was
configured from the other path style (`D:/...` vs `/mnt/d/...`). Use
`AUTO_CLEAN_CMAKE_CACHE=0` if you want it to fail instead.

To produce a local distribution archive after a release build:

```bash
scripts/package_dist_linux.sh
# optional: bundle the system libfbclient.so too
scripts/package_dist_linux.sh --include-fbclient
```

## Step 3 — Bring up a Firebird 5 fixture

```bash
sudo systemctl start firebird5.0-server
# (Optional) reset SYSDBA to a known password:
sudo gsec -modify SYSDBA -pw masterkey

# Build a sandbox database with the FB4 type fixture:
mkdir -p /tmp/fbtest
isql-fb -u SYSDBA -p masterkey <<'EOF'
CREATE DATABASE '/tmp/fbtest/biz4.fdb' DEFAULT CHARACTER SET UTF8;
EOF
isql-fb -u SYSDBA -p masterkey -i scripts/fixture_biz4.sql /tmp/fbtest/biz4.fdb
```

(On distros that don't rename to `isql-fb`, the binary is just `isql`
— check `which isql` first.)

The repo's CI script `scripts/setup_test_firebird.sh` is the
authoritative reference for the older FB3 fixture; the FB4 fixture
above is the new one used by the Windows guide and by the live
verification report in `docs/test_report.md`.

## Step 4 — Smoke test

```bash
./build/release/duckdb -unsigned <<'EOF'
SELECT * FROM firebird_tables('/tmp/fbtest/biz4.fdb');
SELECT typeof(BIG_NUM), BIG_NUM
  FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES', partitions=1)
 LIMIT 2;
EOF
```

Expected:

```
HUGEINT   170141183460469231731687303715884105727
HUGEINT  -170141183460469231731687303715884105728
```

If you see `VARCHAR` instead, you're running a build older than the
RDB$FIELD_TYPE 24-31 fix — pull, `make clean && make release`.

## Step 5 — Federated queries

```sql
-- Materialize a snapshot:
CREATE TABLE local_fb4 AS
  SELECT * FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES');

-- Export to Parquet:
COPY (SELECT * FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES'))
  TO '/tmp/fbtest/fb4.parquet' (FORMAT 'parquet');

-- Native ATTACH:
ATTACH '/tmp/fbtest/biz4.fdb' AS fb (TYPE firebird,
                                     user     'SYSDBA',
                                     password 'masterkey');
SELECT * FROM fb.main.DEPARTMENT;
DETACH fb;
```

The URL form also works — useful for remote Firebird servers:

```sql
SELECT * FROM firebird_scan(
    'firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb?charset=UTF8',
    'EMPLOYEE');
```

## Loading into a stock DuckDB CLI

```bash
duckdb -unsigned <<'EOF'
LOAD '/path/to/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
SELECT * FROM firebird_tables('/tmp/fbtest/biz4.fdb');
EOF
```

If the stock CLI is older than the v1.5.3 ABI the extension was built
against, the LOAD errors out with a version mismatch — match versions
or use our `build/release/duckdb` instead.

## Run the integration test suite

```bash
GEN=ninja make test_release   # uses build/release/test/unittest
```

The suite expects a reachable Firebird server with the fixtures
described in `scripts/setup_test_firebird.sh`. CI handles all of that
in a container; locally you'll point at your own server via
`FIREBIRD_TEST_DB=/tmp/fbtest/test.fdb` etc.

## Troubleshooting

### `libfbclient.so.2: cannot open shared object file`
The package name varies — try `apt install firebird-dev libfbclient2`
or `apt install firebird5.0-utils`. On RHEL-likes:
`dnf install firebird-libs firebird-devel`.

### `op-system-call-failed isc_attach_database`
The Firebird server isn't running, or the path doesn't exist, or the
user the server runs as can't read the `.fdb`. Check
`systemctl status firebird5.0-server` and `ls -l /tmp/fbtest/biz4.fdb`
(the Firebird daemon UID has to be able to open it).

### `partitions > 1` slower than `partitions=1`
On a single-host SuperServer, the server scheduler serialises queries
against the same file — extra cursors only add overhead. `partitions=`
is opt-in for remote or Classic/SuperClassic deployments where
parallelism is cheap. See `docs/architecture.md` "Why the conservative
default?".
