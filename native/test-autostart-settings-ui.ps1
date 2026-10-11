param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\autostart-ui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null

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

if (-not ('RenderMeshAutoStartUi.NativeWindow' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

namespace RenderMeshAutoStartUi {
    public static class NativeWindow {
        public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder value, int capacity);
        [DllImport("user32.dll")]
        public static extern IntPtr GetDlgItem(IntPtr window, int id);
        [DllImport("user32.dll")]
        public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        public static extern bool IsWindowEnabled(IntPtr window);
        [DllImport("user32.dll")]
        public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

        public static IntPtr FindMain(int processId) {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr window, IntPtr parameter) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != (uint)processId) return true;
                StringBuilder className = new StringBuilder(64);
                GetClassName(window, className, className.Capacity);
                if (className.ToString() != "RenderMeshNativeWindow") return true;
                found = window;
                return false;
            }, IntPtr.Zero);
            return found;
        }
    }
}
'@
}

$port = Get-FreeTcpPort
$config = [ordered]@{
    node_id = 'autostart-ui-node'
    node_name = 'AUTOSTART-UI'
    invite_code = '111111'
    cluster_token = '00112233445566778899aabbccddeeff'
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
$config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
    Join-Path $fixtureRoot 'config.json') -Encoding utf8

$process = $null
try {
    $process = Start-Process -FilePath $ExePath -ArgumentList @(
        '--data-dir', $fixtureRoot, '--port', [string]$port, '--page', '3',
        '--no-update-check', '--no-autostart-registration') -PassThru
    $main = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline -and $main -eq [IntPtr]::Zero) {
        if ($process.HasExited) { throw "RenderMesh exited early with $($process.ExitCode)" }
        $main = [RenderMeshAutoStartUi.NativeWindow]::FindMain($process.Id)
        if ($main -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
    }
    if ($main -eq [IntPtr]::Zero) { throw 'main window not found' }

    $autoStart = [IntPtr]::Zero
    $silentStart = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and
           ($autoStart -eq [IntPtr]::Zero -or $silentStart -eq [IntPtr]::Zero)) {
        $autoStart = [RenderMeshAutoStartUi.NativeWindow]::GetDlgItem($main, 721)
        $silentStart = [RenderMeshAutoStartUi.NativeWindow]::GetDlgItem($main, 722)
        if ($autoStart -eq [IntPtr]::Zero -or $silentStart -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 100
        }
    }
    if ($autoStart -eq [IntPtr]::Zero -or $silentStart -eq [IntPtr]::Zero) {
        throw 'auto-start controls not found'
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and
           [RenderMeshAutoStartUi.NativeWindow]::IsWindowEnabled($silentStart)) {
        Start-Sleep -Milliseconds 50
    }
    if ([RenderMeshAutoStartUi.NativeWindow]::IsWindowEnabled($silentStart)) {
        throw 'silent-start toggle must begin disabled while auto-start is off'
    }

    [void][RenderMeshAutoStartUi.NativeWindow]::SendMessage(
        $autoStart, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    $autoChecked = [int][RenderMeshAutoStartUi.NativeWindow]::SendMessage(
        $autoStart, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($autoChecked -ne 1) { throw 'auto-start toggle ignored its button click' }
    if (-not [RenderMeshAutoStartUi.NativeWindow]::IsWindowEnabled($silentStart)) {
        throw 'silent-start toggle did not become enabled'
    }

    [void][RenderMeshAutoStartUi.NativeWindow]::SendMessage(
        $silentStart, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    $silentChecked = [int][RenderMeshAutoStartUi.NativeWindow]::SendMessage(
        $silentStart, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($silentChecked -ne 1) { throw 'silent-start toggle ignored its button click' }

    $persisted = Get-Content -Raw -Encoding UTF8 -LiteralPath (
        Join-Path $fixtureRoot 'config.json') | ConvertFrom-Json
    if (-not [bool]$persisted.auto_start -or -not [bool]$persisted.silent_auto_start) {
        throw 'auto-start settings were not persisted after clicking'
    }
    'AUTOSTART_SETTINGS_UI_OK'
}
finally {
    if ($process -and -not $process.HasExited) {
        $main = [RenderMeshAutoStartUi.NativeWindow]::FindMain($process.Id)
        if ($main -ne [IntPtr]::Zero) {
            [void][RenderMeshAutoStartUi.NativeWindow]::PostMessage(
                $main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            try { $process | Wait-Process -Timeout 8 } catch {}
        }
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
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
