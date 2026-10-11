[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    # Builds RenderMesh-dev.exe: the same sources plus the embedded self-test
    # suite, UI regression hooks and verification CLI used by the test scripts.
    # The distributed RenderMesh.exe never contains them.
    [switch]$DevTools
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
$buildDir = if ($OutputDirectory) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    Join-Path $nativeRoot "build\$Configuration"
}
$repoRootFull = [IO.Path]::GetFullPath($repoRoot)
if (-not $buildDir.StartsWith($repoRootFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build output must stay inside the repository.'
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$resource = Join-Path $buildDir 'RenderMesh.res.o'
& $resourceCompiler --codepage=65001 (Join-Path $nativeRoot 'RenderMesh.rc') -O coff -o $resource
if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

$common = @(
    '-std=c++20', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00',
    '-Inative/src', '-Inative/third_party', '-Wall', '-Wextra', '-Wpedantic',
    '-Wno-language-extension-token'
)
if ($DevTools) { $common += '-DRM_DEV_TOOLS' }
if ($Configuration -eq 'Release') {
    $common += @('-Os','-DNDEBUG','-flto','-ffunction-sections','-fdata-sections')
} else {
    $common += @('-O0','-g')
}
$sources = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'src') -Filter '*.cpp' |
    Sort-Object Name | ForEach-Object FullName
$sources += $resource
$output = Join-Path $buildDir ($(if ($DevTools) { 'RenderMesh-dev.exe' } else { 'RenderMesh.exe' }))
$link = @(
    '-municode','-mwindows','-static','-static-libgcc','-static-libstdc++',
    '-Wl,--gc-sections','-Wl,--subsystem,windows','-o',$output,
    '-lws2_32','-liphlpapi','-lcomctl32','-lcomdlg32','-lshell32','-lole32','-lbcrypt',
    '-ladvapi32','-ldxgi','-ld2d1','-ldwrite','-lwindowscodecs','-luxtheme','-lwinhttp'
)
if ($Configuration -eq 'Release') { $link += @('-Wl,--icf=safe','-s') }

Push-Location $repoRoot
try {
    & $compiler @common @sources @link
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
} finally {
    Pop-Location
}
Get-Item -LiteralPath $output | Select-Object FullName,Length,LastWriteTime
