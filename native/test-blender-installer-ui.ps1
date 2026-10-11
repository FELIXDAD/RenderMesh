param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('build\blender-installer-ui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null

function ConvertFrom-CodePoints([int[]]$Points) {
    return -join ($Points | ForEach-Object { [char]$_ })
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

if (-not ('RenderMeshBlenderUi.NativeWindow' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

namespace RenderMeshBlenderUi {
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
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr window, StringBuilder value, int capacity);
        [DllImport("user32.dll")]
        public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
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

        public static string Text(IntPtr window) {
            StringBuilder value = new StringBuilder(512);
            GetWindowText(window, value, value.Capacity);
            return value.ToString();
        }
    }
}
'@
}

$port = Get-FreeTcpPort
$config = [ordered]@{
    node_id = 'blender-installer-ui-node'
    node_name = 'BLENDER-INSTALLER-UI'
    invite_code = '111111'
    cluster_token = '00112233445566778899aabbccddeeff'
    pairing_pending = $false
    share_enabled = $false
    idle_after_seconds = 180
    http_port = $port
    discovery_port = Get-FreeTcpPort
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
        '--preview-blender-missing', '--no-update-check',
        '--no-autostart-registration') -PassThru
    $main = [IntPtr]::Zero
    $dialog = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
        if ($process.HasExited) { throw "RenderMesh exited early with $($process.ExitCode)" }
        $main = [RenderMeshBlenderUi.NativeWindow]::FindMain($process.Id)
        if ($main -ne [IntPtr]::Zero) {
            $dialog = [RenderMeshBlenderUi.NativeWindow]::GetDlgItem($main, 600)
        }
        if ($dialog -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
    }
    if ($dialog -eq [IntPtr]::Zero) { throw 'Blender missing dialog not found' }
    $cancel = [RenderMeshBlenderUi.NativeWindow]::GetDlgItem($dialog, 601)
    $confirm = [RenderMeshBlenderUi.NativeWindow]::GetDlgItem($dialog, 602)
    $optix = [RenderMeshBlenderUi.NativeWindow]::GetDlgItem($dialog, 603)
    $settingsButton = [RenderMeshBlenderUi.NativeWindow]::GetDlgItem($main, 726)
    $cancelText = ConvertFrom-CodePoints @(0x6682, 0x4E0D, 0x5B89, 0x88C5)
    $installText = ConvertFrom-CodePoints @(0x7ACB, 0x5373, 0x5B89, 0x88C5)
    $optixText = (ConvertFrom-CodePoints @(
        0x5BFC,0x5165,0x9ED8,0x8BA4,0x6E32,0x67D3,0x914D,0x7F6E,0xFF08)) +
        'NVIDIA ' + (ConvertFrom-CodePoints @(0x4E13,0x7528,0xFF09))
    $settingsText = (ConvertFrom-CodePoints @(0x5B89,0x88C5)) +
        ' / ' + (ConvertFrom-CodePoints @(0x5207,0x6362)) +
        ' Blender ' + (ConvertFrom-CodePoints @(0x7248,0x672C))
    if ([RenderMeshBlenderUi.NativeWindow]::Text($cancel) -ne $cancelText) {
        throw 'Blender missing cancel action is incorrect'
    }
    if ([RenderMeshBlenderUi.NativeWindow]::Text($confirm) -ne $installText) {
        throw 'Blender missing install action is incorrect'
    }
    if ([RenderMeshBlenderUi.NativeWindow]::Text($optix) -ne $optixText) {
        throw 'NVIDIA default configuration option is missing'
    }
    if ([int][RenderMeshBlenderUi.NativeWindow]::SendMessage(
            $optix, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -ne 1) {
        throw 'NVIDIA default configuration option is not selected in the preview'
    }
    if ([RenderMeshBlenderUi.NativeWindow]::Text($settingsButton) -ne $settingsText) {
        throw 'settings Blender version action is missing'
    }
    'BLENDER_INSTALLER_UI_OK'
}
finally {
    if ($process -and -not $process.HasExited) {
        $main = [RenderMeshBlenderUi.NativeWindow]::FindMain($process.Id)
        if ($main -ne [IntPtr]::Zero) {
            [void][RenderMeshBlenderUi.NativeWindow]::PostMessage(
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
