# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

# Record/verify phases of the abrupt-shutdown-hang regression check. See
# shutdown-hang-arm.ps1 for the full background and the two-phase design.
#
#   -Mode record  Run by the boot-time scheduled task the arm phase registers.
#                 Reads the marker, captures the post-reboot outcome (bugcheck
#                 delta, driver state, down duration) into a result file, and
#                 unregisters the task. Never fails the boot.
#
#   -Mode verify  Run by the verify job in a later dispatch. Reads the result
#                 file and exits non-zero if the reboot did not happen, a new
#                 0x9F bugcheck appeared, or the boot-start driver did not come
#                 back. Copies the result into the CI artifacts dir.
#
# The gate is the 0x9F BugCheck count, not timing: under Special Pool a healthy
# verifier boot already takes 5-8 minutes.

Param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("record", "verify")]
    [string] $Mode,
    [string] $WorkDir = "C:\wnbd-ci",
    # verify mode only: where to copy the result for artifact upload.
    [string] $OutputDir = "ci-artifacts",
    # Informational only; a hang manifests as a 0x9F, which is the real gate.
    [int]    $MaxDownSeconds = 900
)

$TaskName = "WnbdShutdownHangRecord"
$MarkerPath = Join-Path $WorkDir "shutdown-hang-marker.json"
$ResultPath = Join-Path $WorkDir "shutdown-hang-result.json"

function Get-BugcheckStats {
    # Mirror of the arm-phase counter: BugCheck reports (Event 1001) since a
    # given time, with the 0x9F subset broken out.
    Param([datetime] $Since)
    $total = 0
    $nineF = 0
    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName      = 'System'
            ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting'
            Id           = 1001
            StartTime    = $Since
        } -ErrorAction SilentlyContinue
        foreach ($e in $events) {
            $total++
            if ($e.Message -match '0x0{0,6}9[fF]\b') { $nineF++ }
        }
    }
    catch {
        Write-Warning "Could not read BugCheck events: $_"
    }
    return @{ Total = $total; NineF = $nineF }
}

if ($Mode -eq "record") {
    # Boot-time capture. Must never throw in a way that leaves the task
    # registered or the box without a result file.
    $ErrorActionPreference = "Continue"
    try {
        if (-not (Test-Path $MarkerPath)) {
            Write-Warning "No marker at $MarkerPath; nothing to record."
        }
        else {
            $marker = Get-Content -Path $MarkerPath -Raw | ConvertFrom-Json
            $armTimeUtc = [datetime]::Parse(
                $marker.armTimeUtc, $null,
                [System.Globalization.DateTimeStyles]::RoundtripKind)
            $shutdownInitUtc = [datetime]::Parse(
                $marker.shutdownInitUtc, $null,
                [System.Globalization.DateTimeStyles]::RoundtripKind)

            $lastBoot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
            $lastBootUtc = $lastBoot.ToUniversalTime()
            # A fresh boot after the arm time proves the reboot actually fired.
            $rebooted = $lastBootUtc -gt $armTimeUtc
            $downSeconds = [math]::Round(
                ($lastBootUtc - $shutdownInitUtc).TotalSeconds, 1)
            if ($downSeconds -lt 0) { $downSeconds = 0 }

            $stats = Get-BugcheckStats -Since $armTimeUtc

            $svc = Get-Service -Name wnbd -ErrorAction SilentlyContinue
            $driverState = if ($svc) { "$($svc.Status)" } else { "NotFound" }

            $result = [ordered]@{
                recordedAtUtc     = (Get-Date).ToUniversalTime().ToString("o")
                armTimeUtc        = $marker.armTimeUtc
                lastBootUpTimeUtc = $lastBootUtc.ToString("o")
                rebooted          = $rebooted
                downSeconds       = $downSeconds
                newBugchecks      = $stats.Total
                new9f             = $stats.NineF
                driverState       = $driverState
                soakFilter        = $marker.soakFilter
            }
            $result | ConvertTo-Json | Set-Content -Path $ResultPath -Encoding UTF8
            Write-Host "Recorded outcome: rebooted=$rebooted new9f=$($stats.NineF) driver=$driverState down=${downSeconds}s"
        }
    }
    catch {
        Write-Warning "record phase error: $_"
    }
    finally {
        # Self-clean so a later unrelated boot does not overwrite the result.
        schtasks.exe /Delete /TN $TaskName /F 2>$null | Out-Null
        Remove-Item -Path $MarkerPath -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

# verify mode.
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if (-not (Test-Path $ResultPath)) {
    throw "No result file at $ResultPath. The boot-time capture did not run - was the arm phase executed on this runner, and has it rebooted since?"
}

$result = Get-Content -Path $ResultPath -Raw | ConvertFrom-Json
Copy-Item -Path $ResultPath -Destination (Join-Path $OutputDir "shutdown-hang-result.json") -Force

Write-Host "---- Abrupt-shutdown-hang result ----"
Write-Host "rebooted     : $($result.rebooted)"
Write-Host "downSeconds  : $($result.downSeconds)"
Write-Host "newBugchecks : $($result.newBugchecks)"
Write-Host "new 0x9F     : $($result.new9f)"
Write-Host "driverState  : $($result.driverState)"
Write-Host "-------------------------------------"

$failures = @()
if (-not $result.rebooted) {
    $failures += "No fresh boot after the arm time; the abrupt reboot did not take effect."
}
if ([int]$result.new9f -gt 0) {
    $failures += "$($result.new9f) new 0x9F DRIVER_POWER_STATE_FAILURE bugcheck(s) after the abrupt reboot - the shutdown-hang regression is back."
}
if ("$($result.driverState)" -ne "Running") {
    $failures += "wnbd service state is '$($result.driverState)', expected 'Running' - the boot-start driver did not come back cleanly."
}
if ([double]$result.downSeconds -gt $MaxDownSeconds) {
    $failures += "System was down for $($result.downSeconds)s (> $MaxDownSeconds s), which suggests a stalled power transition."
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "SHUTDOWN-HANG CHECK FAILED:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    throw "Abrupt-shutdown-hang regression check failed."
}

Write-Host "PASS: abrupt reboot under in-flight I/O completed with 0 new 0x9F bugchecks and wnbd back RUNNING."
exit 0
