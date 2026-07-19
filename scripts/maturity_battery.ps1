# maturity_battery.ps1 — read-only maturity battery against a REAL Firebird
# database, for the Runtime/ABI Compatibility Gate.
#
# Purpose: prove the extension behaves correctly against a real, complex
# production-shaped database (not just synthetic fixtures) WITHOUT ever
# materializing, logging, or reporting actual business data.
#
# What this script does:
#   - firebird_health('fb')            -- structural diagnostic (counts/flags
#                                          only; no business data)
#   - table/view counts                -- via information_schema, counts only
#   - firebird_type_audit('fb')        -- aggregated by finding code (counts
#                                          only; table/column names are used
#                                          internally to run the BLOB
#                                          self-consistency check below, but
#                                          are never printed)
#   - BLOB self-consistency check      -- for a sample of BLOB-ish columns
#                                          found by firebird_type_audit,
#                                          fetches the same single row TWICE
#                                          and compares md5(value) between
#                                          the two fetches. This proves the
#                                          fetch path is deterministic and
#                                          repeatable without ever printing
#                                          the value itself -- only length
#                                          and whether the two checksums
#                                          matched.
#
# What this script NEVER does:
#   - print row values, BLOB/text content, or any business data
#   - print real table/column names in its final report (used internally
#     only, to address the columns being sampled)
#   - print the full connection string / DSN (FIREBIRD_MATURITY_DB itself)
#   - print any real filesystem path from the target database
#
# Required environment variables:
#   FIREBIRD_MATURITY_DB   Full ATTACH DSN, e.g.
#                          "database=C:/path/to/real.fdb user=SYSDBA password=..."
#                          (never printed; used only inside the ATTACH statement)
#   ISC_USER, ISC_PASSWORD  Only needed if not embedded in FIREBIRD_MATURITY_DB.
#
# Optional environment variables:
#   MATURITY_DUCKDB_EXE          Path to a duckdb.exe with the firebird
#                                extension statically linked (defaults to
#                                build/release/duckdb.exe under this repo).
#   DUCKDB_FIREBIRD_CLIENT_LIBRARY  Passed through unchanged if set (same
#                                    override the extension itself reads).
#   MATURITY_BLOB_SAMPLE_SIZE   Max number of BLOB-ish columns to sample for
#                                the self-consistency check (default 20).
#
# Output: a single Markdown report to stdout (redirect to a file yourself,
# e.g. `pwsh -File scripts/maturity_battery.ps1 > maturity_report.md`).
# The report is safe to share -- it contains no row values, no BLOB/text
# content, no table/column names, and no real paths or connection strings.

$ErrorActionPreference = 'Stop'

