Param(
    [Parameter(Mandatory=$true)] [string] $XmlPath,
    [string] $Title = "WNBD test summary"
)

function Add-StepSummaryLine($Line) {
    $Line | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
}

Add-StepSummaryLine "## $Title"

if (!(Test-Path $XmlPath)) {
    Add-StepSummaryLine "- Test XML not found: $XmlPath"
    exit 0
}

try {
    [xml]$Report = Get-Content $XmlPath
    $tests = $Report.testsuites.tests
    $failures = $Report.testsuites.failures
    $disabled = $Report.testsuites.disabled
    $errors = $Report.testsuites.errors
    $time = $Report.testsuites.time
    Add-StepSummaryLine "- Test XML: $XmlPath"
    Add-StepSummaryLine "- Tests: $tests"
    Add-StepSummaryLine "- Failures: $failures"
    Add-StepSummaryLine "- Errors: $errors"
    Add-StepSummaryLine "- Disabled/skipped: $disabled"
    Add-StepSummaryLine "- Runtime: $time seconds"
}
catch {
    Add-StepSummaryLine "- Could not parse test XML ${XmlPath}: $_"
}
