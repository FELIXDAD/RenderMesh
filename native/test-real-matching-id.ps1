param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe'),
    [string]$LegacyExePath = ''
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\real-matching-id-' + [guid]::NewGuid().ToString('N'))
$nodeA = Join-Path $fixtureRoot 'node-a'
$nodeB = Join-Path $fixtureRoot 'node-b'
New-Item -ItemType Directory -Force -Path $nodeA, $nodeB | Out-Null

function Get-FreePort {
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

$portA = Get-FreePort
$portB = Get-FreePort
while ($portB -eq $portA) { $portB = Get-FreePort }
$discoveryPort = Get-FreePort
$legacyToken = '00112233445566778899aabbccddeeff'

function Write-NodeConfig([string]$path, [string]$nodeId, [string]$nodeName,
                          [int]$httpPort, [string]$peerEndpoint,
                          [string]$matchingId) {
    $config = [ordered]@{
        node_id = $nodeId
        node_name = $nodeName
        invite_code = $matchingId
        cluster_token = $legacyToken
        pairing_pending = $false
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
        manual_peer_endpoint = $peerEndpoint
        theme = 'dark'
    }
    $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $path 'config.json') -Encoding utf8
}

function Invoke-PairProbe([string]$label, [string[]]$argumentsA,
                          [string[]]$argumentsB, [int]$runSeconds = 5,
                          [string]$exeA = $ExePath, [string]$exeB = $ExePath) {
    $reportA = Join-Path $fixtureRoot ("state-$label-a.json")
    $reportB = Join-Path $fixtureRoot ("state-$label-b.json")
    $processes = @()
    try {
        $processes += Start-Process $exeA -ArgumentList ($argumentsA + @(
            '--headless', '--run-seconds', [string]$runSeconds, '--data-dir', $nodeA,
            '--state-report', $reportA)) -PassThru
        Start-Sleep -Milliseconds 250
        $processes += Start-Process $exeB -ArgumentList ($argumentsB + @(
            '--headless', '--run-seconds', [string]$runSeconds, '--data-dir', $nodeB,
            '--state-report', $reportB)) -PassThru
        $processes | Wait-Process -Timeout ($runSeconds + 10)
        foreach ($process in $processes) {
            $process.Refresh()
            if ($process.ExitCode -ne 0) {
                throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
            }
        }
        return @(
            (Get-Content -LiteralPath $reportA -Raw -Encoding UTF8 | ConvertFrom-Json),
            (Get-Content -LiteralPath $reportB -Raw -Encoding UTF8 | ConvertFrom-Json))
    }
    finally {
        foreach ($process in $processes) {
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
            }
        }
    }
}

# Reproduce the upgraded legacy state: both nodes still possess the same old
# internal token, but their displayed IDs differ. They must not appear as peers.
Write-NodeConfig $nodeA 'real-node-a' 'REAL-NODE-A' $portA "127.0.0.1:$portB" '123456'
Write-NodeConfig $nodeB 'real-node-b' 'REAL-NODE-B' $portB "127.0.0.1:$portA" '777777'
$mismatch = Invoke-PairProbe 'mismatch' @() @() 4
foreach ($state in $mismatch) {
    if ([int]$state.peer_count -ne 0) {
        throw 'different matching IDs authenticated through a legacy shared credential'
    }
}

# Change both nodes from the same established ID and prove the saved LAN routes
# automatically repair the group with a freshly rotated credential.
Write-NodeConfig $nodeA 'real-node-a' 'REAL-NODE-A' $portA "127.0.0.1:$portB" '123456'
Write-NodeConfig $nodeB 'real-node-b' 'REAL-NODE-B' $portB "127.0.0.1:$portA" '123456'
$repaired = Invoke-PairProbe 'repaired' @('--set-match-id', '777777') @('--set-match-id', '777777') 6
$a = $repaired[0]
$b = $repaired[1]
foreach ($state in $repaired) {
    if ($state.invite_code -ne '777777') { throw 'matching ID was not persisted' }
    if ($state.pairing_pending) { throw 'same matching IDs did not complete pairing' }
    if ([string]::IsNullOrWhiteSpace($state.manual_peer_endpoint)) {
        throw 'saved peer endpoint was cleared'
    }
    if ([int]$state.peer_count -lt 1) { throw 'real peer connection was not observed' }
}
if ($a.cluster_token -ne $b.cluster_token) { throw 'paired nodes did not share the new credential' }
if ($a.cluster_token -eq $legacyToken) { throw 'matching ID change reused the legacy credential' }

if ($LegacyExePath) {
    if (-not (Test-Path -LiteralPath $LegacyExePath -PathType Leaf)) {
        throw "Legacy executable was not found: $LegacyExePath"
    }
    Write-NodeConfig $nodeA 'real-node-a' 'REAL-NODE-A' $portA "127.0.0.1:$portB" '123456'
    Write-NodeConfig $nodeB 'real-node-b' 'REAL-NODE-B' $portB "127.0.0.1:$portA" '123456'
    $mixed = Invoke-PairProbe 'mixed-protocol9' @() @() 4 $ExePath $LegacyExePath
    if ([int]$mixed[0].peer_count -ne 0) {
        throw 'current node trusted a legacy protocol-9 peer without matching-ID binding'
    }
    if ([string]$mixed[0].manual_peer_status -notmatch '0\.2\.22') {
        throw "legacy protocol-9 diagnostic was unclear: $($mixed[0].manual_peer_status)"
    }
}

"REAL_MATCHING_ID_OK mismatch_peers=$($mismatch[0].peer_count),$($mismatch[1].peer_count) repaired_peers=$($a.peer_count),$($b.peer_count) ports=$portA,$portB fixture=$fixtureRoot"
