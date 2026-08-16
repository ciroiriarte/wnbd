# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

# Arm phase of the abrupt-shutdown-hang regression check.
#
# Background: WnbdHwAdapterControl (driver/scsi_driver_extensions.c) used to
# advertise ScsiStopAdapter support but no-op it. On an ABRUPT reboot with an
# active mapping and in-flight I/O (the userspace daemon dying with the power
# transition), the device never quiesced, the PnP power transition hit its 600s
# watchdog, and the box bugchecked 0x9F DRIVER_POWER_STATE_FAILURE. Commit
# b4a2d29 handles ScsiStopAdapter by draining/aborting I/O at PASSIVE_LEVEL so
# the transition completes.
#
# A GitHub Actions job cannot reboot its own runner and keep running, so the
# check is split in two:
#   * arm    (this script) - starts an in-flight I/O soak, records a baseline
#            bugcheck count, registers a boot-time task that captures the
#            outcome, then triggers an abrupt reboot on a short delay and exits
#            green. The reboot fires while the soak is still hammering I/O.
#   * verify (shutdown-hang-verify.ps1 -Mode verify) - a later dispatch reads
#            the outcome the boot-time task recorded and fails if a new 0x9F
#            bugcheck appeared or the driver did not come back.
#
# The pass/fail signal is the 0x9F bugcheck count, NOT reboot duration: under
# Special Pool a healthy verifier boot already takes 5-8 minutes, so timing
# cannot distinguish a real hang from a slow boot. Before the fix this scenario
# produced a 0x9F every time; after it, zero.

Param(
    [string] $SoakBinary = "libwnbd_tests.exe",
    # A soak test that maintains an active mapping with continuous I/O for its
    # whole duration. DisjointRegionsNoCorruption writes/reads back the disk
    # under load, which keeps requests in flight.
    [string] $SoakFilter = "TestNbdSoak.DisjointRegionsNoCorruption",
    [string] $ExportName = "test_5tb",
    [string] $HostName = "127.0.0.1",
    [int]    $Port = 10809,
    # Keep the soak running well past the reboot delay so I/O is genuinely
    # in flight when the power transition fires.
    [int]    $SoakSeconds = 240,
    [int]    $SoakThreads = 16,
    [int]    $DispatcherThreads = 4,
    # Short enough that the reboot fires soon after this job goes green, long
    # enough for the job to finish first so GitHub does not mark it failed.
    [int]    $RebootDelaySeconds = 20,
    [string] $WorkDir = "C:\wnbd-ci"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# Resolve the soak binary to an absolute path; the detached process keeps
# running after this step ends, and its cwd is not guaranteed.
$SoakBinaryResolved = (Get-Command $SoakBinary -ErrorAction Stop).Source

# The verify script must survive the reboot independently of the runner
# workspace, so stage it next to the marker/result files.
$VerifyScriptSrc = Join-Path $PSScriptRoot "shutdown-hang-verify.ps1"
$VerifyScriptDst = Join-Path $WorkDir "shutdown-hang-verify.ps1"
Copy-Item -Path $VerifyScriptSrc -Destination $VerifyScriptDst -Force

function Get-BugcheckStats {
    # Returns @{ Total = <int>; NineF = <int> } for BugCheck reports (Event
    # 1001, Microsoft-Windows-WER-SystemErrorReporting). 0x9F is the shutdown
    # hang signature.
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
            if ($e.Message -match '0x0*9[fF]\b' -or $e.Message -match '0x0{0,6}9f') {
                $nineF++
            }
        }
    }
    catch {
        Write-Warning "Could not read BugCheck events: $_"
    }
    return @{ Total = $total; NineF = $nineF }
}

# Baseline is counted from boot so a pre-existing bugcheck from an earlier run
# is never blamed on this one; the verify step only compares against events
# newer than the arm time.
$BootTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$Baseline = Get-BugcheckStats -Since $BootTime
$ArmTimeUtc = (Get-Date).ToUniversalTime()

Write-Host "Baseline since boot ($BootTime): $($Baseline.Total) bugcheck(s), $($Baseline.NineF) of code 0x9F."

# Launch the in-flight I/O soak, detached, so it keeps running after this step
# returns and is still doing I/O when the reboot fires.
$env:WNBD_SOAK_SECONDS = "$SoakSeconds"
$env:WNBD_SOAK_THREADS = "$SoakThreads"
$env:WNBD_DISPATCHER_THREADS = "$DispatcherThreads"

