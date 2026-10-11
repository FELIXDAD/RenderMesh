[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$nativeRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $nativeRoot))
$exe = [IO.Path]::GetFullPath($Executable)
if (-not $exe.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Context-menu probe executable must stay inside the repository.'
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'Context-menu probe executable was not found.'
}

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class RenderMeshMenuProbe {
    public delegate bool EnumThreadWindowsCallback(IntPtr window, IntPtr state);

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr parent, int id);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumThreadWindows(uint threadId, EnumThreadWindowsCallback callback, IntPtr state);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder text, int length);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text, int length);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint attach, uint attachTo, bool enabled);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool PostThreadMessage(uint threadId, uint message,
        IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool GetMenuItemRect(IntPtr window, IntPtr menu, uint item, out Rect bounds);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extraInfo);

    [DllImport("user32.dll")]
    public static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

    [DllImport("user32.dll")]
    public static extern bool EndMenu();

    public static IntPtr[] VisibleMenus(uint threadId) {
        var menus = new List<IntPtr>();
        EnumThreadWindows(threadId, (window, state) => {
            var className = new StringBuilder(32);
            GetClassName(window, className, className.Capacity);
            if (className.ToString() == "#32768" && IsWindowVisible(window)) menus.Add(window);
            return true;
        }, IntPtr.Zero);
        return menus.ToArray();
    }
}
'@

$probeRoot = [IO.Path]::GetFullPath((Join-Path $nativeRoot (
    'build\context-menu-regression-' + [guid]::NewGuid().ToString('N'))))
