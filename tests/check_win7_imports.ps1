# Windows 7 import-table check for the portable exe.
#
# The loader refuses to start tinta.exe on Windows 7 when its import table
# names an API added after Windows 7 ("procedure entry point ... not
# located in ..."). The source scan tests/win7_compat_tests.cpp catches
# direct calls in Tinta's own code, but a static library can smuggle an
# import in too (the MSVC STL's <filesystem> statically imports
# CreateFile2/CopyFile2 since the April 2025 "Win8 baseline"). This dumps
# the real exe's import table and fails on any of the forbidden names.
#
# Run from the CI workflow after the build:
#   powershell -ExecutionPolicy Bypass -File tests/check_win7_imports.ps1 -ExePath build\Release\tinta.exe
param(
    [Parameter(Mandatory = $true)][string]$ExePath
)

$ErrorActionPreference = "Stop"

# Windows 8/10-only APIs the exe must never statically import.
$forbidden = @(
    "CreateFile2",
    "CopyFile2",
    "GetDpiForWindow",
    "GetSystemMetricsForDpi",
    "SetProcessDpiAwarenessContext",
    "GetCurrentPackageFullName",
    "GetPointerType",
    "GetPointerInfo",
    "WaitOnAddress",
    "WakeByAddressAll",
    "WakeByAddressSingle",
    "GetTempPath2W",
    "GetSystemTimePreciseAsFileTime",
    "PathCchAppend",
    "PathCchCombineEx",
    "PathCchRemoveFileSpec"
)

# Names that can only get into the exe as static imports: unlike the six
# shimmed APIs above, they never appear as GetProcAddress string literals in
# Tinta's own code (win7_compat.h embeds the shimmed names for the runtime
# lookup, so bytes alone cannot distinguish an import from a lookup for
# those). This is exactly the class of bug the MSVC STL's <filesystem>
# caused, and it is what the byte-scan fallback below can check reliably.
$forbiddenLibrary = @(
    "CreateFile2",
    "CopyFile2",
    "GetSystemTimePreciseAsFileTime",
    "WaitOnAddress",
    "WakeByAddressAll",
    "WakeByAddressSingle",
    "GetTempPath2W",
    "PathCchAppend",
    "PathCchCombineEx",
    "PathCchRemoveFileSpec"
)

if (-not (Test-Path $ExePath)) {
    Write-Error "Exe not found: $ExePath"
}

# Locates dumpbin.exe across Visual Studio layouts (PATH, the standard
# Program Files\<year>\<edition> tree, and vswhere for non-standard
# installs), so the exact toolchain path never matters.
function Find-Dumpbin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $roots = @()
    if ($env:ProgramFiles) {
        $roots += Join-Path $env:ProgramFiles "Microsoft Visual Studio"
    }
    if (${env:ProgramFiles(x86)}) {
        $roots += Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"
    }
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($bin in @("Hostx64\x64", "Hostx86\x86")) {
            $hit = Get-ChildItem (Join-Path $root "*\VC\Tools\MSVC\*\bin\$bin\dumpbin.exe") `
                -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }

    # The official Visual Studio locator, for installs outside the standard
    # Program Files\Microsoft Visual Studio\<year>\<edition> layout.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($installPath) {
            $hit = Get-ChildItem (Join-Path $installPath "VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe") `
                -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

$found = @()
$usedDumpbin = $false
$dumpbin = Find-Dumpbin
if ($dumpbin) {
    $imports = & $dumpbin /imports $ExePath
    if ($LASTEXITCODE -eq 0) {
        $usedDumpbin = $true
        $dumpText = $imports | Out-String
        foreach ($name in $forbidden) {
            $pattern = "(?i)\b$([regex]::Escape($name))\b"
            if ($dumpText -match $pattern) {
                $found += $name
            }
        }
    }
}
if (-not $usedDumpbin) {
    # No dumpbin available. The PE import table stores imported-by-name
    # functions as ASCII strings, so a literal byte scan of the exe catches
    # the same imports. Only the library-smuggled names are scanned (see
    # $forbiddenLibrary): the six shimmed APIs also exist as GetProcAddress
    # literals, and the source scan tests/win7_compat_tests.cpp covers them.
    $bytes = [System.IO.File]::ReadAllBytes($ExePath)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($name in $forbiddenLibrary) {
        if ($text.Contains($name)) {
            $found += $name
        }
    }
    Write-Host "note: dumpbin.exe not found; checked the exe's import names with a byte scan."
}

if ($found.Count -gt 0) {
    Write-Host "FAIL: tinta.exe imports Windows 8/10-only APIs:"
    $found | ForEach-Object { Write-Host "  $_" }
    Write-Host "Route the call through include/win7_compat.h or path_utils.h,"
    Write-Host "or drop the std::filesystem dependency that pulled it in."
    exit 1
}

if ($usedDumpbin) {
    Write-Host "PASS: tinta.exe imports no Windows 8/10-only APIs (dumpbin /imports)."
} else {
    Write-Host "PASS: tinta.exe contains no Windows 8/10-only import names (byte scan)."
}
