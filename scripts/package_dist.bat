@echo off
REM Package the built firebird.duckdb_extension for distribution.
REM
REM Produces dist\duckdb-firebird-<version>-windows-x64\ with:
REM   firebird.duckdb_extension    (the extension binary, statically linked)
REM   fbclient.dll                 (Firebird 5 client lib - used at runtime)
REM   README.txt                   (how to load + caveats)
REM   fixture_create.sql           (optional sample database scaffold)
REM   fixture_biz4.sql             (optional sample tables incl. FB4 types)
REM
REM Plus a duckdb-firebird-<version>-windows-x64.zip alongside it.
REM
REM Pre-req: scripts\build_windows_local.bat already ran and produced
REM build\release\extension\firebird\firebird.duckdb_extension + fbclient.dll.

setlocal enabledelayedexpansion

set "PROJ_DIR=%cd%"
set "EXT=%PROJ_DIR%\build\release\extension\firebird\firebird.duckdb_extension"
set "FBCLIENT=%PROJ_DIR%\build\release\fbclient.dll"

if not exist "%EXT%" (
    echo ERROR: extension not found at %EXT%
    echo Run scripts\build_windows_local.bat first.
    exit /b 1
)
if not exist "%FBCLIENT%" (
    echo ERROR: fbclient.dll not found at %FBCLIENT%
    echo build_windows_local.bat copies it automatically; re-run that script.
    exit /b 1
)

REM Version source priority:
REM   1. RELEASE_VERSION when set -- release-assets.yml exports this
REM      explicitly (derived from the release tag, whether the run was
REM      triggered by a tag push or a manual workflow_dispatch against a
REM      branch). Authoritative whenever set; immune to GITHUB_REF_NAME
REM      reflecting the dispatch ref ("main") instead of the release tag
REM      on a manual re-run.
REM   2. GITHUB_REF_NAME when it looks like a tag (v<semver>) -- true on
REM      a real tag-push trigger even if RELEASE_VERSION wasn't set.
REM   3. community-extensions/description.yml -- local dev fallback.
REM   4. "unknown" -- last resort.
set "VERSION="
if defined RELEASE_VERSION (
    if "%RELEASE_VERSION:~0,1%"=="v" (
        set "VERSION=%RELEASE_VERSION:~1%"
    ) else (
        set "VERSION=%RELEASE_VERSION%"
    )
)
if "%VERSION%"=="" if defined GITHUB_REF_NAME (
    if "%GITHUB_REF_NAME:~0,1%"=="v" set "VERSION=%GITHUB_REF_NAME:~1%"
)
if "%VERSION%"=="" for /f "delims=" %%v in ('powershell -NoProfile -Command "((Select-String -Path community-extensions/description.yml -Pattern '^^  version:').Line -split ':')[1].Trim()"') do set "VERSION=%%v"
if "%VERSION%"=="" set "VERSION=unknown"

set "STAGE=dist\duckdb-firebird-%VERSION%-windows-x64"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 2>nul

copy /Y "%EXT%"      "%STAGE%\firebird.duckdb_extension" >nul
copy /Y "%FBCLIENT%" "%STAGE%\fbclient.dll"              >nul

REM Sample SQL is small + handy for first-time users; ship it alongside.
if exist scripts\fixture_create.sql copy /Y scripts\fixture_create.sql "%STAGE%\" >nul
if exist scripts\fixture_biz4.sql   copy /Y scripts\fixture_biz4.sql   "%STAGE%\" >nul

REM Copy a pre-written README template; substitute %VERSION% inline via PowerShell.
powershell -NoProfile -Command "(Get-Content scripts\dist_README.template.txt -Raw) -replace '@@VERSION@@', '%VERSION%' | Set-Content -Path '%STAGE%\README.txt' -Encoding utf8"

REM ZIP via PowerShell ^(Compress-Archive ships with every Windows 10+^).
set "ZIP=%STAGE%.zip"
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"

echo.
echo --- packaged ---
echo Stage dir: %STAGE%
echo ZIP:       %ZIP%
dir /b "%STAGE%"
echo.
echo SHA-256:
powershell -NoProfile -Command "(Get-FileHash '%ZIP%' -Algorithm SHA256).Hash"

endlocal
