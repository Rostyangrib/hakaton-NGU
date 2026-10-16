param(
    [string]$SdkPath = '',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $SdkPath) {
    $SdkPath = if ($env:CANGJIE_HOME) {
        $env:CANGJIE_HOME
    } else {
        Join-Path $PSScriptRoot '.tools\cangjie-1.0.5\cangjie'
    }
}
$SdkPath = (Resolve-Path -LiteralPath $SdkPath).Path
$caseCompiler = Join-Path $SdkPath 'bin\cjc.exe'
if (-not (Test-Path -LiteralPath $caseCompiler)) {
    throw "cjc.exe not found: $caseCompiler"
}

# These variables affect this shell and its children, not system settings.
$env:CANGJIE_HOME = $SdkPath
$caseRuntimePaths = @(
    (Join-Path $SdkPath 'bin'),
    (Join-Path $SdkPath 'tools\bin'),
    (Join-Path $SdkPath 'tools\lib'),
    (Join-Path $SdkPath 'lib\windows_x86_64_cjnative'),
    (Join-Path $SdkPath 'runtime\lib\windows_x86_64_cjnative')
)
$env:PATH = ($caseRuntimePaths -join ';') + ';' + $env:PATH
$caseVersion = & $caseCompiler --version
if ($LASTEXITCODE -ne 0 -or ($caseVersion -join ' ') -notmatch '\b1\.0\.5\b') {
    throw "Expected Cangjie 1.0.5: $caseVersion"
}
Write-Output ($caseVersion -join "`n")

$caseBuildDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $caseBuildDir -Force | Out-Null
Push-Location -LiteralPath $PSScriptRoot
try {
    & $caseCompiler 'solution.cj' '-O2' '-o' (Join-Path $caseBuildDir 'solution.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Solution compilation failed' }
    if ($Test) {
        & $caseCompiler 'solution.cj' 'solution_test.cj' '--test' '--mock=off' '-O2' '-o' (Join-Path $caseBuildDir 'solution_tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Unit test compilation failed' }
        & (Join-Path $caseBuildDir 'solution_tests.exe') '--no-color' '--no-progress' 2>&1 | Tee-Object -FilePath (Join-Path $caseBuildDir 'unit-tests.log')
        if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed' }
    }
} finally {
    Pop-Location
}
