param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [string]$Blender = 'D:\SteamLibrary\steamapps\common\Blender\blender.exe'
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path $Executable).Path
$blenderExe = (Resolve-Path $Blender).Path
$fixtureScript = Join-Path $PSScriptRoot 'test-missing-asset-override.py'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $tempBase ('RenderMesh-missing-asset-override-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

function Invoke-Inspect([string[]]$arguments) {
    $output = Join-Path $root ([guid]::NewGuid().ToString('N') + '.json')
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $output
    [pscustomobject]@{
        ExitCode = $process.ExitCode
        Report = Get-Content $output -Raw -Encoding UTF8 | ConvertFrom-Json
    }
}

try {
    $fixture = Join-Path $root 'unused-missing-asset.blend'
    & $blenderExe --background --factory-startup --disable-autoexec --python $fixtureScript -- $fixture
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $fixture -PathType Leaf)) {
        throw 'Could not create the unused missing-asset Blender fixture.'
    }

    $dataDir = Join-Path $root 'data'
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    @{ blender_path = $blenderExe } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $dataDir 'config.json') -Encoding ascii

    $initial = Invoke-Inspect @('inspect', $fixture, '--json', '--data-dir', $dataDir)
    $warning = $initial.Report
    if ($initial.ExitCode -ne 1 -or $warning.pass -or -not $warning.can_ignore_asset_warnings -or
        $warning.asset_warnings_ignored -or $warning.missing.Count -lt 1) {
        throw "Initial inspection should warn and stop (exit 1), got $($initial.ExitCode)."
    }

    $forced = Invoke-Inspect @('inspect', $fixture, '--ignore-warnings', '--json',
        '--data-dir', $dataDir)
    $override = $forced.Report
    if ($forced.ExitCode -ne 0 -or -not $override.pass -or $override.can_ignore_asset_warnings -or
        -not $override.asset_warnings_ignored -or -not $override.validation_skipped_by_user -or
        $override.missing.Count -lt 1) {
        throw "Override should accept the project and keep the warning, got $($forced.ExitCode)."
    }

    Write-Output 'IGNORE_ALL_WARNINGS_OK'
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    if ($resolvedRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedRoot).StartsWith('RenderMesh-missing-asset-override-')) {
        [IO.Directory]::Delete($resolvedRoot, $true)
    }
}
