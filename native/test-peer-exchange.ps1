param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe'),
    [string]$LegacyPeerExePath = '',
    [int]$RunSeconds = 7
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\peer-exchange-' + [guid]::NewGuid().ToString('N'))
$hubRoot = Join-Path $fixtureRoot 'hub'
$peerRoot = Join-Path $fixtureRoot 'peer'
$newRoot = Join-Path $fixtureRoot 'newcomer'
New-Item -ItemType Directory -Force -Path $hubRoot, $peerRoot, $newRoot | Out-Null

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
    [int]$httpPort, [int]$discoveryPort, [string]$token,
    [bool]$pending, [string]$manualEndpoint) {
    $config = [ordered]@{
        node_id = $nodeId
        node_name = $nodeName
        invite_code = '111111'
        cluster_token = $token
        pairing_pending = $pending
        share_enabled = $true
        idle_after_seconds = 1
        http_port = $httpPort
        discovery_port = $discoveryPort
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
$peerPort = Get-FreeTcpPort
$newPort = Get-FreeTcpPort
$sharedToken = '00112233445566778899aabbccddeeff'
Write-NodeConfig $hubRoot 'exchange-hub' 'EXCHANGE-HUB' $hubPort (Get-FreeUdpPort) `
    $sharedToken $false "127.0.0.1:$peerPort"
Write-NodeConfig $peerRoot 'exchange-peer' 'EXCHANGE-PEER' $peerPort (Get-FreeUdpPort) `
    $sharedToken $false "127.0.0.1:$hubPort"
Write-NodeConfig $newRoot 'exchange-newcomer' 'EXCHANGE-NEWCOMER' $newPort `
    (Get-FreeUdpPort) 'ffeeddccbbaa99887766554433221100' $true "127.0.0.1:$hubPort"

$hubReport = Join-Path $fixtureRoot 'hub.json'
$peerReport = Join-Path $fixtureRoot 'peer.json'
$newReport = Join-Path $fixtureRoot 'newcomer.json'
$processes = @()
try {
    $peerExecutable = if ($LegacyPeerExePath) { $LegacyPeerExePath } else { $ExePath }
    foreach ($entry in @(
        @($ExePath, $hubRoot, $hubReport),
        @($peerExecutable, $peerRoot, $peerReport),
        @($ExePath, $newRoot, $newReport))) {
        $processes += Start-Process $entry[0] -ArgumentList @(
            '--headless', '--run-seconds', [string]$RunSeconds,
            '--data-dir', $entry[1], '--state-report', $entry[2]) -PassThru
        Start-Sleep -Milliseconds 150
    }
    $processes | Wait-Process -Timeout ($RunSeconds + 12)
    foreach ($process in $processes) {
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
        }
    }
    $hub = Get-Content -LiteralPath $hubReport -Raw -Encoding UTF8 | ConvertFrom-Json
    $peer = Get-Content -LiteralPath $peerReport -Raw -Encoding UTF8 | ConvertFrom-Json
    $newcomer = Get-Content -LiteralPath $newReport -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($newcomer.pairing_pending) { throw 'newcomer did not pair with the hub' }
    if ($newcomer.cluster_token -ne $sharedToken) {
        throw 'newcomer did not adopt the established cluster credential'
    }
    if ([int]$newcomer.peer_count -lt 2) {
        throw "newcomer only discovered $($newcomer.peer_count) peer; authenticated peer exchange is missing"
    }
    "PEER_EXCHANGE_OK hub=$($hub.peer_count) peer=$($peer.peer_count) newcomer=$($newcomer.peer_count) fixture=$fixtureRoot"
}
finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
}
