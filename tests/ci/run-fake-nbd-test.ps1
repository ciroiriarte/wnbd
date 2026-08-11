Param(
  [string]$TestExecutable = "libwnbd_tests.exe",
  [string]$PythonExecutable = "python",
  [string]$HostName = "127.0.0.1",
  [int]$Port = 10810,
  [string]$Scenario = "assert-disc",
  [string]$ExportName = "test",
  [string]$GTestFilter = "TestNbd.TestMap",
  [string]$OutputDir = "ci-artifacts"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$ServerStdout = Join-Path $OutputDir "fake-nbd-$Scenario.out.log"
$ServerStderr = Join-Path $OutputDir "fake-nbd-$Scenario.err.log"
$TestXml = Join-Path $OutputDir "fake-nbd-$Scenario.xml"
Remove-Item $ServerStdout, $ServerStderr, $TestXml -ErrorAction SilentlyContinue

$ServerArgs = @(
  "tests\nbd_protocol_harness\fake_nbd_server.py",
  "--host", $HostName,
  "--port", $Port,
  "--scenario", $Scenario,
  "--max-requests", "128"
)

$Server = Start-Process `
  -FilePath $PythonExecutable `
  -ArgumentList $ServerArgs `
  -RedirectStandardOutput $ServerStdout `
  -RedirectStandardError $ServerStderr `
  -PassThru `
  -WindowStyle Hidden

try {
  $Deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $Deadline) {
    if ((Test-Path $ServerStdout) -and
        ((Get-Content $ServerStdout -Raw) -match "listening")) {
      break
    }
    if ($Server.HasExited) {
      throw "Fake NBD server exited before becoming ready. Exit code: $($Server.ExitCode)"
    }
    Start-Sleep -Milliseconds 200
  }
  if (!(Test-Path $ServerStdout) -or
      !((Get-Content $ServerStdout -Raw) -match "listening")) {
    throw "Timed out waiting for fake NBD server readiness line."
  }

  & $TestExecutable `
    --gtest_filter=$GTestFilter `
    --gtest_output=xml:$TestXml `
    --nbd-export-name $ExportName `
    --nbd-hostname $HostName `
    --nbd-port $Port
  $TestExit = $LASTEXITCODE

  if (!$Server.WaitForExit(20000)) {
    throw "Fake NBD server did not exit after $GTestFilter completed."
  }
  $ServerExit = $Server.ExitCode

  Write-Host "Fake NBD server stdout:"
  if (Test-Path $ServerStdout) { Get-Content $ServerStdout }
  Write-Host "Fake NBD server stderr:"
  if (Test-Path $ServerStderr) { Get-Content $ServerStderr }

  if ($TestExit) {
    throw "$GTestFilter failed with exit code $TestExit."
  }
  if ($ServerExit) {
    throw "Fake NBD server scenario '$Scenario' failed with exit code $ServerExit."
  }
}
finally {
  if (!$Server.HasExited) {
    Stop-Process -Id $Server.Id -Force
  }
}
