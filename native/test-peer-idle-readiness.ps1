param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot (
    'build\peer-idle-readiness-' + [guid]::NewGuid().ToString('N'))
$workerRoot = Join-Path $fixtureRoot 'worker'
$observerRoot = Join-Path $fixtureRoot 'observer'
New-Item -ItemType Directory -Force -Path $workerRoot, $observerRoot | Out-Null

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
    [string]$Path, [string]$NodeId, [string]$NodeName,
    [int]$HttpPort, [int]$IdleAfterSeconds, [string]$ManualEndpoint) {
    $config = [ordered]@{
        node_id = $NodeId
        node_name = $NodeName
        invite_code = '111111'
        cluster_token = '00112233445566778899aabbccddeeff'
        pairing_pending = $false
        share_enabled = $true
        idle_after_seconds = $IdleAfterSeconds
        http_port = $HttpPort
        discovery_port = Get-FreeUdpPort
        blender_path = ''
        render_software_profiles = @([ordered]@{
            id = 'default-blender'; name = 'Blender'; render_system = 'Windows'
            render_software = 'Blender'; install_path = ''
        })
        last_render_software_profile_id = 'default-blender'
        manual_peer_endpoint = $ManualEndpoint
        theme = 'dark'
    }
    $config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
        Join-Path $Path 'config.json') -Encoding utf8
}

function Invoke-Phase([string]$Label, [int]$WorkerIdleAfterSeconds) {
    Write-NodeConfig $workerRoot 'idle-worker' 'IDLE-WORKER' $workerPort `
        $WorkerIdleAfterSeconds "127.0.0.1:$observerPort"
    Write-NodeConfig $observerRoot 'idle-observer' 'IDLE-OBSERVER' $observerPort `
        1 "127.0.0.1:$workerPort"
    $report = Join-Path $fixtureRoot ($Label + '-observer.json')
    $processes = @()
    try {
        $processes += Start-Process $ExePath -ArgumentList @(
            '--headless', '--run-seconds', '6', '--data-dir', $workerRoot) -PassThru
        Start-Sleep -Milliseconds 150
        $processes += Start-Process $ExePath -ArgumentList @(
            '--headless', '--run-seconds', '6', '--data-dir', $observerRoot,
            '--state-report', $report) -PassThru
        $processes | Wait-Process -Timeout 18
        foreach ($process in $processes) {
            $process.Refresh()
            if ($process.ExitCode -ne 0) {
                throw "RenderMesh process $($process.Id) exited with $($process.ExitCode)"
            }
        }
        $state = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
        $worker = @($state.peers | Where-Object node_id -eq 'idle-worker')
        if ($worker.Count -ne 1) { throw "$Label observer did not find worker" }
        return $worker[0]
    }
    finally {
        foreach ($process in $processes) {
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
            }
        }
    }
}

$workerPort = Get-FreeTcpPort
$observerPort = Get-FreeTcpPort
while ($observerPort -eq $workerPort) { $observerPort = Get-FreeTcpPort }

$active = Invoke-Phase 'active' 86400
if (-not [bool]$active.idle_ready_known) {
    throw 'protocol did not report signed idle readiness'
}
if ([bool]$active.idle_ready) {
    throw 'worker below its own idle threshold was reported ready'
}

"PEER_IDLE_READINESS_OK active=$($active.idle_ready) seconds=$($active.idle_seconds) fixture=$fixtureRoot"
