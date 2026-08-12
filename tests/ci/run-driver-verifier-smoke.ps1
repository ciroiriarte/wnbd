# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

Param(
    [string] $TestExecutable = "libwnbd_tests.exe",
    [string] $DriverName = "wnbd.sys",
    [string] $OutputDir = "ci-artifacts",
    [string] $GTestFilter = "TestMap.CacheEnabled:TestWrite.CacheEnabled:TestRead.CacheEnabled:TestPendingIoRemoval.*"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$VerifierBefore = Join-Path $OutputDir "driver-verifier-before.txt"
$VerifierActive = Join-Path $OutputDir "driver-verifier-active.txt"
$VerifierAfter = Join-Path $OutputDir "driver-verifier-after.txt"
$TestXml = Join-Path $OutputDir "driver-verifier-smoke.xml"

function Run-VerifierCommand([string[]] $CommandArgs, [switch] $AllowFailure) {
    Write-Host "verifier $($CommandArgs -join ' ')"
    & verifier.exe @CommandArgs
    $ExitCode = $LASTEXITCODE
    Write-Host "verifier exit code: $ExitCode"
    if ($ExitCode -and !$AllowFailure) {
        throw "verifier $($CommandArgs -join ' ') failed with exit code $ExitCode"
    }
}

try {
    verifier.exe /query > $VerifierBefore 2>&1

    # Use volatile settings so scheduled CI can exercise verifier without a
    # reboot. Add the loaded driver first, then enable immediate volatile
    # checks. Keep this smoke conservative: Pool Tracking is immediate,
    # rebootless, and avoids destabilizing hosted/smoke runners while still
    # proving that Driver Verifier is attached to wnbd.sys during I/O tests.
    Run-VerifierCommand @("/volatile", "/adddriver", $DriverName)
    Run-VerifierCommand @("/volatile", "/flags", "0x8")

    verifier.exe /query > $VerifierActive 2>&1
    Get-Content $VerifierActive

    & $TestExecutable --gtest_filter=$GTestFilter --gtest_output=xml:$TestXml
    $TestExit = $LASTEXITCODE
    if ($TestExit) {
        throw "Driver Verifier smoke tests failed with exit code $TestExit"
    }
}
finally {
    # Volatile verification of a currently loaded driver may not be removable
    # until reboot, but disabling all volatile flags minimizes remaining
    # overhead for the rest of the scheduled job.
    Run-VerifierCommand @("/volatile", "/flags", "0") -AllowFailure
    verifier.exe /query > $VerifierAfter 2>&1
}
