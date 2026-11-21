# Builds and runs the riding simulation tests with g++ (e.g. the MinGW bundled with CLion).
# Usage: powershell -ExecutionPolicy Bypass -File Tests\SimTests\build_and_run_gcc.ps1 [-Gxx path\to\g++.exe]
param([string]$Gxx = "")
$ErrorActionPreference = "Stop"
if (-not $Gxx) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $Gxx = $cmd.Source }
    else {
        $clion = Get-ChildItem "C:\Program Files\JetBrains" -Directory -Filter "CLion*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($clion) { $Gxx = Join-Path $clion.FullName "bin\mingw\bin\g++.exe" }
    }
}
if (-not $Gxx -or -not (Test-Path $Gxx)) { Write-Error "g++ not found; pass -Gxx"; exit 2 }
$env:PATH = (Split-Path $Gxx) + ";" + $env:PATH
# Relative paths keep us under the Windows MAX_PATH limit that MinGW still enforces.
Push-Location (Join-Path $PSScriptRoot "..\..")
try {
    New-Item -ItemType Directory -Force "Tests\SimTests\out" | Out-Null
    $sim = "Source\ManchesterRide\Sim"
    & $Gxx -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I $sim "Tests\SimTests\SimTests.cpp" "$sim\RideSim.cpp" "$sim\TrickTracker.cpp" "$sim\VehicleCatalog.cpp" "$sim\GeoProjection.cpp" -o "Tests\SimTests\out\SimTests.exe"
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & ".\Tests\SimTests\out\SimTests.exe"
    exit $LASTEXITCODE
} finally { Pop-Location }
