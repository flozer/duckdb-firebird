# build_matrix.ps1 — build + run the local Firebird test suite against a matrix
# of DuckDB release tags, locally (Windows). Proves the extension builds/tests
# per DuckDB version (it is version-locked to the DuckDB used at build time).
#
# Adapted from D:\Dados\duckdb-salesforce\scripts\build_matrix.ps1. Differences
# for Firebird: no vcpkg manifest/toolchain (no OpenSSL dep), uses FB_SDK_ROOT,
# copies fbclient.dll next to the test/duckdb binaries, and runs the
# firebird_*.test SQL suite.
#
# - Does NOT change the committed submodule pin (restored to v1.5.3 at the end).
# - Each version builds into build/matrix/<tag> (isolated build tree).
# - Never touches tags/releases/publication/community. Local only.
#
# Test fixtures are provisioned out-of-band (scripts/provision_test_dbs.ps1).
# This script reads FIREBIRD_TEST_DB / FIREBIRD_DECFLOAT_DB / FIREBIRD_NONE_DB /
# ISC_USER / ISC_PASSWORD from the environment; if a fixture DB is absent the
# dependent tests are reported ENV-MISSING (not a compatibility failure).
#
# Usage:  pwsh -File scripts/build_matrix.ps1
#         pwsh -c "& ./scripts/build_matrix.ps1 -Tags v1.5.3,v1.5.4"
#         (use -c "& ..." for a custom -Tags list; `-File -Tags a,b` passes one
#          string, not an array.)
#
# Exit code: non-zero if the baseline v1.5.3 fails to build.

param([string[]]$Tags = @('v1.5.2', 'v1.5.3', 'v1.5.4'))

$ErrorActionPreference = 'Continue'
$root = 'd:/Dados/duckdb-firebird'
$pin = '14eca11bd9d4a0de2ea0f078be588a9c1c5b279c' # v1.5.3 (committed submodule pin)
$baseline = 'v1.5.3'

$fbSdk = 'C:/Program Files/Firebird/Firebird_5_0'
$ninja = 'C:\Users\fernando.souza\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'

# Locate vcvars64.bat across the common VS 2022 editions.
$vcvars = $null
foreach ($base in @('C:\Program Files (x86)\Microsoft Visual Studio\2022', 'C:\Program Files\Microsoft Visual Studio\2022')) {
    foreach ($ed in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
        $cand = "$base\$ed\VC\Auxiliary\Build\vcvars64.bat"
        if (-not $vcvars -and (Test-Path $cand)) { $vcvars = $cand }
    }
}
if (-not $vcvars) { Write-Host "vcvars64.bat not found"; exit 1 }

$Tags = @(
    $Tags | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }
)

# Test SQL files and the fixture DB each one needs (for ENV-MISSING accounting).
$testFixtureVar = @{
    'firebird_attach.test'        = 'FIREBIRD_TEST_DB'
    'firebird_scan.test'          = 'FIREBIRD_TEST_DB'
    'firebird_metadata.test'      = 'FIREBIRD_TEST_DB'
    'firebird_predicates.test'    = 'FIREBIRD_TEST_DB'
    'firebird_bind_params.test'   = 'FIREBIRD_TEST_DB'
    'firebird_paging.test'        = 'FIREBIRD_TEST_DB'
    'firebird_pool.test'          = 'FIREBIRD_TEST_DB'
    'firebird_pool_stats.test'    = 'FIREBIRD_TEST_DB'
    'firebird_observability.test' = 'FIREBIRD_TEST_DB'
    'firebird_profile_table.test' = 'FIREBIRD_TEST_DB'
    'firebird_dbt_sources.test'        = 'FIREBIRD_TEST_DB'
    'firebird_metadata_bridge.test'    = 'FIREBIRD_TEST_DB'
    'firebird_decfloat.test'           = 'FIREBIRD_DECFLOAT_DB'
    'firebird_none_charset.test'       = 'FIREBIRD_NONE_DB'
}

