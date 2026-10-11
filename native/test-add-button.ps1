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
    throw 'UI probe executable must stay inside the repository.'
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'UI probe executable was not found.'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RenderMeshAddButtonProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct Point {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public delegate bool EnumWindowsCallback(IntPtr window, IntPtr state);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr parent, int id);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect bounds);

    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(Point point);

    [DllImport("user32.dll")]
    public static extern bool ScreenToClient(IntPtr window, ref Point point);

    [DllImport("user32.dll")]
    public static extern IntPtr ChildWindowFromPointEx(IntPtr parent, Point point, uint flags);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr state);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int length);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int length);

    public static IntPtr WindowAtCenter(IntPtr window) {
        Rect bounds;
        if (!GetWindowRect(window, out bounds)) return IntPtr.Zero;
        return WindowFromPoint(new Point {
            X = (bounds.Left + bounds.Right) / 2,
            Y = (bounds.Top + bounds.Bottom) / 2
        });
    }

    public static IntPtr ChildAtCenter(IntPtr parent, IntPtr window) {
        Rect bounds;
        if (!GetWindowRect(window, out bounds)) return IntPtr.Zero;
        var point = new Point {
            X = (bounds.Left + bounds.Right) / 2,
            Y = (bounds.Top + bounds.Bottom) / 2
        };
        if (!ScreenToClient(parent, ref point)) return IntPtr.Zero;
        return ChildWindowFromPointEx(parent, point, 0x0001 | 0x0002 | 0x0004);
    }

    public static IntPtr FindProcessDialog(uint processId) {
        var found = new IntPtr[] { IntPtr.Zero };
        EnumWindows((window, state) => {
            uint candidateProcess;
            GetWindowThreadProcessId(window, out candidateProcess);
            if (candidateProcess != processId) return true;
            var className = new System.Text.StringBuilder(64);
            GetClassName(window, className, className.Capacity);
            if (className.ToString() != "#32770") return true;
            found[0] = window;
            return true;
        }, IntPtr.Zero);
        return found[0];
    }
}
'@

$probeRoot = [IO.Path]::GetFullPath((Join-Path $nativeRoot (
    'build\add-button-regression-' + [guid]::NewGuid().ToString('N'))))
if (-not $probeRoot.StartsWith($nativeRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UI probe directory escaped the native workspace.'
}
New-Item -ItemType Directory -Path $probeRoot | Out-Null
$port = Get-Random -Minimum 50000 -Maximum 59000
$process = $null
$mainWindow = [IntPtr]::Zero
$dialog = [IntPtr]::Zero

try {
    $process = Start-Process -FilePath $exe -ArgumentList @(
        '--data-dir', (Join-Path $probeRoot 'data'),
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

    # ID_ADD_JOB is 409 in the native ControlId enum. The center-point hit test
    # catches sibling windows that visually overlap the floating action button.
    # The top-level HWND becomes visible slightly before the page controls are
    # created on slower/just-built binaries. Wait for the actual interaction
    # target instead of treating that normal startup interval as a regression.
    $button = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 120 -and $button -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 50
        $button = [RenderMeshAddButtonProbe]::GetDlgItem($mainWindow, 409)
    }
    if ($button -eq [IntPtr]::Zero) {
        throw 'Add-job button was not created.'
    }
    if (-not [RenderMeshAddButtonProbe]::IsWindowVisible($button) -or
        -not [RenderMeshAddButtonProbe]::IsWindowEnabled($button)) {
        throw 'Add-job button is not visible and enabled.'
    }
    # Probe the RenderMesh child hierarchy itself. A global WindowFromPoint check
    # is flaky when another desktop window happens to cover the test process.
    $hitWindow = [RenderMeshAddButtonProbe]::ChildAtCenter($mainWindow, $button)
    if ($hitWindow -ne $button) {
        $hitClass = [System.Text.StringBuilder]::new(128)
        $hitText = [System.Text.StringBuilder]::new(256)
        [void][RenderMeshAddButtonProbe]::GetClassName(
            $hitWindow, $hitClass, $hitClass.Capacity)
        [void][RenderMeshAddButtonProbe]::GetWindowText(
            $hitWindow, $hitText, $hitText.Capacity)
        throw ('Add-job button is visually present but another child window owns its ' +
            "click area: handle=$hitWindow class='$hitClass' text='$hitText'.")
    }
    if (-not [RenderMeshAddButtonProbe]::PostMessage(
            $button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'BM_CLICK could not be posted to the add-job button.'
    }

    for ($attempt = 0; $attempt -lt 60 -and $dialog -eq [IntPtr]::Zero; ++$attempt) {
        Start-Sleep -Milliseconds 50
        $dialog = [RenderMeshAddButtonProbe]::FindProcessDialog([uint32]$process.Id)
    }
    if ($dialog -eq [IntPtr]::Zero) {
        throw 'Add-job click did not open the Blender project picker.'
    }
    Write-Output 'ADD_BUTTON_DIALOG_OK'
} finally {
    if ($dialog -ne [IntPtr]::Zero) {
        [void][RenderMeshAddButtonProbe]::PostMessage(
            $dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if ($mainWindow -ne [IntPtr]::Zero) {
        [void][RenderMeshAddButtonProbe]::PostMessage(
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
            throw 'Refusing to remove an unverified UI probe directory.'
        }
        Remove-Item -LiteralPath $verifiedProbeRoot -Recurse -Force
    }
}
