param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe'),
    [int]$FirstPhaseSeconds = 12,
    [int]$SecondPhaseSeconds = 11
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot (
    'build\decentralized-discovery-' + [guid]::NewGuid().ToString('N'))
$nodes = @(
    [pscustomobject]@{ Id = 'mesh-node-a'; Name = 'MESH-NODE-A'; Token = '00112233445566778899aabbccddeeff' },
    [pscustomobject]@{ Id = 'mesh-node-b'; Name = 'MESH-NODE-B'; Token = '11223344556677889900aabbccddeeff' },
    [pscustomobject]@{ Id = 'mesh-node-c'; Name = 'MESH-NODE-C'; Token = '22334455667788990011aabbccddeeff' }
)

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

function Write-NodeConfig(
    [string]$Path, [pscustomobject]$Node, [int]$HttpPort,
    [int]$DiscoveryPort) {
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    $config = [ordered]@{
        node_id = $Node.Id
        node_name = $Node.Name
        invite_code = '111111'
        cluster_token = $Node.Token
        pairing_pending = $true
        share_enabled = $true
        idle_after_seconds = 1
        http_port = $HttpPort
        discovery_port = $DiscoveryPort
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
        Join-Path $Path 'config.json') -Encoding utf8
}

function Start-Phase([pscustomobject[]]$PhaseNodes, [int]$Seconds, [string]$Label) {
    $processes = @()
    $reports = @{}
    try {
        foreach ($node in $PhaseNodes) {
            $root = Join-Path $fixtureRoot $node.Id
            $report = Join-Path $fixtureRoot ($Label + '-' + $node.Id + '.json')
            $reports[$node.Id] = $report
            $processes += Start-Process $ExePath -ArgumentList @(
                '--headless', '--run-seconds', [string]$Seconds,
                '--data-dir', $root, '--state-report', $report) -PassThru
            Start-Sleep -Milliseconds 150
        }
        $processes | Wait-Process -Timeout ($Seconds + 15)
        foreach ($process in $processes) {
            $process.Refresh()
            if ($process.ExitCode -ne 0) {
                throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
            }
        }
        $states = @{}
        foreach ($node in $PhaseNodes) {
            $states[$node.Id] = Get-Content -LiteralPath $reports[$node.Id] -Raw `
                -Encoding UTF8 | ConvertFrom-Json
        }
        return $states
    }
    finally {
        foreach ($process in $processes) {
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
            }
        }
    }
}

$discoveryPort = Get-FreeUdpPort
foreach ($node in $nodes) {
    $node | Add-Member -NotePropertyName Root -NotePropertyValue (
        Join-Path $fixtureRoot $node.Id)
    $node | Add-Member -NotePropertyName Port -NotePropertyValue (Get-FreeTcpPort)
    Write-NodeConfig $node.Root $node $node.Port $discoveryPort
}

$phaseOne = Start-Phase $nodes $FirstPhaseSeconds 'all'
$tokens = @($nodes | ForEach-Object { [string]$phaseOne[$_.Id].cluster_token } |
    Select-Object -Unique)
if ($tokens.Count -ne 1) {
    throw 'three fresh nodes did not converge on one internal cluster credential'
}
foreach ($node in $nodes) {
    $state = $phaseOne[$node.Id]
    if ($state.pairing_pending) { throw "$($node.Id) remained pairing_pending" }
    if ([int]$state.peer_count -lt 2) {
        throw "$($node.Id) discovered only $($state.peer_count) peers without manual IPs"
    }
}

# Simulate taking the first/test machine offline. The remaining two nodes must
# rediscover one another directly from LAN announcements, without a saved manual
# route or a peer directory supplied by the stopped node.
$survivors = @($nodes[1], $nodes[2])
foreach ($node in $survivors) {
    $configPath = Join-Path $node.Root 'config.json'
    $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $config.manual_peer_endpoint = ''
    $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $configPath -Encoding utf8
}
$phaseTwo = Start-Phase $survivors $SecondPhaseSeconds 'without-test-machine'
foreach ($node in $survivors) {
    $state = $phaseTwo[$node.Id]
    $otherId = @($survivors | Where-Object Id -ne $node.Id)[0].Id
    $directPeer = @($state.peers | Where-Object node_id -eq $otherId)
    if ($directPeer.Count -ne 1 -or [int]$state.peer_count -lt 1) {
        throw "$($node.Id) depended on the stopped test machine to discover $otherId"
    }
}

$allPeerCounts = @($nodes | ForEach-Object { $phaseOne[$_.Id].peer_count }) -join ','
$survivorPeerCounts = @($survivors | ForEach-Object { $phaseTwo[$_.Id].peer_count }) -join ','
"DECENTRALIZED_DISCOVERY_OK peers=$allPeerCounts survivors=$survivorPeerCounts discovery=$discoveryPort fixture=$fixtureRoot"
