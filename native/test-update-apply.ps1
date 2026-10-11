[CmdletBinding()]
param(
    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\RenderMesh.exe')
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$winMainSource = Get-Content -LiteralPath (
    Join-Path $PSScriptRoot 'src\WinMain.cpp') -Raw
if ($winMainSource -notmatch
        'CopyFileW\(staged\.c_str\(\), helper\.c_str\(\), FALSE\)' -or
    $winMainSource -notmatch
        'Sha256File\(pending\) != expectedSha256' -or
    $winMainSource -notmatch
        'RecoverUpdateExecutableAfterReplaceFailure' -or
    $winMainSource -notmatch
        'MoveFileExW\(backup\.c_str\(\), target\.c_str\(\)' -or
    $winMainSource -notmatch
        'WaitForMultipleObjects\(2, readyWaits, FALSE, 10000\)' -or
    $winMainSource -notmatch
        'OpenEventW\([\s\S]{0,120}?readyEventName\.c_str\(\)\)' -or
    $winMainSource -notmatch
        'HANDLE parent = OpenProcess\([\s\S]{0,900}?SetEvent\(readyEvent\)' -or
    $winMainSource -notmatch
        'RelaunchAfterUpdateFailure\(target, backup, 21, waitError\)' -or
    $winMainSource -notmatch
        'RestorePreviousUpdateWithRetry' -or
    $winMainSource -notmatch
        '--update-health-event' -or
    $winMainSource -notmatch
        'WaitForMultipleObjects\([\s\S]{0,100}?startupWaits[\s\S]{0,100}?15000\)' -or
    $winMainSource -notmatch
        'updated instance acknowledged UI and node service readiness' -or
    $winMainSource -notmatch
        'gUpdateApplyThread = std::thread' -or
    $winMainSource -notmatch
        'case WM_RM_UPDATE_APPLIED:') {
    throw 'The updater helper must come from the verified incoming executable.'
}
$toolchain = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tools') -Directory |
    Where-Object Name -Like 'llvm-mingw-*-ucrt-x86_64' |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $toolchain) { throw 'Portable llvm-mingw toolchain not found.' }

$buildRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build'))
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $buildRoot (
    'update-apply-' + [guid]::NewGuid().ToString('N'))))
