# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

# One-time setup for a self-hosted WNBD verification runner.
#
# Special Pool and Force IRQL checking cannot be enabled without a reboot, so
# they are impossible on GitHub-hosted runners (which cannot reboot mid-job).
# Instead, dedicate a self-hosted Windows VM to verification: run this script
# once, REBOOT, and the strong Driver Verifier flags stay active for wnbd.sys
# across boots. That VM then serves as the runner for the
# self-hosted-driver-verifier workflow, which runs the concurrency soak under
# real Special Pool + Force IRQL without any per-job reboot.
#
# Default flags (0x3B):
#   0x01 Special pool          - catches out-of-bounds and use-after-free
#   0x02 Force IRQL checking   - catches pageable access at raised IRQL
#   0x08 Pool tracking         - catches leaks and mismatched frees
#   0x10 I/O verification      - catches IRP / I/O misuse
#   0x20 Deadlock detection    - catches lock-ordering violations
#
# For a low-resource-injection pass (0x04, exercises the fetch-lock/MDL and
# SRB-pool allocation-failure branches), enable it on a SEPARATE run/VM with
# -Flags 0x3F and drive a failure-tolerant workload -- the integrity soak
# asserts zero I/O errors and would (correctly) fail under injected failures.
#
# To disable verification later:  verifier.exe /reset   (then reboot)

Param(
    [string] $DriverName = "wnbd.sys",
    [string] $Flags = "0x3B"
)

$ErrorActionPreference = "Stop"

# Driver Verifier and the later driver install both require an elevated,
# administrative context.
$Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
if (-not $Principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}

Write-Host "Configuring Driver Verifier for $DriverName with flags $Flags."
verifier.exe /flags $Flags /driver $DriverName

Write-Host ""
verifier.exe /querysettings
Write-Host ""
Write-Host "Driver Verifier is configured but NOT yet active."
Write-Host "REBOOT now for the settings to take effect, then confirm with:"
Write-Host "    verifier.exe /query"
Write-Host "The query must list $DriverName and show the flags as active."
Write-Host "To disable later:  verifier.exe /reset   (then reboot)."
