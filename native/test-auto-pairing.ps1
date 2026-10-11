param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe'),
    [int]$RunSeconds = 8
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\auto-pairing-' + [guid]::NewGuid().ToString('N'))
$nodeA = Join-Path $fixtureRoot 'node-a'
$nodeB = Join-Path $fixtureRoot 'node-b'
New-Item -ItemType Directory -Force -Path $nodeA, $nodeB | Out-Null

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

function Write-PendingNodeConfig(
    [string]$path, [string]$nodeId, [string]$nodeName,
    [int]$httpPort, [int]$discoveryPort, [string]$token) {
    $config = [ordered]@{
        node_id = $nodeId
        node_name = $nodeName
        invite_code = '111111'
        cluster_token = $token
        pairing_pending = $true
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
        manual_peer_endpoint = ''
        theme = 'dark'
    }
    $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
        Join-Path $path 'config.json') -Encoding utf8
}

$portA = Get-FreeTcpPort
$portB = Get-FreeTcpPort
while ($portB -eq $portA) { $portB = Get-FreeTcpPort }
$discoveryPort = Get-FreeUdpPort
$reportA = Join-Path $fixtureRoot 'state-a.json'
$reportB = Join-Path $fixtureRoot 'state-b.json'
Write-PendingNodeConfig $nodeA 'fresh-node-a' 'FRESH-NODE-A' $portA $discoveryPort (
    '00112233445566778899aabbccddeeff')
Write-PendingNodeConfig $nodeB 'fresh-node-b' 'FRESH-NODE-B' $portB $discoveryPort (
    'ffeeddccbbaa99887766554433221100')

$processes = @()
try {
    $processes += Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', [string]$RunSeconds,
        '--data-dir', $nodeA, '--state-report', $reportA) -PassThru
    Start-Sleep -Milliseconds 150
    $processes += Start-Process $ExePath -ArgumentList @(
        '--headless', '--run-seconds', [string]$RunSeconds,
        '--data-dir', $nodeB, '--state-report', $reportB) -PassThru
    $processes | Wait-Process -Timeout ($RunSeconds + 12)
    foreach ($process in $processes) {
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
        }
    }
    $a = Get-Content -LiteralPath $reportA -Raw -Encoding UTF8 | ConvertFrom-Json
    $b = Get-Content -LiteralPath $reportB -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($state in @($a, $b)) {
        if ($state.invite_code -ne '111111') { throw 'matching ID changed unexpectedly' }
        if ($state.pairing_pending) { throw 'fresh node remained pairing_pending' }
        if ([int]$state.peer_count -lt 1) { throw 'fresh node did not discover a peer' }
    }
    if ($a.cluster_token -ne $b.cluster_token) {
        throw 'fresh nodes did not converge on one internal cluster credential'
    }
    if ([string]::IsNullOrWhiteSpace($a.manual_peer_endpoint) -and
        [string]::IsNullOrWhiteSpace($b.manual_peer_endpoint)) {
        throw 'automatic pairing did not persist the initiating discovered route'
    }
    "AUTO_PAIRING_OK peers=$($a.peer_count),$($b.peer_count) ports=$portA,$portB discovery=$discoveryPort fixture=$fixtureRoot"
}
finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
}
