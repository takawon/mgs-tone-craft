# SPDX-License-Identifier: AGPL-3.0-only
# Fails fast when MSVC standard headers are unavailable (INCLUDE unset).

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:INCLUDE)) {
    Write-Host ""
    Write-Host "MGSTC: Visual Studio C++ environment is not active (INCLUDE is unset)." `
        -ForegroundColor Red
    Write-Host "  cl.exe cannot find standard headers such as <array> or <cstdint>." `
        -ForegroundColor Red
    Write-Host ""
    Write-Host "  Use:  .\tools\build.cmd" -ForegroundColor Yellow
    Write-Host "  Or:   Developer PowerShell for VS" -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

exit 0
