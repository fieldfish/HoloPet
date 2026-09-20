<#
.SYNOPSIS
  HoloPet V6 AI (SDL) Windows build entry - V6 AI R4-R2.
.DESCRIPTION
  Preflight: requires cmake, ctest, python on PATH and a valid SDL dev root.
  Builds into an independent build directory (default build-ai).
  Does NOT delete user directories, does NOT write any API key.
  -SdlPrefix: common root of the SDL dev package. The script discovers
    cmake\sdl2-config.cmake and cmake\sdl2_ttf-config.cmake inside it
    (direct child or one level down) and prints the two files it will use.
  -SkipTests: skip ctest (default: tests run - R4 acceptance requires them).
  -ResolveOnly: R7_R3 (task D) test seam - resolve and print the canonical
    absolute BuildDir, then exit before preflight/configure/build. Used by
    the lightweight regression test to verify path normalization without a
    full 39-item AI compile.
  NOTE: this file must stay pure ASCII (PS 5.1 without BOM misreads UTF-8
    as ANSI and can swallow newlines; verify_r4r1 enforces ASCII).
.EXAMPLE
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\build_ai_windows.ps1 -SdlPrefix C:\sdl\SDL2-devel-VC
#>
param(
    [string]$BuildDir = "build-ai",
    [string]$SdlPrefix = $env:SDL_PREFIX,
    [switch]$SkipTests,
    [switch]$ResolveOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Host "=== HoloPet V6 AI build (V6 AI R4-R2) ===" -ForegroundColor Cyan
Write-Host "source : $root"

# R7_R3 (task D): resolve BuildDir to a unique canonical absolute path once.
# Both relative (build-ai) and absolute (C:\...\build-ai) inputs must yield
# one canonical path; configure/build/ctest/final output all use this value.
# Never re-join the project root onto an already-absolute path (R7_R2 printed
# a duplicated invalid path in the final output line).
$BuildDirAbs = $BuildDir
if (-not [System.IO.Path]::IsPathRooted($BuildDirAbs)) {
    $BuildDirAbs = Join-Path $root $BuildDirAbs
}
$BuildDirAbs = [System.IO.Path]::GetFullPath($BuildDirAbs)
Write-Host "build  : $BuildDirAbs"

if ($ResolveOnly) {
    Write-Host "build path: $BuildDirAbs"
    exit 0
}

# --- preflight: cmake / ctest / python ---
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: cmake not found on PATH." -ForegroundColor Red; exit 1
}
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: ctest not found on PATH (AI acceptance requires ctest)." -ForegroundColor Red; exit 1
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: python not found on PATH." -ForegroundColor Red; exit 1
}
$cmakeVer = (& cmake --version | Select-Object -First 1)
Write-Host "cmake  : $cmakeVer"

# --- SDL discovery (R4 P0): common root -> find both config files ---
function Find-SdlConfig([string]$name, [string]$prefix) {
    $direct = Join-Path $prefix ("cmake\" + $name)
    if (Test-Path $direct) { return $direct }
    if (Test-Path $prefix) {
        foreach ($d in (Get-ChildItem -Path $prefix -Directory)) {
            $c = Join-Path $d.FullName ("cmake\" + $name)
            if (Test-Path $c) { return $c }
        }
    }
    return $null
}

if (-not $SdlPrefix) {
    Write-Host "ERROR: SDL dev root missing. Pass -SdlPrefix <dir> or set env SDL_PREFIX. Expected inside it: cmake\sdl2-config.cmake and cmake\sdl2_ttf-config.cmake (direct child or one level down)." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $SdlPrefix)) {
    Write-Host "ERROR: SdlPrefix does not exist: $SdlPrefix" -ForegroundColor Red; exit 1
}
$sdl2Cfg   = Find-SdlConfig "sdl2-config.cmake" $SdlPrefix
$sdl2TtfCfg = Find-SdlConfig "sdl2_ttf-config.cmake" $SdlPrefix
if (-not $sdl2Cfg) {
    Write-Host "ERROR: sdl2-config.cmake not found under $SdlPrefix" -ForegroundColor Red; exit 1
}
if (-not $sdl2TtfCfg) {
    Write-Host "ERROR: sdl2_ttf-config.cmake not found under $SdlPrefix" -ForegroundColor Red; exit 1
}
$sdl2Root  = Split-Path -Parent (Split-Path -Parent $sdl2Cfg)
$sdl2TtfRoot = Split-Path -Parent (Split-Path -Parent $sdl2TtfCfg)
Write-Host "SDL    : $SdlPrefix"
Write-Host "  sdl2 cfg     : $sdl2Cfg"
Write-Host "  sdl2_ttf cfg : $sdl2TtfCfg"
$cmakePrefixPath = "$sdl2Root;$sdl2TtfRoot"

$tests = $(if ($SkipTests) { "skipped (-SkipTests)" } else { "enabled (default)" })
Write-Host "tests  : $tests"

& cmake -S . -B $BuildDirAbs "-DBUILD_TESTS=ON" "-DBUILD_AI=ON" "-DBUILD_AUDIO=ON" "-DBUILD_PROJECTION=ON" "-DCMAKE_PREFIX_PATH=$cmakePrefixPath"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: configure failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

& cmake --build $BuildDirAbs --config Debug
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: build failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

if (-not $SkipTests) {
    & ctest --test-dir $BuildDirAbs --output-on-failure -C Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: ctest failed (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

Write-Host ""
if ($SkipTests) {
    # R4-R1 P5: -SkipTests must not print tests passed
    Write-Host "=== AI build done (tests SKIPPED) ===" -ForegroundColor Yellow
} else {
    Write-Host "=== AI build done (tests passed) ===" -ForegroundColor Green
}
Write-Host "build path: $BuildDirAbs"
exit 0
