Param(
  [switch]$Clean
)

$scriptLocation = [System.IO.Path]::GetDirectoryName(
    $myInvocation.MyCommand.Definition)
$ErrorActionPreference = "Stop"

# Enforce Tls1.2, as most modern websites require it
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$nugetUrl = "https://dist.nuget.org/win-x86-commandline/v4.5.1/nuget.exe"

$depsDir = "$scriptLocation\vstudio\deps"
$nugetPath = "$depsDir\nuget.exe"
$depsConfig = "$scriptLocation\packages.config"
$completeFlag = "$depsDir\complete"
$depsHashPath = "$depsDir\packages.sha256"

# Use the .NET SHA256 API rather than Get-FileHash. This project's MSBuild
# invocation runs get_dependencies.ps1 through a powershell.exe whose
# PSModulePath omits the system module dir, so Get-FileHash (from
# Microsoft.PowerShell.Utility) fails to load with a CommandNotFoundException.
# The .NET API needs no module loading and yields the same uppercase hex.
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $currentDepsHash = [System.BitConverter]::ToString(
        $sha256.ComputeHash([System.IO.File]::ReadAllBytes($depsConfig))).Replace("-", "")
}
finally {
    $sha256.Dispose()
}

function safe_exec($cmd) {
    cmd /c "$cmd 2>&1"
    $exitCode = $LASTEXITCODE
    if ($exitCode) {
        throw "Command failed: $cmd. Exit code: $exitCode"
    }
}

if ($Clean -and (test-path $depsDir)) {
    rm -recurse -force $depsDir
    Write-Host "Cleaning up dependencies: $depsDir"
}

mkdir -force $depsDir
if (!(test-path $nugetPath)) {
    Write-Host "Fetching nuget from $nugetUrl."
    Invoke-WebRequest $nugetUrl -OutFile $nugetPath
}

if ((test-path $completeFlag) -and (test-path $depsHashPath)) {
    $storedDepsHash = (Get-Content $depsHashPath -Raw).Trim()
    if ($storedDepsHash -ne $currentDepsHash) {
        Write-Host "Dependency manifest changed; refreshing NuGet dependencies."
        Remove-Item -Force $completeFlag
    }
}
elseif (test-path $completeFlag) {
    Write-Host "Dependency completion marker predates hashing; refreshing NuGet dependencies."
    Remove-Item -Force $completeFlag
}

if (!(test-path $completeFlag)) {
    Write-Host "Retrieving dependencies."
    safe_exec "$nugetPath install $depsConfig -OutputDirectory `"$depsDir`""
    Set-Content $completeFlag "Finished retrieving dependencies."
    Set-Content $depsHashPath $currentDepsHash
}
else {
    write-host "Nuget dependencies already fetched."
}