if (-not $fixtureRoot.StartsWith(
        $buildRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Update fixture escaped the build directory.'
}
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$appRoot = Join-Path $fixtureRoot 'app'
$dataRoot = Join-Path $fixtureRoot 'data'
$helperRoot = Join-Path $fixtureRoot 'helper'
$failureAppRoot = Join-Path $fixtureRoot 'failure-app'
$failureDataRoot = Join-Path $fixtureRoot 'failure-data'
$failureHelperRoot = Join-Path $fixtureRoot 'failure-helper'
New-Item -ItemType Directory -Path $appRoot,$dataRoot,$helperRoot,
    $failureAppRoot,$failureDataRoot,$failureHelperRoot | Out-Null

$target = Join-Path $appRoot 'RenderMesh.exe'
$pending = Join-Path $appRoot 'RenderMesh-0.2.37.exe.pending'
$backup = $target + '.previous'
$helper = Join-Path $helperRoot 'RenderMesh-Updater.exe'
$stub = Join-Path $fixtureRoot 'UpdateTargetStub.exe'
$oldStub = Join-Path $fixtureRoot 'UpdateTargetOldStub.exe'
$marker = Join-Path $fixtureRoot 'replacement-started.txt'
$parent = $null
$updater = $null
$lock = $null
$readyEvent = $null
$failureParent = $null
$failureUpdater = $null
$rollbackLock = $null
$failureReadyEvent = $null
$previousMarker = $env:RM_UPDATE_TEST_MARKER
$previousSkipHealth = $env:RM_UPDATE_TEST_SKIP_HEALTH

try {
    $compiler = Join-Path $toolchain.FullName 'bin\clang++.exe'
    & $compiler -std=c++20 -DUNICODE -D_UNICODE -municode -O2 `
        -static -static-libgcc -static-libstdc++ `
        (Join-Path $PSScriptRoot 'tests\UpdateTargetStub.cpp') -o $stub
    if ($LASTEXITCODE -ne 0) { throw 'Update target stub build failed.' }
    & $compiler -std=c++20 -DUNICODE -D_UNICODE -DUPDATE_STUB_OLD=1 `
        -municode -O2 -static -static-libgcc -static-libstdc++ `
        (Join-Path $PSScriptRoot 'tests\UpdateTargetStub.cpp') -o $oldStub
    if ($LASTEXITCODE -ne 0) { throw 'Old update target stub build failed.' }
    Copy-Item -LiteralPath $ExePath -Destination $target
    Copy-Item -LiteralPath $ExePath -Destination $helper
    Copy-Item -LiteralPath $stub -Destination $pending
    $pendingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pending).Hash
    $oldTargetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash

    $lock = [IO.File]::Open(
        $target, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $parent = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-Command', 'Start-Sleep -Milliseconds 350') `
        -WindowStyle Hidden -PassThru
    $env:RM_UPDATE_TEST_MARKER = $marker
    $readyEventName = 'Local\RenderMesh.UpdateReady.Test.' +
        [guid]::NewGuid().ToString('N')
    $readyEvent = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $readyEventName)
    $updater = Start-Process -FilePath $helper -ArgumentList @(
        '--apply-update', [string]$parent.Id,
        $pending, $target, $backup, '0.2.37', $pendingHash.ToLowerInvariant(),
        '--update-ready-event', $readyEventName,
        '--data-dir', $dataRoot) -WindowStyle Hidden -PassThru

    if (-not $readyEvent.WaitOne(5000)) {
        throw 'Updater helper did not acknowledge readiness before replacement.'
    }

    $parent | Wait-Process -Timeout 5
    Start-Sleep -Milliseconds 1400
    $lock.Dispose()
    $lock = $null
    $updater | Wait-Process -Timeout 15

    $pendingStillExists = Test-Path -LiteralPath $pending -PathType Leaf
    $targetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash
    $backupExists = Test-Path -LiteralPath $backup -PathType Leaf
    $markerExists = Test-Path -LiteralPath $marker -PathType Leaf
    if ($updater.ExitCode -ne 0 -or $pendingStillExists -or
        $targetHash -ne $pendingHash -or -not $backupExists -or
        -not $markerExists) {
        throw ('Updater did not recover from a transient target lock: ' +
            "exit=$($updater.ExitCode); pending=$pendingStillExists; " +
            "target_unchanged=$($targetHash -eq $oldTargetHash); " +
            "backup=$backupExists; replacement_started=$markerExists")
    }

    $failureTarget = Join-Path $failureAppRoot 'RenderMesh.exe'
    $failurePending = Join-Path $failureAppRoot 'RenderMesh-0.2.37.exe.pending'
    $failureBackup = $failureTarget + '.previous'
    $failureHelper = Join-Path $failureHelperRoot 'RenderMesh-Updater.exe'
    $failureMarker = Join-Path $fixtureRoot 'failure-replacement-started.txt'
    Copy-Item -LiteralPath $oldStub -Destination $failureTarget
    Copy-Item -LiteralPath $stub -Destination $failurePending
    Copy-Item -LiteralPath $ExePath -Destination $failureHelper
    $failureOldHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $failureTarget).Hash
    $failurePendingHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $failurePending).Hash
    $env:RM_UPDATE_TEST_MARKER = $failureMarker
    $env:RM_UPDATE_TEST_SKIP_HEALTH = '1'
    $failureReadyEventName = 'Local\RenderMesh.UpdateReady.RollbackTest.' +
        [guid]::NewGuid().ToString('N')
    $failureReadyEvent = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $failureReadyEventName)
    $failureParent = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-Command', 'Start-Sleep -Milliseconds 350') `
        -WindowStyle Hidden -PassThru
    $failureUpdater = Start-Process -FilePath $failureHelper -ArgumentList @(
        '--apply-update', [string]$failureParent.Id,
        $failurePending, $failureTarget, $failureBackup, '0.2.37',
        $failurePendingHash.ToLowerInvariant(),
        '--update-ready-event', $failureReadyEventName,
        '--data-dir', $failureDataRoot) -WindowStyle Hidden -PassThru
    if (-not $failureReadyEvent.WaitOne(5000)) {
        throw 'Rollback updater helper did not acknowledge readiness.'
    }
    $failureParent | Wait-Process -Timeout 5
    $markerDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not (Test-Path -LiteralPath $failureMarker -PathType Leaf) -and
           [DateTime]::UtcNow -lt $markerDeadline) {
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path -LiteralPath $failureMarker -PathType Leaf)) {
        throw 'Unhealthy replacement did not start.'
    }
    $rollbackLock = [IO.File]::Open(
        $failureTarget, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    Start-Sleep -Milliseconds 8000
    $rollbackLock.Dispose()
    $rollbackLock = $null
    $failureUpdater | Wait-Process -Timeout 20
    $failureTargetHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $failureTarget).Hash
    $failurePendingExists = Test-Path -LiteralPath $failurePending -PathType Leaf
    $failedReplacementExists = Test-Path -LiteralPath (
        $failureTarget + '.failed-update') -PathType Leaf
    if ($failureUpdater.ExitCode -ne 26 -or
        $failureTargetHash -ne $failureOldHash -or
        $failurePendingExists -or $failedReplacementExists) {
        throw ('Updater did not roll back an unhealthy replacement: ' +
            "exit=$($failureUpdater.ExitCode); restored=" +
            "$($failureTargetHash -eq $failureOldHash); " +
            "pending=$failurePendingExists; failed_copy=$failedReplacementExists")
    }
    'UPDATE_APPLY_OK'
}
finally {
    if ($lock) { $lock.Dispose() }
    if ($readyEvent) { $readyEvent.Dispose() }
    if ($rollbackLock) { $rollbackLock.Dispose() }
    if ($failureReadyEvent) { $failureReadyEvent.Dispose() }
    $env:RM_UPDATE_TEST_MARKER = $previousMarker
    $env:RM_UPDATE_TEST_SKIP_HEALTH = $previousSkipHealth
    foreach ($process in @($parent, $updater,$failureParent,$failureUpdater)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ExecutablePath -and
            $_.ExecutablePath.StartsWith(
                $fixtureRoot, [StringComparison]::OrdinalIgnoreCase)
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        }
    Start-Sleep -Milliseconds 150
    if (Test-Path -LiteralPath $fixtureRoot) {
        $verified = [IO.Path]::GetFullPath($fixtureRoot)
        if ($verified.StartsWith(
                $buildRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $verified -Recurse -Force
        }
    }
}
