# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

# Starts qemu-storage-daemon as an NBD server and waits until it is actually
# accepting connections, failing fast with the daemon's own output when it
# does not. The previous approach started the daemon and blindly polled the
# TCP port, so a daemon that died on startup produced no diagnostics at all --
# just an opaque "timed out" after the wait expired.

Param(
    [string] $ImagePath = "test_5tb.qcow2",
    [string] $HostName = "127.0.0.1",
    [int]    $Port = 10809,
    [string] $ExportName = "test_5tb",
    [string] $OutputDir = "ci-artifacts",
    [int]    $TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$stdoutLog = Join-Path $OutputDir "qemu-storage-daemon.out.log"
$stderrLog = Join-Path $OutputDir "qemu-storage-daemon.err.log"

# A missing runtime DLL or an otherwise broken binary surfaces here rather than
# as a silent daemon that never binds its port.
Write-Host "qemu-storage-daemon version:"
& qemu-storage-daemon --version

# Build the argument list as an array. The daemon step used to assign the
# command line to $args -- an automatic PowerShell variable -- as a single
# concatenated string, which is fragile; an explicit array is passed through
# Start-Process cleanly.
$qsdArgs = @(
    "--blockdev", "driver=file,node-name=file,filename=$ImagePath",
    "--blockdev", "driver=qcow2,node-name=qcow2,file=file",
    "--nbd-server", "addr.type=inet,addr.host=$HostName,addr.port=$Port",
    "--export", "type=nbd,id=export,node-name=qcow2,name=$ExportName,writable=on"
)

Write-Host "Starting qemu-storage-daemon $($qsdArgs -join ' ')"
# -NoNewWindow keeps the daemon attached to the runner's existing console;
# launching it in a fresh window on a headless runner is a likely reason the
# process appeared to start yet never served the port.
$proc = Start-Process qemu-storage-daemon `
    -ArgumentList $qsdArgs `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -NoNewWindow `
    -PassThru

function Show-DaemonLogs {
    Write-Host "qemu-storage-daemon stdout:"
    if (Test-Path $stdoutLog) { Get-Content $stdoutLog }
    Write-Host "qemu-storage-daemon stderr:"
    if (Test-Path $stderrLog) { Get-Content $stderrLog }
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) {
        Show-DaemonLogs
        throw "qemu-storage-daemon exited before binding ${HostName}:${Port}. Exit code: $($proc.ExitCode)."
    }

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $connect = $client.BeginConnect($HostName, $Port, $null, $null)
        if ($connect.AsyncWaitHandle.WaitOne([TimeSpan]::FromMilliseconds(200))) {
            $client.EndConnect($connect)
            Write-Host "qemu NBD server is ready on ${HostName}:${Port} (pid $($proc.Id))."
            exit 0
        }
    }
    catch {
        # Keep polling until the deadline; only the final failure is reported.
    }
    finally {
        $client.Close()
    }
    Start-Sleep -Milliseconds 200
}

Show-DaemonLogs
if (!$proc.HasExited) {
    Write-Host "qemu-storage-daemon (pid $($proc.Id)) is still running but never bound ${HostName}:${Port}."
}
throw "Timed out waiting for qemu NBD server on ${HostName}:${Port} after $TimeoutSeconds second(s)."
