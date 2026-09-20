<#
.SYNOPSIS
  HoloPet CMake switch-combination matrix entry - V6 AI R4-R2.
.DESCRIPTION
  Wraps scripts\check_build_matrix.py (toolchain preflight + expected
  failure reason matching + source-dir cleanliness).
  Acceptance mode is the script default: without SDL_PREFIX the ai_on
  combination cannot run and the whole run exits non-zero (INCOMPLETE).
  No user directories are deleted; no API key is written.
  PYTHONDONTWRITEBYTECODE=1 is set so no .pyc pollutes the tree.
.EXAMPLE
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\build_matrix_windows.ps1 -SdlPrefix C:\sdl\SDL2-devel-VC
#>
param(
    [string]$SdlPrefix = $env:SDL_PREFIX,
    [switch]$DevMode
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Error "python not found on PATH."; exit 1
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake not found on PATH."; exit 1
}

$env:PYTHONDONTWRITEBYTECODE = "1"
if ($SdlPrefix) { $env:SDL_PREFIX = $SdlPrefix }

$extra = @()
if ($DevMode) { $extra += "--allow-skip-ai" } else { $extra += "--acceptance" }

$modeName = $(if ($DevMode) { "dev (allow-skip-ai)" } else { "acceptance" })
Write-Host "=== build matrix mode: $modeName ===" -ForegroundColor Cyan
if ($SdlPrefix) { Write-Host "     SDL_PREFIX=$SdlPrefix" } else { Write-Host "     SDL_PREFIX unset -> ai_on INCOMPLETE, exit non-zero" }
& python scripts\check_build_matrix.py @extra
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: build matrix FAIL (exit $LASTEXITCODE) - see logs\build_matrix.txt" -ForegroundColor Red
    exit $LASTEXITCODE   # R4-R1 P5: preserve external tool exit code
}
Write-Host ""
if ($DevMode) {
    # R4-R1 P5: DevMode must not print acceptance 6/6
    Write-Host "=== build matrix result (DEV mode allow-skip-ai; NOT acceptance) ===" -ForegroundColor Yellow
} else {
    Write-Host "=== build matrix PASS (6/6, acceptance) ===" -ForegroundColor Green
}
exit 0
