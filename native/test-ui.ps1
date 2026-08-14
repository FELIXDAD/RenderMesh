[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = Split-Path -Parent $nativeRoot
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tools') -Directory |
    Where-Object Name -Like 'llvm-mingw-*-ucrt-x86_64' |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $toolchain) {
    throw 'Portable llvm-mingw toolchain not found under tools\.'
}

$compiler = Join-Path $toolchain.FullName 'bin\clang++.exe'
$buildDir = Join-Path $nativeRoot 'build\tests'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$output = Join-Path $buildDir 'UiModelTests.exe'

Push-Location $repoRoot
try {
    & $compiler -std=c++20 -DUNICODE -D_UNICODE -Inative/src `
        -Wall -Wextra -Wpedantic -O0 -g `
        native/tests/UiModelTests.cpp native/src/UiModel.cpp `
        -static -static-libgcc -static-libstdc++ -o $output
    if ($LASTEXITCODE -ne 0) { throw 'UI model test build failed.' }
    & $output
    if ($LASTEXITCODE -ne 0) { throw 'UI model tests failed.' }
} finally {
    Pop-Location
}
