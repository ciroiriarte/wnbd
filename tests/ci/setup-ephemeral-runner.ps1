# Copyright (c) 2026 Ciro Iriarte
#
# Licensed under LGPL-2.1 (see LICENSE)

# Register this machine as an ON-DEMAND, EPHEMERAL GitHub Actions runner for the
# self-hosted-driver-verifier workflow, run one job, then auto-deregister.
#
# Why ephemeral: the verifier jobs need a real reboot (Special Pool / Force IRQL)
# and the shutdown-hang check reboots the box, so they cannot run on
# GitHub-hosted nodes and need a dedicated Windows VM. But a permanently
# registered runner on a PUBLIC repo is a standing risk (a fork PR could target
# self-hosted). An ephemeral runner accepts exactly ONE job and removes itself,
# so the VM is only connected to GitHub for the duration of a single dispatched
# job. Register at the start of a validation session, let the job run, done.
#
# Routing is by label: this runner carries 'wnbd-verifier', so it only ever
# picks up jobs whose runs-on lists that label - i.e. only the self-hosted
# workflow. Hosted CI (pr-tests, scheduled verification) is unaffected.
#
# The runner needs git for actions/checkout. If git is absent, a portable MinGit
# is fetched and put on PATH.
#
# Registration tokens are short-lived (~1 hour); mint a fresh one per session:
#   gh api -X POST repos/<owner>/<repo>/actions/runners/registration-token --jq .token
#
# Run this ELEVATED (driver install + Driver Verifier need admin). Under the
# QEMU guest agent (qm guest exec) the SYSTEM context is already elevated.

Param(
    [Parameter(Mandatory = $true)]
    [string] $Url,                                   # e.g. https://github.com/ciroiriarte/wnbd
    [Parameter(Mandatory = $true)]
    [string] $Token,                                 # registration token (short-lived)
    [string] $Labels = "wnbd-verifier",
    [string] $RunnerName = "wnbd-verifier-eph",
    [string] $RunnerVersion = "2.336.0",
    [string] $RunnerDir = "C:\actions-runner",
    [string] $GitDir = "C:\mingit",
    # git-for-windows MinGit asset version, e.g. 2.55.0.4 -> tag v2.55.0.windows.4
    [string] $MinGitVersion = "2.55.0.4",
    # Foreground blocks until the one job completes (handy for interactive use);
    # default launches the listener detached and returns, so an orchestrator can
    # then dispatch the workflow.
    [switch] $Foreground
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- git (required by actions/checkout) -------------------------------------
$GitCmdDir = Join-Path $GitDir "cmd"
if (-not (Get-Command git -ErrorAction SilentlyContinue) -and
    -not (Test-Path (Join-Path $GitCmdDir "git.exe"))) {
    # The tag encodes the build number as ".windows.N" while the asset name
    # keeps it as the 4th version component: 2.55.0.4 -> v2.55.0.windows.4.
    $parts = $MinGitVersion.Split(".")
    if ($parts.Count -ne 4) {
        throw "MinGitVersion '$MinGitVersion' must be 4 components (e.g. 2.55.0.4)."
    }
    $tag = "v$($parts[0]).$($parts[1]).$($parts[2]).windows.$($parts[3])"
    $asset = "MinGit-$MinGitVersion-64-bit.zip"
    $mingitUrl = "https://github.com/git-for-windows/git/releases/download/$tag/$asset"
    $mingitZip = Join-Path $env:TEMP $asset
    Write-Host "git not found; downloading MinGit from $mingitUrl"
    Invoke-WebRequest -Uri $mingitUrl -OutFile $mingitZip -UseBasicParsing
    New-Item -ItemType Directory -Force -Path $GitDir | Out-Null
    Expand-Archive -Path $mingitZip -DestinationPath $GitDir -Force
    Remove-Item $mingitZip -Force -ErrorAction SilentlyContinue
}
if (Test-Path (Join-Path $GitCmdDir "git.exe")) {
    # Put git on PATH for this process (inherited by run.cmd -> the job steps)
    # and persist it on the machine so later sessions have it too.
    if ($env:Path -notlike "*$GitCmdDir*") { $env:Path = "$GitCmdDir;$env:Path" }
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($machinePath -notlike "*$GitCmdDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$GitCmdDir;$machinePath", "Machine")
    }
}
$gitPath = (Get-Command git -ErrorAction SilentlyContinue).Source
if (-not $gitPath) { throw "git is still not available after MinGit setup." }
Write-Host "git: $gitPath"

# --- runner agent -----------------------------------------------------------
if (-not (Test-Path (Join-Path $RunnerDir "config.cmd"))) {
    New-Item -ItemType Directory -Force -Path $RunnerDir | Out-Null
    $asset = "actions-runner-win-x64-$RunnerVersion.zip"
    $runnerUrl = "https://github.com/actions/runner/releases/download/v$RunnerVersion/$asset"
    $runnerZip = Join-Path $env:TEMP $asset
    Write-Host "Downloading runner from $runnerUrl"
    Invoke-WebRequest -Uri $runnerUrl -OutFile $runnerZip -UseBasicParsing
    Expand-Archive -Path $runnerZip -DestinationPath $RunnerDir -Force
    Remove-Item $runnerZip -Force -ErrorAction SilentlyContinue
}

# --- configure (ephemeral, replace any prior registration) ------------------
$ConfigCmd = Join-Path $RunnerDir "config.cmd"
$RunCmd = Join-Path $RunnerDir "run.cmd"

Write-Host "Configuring ephemeral runner '$RunnerName' with labels '$Labels'."
& $ConfigCmd --unattended --ephemeral --replace `
    --url $Url --token $Token --labels $Labels --name $RunnerName --work _work
if ($LASTEXITCODE -ne 0) {
    throw "config.cmd failed with exit code $LASTEXITCODE."
}

if ($Foreground) {
    Write-Host "Starting runner in foreground (one job, then auto-deregister)."
    & $RunCmd
}
else {
    # Detached, with stdio redirected to files so the QEMU guest agent does not
    # block waiting on inherited pipes.
    $outLog = Join-Path $RunnerDir "run.out.log"
    $errLog = Join-Path $RunnerDir "run.err.log"
    $p = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "`"$RunCmd`"" `
        -WorkingDirectory $RunnerDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    Start-Sleep -Seconds 5
    if ($p.HasExited) {
        throw "Runner listener exited immediately (code $($p.ExitCode)). See $outLog / $errLog."
    }
    Write-Host "RUNNER_STARTED pid=$($p.Id) name=$RunnerName labels=$Labels"
    Write-Host "It will accept one job, then remove itself from the repo."
}