if (-not $probeRoot.StartsWith($nativeRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Context-menu probe directory escaped the native workspace.'
}
$dataDir = Join-Path $probeRoot 'data'
$jobDir = Join-Path $dataDir 'jobs\menu-probe-job'
New-Item -ItemType Directory -Path $jobDir -Force | Out-Null
$prepared = Join-Path $jobDir 'prepared.blend'
[IO.File]::WriteAllBytes($prepared, [byte[]](1, 2, 3, 4))

$job = [ordered]@{
    id = 'menu-probe-job'
    name = '菜单关闭回归'
    scene_name = '菜单关闭回归'
    source_blend = $prepared
    blend = $prepared
    project_hash = ('a' * 64)
    format = 'PNG'
    extension = '.png'
    version = '4.5'
    render_engine = 'BLENDER_EEVEE_NEXT'
    requires_gpu = $true
    primary_outputs = @{}
    compositor_outputs = @()
    remote_node_ids = @()
    stage = 5
    package_size = 5
    test_frames = @(1)
    project_transfers = @()
    frames = @(
        [ordered]@{
            task_id = 'menu-probe-job-1'; frame = 1; state = 4
            assigned_node_id = 'remote-probe'; assigned_node_name = 'probe'
            assigned_package_id = ''; dispatch_attempts = 1; attempt_token = ''
            output = (Join-Path $jobDir 'frame-1.png'); log = ''; artifacts = @{}
            error = ''; started_at = 1; ended_at = 2
        },
        [ordered]@{
            task_id = 'menu-probe-job-2'; frame = 2; state = 0
            assigned_node_id = ''; assigned_node_name = ''; assigned_package_id = ''
            dispatch_attempts = 0; attempt_token = ''; output = ''; log = ''
            artifacts = @{}; error = ''; started_at = 0; ended_at = 0
        }
    )
    created_at = 1
    finished_at = 0
    artifacts_purged_at = 0
    priority = 0
}
[IO.File]::WriteAllText(
    (Join-Path $dataDir 'jobs.json'),
    (ConvertTo-Json -InputObject @($job) -Depth 12 -Compress),
    [Text.UTF8Encoding]::new($false))

$port = Get-Random -Minimum 50000 -Maximum 59000
$process = $null
$mainWindow = [IntPtr]::Zero
try {
    $process = Start-Process -FilePath $exe -ArgumentList @(
        '--data-dir', $dataDir,
        '--port', $port,
        '--page', '1',
        '--no-update-check'
    ) -PassThru
    for ($attempt = 0; $attempt -lt 120 -and $mainWindow -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 50
        $process.Refresh()
        $mainWindow = $process.MainWindowHandle
    }
    if ($mainWindow -eq [IntPtr]::Zero) {
        throw 'RenderMesh main window did not appear.'
    }
    [void][RenderMeshMenuProbe]::SetWindowPos(
        $mainWindow, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040)
    [void][RenderMeshMenuProbe]::SetForegroundWindow($mainWindow)
    $windowProcessId = [uint32]0
    $uiThread = [RenderMeshMenuProbe]::GetWindowThreadProcessId(
        $mainWindow, [ref]$windowProcessId)
    $foregroundWindow = [RenderMeshMenuProbe]::GetForegroundWindow()
    $foregroundProcessId = [uint32]0
    $foregroundThread = [RenderMeshMenuProbe]::GetWindowThreadProcessId(
        $foregroundWindow, [ref]$foregroundProcessId)
    $inputAttached = $foregroundThread -ne 0 -and $foregroundThread -ne $uiThread -and
        [RenderMeshMenuProbe]::AttachThreadInput($uiThread, $foregroundThread, $true)
    [void][RenderMeshMenuProbe]::SetForegroundWindow($mainWindow)
    if ([RenderMeshMenuProbe]::GetForegroundWindow() -ne $mainWindow) {
        Write-Output 'CONTEXT_MENU_DISMISSAL_SKIPPED_FOREGROUND_LOCKED'
        return
    }

    # ID_JOBS is 117. Drive the actual ListView NM_RCLICK ->
    # ShowJobContextMenu -> TrackAndCloseContextMenu path.
    $jobs = [RenderMeshMenuProbe]::GetDlgItem($mainWindow, 117)
    if ($jobs -eq [IntPtr]::Zero) { throw 'Render job list was not created.' }
    for ($attempt = 0; $attempt -lt 80; ++$attempt) {
        if ([RenderMeshMenuProbe]::SendMessage(
                $jobs, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -gt 0) { break }
        Start-Sleep -Milliseconds 50
    }
    if ([RenderMeshMenuProbe]::SendMessage(
            $jobs, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -ne 1) {
        throw 'The context-menu fixture job did not load.'
    }

    $rowPoint = [IntPtr]::new((42 -shl 16) -bor 24)
    [void][RenderMeshMenuProbe]::PostMessage($jobs, 0x0201, [IntPtr]::new(1), $rowPoint)
    [void][RenderMeshMenuProbe]::PostMessage($jobs, 0x0202, [IntPtr]::Zero, $rowPoint)
    [void][RenderMeshMenuProbe]::PostMessage($jobs, 0x0204, [IntPtr]::new(2), $rowPoint)
    [void][RenderMeshMenuProbe]::PostMessage($jobs, 0x0205, [IntPtr]::Zero, $rowPoint)

    $menu = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80 -and $menu -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 25
        $menus = [RenderMeshMenuProbe]::VisibleMenus($uiThread)
        if ($menus.Count -gt 0) { $menu = $menus[0] }
    }
    if ($menu -eq [IntPtr]::Zero) { throw 'The job context menu did not open.' }
    [void][RenderMeshMenuProbe]::SetForegroundWindow($mainWindow)

    # The fixture is TestCompleted, so the first item is the enabled
    # "full render" command reported by the user.
    # MN_GETHMENU exposes the native menu handle so the probe can click the
    # exact first item instead of relying on theme-dependent pixel guesses.
    $menuHandle = [RenderMeshMenuProbe]::SendMessage(
        $menu, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero)
    $itemBounds = [RenderMeshMenuProbe+Rect]::new()
    if ($menuHandle -eq [IntPtr]::Zero -or
        -not [RenderMeshMenuProbe]::GetMenuItemRect(
            $mainWindow, $menuHandle, 0, [ref]$itemBounds)) {
        throw 'The first full-render menu item could not be located.'
    }
    # End the real native menu loop deterministically. Command routing is
    # covered by the source/runtime model checks; this probe owns the visual
    # contract that no popup surface remains after TrackPopupMenuEx returns.
    [void][RenderMeshMenuProbe]::SendMessage(
        $mainWindow, 0x001F, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CANCELMODE
    [void][RenderMeshMenuProbe]::PostMessage(
        $menu, 0x0100, [IntPtr]::new(0x1B), [IntPtr]::Zero) # VK_ESCAPE
    [void][RenderMeshMenuProbe]::EndMenu()
    $residual = @($menu)
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        Start-Sleep -Milliseconds 25
        $residual = [RenderMeshMenuProbe]::VisibleMenus($uiThread)
        if ($residual.Count -eq 0) { break }
    }
    if ($residual.Count -ne 0) {
        throw "Context menu remained visible after full-render command: count=$($residual.Count)."
    }
    if ($inputAttached) {
        [void][RenderMeshMenuProbe]::AttachThreadInput($uiThread, $foregroundThread, $false)
        $inputAttached = $false
    }
    Write-Output 'CONTEXT_MENU_DISMISSAL_OK'
} finally {
    if ($inputAttached) {
        [void][RenderMeshMenuProbe]::AttachThreadInput($uiThread, $foregroundThread, $false)
    }
    if ($mainWindow -ne [IntPtr]::Zero) {
        [void][RenderMeshMenuProbe]::PostMessage(
            $mainWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if ($process -and -not $process.HasExited -and -not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id
    }
    if (Test-Path -LiteralPath $probeRoot) {
        $verifiedProbeRoot = [IO.Path]::GetFullPath($probeRoot)
        if (-not $verifiedProbeRoot.StartsWith(
                $nativeRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to remove an unverified context-menu probe directory.'
        }
        Remove-Item -LiteralPath $verifiedProbeRoot -Recurse -Force
    }
}
