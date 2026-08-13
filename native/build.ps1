[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release'
)

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
$bin = Join-Path $toolchain.FullName 'bin'
$compiler = Join-Path $bin 'clang++.exe'
$resourceCompiler = Join-Path $bin 'llvm-windres.exe'
$buildDir = Join-Path $nativeRoot "build\$Configuration"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$resource = Join-Path $buildDir 'RenderMesh.res.o'
& $resourceCompiler --codepage=65001 (Join-Path $nativeRoot 'RenderMesh.rc') -O coff -o $resource
if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

$common = @(
    '-std=c++20', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00',
    '-Inative/src', '-Inative/third_party', '-Wall', '-Wextra', '-Wpedantic',
    '-Wno-language-extension-token'
)
if ($Configuration -eq 'Release') {
    $common += @('-Os','-DNDEBUG','-ffunction-sections','-fdata-sections')
} else {
    $common += @('-O0','-g')
}
$sources = @(
    (Join-Path $nativeRoot 'src\Core.cpp'),
    (Join-Path $nativeRoot 'src\Node.cpp'),
    (Join-Path $nativeRoot 'src\WinMain.cpp'),
    $resource
)
$output = Join-Path $buildDir 'RenderMesh.exe'
$link = @(
    '-municode','-mwindows','-static','-static-libgcc','-static-libstdc++',
    '-Wl,--gc-sections','-Wl,--subsystem,windows','-o',$output,
    '-lws2_32','-liphlpapi','-lcomctl32','-lcomdlg32','-lshell32','-lole32','-lbcrypt','-luuid'
)
if ($Configuration -eq 'Release') { $link += '-s' }

Push-Location $repoRoot
try {
    & $compiler @common @sources @link
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
} finally {
    Pop-Location
}
Get-Item -LiteralPath $output | Select-Object FullName,Length,LastWriteTime
