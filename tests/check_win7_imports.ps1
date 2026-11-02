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

if (-not (Test-Path $ExePath)) {
    Write-Error "Exe not found: $ExePath"
}

# Locate dumpbin.exe under a Visual Studio install (any edition or year;
# the GitHub Actions windows-latest image ships one).
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" `
    -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dumpbin) {
    Write-Error "dumpbin.exe not found under Visual Studio"
}

$imports = & $dumpbin.FullName /imports $ExePath
if ($LASTEXITCODE -ne 0) {
    Write-Error "dumpbin /imports failed on $ExePath"
}

$found = @()
foreach ($name in $forbidden) {
    $pattern = "(?i)\b$([regex]::Escape($name))\b"
    if (($imports | Out-String) -match $pattern) {
        $found += $name
    }
}

if ($found.Count -gt 0) {
    Write-Host "FAIL: tinta.exe imports Windows 8/10-only APIs:"
    $found | ForEach-Object { Write-Host "  $_" }
    Write-Host "Route the call through include/win7_compat.h or path_utils.h,"
    Write-Host "or drop the std::filesystem dependency that pulled it in."
    exit 1
}

Write-Host "PASS: tinta.exe imports no Windows 8/10-only APIs."
