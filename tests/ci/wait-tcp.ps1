# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

Param(
    [Parameter(Mandatory=$true)] [string] $HostName,
    [Parameter(Mandatory=$true)] [int] $Port,
    [int] $TimeoutSeconds = 15,
    [int] $DelayMilliseconds = 100
)

$ErrorActionPreference = "Stop"
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$attempt = 0

while ((Get-Date) -lt $deadline) {
    $attempt++
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $connect = $client.BeginConnect($HostName, $Port, $null, $null)
        if ($connect.AsyncWaitHandle.WaitOne([TimeSpan]::FromMilliseconds($DelayMilliseconds))) {
            $client.EndConnect($connect)
            Write-Host "TCP endpoint ${HostName}:${Port} is ready after $attempt attempt(s)."
            exit 0
        }
    }
    catch {
        # Retry until the deadline; print only final failure below.
    }
    finally {
        $client.Close()
    }
    Start-Sleep -Milliseconds $DelayMilliseconds
}

throw "Timed out waiting for TCP endpoint ${HostName}:${Port} after $TimeoutSeconds second(s)."
