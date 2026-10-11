[CmdletBinding()]
param(
    [string]$Executable = '',
    # RenderMesh-dev.exe built from the same sources with -DevTools. It runs the
    # embedded self-test and the UI regression suite; the shipped exe runs every
    # functional test.
    [string]$DevExecutable = '',
    [string]$OutputFileName = 'RenderMesh-Windows-x64-0.3.0.zip',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $nativeRoot))
$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
if (-not $distRoot.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Distribution path escaped the repository.'
}

if (-not $SkipBuild) {
    & (Join-Path $nativeRoot 'build.ps1') -Configuration Release | Out-Null
    & (Join-Path $nativeRoot 'build.ps1') -Configuration Release -DevTools | Out-Null
}
$exe = if ($Executable) { [IO.Path]::GetFullPath($Executable) } else {
    Join-Path $nativeRoot 'build\Release\RenderMesh.exe'
}
$devExe = if ($DevExecutable) { [IO.Path]::GetFullPath($DevExecutable) } else {
    Join-Path $nativeRoot 'build\Release\RenderMesh-dev.exe'
}
if (-not $exe.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package executable must stay inside the repository.'
}
foreach ($required in $exe, $devExe) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Release executable was not produced: $required"
    }
}
if ((Get-Item -LiteralPath $devExe).VersionInfo.ProductVersion -ne
        (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion) {
    throw 'RenderMesh.exe and RenderMesh-dev.exe must come from the same version.'
}
$productVersion = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if ($productVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Release executable has an invalid product version: $productVersion"
}
$expectedOutputFileName = "RenderMesh-Windows-x64-$productVersion.zip"
if ($OutputFileName -ne $expectedOutputFileName) {
    throw "Package name must match the executable version: $expectedOutputFileName"
}
$standaloneExe = Join-Path $distRoot "RenderMesh-$productVersion.exe"
$sourceExeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
if (Test-Path -LiteralPath $standaloneExe -PathType Leaf) {
    $publishedExeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $standaloneExe).Hash
    if ($publishedExeHash -ne $sourceExeHash) {
        throw "Version $productVersion already has a different published EXE. Bump the version before packaging another build."
    }
}

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$stage = [IO.Path]::GetFullPath((Join-Path $distRoot ('stage-' + [guid]::NewGuid().ToString('N'))))
if (-not $stage.StartsWith($distRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging path escaped dist.'
}
New-Item -ItemType Directory -Path $stage | Out-Null

try {
    $selfTestReport = Join-Path $stage 'self-test.txt'
    $selfTest = Start-Process -FilePath $devExe `
        -ArgumentList '--self-test', '--no-dialog', '--report', $selfTestReport `
        -WindowStyle Hidden -PassThru -Wait
    if ($selfTest.ExitCode -ne 0) {
        $details = if (Test-Path -LiteralPath $selfTestReport) {
            Get-Content -Raw -Encoding UTF8 -LiteralPath $selfTestReport
        } else {
            'self-test report was not produced'
        }
        throw "Refusing to package a failing executable (exit $($selfTest.ExitCode)): $details"
    }
    Remove-Item -LiteralPath $selfTestReport -Force

    & (Join-Path $nativeRoot 'verify-ui.ps1') -SkipBuild -Executable $devExe
    & (Join-Path $nativeRoot 'test-large-project-transfer.ps1')
    & (Join-Path $nativeRoot 'test-lan-update-package.ps1') -ExePath $exe
    & (Join-Path $nativeRoot 'test-update-apply.ps1') -ExePath $exe
    & (Join-Path $nativeRoot 'test-single-instance.ps1') -ExePath $exe
    & (Join-Path $nativeRoot 'test-auto-pairing.ps1') -ExePath $exe -RunSeconds 8
    & (Join-Path $nativeRoot 'test-peer-exchange.ps1') -ExePath $exe -RunSeconds 7
    & (Join-Path $nativeRoot 'test-peer-state-refresh.ps1') -ExePath $exe
    & (Join-Path $nativeRoot 'test-decentralized-discovery.ps1') -ExePath $exe
    & (Join-Path $nativeRoot 'test-peer-idle-readiness.ps1') -ExePath $exe
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        & $python.Source (Join-Path $nativeRoot 'test-legacy-job-import.py') $exe
        if ($LASTEXITCODE -ne 0) { throw 'Legacy job history import test failed.' }
    } else {
        Write-Warning 'Python was not found; the legacy job history import test did not run.'
    }

    if (-not (Test-Path -LiteralPath $standaloneExe -PathType Leaf)) {
        Copy-Item -LiteralPath $exe -Destination $standaloneExe
    }

    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'RenderMesh.exe')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'QUICKSTART-zh-CN.txt') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $stage

    if ([IO.Path]::GetFileName($OutputFileName) -ne $OutputFileName -or
        -not $OutputFileName.EndsWith('.zip', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package output name must be a simple .zip file name.'
    }
    $zip = Join-Path $distRoot $OutputFileName
    Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $stage).FullName `
        -DestinationPath $zip -CompressionLevel Optimal -Force
    $sha256 = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($zip)
    try {
        $hash = -join ($sha256.ComputeHash($stream) | ForEach-Object { $_.ToString('X2') })
    } finally {
        $stream.Dispose()
        $sha256.Dispose()
    }
    [pscustomobject]@{
        FullName = $zip
        StandaloneExe = $standaloneExe
        ExeLength = (Get-Item -LiteralPath $exe).Length
        Length = (Get-Item -LiteralPath $zip).Length
        SHA256 = $hash
    }
} finally {
    if (Test-Path -LiteralPath $stage) {
        $verifiedStage = [IO.Path]::GetFullPath($stage)
        if (-not $verifiedStage.StartsWith($distRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to remove an unverified staging path.'
        }
        Remove-Item -LiteralPath $verifiedStage -Recurse -Force
    }
}
