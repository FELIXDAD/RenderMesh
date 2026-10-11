param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\single-instance-' + [guid]::NewGuid().ToString('N'))
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

if (-not ('RenderMeshSingleInstance.NativeWindow' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

namespace RenderMeshSingleInstance {
    public static class NativeWindow {
        public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder value, int capacity);
        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")]
        public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

        public static IntPtr Find(int processId) {
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
    node_id = 'single-instance-node'
    node_name = 'SINGLE-INSTANCE'
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
    auto_start = $true
    silent_auto_start = $true
}
$config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
    Join-Path $fixtureRoot 'config.json') -Encoding utf8

$first = $null
$second = $null
try {
    $commonArguments = @(
        '--data-dir', $fixtureRoot, '--port', [string]$port,
        '--no-update-check', '--no-autostart-registration')
    $first = Start-Process -FilePath $ExePath -ArgumentList (
        @('--startup') + $commonArguments) -PassThru

    $window = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    while ([DateTime]::UtcNow -lt $deadline -and $window -eq [IntPtr]::Zero) {
        if ($first.HasExited) {
            throw "silent instance exited early with code $($first.ExitCode)"
        }
        $window = [RenderMeshSingleInstance.NativeWindow]::Find($first.Id)
        if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'silent instance did not create its main window' }
    if ([RenderMeshSingleInstance.NativeWindow]::IsWindowVisible($window)) {
        throw 'startup-only silent mode displayed the main window'
    }

    $second = Start-Process -FilePath $ExePath -ArgumentList $commonArguments -PassThru
    $second | Wait-Process -Timeout 8
    $second.Refresh()
    if ($second.ExitCode -ne 0) {
        throw "second launch exited with code $($second.ExitCode)"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [RenderMeshSingleInstance.NativeWindow]::IsWindowVisible($window)) {
        Start-Sleep -Milliseconds 100
    }
    if (-not [RenderMeshSingleInstance.NativeWindow]::IsWindowVisible($window)) {
        throw 'manual launch did not restore the silent instance'
    }

    [void][RenderMeshSingleInstance.NativeWindow]::PostMessage(
        $window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    $first | Wait-Process -Timeout 8
    $first.Refresh()
    if ($first.ExitCode -ne 0) { throw 'first silent instance did not exit cleanly' }

    $first = Start-Process -FilePath $ExePath -ArgumentList (
        @('--startup', '--ui-test-startup-delay-ms', '4500') + $commonArguments) -PassThru
    Start-Sleep -Milliseconds 500
    if ($first.HasExited) { throw 'delayed startup instance exited before acquiring its lock' }
    if ([RenderMeshSingleInstance.NativeWindow]::Find($first.Id) -ne [IntPtr]::Zero) {
        throw 'startup-race fixture did not delay main-window creation'
    }
    $second = Start-Process -FilePath $ExePath -ArgumentList $commonArguments -PassThru
    $second | Wait-Process -Timeout 12
    $second.Refresh()
    if ($second.ExitCode -ne 0) {
        throw "startup-race second launch exited with code $($second.ExitCode)"
    }
    $window = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and $window -eq [IntPtr]::Zero) {
        $window = [RenderMeshSingleInstance.NativeWindow]::Find($first.Id)
        if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
    }
    if ($window -eq [IntPtr]::Zero -or
        -not [RenderMeshSingleInstance.NativeWindow]::IsWindowVisible($window)) {
        throw 'manual launch during startup did not restore the delayed silent instance'
    }
    "SINGLE_INSTANCE_OK pid=$($first.Id) port=$port startup_race=True"
}
finally {
    if ($first -and -not $first.HasExited) {
        $window = [RenderMeshSingleInstance.NativeWindow]::Find($first.Id)
        if ($window -ne [IntPtr]::Zero) {
            [void][RenderMeshSingleInstance.NativeWindow]::PostMessage(
                $window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            try { $first | Wait-Process -Timeout 8 } catch {}
        }
        if (-not $first.HasExited) { Stop-Process -Id $first.Id -ErrorAction SilentlyContinue }
    }
    if ($second -and -not $second.HasExited) {
        Stop-Process -Id $second.Id -ErrorAction SilentlyContinue
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
