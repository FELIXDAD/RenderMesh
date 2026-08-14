[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $nativeRoot))
$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
if (-not $distRoot.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Distribution path escaped the repository.'
}

& (Join-Path $nativeRoot 'build.ps1') -Configuration Release
$exe = Join-Path $nativeRoot 'build\Release\RenderMesh.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'Release executable was not produced.'
}

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$stage = [IO.Path]::GetFullPath((Join-Path $distRoot ('stage-' + [guid]::NewGuid().ToString('N'))))
if (-not $stage.StartsWith($distRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging path escaped dist.'
}
New-Item -ItemType Directory -Path $stage | Out-Null

try {
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'RenderMesh.exe')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'QUICKSTART-zh-CN.txt') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $stage

    $zip = Join-Path $distRoot 'RenderMesh-Windows-x64-0.2.10.zip'
    Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $stage).FullName `
        -DestinationPath $zip -CompressionLevel Optimal -Force
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zip
    [pscustomobject]@{
        FullName = $zip
        Length = (Get-Item -LiteralPath $zip).Length
        SHA256 = $hash.Hash
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