function Invoke-Build([string]$buildDir) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $extCfg = "$root/extension_config.cmake"
    $cfg = "-G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=Release " +
           "-DEXTENSION_STATIC_BUILD=1 -DDUCKDB_EXTENSION_CONFIGS=`"$extCfg`" " +
           "-DFB_SDK_ROOT=`"$fbSdk`" -DDUCKDB_EXPLICIT_PLATFORM=windows_amd64 " +
           "-DENABLE_EXTENSION_AUTOLOADING=0 -DENABLE_EXTENSION_AUTOINSTALL=0 " +
           "-DENABLE_UNITTEST_CPP_TESTS=FALSE -DUNITTEST_ROOT_DIRECTORY=`"$root`" " +
           "-S `"$root/duckdb`" -B `"$buildDir`""
    cmd /c "`"$vcvars`" >nul && `"$cmake`" $cfg && `"$cmake`" --build `"$buildDir`" --config Release" 2>&1 |
        Tee-Object -FilePath "$buildDir/build.log" | Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Find-Artifact([string]$buildDir) {
    $hit = Get-ChildItem -Path $buildDir -Recurse -Filter 'firebird.duckdb_extension' -ErrorAction SilentlyContinue | Select-Object -First 1
    return $hit
}

function Invoke-Tests([string]$buildDir) {
    $unittest = Join-Path $buildDir 'test/unittest.exe'
    if (-not (Test-Path $unittest)) { return @{ status = 'TESTS-FAIL'; note = 'no unittest binary' } }

    # fbclient.dll next to the test binary so it links the FB5 client.
    Copy-Item "$fbSdk/fbclient.dll" (Split-Path $unittest) -Force -ErrorAction SilentlyContinue
    $duckdb = Join-Path $buildDir 'duckdb.exe'
    if (Test-Path $duckdb) { Copy-Item "$fbSdk/fbclient.dll" (Split-Path $duckdb) -Force -ErrorAction SilentlyContinue }

    $pass = 0; $fail = 0; $asserts = 0; $envmiss = 0; $failed = @()
    Push-Location $root
    try {
        foreach ($t in ($testFixtureVar.Keys | Sort-Object)) {
            $needVar = $testFixtureVar[$t]
            $dbPath = [Environment]::GetEnvironmentVariable($needVar)
            if (-not $dbPath -or -not (Test-Path $dbPath)) { $envmiss++; continue }
            $out = (& $unittest "test/sql/$t" 2>&1 | Out-String)
            if ($out -match 'All tests passed \((\d+) assertions?') { $pass++; $asserts += [int]$Matches[1] }
            elseif ($out -match 'No tests were run|Skipped') { $envmiss++ }
            else { $fail++; $failed += $t }
        }
    } finally { Pop-Location }

    $status = if ($fail -gt 0) { 'TESTS-FAIL' }
              elseif ($pass -eq 0) { 'ENV-MISSING' }
              elseif ($envmiss -gt 0) { 'PASS (partial)' }
              else { 'PASS' }
    return @{ status = $status; pass = $pass; fail = $fail; envmiss = $envmiss; assertions = $asserts; failed = ($failed -join ',') }
}

$results = @()
foreach ($tag in $Tags) {
    Write-Host "=== DuckDB $tag ==="
    git -C "$root/duckdb" rev-parse "$tag^{commit}" *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  fetching tag $tag ..."
        git -C "$root/duckdb" fetch --depth 1 origin tag $tag *> $null
    }
    $commit = (git -C "$root/duckdb" rev-parse "$tag^{commit}" 2>$null)
    git -C "$root/duckdb" checkout -q $tag 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $results += [pscustomobject]@{ Version = $tag; Commit = '-'; Build = 'n/a'; Tests = 'n/a'; Status = 'CHECKOUT-FAIL'; Notes = '' }
        continue
    }
    $bdir = "$root/build/matrix/$tag"
    Write-Host "  building -> $bdir (slow; full DuckDB)"
    $built = Invoke-Build $bdir
    if (-not $built) {
        $results += [pscustomobject]@{ Version = $tag; Commit = $commit.Substring(0,10); Build = 'FAIL'; Tests = '-'; Status = 'BUILD-FAIL'; Notes = "see $bdir/build.log" }
        continue
    }
    $art = Find-Artifact $bdir
    $artOk = [bool]$art
    $t = Invoke-Tests $bdir
    $status = if (-not $artOk) { 'BUILD-FAIL (no artifact)' } else { $t.status }
    $results += [pscustomobject]@{
        Version    = $tag
        Commit     = $commit.Substring(0,10)
        Build      = $(if ($artOk) { 'ok' } else { 'no-artifact' })
        Tests      = $(if ($t.fail -gt 0) { "FAIL($($t.failed))" } else { "$($t.pass) pass / $($t.envmiss) env-missing" })
        Assertions = $t.assertions
        Status     = $status
        Artifact   = $(if ($art) { 'firebird.duckdb_extension' } else { 'MISSING' })
    }
}

# Restore the committed pin so the worktree is clean.
git -C "$root/duckdb" checkout -q $pin 2>&1 | Out-Null
Write-Host "submodule duckdb restored to $pin (v1.5.3)"

Write-Host ""
Write-Host "===== DuckDB build matrix (Firebird) ====="
$results | Format-Table -AutoSize

$base = $results | Where-Object { $_.Version -eq $baseline }
if ($base -and $base.Build -eq 'ok') {
    Write-Host "baseline ${baseline}: builds OK"
    exit 0
} else {
    Write-Host "baseline ${baseline}: NOT building — failing the matrix"
    exit 1
}
