[CmdletBinding()]
param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot (
    'build\lan-update-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

function Get-FreeUdpPort {
    $client = [System.Net.Sockets.UdpClient]::new(0)
    $port = ([System.Net.IPEndPoint]$client.Client.LocalEndPoint).Port
    $client.Dispose()
    return $port
}

$port = Get-FreeTcpPort
$token = '00112233445566778899aabbccddeeff'
$config = [ordered]@{
    node_id = 'update-package-server'
    node_name = 'UPDATE-PACKAGE-SERVER'
    invite_code = '111111'
    cluster_token = $token
    pairing_pending = $false
    share_enabled = $false
    idle_after_seconds = 180
    http_port = $port
    discovery_port = Get-FreeUdpPort
    blender_path = ''
    render_software_profiles = @()
    last_render_software_profile_id = ''
    manual_peer_endpoint = ''
    theme = 'dark'
    close_behavior = 'exit'
    auto_start = $false
    silent_auto_start = $false
}
$json = $config | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText(
    (Join-Path $fixtureRoot 'config.json'), $json,
    [Text.UTF8Encoding]::new($false))

$process = $null
try {
    $process = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless', '--run-seconds', '20',
        '--data-dir', $fixtureRoot, '--port', [string]$port,
        '--no-update-check', '--no-autostart-registration') `
        -WindowStyle Hidden -PassThru
    $base = "http://127.0.0.1:$port"
    $headers = @{ 'X-RM-Token' = $token }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $manifestResponse = $null
    while ([DateTime]::UtcNow -lt $deadline -and -not $manifestResponse) {
        if ($process.HasExited) {
            throw "RenderMesh update fixture exited early with $($process.ExitCode)"
        }
        try {
            $manifestResponse = Invoke-WebRequest -UseBasicParsing `
                -Uri "$base/v1/update/manifest" -Headers $headers -TimeoutSec 3
        } catch {
            Start-Sleep -Milliseconds 150
        }
    }
    if (-not $manifestResponse -or $manifestResponse.StatusCode -ne 200) {
        throw 'production update manifest endpoint did not become ready'
    }
    $manifest = $manifestResponse.Content | ConvertFrom-Json
    if (-not $manifest.sha256 -or [uint64]$manifest.size -le 0) {
        throw 'production update manifest is incomplete'
    }
    $servedVersion = [string]$manifest.version
    $sourceVersion = (Get-Item -LiteralPath $ExePath).VersionInfo.ProductVersion
    if (-not $servedVersion -or $servedVersion -ne $sourceVersion) {
        throw "production update manifest version does not match executable resources: manifest=$servedVersion executable=$sourceVersion"
    }
    $packagePath = Join-Path $fixtureRoot 'downloaded-RenderMesh.exe'
    $packageHeaders = @{
        'X-RM-Token' = $token
        'X-RM-Sha256' = [string]$manifest.sha256
    }
    $packageResponse = Invoke-WebRequest -UseBasicParsing `
        -Uri "$base/v1/update/package" -Headers $packageHeaders `
        -OutFile $packagePath -PassThru -TimeoutSec 15
    if ($packageResponse.StatusCode -ne 200) {
        throw "production update package endpoint returned HTTP $($packageResponse.StatusCode)"
    }
    $download = Get-Item -LiteralPath $packagePath
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $packagePath).Hash.ToLowerInvariant()
    if ([uint64]$download.Length -ne [uint64]$manifest.size -or
        $hash -ne ([string]$manifest.sha256).ToLowerInvariant()) {
        throw 'production update package bytes do not match the signed manifest'
    }
    $downloadVersion = $download.VersionInfo.ProductVersion
    if ($downloadVersion -ne $servedVersion) {
        throw "downloaded update package version does not match manifest: package=$downloadVersion manifest=$servedVersion"
    }
    'LAN_UPDATE_PACKAGE_OK'
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        try { $process | Wait-Process -Timeout 5 } catch {}
    }
    if (Test-Path -LiteralPath $fixtureRoot) {
        $verified = [IO.Path]::GetFullPath($fixtureRoot)
        $buildRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build'))
        if ($verified.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $verified -Recurse -Force
        }
    }
}
