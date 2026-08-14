[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = Split-Path -Parent $nativeRoot

& (Join-Path $nativeRoot 'test-ui.ps1')
& (Join-Path $nativeRoot 'build.ps1') -Configuration $Configuration

$exe = Join-Path $nativeRoot "build\$Configuration\RenderMesh.exe"
$verificationRoot = Join-Path $nativeRoot 'build\ui-regression'
$dataDir = Join-Path $verificationRoot 'data'
$reportPath = Join-Path $verificationRoot 'metrics.json'
New-Item -ItemType Directory -Force -Path $verificationRoot | Out-Null

$process = Start-Process -FilePath $exe -ArgumentList @(
    '--data-dir', $dataDir,
    '--port', '49391',
    '--page', '1',
    '--ui-test-rows', '100',
    '--ui-metrics-report', $reportPath
) -WindowStyle Hidden -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "RenderMesh UI regression process failed with exit code $($process.ExitCode)."
}
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
    throw 'RenderMesh did not produce the UI regression report.'
}

$metrics = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
$checks = [ordered]@{
    jobs_visible_rows = $metrics.jobs_visible_rows -ge 7
    stable_snapshot_is_noop = -not $metrics.stable_snapshot_mutated -and
        $metrics.stable_snapshot_mutation_delta -eq 0
    one_row_change_is_incremental = $metrics.incremental_mutation_delta -eq 1
    structural_update_is_exercised = [bool]$metrics.structural_update_applied
    scroll_anchor_preserved = [bool]$metrics.scroll_anchor_preserved
    selection_preserved = [bool]$metrics.selection_preserved
    focus_preserved = [bool]$metrics.focus_preserved
    horizontal_scroll_preserved = [bool]$metrics.horizontal_scroll_preserved
    horizontal_scroll_was_nonzero = [bool]$metrics.horizontal_scroll_was_nonzero
    top_pixel_offset_preserved = [bool]$metrics.top_pixel_offset_preserved
    control_order_matches_model = [bool]$metrics.control_order_matches_model
    idle_heartbeat_is_1hz = $metrics.idle_heartbeat_interval_ms -eq 1000
    burst_refresh_is_coalesced = $metrics.burst_refresh_min_interval_ms -ge 16
    burst_notifications_coalesce_to_one_refresh =
        $metrics.ui_test_burst_notifications -eq 100 -and
        $metrics.observed_burst_refreshes -eq 1
    idle_heartbeat_was_observed = $metrics.observed_heartbeat_refreshes -ge 1 -and
        $metrics.observed_heartbeat_refreshes -le 2
    share_control_fits = [bool]$metrics.share_text_fits
}
$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value })
if ($failed.Count -gt 0) {
    throw ('UI regression checks failed: ' + (($failed | ForEach-Object Key) -join ', '))
}

$fullResetCalls = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'src') -File |
    Select-String -SimpleMatch 'ListView_DeleteAllItems'
if ($fullResetCalls) {
    throw 'A full ListView reset call was reintroduced into the native UI.'
}
$legacyControls = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'src') -File |
    Select-String -Pattern 'BS_PUSHBUTTON|DarkMode_ItemsView'
if ($legacyControls) {
    throw 'A legacy default button or header theme was reintroduced into the native UI.'
}

[pscustomobject]@{
    Result = 'UI_REGRESSION_OK'
    JobsVisibleRows = $metrics.jobs_visible_rows
    StableSnapshotMutations = $metrics.stable_snapshot_mutation_delta
    IncrementalMutationDelta = $metrics.incremental_mutation_delta
    ScrollAnchorPreserved = $metrics.scroll_anchor_preserved
    SelectionPreserved = $metrics.selection_preserved
    FullResetCalls = 0
    LegacyControlStyles = 0
}