$SoakLog = Join-Path $WorkDir "shutdown-hang-soak.log"
$SoakArgs = @(
    "--gtest_filter=$SoakFilter",
    "--nbd-export-name", $ExportName,
    "--nbd-hostname", $HostName,
    "--nbd-port", "$Port"
)
Write-Host "Launching detached soak: $SoakBinaryResolved $($SoakArgs -join ' ')"
$SoakProc = Start-Process -FilePath $SoakBinaryResolved -ArgumentList $SoakArgs `
    -RedirectStandardOutput $SoakLog -RedirectStandardError "$SoakLog.err" `
    -PassThru -WindowStyle Hidden

# Confirm a mapping actually came up and I/O is flowing before we commit to the
# reboot; arming with no in-flight I/O would silently test nothing.
$mapped = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 1
    if ($SoakProc.HasExited) {
        throw "Soak process exited early (code $($SoakProc.ExitCode)) before a mapping was confirmed. See $SoakLog."
    }
    # wnbd-client writes INFO logs to stderr, so guard against a native-command
    # error record surfacing as a terminating error under some PS hosts.
    $list = ""
    try { $list = (& wnbd-client list 2>&1 | Out-String) } catch { $list = "$_" }
    if ($list -match [regex]::Escape($ExportName) -or $list -match 'Connected') {
        $mapped = $true
        break
    }
}
if (-not $mapped) {
    Stop-Process -Id $SoakProc.Id -Force -ErrorAction SilentlyContinue
    throw "No active WNBD mapping observed within 30s; refusing to arm the reboot with no in-flight I/O."
}
Write-Host "Active mapping confirmed; soak PID $($SoakProc.Id) is generating in-flight I/O."

$ShutdownInitUtc = $ArmTimeUtc.AddSeconds($RebootDelaySeconds)
$Marker = [ordered]@{
    armTimeUtc          = $ArmTimeUtc.ToString("o")
    shutdownInitUtc     = $ShutdownInitUtc.ToString("o")
    rebootDelaySeconds  = $RebootDelaySeconds
    baselineBugchecks   = $Baseline.Total
    baseline9f          = $Baseline.NineF
    soakPid             = $SoakProc.Id
    soakFilter          = $SoakFilter
}
$MarkerPath = Join-Path $WorkDir "shutdown-hang-marker.json"
$Marker | ConvertTo-Json | Set-Content -Path $MarkerPath -Encoding UTF8
Write-Host "Wrote marker: $MarkerPath"

# Register a boot-time task to capture the outcome. It runs as SYSTEM at
# startup, records the result, and unregisters itself. The default WorkDir has
# no spaces, so the /TR command needs no embedded quotes (which schtasks parses
# unreliably when passed through from PowerShell).
if ($WorkDir -match '\s' -or $VerifyScriptDst -match '\s') {
    throw "WorkDir '$WorkDir' contains spaces; the boot-time task command cannot be quoted reliably. Use a space-free path."
}
$TaskName = "WnbdShutdownHangRecord"
$TaskCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File $VerifyScriptDst -Mode record -WorkDir $WorkDir"
schtasks.exe /Create /TN $TaskName /TR $TaskCmd /SC ONSTART /RU SYSTEM /RL HIGHEST /F | Out-Null
Write-Host "Registered boot-time capture task '$TaskName'."

# Remove any stale result so the verify step cannot read a previous run's file.
$ResultPath = Join-Path $WorkDir "shutdown-hang-result.json"
Remove-Item -Path $ResultPath -Force -ErrorAction SilentlyContinue

Write-Host "Triggering abrupt reboot in $RebootDelaySeconds second(s) while I/O is in flight."
# /f forces a hard reboot without waiting on applications - this is the abrupt
# transition that used to hang.
shutdown.exe /r /f /t $RebootDelaySeconds /c "WNBD shutdown-hang regression check (abrupt reboot under I/O)" | Out-Null

Write-Host "Armed. This job will now go green; the runner reboots in $RebootDelaySeconds second(s)."
Write-Host "After it comes back, dispatch this workflow with mode=shutdown-hang-verify to read the outcome."
exit 0
