param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\peer-state-' + [guid]::NewGuid().ToString('N'))
$hubRoot = Join-Path $fixtureRoot 'hub'
$workerRoot = Join-Path $fixtureRoot 'worker'
$observerRoot = Join-Path $fixtureRoot 'observer'
New-Item -ItemType Directory -Force -Path $hubRoot, $workerRoot, $observerRoot | Out-Null

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
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

function Write-NodeConfig(
    [string]$path, [string]$nodeId, [string]$nodeName,
    [int]$httpPort, [string]$token, [bool]$pending,
    [bool]$shareEnabled, [string]$manualEndpoint) {
    $config = [ordered]@{
        node_id = $nodeId
        node_name = $nodeName
        invite_code = '111111'
        cluster_token = $token
        pairing_pending = $pending
        share_enabled = $shareEnabled
        idle_after_seconds = 1
        http_port = $httpPort
        discovery_port = Get-FreeUdpPort
        blender_path = ''
        render_software_profiles = @([ordered]@{
            id = 'default-blender'; name = 'Blender'; render_system = 'Windows'
            render_software = 'Blender'; install_path = ''
        })
        last_render_software_profile_id = 'default-blender'
        manual_peer_endpoint = $manualEndpoint
        theme = 'dark'
    }
    $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
        Join-Path $path 'config.json') -Encoding utf8
}

$hubPort = Get-FreeTcpPort
$workerPort = Get-FreeTcpPort
$observerPort = Get-FreeTcpPort
$sharedToken = '00112233445566778899aabbccddeeff'
$observerToken = 'ffeeddccbbaa99887766554433221100'
$hubReport = Join-Path $fixtureRoot 'hub.json'
$observerReport = Join-Path $fixtureRoot 'observer.json'

Write-NodeConfig $hubRoot 'state-hub' 'STATE-HUB' $hubPort $sharedToken $false $true `
    "127.0.0.1:$workerPort"
Write-NodeConfig $workerRoot 'state-worker' 'STATE-WORKER' $workerPort $sharedToken $false `
    $true "127.0.0.1:$hubPort"
Write-NodeConfig $observerRoot 'state-observer' 'STATE-OBSERVER' $observerPort $observerToken `
    $true $true "127.0.0.1:$hubPort"

$processes = @()
try {
    $hub = Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', '14', '--data-dir', $hubRoot,
        '--state-report', $hubReport) -PassThru
    $processes += $hub
    Start-Sleep -Milliseconds 200
    $workerFirst = Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', '4', '--data-dir', $workerRoot) -PassThru
    $processes += $workerFirst
    Start-Sleep -Milliseconds 200
    $observer = Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', '14', '--data-dir', $observerRoot,
        '--state-report', $observerReport) -PassThru
    $processes += $observer

    $workerFirst | Wait-Process -Timeout 10
    $workerFirst.Refresh()
    if ($workerFirst.ExitCode -ne 0) { throw 'first worker phase failed' }

    $workerConfigPath = Join-Path $workerRoot 'config.json'
    $workerConfig = Get-Content -LiteralPath $workerConfigPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $workerConfig.share_enabled = $false
    $workerConfig | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $workerConfigPath `
        -Encoding utf8
    $workerSecond = Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', '7', '--data-dir', $workerRoot) -PassThru
    $processes += $workerSecond

    $hub, $observer, $workerSecond | Wait-Process -Timeout 18
    foreach ($process in @($hub, $observer, $workerSecond)) {
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
        }
    }

    $hubState = Get-Content -LiteralPath $hubReport -Raw -Encoding UTF8 | ConvertFrom-Json
    $observerState = Get-Content -LiteralPath $observerReport -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $hubWorker = @($hubState.peers | Where-Object node_id -eq 'state-worker')
    $observerWorker = @($observerState.peers | Where-Object node_id -eq 'state-worker')
    if ($hubWorker.Count -ne 1 -or [bool]$hubWorker[0].share) {
        throw 'hub did not observe the worker state transition'
    }
    if ($observerWorker.Count -ne 1) {
        throw 'observer lost the introduced worker'
    }
    if ([bool]$observerWorker[0].share) {
        throw 'observer retained stale worker state after authenticated directory refresh'
    }
    "PEER_STATE_REFRESH_OK hub_share=$($hubWorker[0].share) observer_share=$($observerWorker[0].share) fixture=$fixtureRoot"
}
finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
}
