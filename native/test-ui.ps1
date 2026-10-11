[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = Split-Path -Parent $nativeRoot
$winMainSource = Get-Content -LiteralPath (Join-Path $nativeRoot 'src\WinMain.cpp') -Raw
if ($winMainSource -match 'TaskDialogIndirect|MB_YESNOCANCEL' -or
    $winMainSource -notmatch 'InternalDialogMode::CloseBehavior' -or
    $winMainSource -notmatch 'L"直接退出"' -or
    $winMainSource -notmatch 'L"最小化到系统托盘"' -or
    $winMainSource -notmatch 'L"记住本次选择"' -or
    $winMainSource -notmatch 'ID_INTERNAL_REMEMBER') {
    throw 'Close confirmation must use the RenderMesh internal dialog with two explicit choices and a remember checkbox.'
}
if ($winMainSource -notmatch 'bool UpdateTrayIcon\(\)' -or
    $winMainSource -notmatch 'RegisterWindowMessageW\(L"TaskbarCreated"\)' -or
    $winMainSource -notmatch 'if \(!UpdateTrayIcon\(\)\)' -or
    $winMainSource -notmatch '避免程序隐藏后无法打开') {
    throw 'Close-to-tray must keep the window visible on registration failure and recover after Explorer restarts.'
}
if ($winMainSource -notmatch 'ReplaceFileW\(target\.c_str\(\), pending\.c_str\(\), backup\.c_str\(\)' -or
    $winMainSource -match 'MoveFileExW\(target\.c_str\(\), backup\.c_str\(\)') {
    throw 'The updater must replace the executable and create its rollback backup atomically.'
}
if ($winMainSource -notmatch 'gUploadSelectableNodeIds' -or
    $winMainSource -notmatch 'void ApplyUploadRowSelection\(' -or
    $winMainSource -notmatch 'void SelectUploadRows\(' -or
    $winMainSource -notmatch 'SelectedUploadNodeIds\(') {
    throw 'Participant multi-select and select-all must operate on explicit row selection.'
}
if ($winMainSource -notmatch 'void FitListColumns\(' -or
    $winMainSource -notmatch 'void FitMainListColumns\(' -or
    $winMainSource -match 'AddColumn\(nodes, 3, 85, L"调度策略"\)' -or
    $winMainSource -match 'L"应用选择"') {
    throw 'Visible lists must fit their client width and the obsolete scheduling/apply-selection UI must be removed.'
}
$localComputeStatus = [regex]::Match(
    $winMainSource,
    'std::wstring LocalComputeStatusText\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$remoteComputeStatus = [regex]::Match(
    $winMainSource,
    'std::wstring RemoteComputeStatusText\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $localComputeStatus.Success -or
    -not $remoteComputeStatus.Success -or
    $remoteComputeStatus.Value -match 'idleSeconds|to_wstring' -or
    $winMainSource -notmatch 'localReadyShowsAvailable' -or
    $winMainSource -notmatch 'localActiveShowsInUse' -or
    $winMainSource -notmatch 'remoteReadyHidesIdleDuration' -or
    $winMainSource -notmatch 'remoteActiveShowsInUse') {
    throw 'Compute availability must use only ready/in-use labels and must not expose idle duration.'
}
if ($winMainSource -notmatch 'void PositionRenderSoftwareProfileDrawer\(' -or
    $winMainSource -notmatch 'case WM_WINDOWPOSCHANGED:[\s\S]{0,260}?PositionRenderSoftwareProfileDrawer' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_EDIT' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_DELETE') {
    throw 'The software-profile drawer must follow its owner window and configuration editing must support deletion.'
}
if ($winMainSource -notmatch 'void HideNativeListScrollbars\(' -or
    $winMainSource -notmatch 'void DrawCustomListScrollbar\(' -or
    $winMainSource -notmatch 'CustomListScrollbarThumb\(' -or
    $winMainSource -notmatch 'void LayoutConfigWindow\(' -or
    $winMainSource -notmatch 'CreateBrandedTrayIcon\(TrayVisualState') {
    throw 'Lists need the shared internal scrollbar, render config must use responsive content bounds, and tray icons need branded state badges.'
}
if ($winMainSource -notmatch 'ID_UPLOAD_ADD' -or
    $winMainSource -notmatch 'ID_UPLOAD_DISCONNECT' -or
    $winMainSource -notmatch 'void ShowUploadHostContextMenu\(' -or
    $winMainSource -notmatch 'void LayoutUploadWindow\(' -or
    $winMainSource -notmatch 'case WM_SIZE:[\s\S]{0,180}?LayoutUploadWindow') {
    throw 'Participant editing must expose explicit add/disconnect actions, a matching context menu, and responsive layout.'
}
if ($winMainSource -notmatch 'RECT CenteredOwnedWindowRect\(' -or
    ([regex]::Matches($winMainSource, 'CenterOwnedWindow\(').Count -lt 4)) {
    throw 'Top-level workflow, configuration, detail, and log windows must center on their current owner.'
}
if ($winMainSource -notmatch 'gMainRefreshDeferredByModal' -or
    $winMainSource -notmatch 'modal_dialog_blocks_list_refresh' -or
    $winMainSource -notmatch 'SetWindowPos\(gInternalDialog, HWND_TOP') {
    throw 'Internal dialogs must stay above lists and defer background list repaint until close.'
}
if ($winMainSource -notmatch 'enum class WorkflowProgressStage' -or
    $winMainSource -notmatch 'void ShowWorkflowProgress\(' -or
    $winMainSource -notmatch 'void CloseWorkflowProgress\(' -or
    $winMainSource -notmatch 'ID_WORKFLOW_PROGRESS' -or
    $winMainSource -notmatch 'ID_WORKFLOW_CANCEL' -or
    $winMainSource -notmatch 'workflow_progress_blocks_main_input' -or
    $winMainSource -notmatch 'workflow_progress_allows_window_drag' -or
    $winMainSource -notmatch 'workflow_progress_shows_cancel_after_timeout' -or
    $winMainSource -notmatch 'workflow_progress_cancel_sets_stop_signal') {
    throw 'Blend inspection and preflight must use a staged overlay that blocks unsafe actions while preserving window drag and delayed cancellation.'
}
if ($winMainSource -notmatch 'void StartCancelUploadJob\(\)' -or
    $winMainSource -notmatch 'gUploadCancelRunning = true;' -or
    $winMainSource -notmatch 'gJobOperation = std::thread\(\[jobId\]' -or
    $winMainSource -notmatch 'gApp->StopJob\(jobId, result->message\)' -or
    $winMainSource -notmatch 'StartCancelUploadJob\(\);' -or
    $winMainSource -notmatch
        'case WM_RM_JOB_OPERATION:[\s\S]{0,520}?gJobOperationRunning = false;') {
    throw 'Long-running upload cancellation must execute away from the UI thread.'
}
$importThread = [regex]::Match(
    $winMainSource,
    'gPreflight\s*=\s*std::thread[\s\S]*?\n    \}\);',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $importThread.Success -or
    $importThread.Value.IndexOf('adapter->Prepare(') -lt 0 -or
    $importThread.Value.IndexOf('fs::remove_all(work') -lt 0 -or
    $importThread.Value.IndexOf('gPreflightRunning = false;') -gt
        $importThread.Value.IndexOf('PostMessageW(gWindow, WM_RM_PREFLIGHT')) {
    throw 'Project import must run one adapter pass, remove its work folder and publish completion before notifying the UI.'
}
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tools') -Directory |
    Where-Object Name -Like 'llvm-mingw-*-ucrt-x86_64' |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $toolchain) {
    throw 'Portable llvm-mingw toolchain not found under tools\.'
}

$compiler = Join-Path $toolchain.FullName 'bin\clang++.exe'
$buildDir = Join-Path $nativeRoot 'build\tests'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$output = Join-Path $buildDir 'UiModelTests.exe'

Push-Location $repoRoot
try {
    & $compiler -std=c++20 -DUNICODE -D_UNICODE -Inative/src `
        -Wall -Wextra -Wpedantic -O0 -g `
        native/tests/UiModelTests.cpp native/src/UiModel.cpp `
        -static -static-libgcc -static-libstdc++ -o $output
    if ($LASTEXITCODE -ne 0) { throw 'UI model test build failed.' }
    & $output
    if ($LASTEXITCODE -ne 0) { throw 'UI model tests failed.' }
} finally {
    Pop-Location
}
