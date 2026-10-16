param(
    [string]$CompilerPath = '',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $CompilerPath) {
    $caseBundledCompiler = Join-Path $PSScriptRoot '.tools/w64devkit/bin/g++.exe'
    $CompilerPath = if (Test-Path -LiteralPath $caseBundledCompiler) {
        $caseBundledCompiler
    } else {
        (Get-Command 'g++.exe' -ErrorAction Stop).Source
    }
}
$CompilerPath = (Resolve-Path -LiteralPath $CompilerPath).Path
$env:PATH = (Split-Path -Parent $CompilerPath) + ';' + $env:PATH
& $CompilerPath --version
if ($LASTEXITCODE -ne 0) { throw 'Cannot run g++' }
$caseBuildDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $caseBuildDir -Force | Out-Null
Push-Location -LiteralPath $PSScriptRoot
try {
    & $CompilerPath '-std=c++20' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-static' 'solution.cpp' '-o' (Join-Path $caseBuildDir 'solution-cpp.exe')
    if ($LASTEXITCODE -ne 0) { throw 'C++ solution compilation failed' }
    if ($Test) {
        & $CompilerPath '-std=c++20' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-static' 'solution_cpp_test.cpp' '-o' (Join-Path $caseBuildDir 'solution-cpp-tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'C++ unit test compilation failed' }
        & (Join-Path $caseBuildDir 'solution-cpp-tests.exe') 2>&1 | Tee-Object -FilePath (Join-Path $caseBuildDir 'unit-tests-cpp-windows.log')
        if ($LASTEXITCODE -ne 0) { throw 'C++ unit tests failed' }
    }
} finally {
    Pop-Location
}
