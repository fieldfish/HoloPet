<#
.SYNOPSIS
  HoloPet Core (no SDL) Windows build entry - V6 AI R4-R2.
.DESCRIPTION
  Preflight: requires cmake on PATH.
  Builds into an independent build directory (default build-core).
  Does NOT delete user directories, does NOT write any API key.
  -RunTests: also runs ctest (BUILD_TESTS=ON). Default: OFF (build only).
.EXAMPLE
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -RunTests
#>
param(
    [string]$BuildDir = "build-core",
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Host "=== HoloPet Core build (V6 AI R4-R2) ===" -ForegroundColor Cyan
Write-Host "source : $root"
Write-Host "build  : $BuildDir"

# --- preflight: cmake ---
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: cmake not found on PATH." -ForegroundColor Red; exit 1
}
$cmakeVer = (& cmake --version | Select-Object -First 1)
Write-Host "cmake  : $cmakeVer"

# --- preflight: ctest (only when tests requested) ---
if ($RunTests -and -not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: ctest not found on PATH (required by -RunTests)." -ForegroundColor Red; exit 1
}

$tests = $(if ($RunTests) { "ON" } else { "OFF" })
Write-Host "tests  : $tests (use -RunTests to enable BUILD_TESTS=ON and run ctest)"

& cmake -S . -B $BuildDir "-DBUILD_TESTS=$tests"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: configure failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE   # R4-R1 P5: preserve external tool exit code
}

& cmake --build $BuildDir --config Debug
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: build failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

if ($RunTests) {
    & ctest --test-dir $BuildDir --output-on-failure -C Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: ctest failed (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

Write-Host ""
Write-Host "=== Core build done ===" -ForegroundColor Green
Write-Host "build path: $(Join-Path $root $BuildDir)"
exit 0