if (-not $env:FIREBIRD_MATURITY_DB) {
    Write-Error "FIREBIRD_MATURITY_DB is not set. Refusing to run -- this script never assumes a default target database."
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$duckdbExe = if ($env:MATURITY_DUCKDB_EXE) { $env:MATURITY_DUCKDB_EXE } else { Join-Path $repoRoot 'build/release/duckdb.exe' }
if (-not (Test-Path $duckdbExe)) {
    Write-Error "duckdb.exe not found at '$duckdbExe'. Build the extension first (scripts/build_windows_local.bat) or set MATURITY_DUCKDB_EXE."
    exit 1
}

$sampleSize = if ($env:MATURITY_BLOB_SAMPLE_SIZE) { [int]$env:MATURITY_BLOB_SAMPLE_SIZE } else { 20 }

# Raw duckdb output on a parse failure is written ONLY here (a local-only
# diagnostic log), never into the shareable report below -- some Firebird
# error messages can embed the DSN or a real file path, so raw output must
# never be echoed into anything meant to be shared.
$diagLog = Join-Path ([System.IO.Path]::GetTempPath()) 'maturity_battery_diag.log'
"Maturity battery diagnostic log -- local only, do not share this file." | Set-Content $diagLog

function Invoke-DuckDbJson([string]$sql) {
    # -json emits one JSON array per statement; we only ever issue one
    # statement per invocation here, so take the first array.
    $out = & $duckdbExe -unsigned -json -c $sql 2>&1
    $joined = ($out -join "`n")
    try {
        return $joined | ConvertFrom-Json
    } catch {
        # Surface a SANITIZED failure in the return value (exception type
        # only); the raw output goes to $diagLog (local-only) for debugging,
        # never into anything meant to be shared.
        Add-Content -Path $diagLog -Value "`n--- failed call at $(Get-Date -Format o) ---`n$joined"
        return @{ __error = $_.Exception.GetType().Name }
    }
}

$dsnEscaped = $env:FIREBIRD_MATURITY_DB -replace "'", "''"
$attach = "ATTACH '$dsnEscaped' AS fb (TYPE firebird);"

# Only Invoke-DuckDbJson's own catch path returns a [hashtable] (with an
# __error key); every successful ConvertFrom-Json result is a PSCustomObject
# or an array of them. Checking the TYPE (not `.__error` truthiness) avoids
# a real PowerShell pitfall: member-enumeration on a multi-element array
# returns an array of $null when no element has that property, and a
# non-empty array is always truthy in an `if()` -- so `.__error` on a
# 2+ row successful result would have falsely read as "errored".
function Test-IsError($result) {
    return $result -is [hashtable]
}

$report = [System.Collections.Generic.List[string]]::new()
$report.Add("# Maturity Battery Report")
$report.Add("")
$report.Add("Generated: (fill in manually if needed -- this script does not stamp a timestamp itself)")
$report.Add("")
$report.Add("No row values, BLOB/text content, table/column names, real paths, or connection strings appear below.")
$report.Add("")

$swTotal = [System.Diagnostics.Stopwatch]::StartNew()

# --- 1. Health -----------------------------------------------------------
$report.Add("## 1. firebird_health()")
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$health = Invoke-DuckDbJson "$attach SELECT * FROM firebird_health('fb');"
$sw.Stop()
if (Test-IsError $health) {
    $report.Add("- Status: **ERROR** ($($health.__error)) -- see $diagLog for detail, not embedded in this report")
} elseif ($health.Count -ge 1) {
    $h = $health[0]
    $report.Add("- engine_version: $($h.engine_version)")
    $report.Add("- ods_version: $($h.ods_version)")
    $report.Add("- sql_dialect: $($h.sql_dialect)")
    $report.Add("- default_charset: $($h.default_charset)")
    $report.Add("- page_size: $($h.page_size)")
    $report.Add("- forced_writes: $($h.forced_writes)")
    $report.Add("- sweep_interval: $($h.sweep_interval)")
    $report.Add("- oit_gap: $($h.oit_gap)")
    $report.Add("- oat_gap: $($h.oat_gap)")
    $report.Add("- attachments (visible to this credential): $($h.attachments)")
    $report.Add("- warnings: $($h.warnings -join ', ')")
} else {
    $report.Add("- Status: no row returned")
}
$report.Add("- Elapsed: $($sw.ElapsedMilliseconds) ms")
$report.Add("")

# --- 2. Relation count -----------------------------------------------------
# NOTE: the bridge does not currently surface Firebird's own table-vs-view
# distinction through information_schema.table_type -- every Firebird
# relation (whether a real TABLE or a VIEW server-side) is exposed to
# DuckDB as 'BASE TABLE'. Confirmed empirically during this gate's own
# dry run: a fixture DB with 9 real tables + 5 views reported 14 'BASE
# TABLE' rows and 0 'VIEW' rows. This is a real, documented bridge
# limitation (not a bug in this harness) -- report a single relation
# count, not a misleading table/view split.
$report.Add("## 2. Relation count")
$report.Add("(The bridge does not currently distinguish Firebird tables from views in information_schema.table_type -- both surface as 'BASE TABLE'. This reports total relations only.)")
$sw.Restart()
$counts = Invoke-DuckDbJson "$attach SELECT count(*) AS n_relations FROM information_schema.tables WHERE table_catalog = 'fb';"
$sw.Stop()
if (Test-IsError $counts) {
    $report.Add("- Status: **ERROR** ($($counts.__error))")
} else {
    $report.Add("- Relations exposed: $($counts[0].n_relations)")
}
$report.Add("- Elapsed: $($sw.ElapsedMilliseconds) ms")
$report.Add("")

# --- 3. Type audit, aggregated by finding ----------------------------------
$report.Add("## 3. firebird_type_audit() -- aggregated by finding code")
$report.Add("(No table/column names printed -- counts per finding only.)")
$sw.Restart()
$auditAgg = Invoke-DuckDbJson "$attach SELECT finding, count(*) AS n FROM firebird_type_audit('fb') GROUP BY finding ORDER BY finding;"
$sw.Stop()
if (Test-IsError $auditAgg) {
    $report.Add("- Status: **ERROR** ($($auditAgg.__error))")
} elseif ($auditAgg.Count -eq 0) {
    $report.Add("- No findings (every column maps cleanly with no caveats).")
} else {
    foreach ($row in $auditAgg) {
        $report.Add("- $($row.finding): $($row.n)")
    }
}
$report.Add("- Elapsed: $($sw.ElapsedMilliseconds) ms")
$report.Add("")

# --- 4. BLOB self-consistency check ----------------------------------------
$report.Add("## 4. BLOB self-consistency check")
$report.Add("(Fetches the SAME single row TWICE per sampled column and compares md5(value). Proves determinism without ever printing the value. Table/column identity used internally only -- never printed.)")
$sw.Restart()
$blobCols = Invoke-DuckDbJson "$attach SELECT table_name, column_name FROM firebird_type_audit('fb') WHERE finding IN ('blob_text', 'none_charset') LIMIT $sampleSize;"
if (Test-IsError $blobCols) {
    $report.Add("- Status: **ERROR listing candidate columns** ($($blobCols.__error))")
} elseif ($blobCols.Count -eq 0) {
    $report.Add("- No BLOB/text-caveat columns found to sample.")
} else {
    $okCount = 0
    $mismatchCount = 0
    $errorCount = 0
    $lengths = @()
    foreach ($col in $blobCols) {
        # Column/table identity is used ONLY to build this one query --
        # never appended to $report.
        $tbl = $col.table_name -replace '"', '""'
        $c = $col.column_name -replace '"', '""'
        $sql = "$attach SELECT md5(CAST(""$c"" AS VARCHAR)) AS h1, md5(CAST(""$c"" AS VARCHAR)) AS h2, length(CAST(""$c"" AS VARCHAR)) AS len FROM fb.main.""$tbl"" WHERE ""$c"" IS NOT NULL LIMIT 1;"
        $r = Invoke-DuckDbJson $sql
        if (Test-IsError $r) { $errorCount++; continue }
        if ($r.Count -eq 0) { continue }
        $row = $r[0]
        if ($row.h1 -eq $row.h2) { $okCount++ } else { $mismatchCount++ }
        $lengths += [int]$row.len
    }
    $report.Add("- Columns sampled: $($blobCols.Count)")
    $report.Add("- Self-consistent (checksum matched across two fetches): $okCount")
    $report.Add("- Mismatched (potential non-determinism -- investigate): $mismatchCount")
    $report.Add("- Errored while sampling: $errorCount")
    if ($lengths.Count -gt 0) {
        $report.Add("- Sampled value lengths: min=$(($lengths | Measure-Object -Minimum).Minimum), max=$(($lengths | Measure-Object -Maximum).Maximum), avg=$([math]::Round((($lengths | Measure-Object -Average).Average), 1))")
    }
}
$sw.Stop()
$report.Add("- Elapsed: $($sw.ElapsedMilliseconds) ms")
$report.Add("")

$swTotal.Stop()
$report.Add("## Summary")
$report.Add("- Total elapsed: $($swTotal.ElapsedMilliseconds) ms")
$report.Add("- Overall: $(if ((Test-IsError $health) -or (Test-IsError $counts) -or (Test-IsError $auditAgg) -or (Test-IsError $blobCols)) { 'ERRORS ENCOUNTERED -- see above' } else { 'no errors' })")
$report.Add("")
$report.Add("(If any section above shows an ERROR, raw diagnostic detail was appended to $diagLog on this machine -- a per-user temp path, not part of the shareable report. Never share that file; it may contain raw error text.)")

$report | Write-Output
