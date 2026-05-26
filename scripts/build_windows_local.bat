@echo off
REM Local Windows build, equivalent of `make release` from the
REM extension-ci-tools makefile but invoking cmake directly so we don't
REM need GNU make. Run from the repo root.
REM
REM Prereqs (already installed in this workspace):
REM   * CMake + Ninja in PATH
REM   * MSVC v143 reachable via vcvars64.bat
REM   * Firebird 5 install at C:\Program Files\Firebird\Firebird_5_0
REM   * duckdb submodule pinned at v1.5.3 (already done)
REM   * extension-ci-tools submodule initialised

setlocal

REM Locate a usable vcvars64.bat across the four common VS 2022 editions.
REM "BuildTools" is the headless command-line install winget puts in by
REM default; Community/Professional/Enterprise are the full IDE editions.
set "VCVARS="
for %%E in (BuildTools Community Professional Enterprise) do (
    if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"       set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo Could not find vcvars64.bat under any Visual Studio 2022 edition.
    echo Install MSVC v143 via: winget install Microsoft.VisualStudio.2022.BuildTools
    exit /b 1
)
call "%VCVARS%" || exit /b 1

set "PROJ_DIR=%cd%"
set "FB_SDK_ROOT=C:/Program Files/Firebird/Firebird_5_0"
set "EXT_CONFIG=%PROJ_DIR%\extension_config.cmake"

REM Forward-slash variant: CMake handles spaces with double quotes, but
REM EXTENSION_CONFIGS uses ';' as a separator, so the path must be
REM forward-slash. Quote the entire -D token (not just the value) so that
REM project paths containing spaces survive the cmd.exe parser.
set "EXT_CONFIG_FWD=%EXT_CONFIG:\=/%"

if not exist build\release mkdir build\release

cmake -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DEXTENSION_STATIC_BUILD=1 ^
    "-DDUCKDB_EXTENSION_CONFIGS=%EXT_CONFIG_FWD%" ^
    "-DFB_SDK_ROOT=%FB_SDK_ROOT%" ^
    -DDUCKDB_EXPLICIT_PLATFORM=windows_amd64 ^
    -DENABLE_EXTENSION_AUTOLOADING=0 ^
    -DENABLE_EXTENSION_AUTOINSTALL=0 ^
    -DENABLE_UNITTEST_CPP_TESTS=FALSE ^
    -S duckdb ^
    -B build\release || exit /b 1

REM Ninja is a single-config generator; --config Release would be ignored.
cmake --build build\release || exit /b 1

REM Copy the FB5 fbclient.dll next to duckdb.exe so the build dir runs
REM with FB5 client even when System32 has an older FB3 fbclient.dll.
copy /Y "%FB_SDK_ROOT:/=\%\fbclient.dll" build\release\ >nul

echo.
echo --- artifacts ---
dir /b /s build\release\extension\firebird\firebird.duckdb_extension 2>nul
dir /b /s build\release\extension\firebird\*.dll 2>nul
dir /b build\release\duckdb.exe build\release\fbclient.dll 2>nul

endlocal
