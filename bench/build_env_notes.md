# v0.6.1 build-environment detection (local Windows dev machine)

Recorded before implementing Fix A, to use the repo's already-supported
build flow rather than inventing one.

## Canonical supported flow (`.github/workflows/build-windows.yml`)

MSVC + Ninja + GNU make (run under git-bash). Steps:

1. MSVC x64 dev environment (`vcvars64.bat` / `msvc-dev-cmd`).
2. Ninja generator (`GEN=ninja`).
3. Firebird Windows SDK → `FB_SDK_ROOT` (needs `include/ibase.h` +
   `lib/fbclient_ms.lib`).
4. `DUCKDB_GIT_VERSION=v1.5.3 make set_duckdb_version`.
5. `EXT_FLAGS="-DFB_SDK_ROOT=<fwd-slash path>" make release` (shell: bash).

The root `Makefile` just includes
`extension-ci-tools/makefiles/duckdb_extension.Makefile`. **GNU make is
required** — no `scripts/build_windows_local.bat` exists.

## Detected on this machine

| Component | Status | Path |
| --- | --- | --- |
| VS Build Tools 2022 (MSVC) | present | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| `vcvars64.bat` | present | `...\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |
| cmake (VS-bundled) | present | `...\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| ninja | present | winget shim (`...\Ninja-build.Ninja_...\ninja.exe`) |
| vcpkg | present | `C:\Users\fernando.souza\vcpkg\vcpkg.exe` |
| Firebird SDK (FB5) | present | `C:\Program Files\Firebird\Firebird_5_0` (`include\ibase.h`, `lib\fbclient_ms.lib` 61744 B) |
| **GNU make** | **MISSING** | — (no `make`, `mingw32-make`, or git-bash make) |
| gcc/g++/clang / MinGW | absent | not used by the supported flow |
| duckdb submodule | at `v1.4.0-8893-g14eca11` | CI pins `v1.5.3` via `make set_duckdb_version` |

## Toolchain decision

**MSVC** — it is the CI-supported primary path on Windows. MinGW is not
required and not installed; do not introduce it.

## Single blocker + caveats

- **Blocker:** install GNU make, then use the repo Makefile flow unchanged.
  (`winget install GnuWin32.Make` or `choco install make`.)
- **Caveat — heavy build:** `make release` compiles **DuckDB from source**
  (submodule) plus the extension. Expect a long first build (tens of minutes)
  and several GB of disk.
- **Caveat — DuckDB version:** submodule is at v1.4.0-dev; `make
  set_duckdb_version` to v1.5.3 will move it (extra fetch/checkout).
- **FB SDK note:** CI uses the FB4 SDK; building against the locally
  installed FB5 `ibase.h` + `fbclient_ms.lib` is expected to work (runtime
  fbclient is dlopen-ed by `firebird_client_loader`), but this is a
  deviation from CI worth noting if anything odd surfaces.
