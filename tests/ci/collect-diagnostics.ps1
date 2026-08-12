# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

Param(
    [string] $OutputDir = "ci-artifacts"
)

$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Export-EventLogSafe($Name, $Path) {
    try {
        wevtutil epl $Name $Path /ow:true
        Write-Host "Exported event log $Name to $Path"
    }
    catch {
        Write-Warning "Could not export event log ${Name}: $_"
    }
}

Export-EventLogSafe "System" (Join-Path $OutputDir "event-system.evtx")
Export-EventLogSafe "Application" (Join-Path $OutputDir "event-application.evtx")

try {
    wevtutil qe System /c:200 /f:text /q:"*[System[(Level=1 or Level=2 or Level=3)]]" `
        > (Join-Path $OutputDir "event-system-recent-errors.txt")
}
catch {
    Write-Warning "Could not query recent System errors: $_"
}

try {
    Get-ChildItem -Path $env:SystemRoot\Minidump -Filter *.dmp -ErrorAction SilentlyContinue |
        Copy-Item -Destination $OutputDir -Force -ErrorAction SilentlyContinue
}
catch {
    Write-Warning "Could not copy minidumps: $_"
}
