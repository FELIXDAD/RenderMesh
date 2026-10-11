[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'src\Node.cpp') -Raw

function Get-Function([string]$pattern) {
    [regex]::Match($source, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
}

$ensure = Get-Function 'bool EnsureRemoteProjectCache\([\s\S]*?\n    \}\r?\n'
if (-not $ensure.Success -or $ensure.Value -notmatch 'UploadProjectResumable\(') {
    throw 'Every peer must use resumable project upload, including plain multi-gigabyte .blend files.'
}

$upload = Get-Function 'bool UploadProjectResumable\([\s\S]*?\n    \}\r?\n'
if (-not $upload.Success -or
    $upload.Value -notmatch 'client\.set_keep_alive\(true\)' -or
    $upload.Value -notmatch 'set_read_timeout\(kProjectChunkTimeoutSeconds\)' -or
    $upload.Value -notmatch 'set_read_timeout\(kProjectCommitTimeoutSeconds\)' -or
    $upload.Value -notmatch 'ConfigureClient\(client, 5, &stopRequested, jobCancelled\)' -or
    $upload.Value -notmatch '/v3/cache/chunk' -or
    $upload.Value -notmatch '/v3/cache/commit') {
    throw 'Resumable upload must reuse one cancellable keep-alive connection with chunk and commit timeouts.'
}

if ($source -notmatch 'verifiedUploadBytes' -or
    $source -match 'FILE_FLAG_WRITE_THROUGH' -or
    $source -notmatch 'if \(!verifiedThisSession\) return Sha256File\(committing') {
    throw 'Chunks must not force a disk flush; only bytes from an earlier session need a full re-hash at commit.'
}

if ($source -notmatch 'Pre-cancelled large project request self-test failed' -or
    $source -notmatch '/slow-bytes' -or
    $source -notmatch '/cancel-bytes' -or
    $source -notmatch 'Large project upload-status validation self-test failed') {
    throw 'Cancellation, slow-network timeouts and upload-status validation must remain covered by the self-test.'
}

$snapshot = Get-Function 'bool SnapshotProject\([\s\S]*?\n\}\r?\n'
if (-not $snapshot.Success -or
    $snapshot.Value -notmatch 'MoveFileExW\(report\.preparedProject\.c_str\(\), snapshot\.payload\.c_str\(\)' -or
    $snapshot.Value -notmatch 'CopyIntoWriter\(' -or
    $snapshot.Value -notmatch 'WriteProjectBundle\(snapshot\.payload, files, error, cancel, progress\)' -or
    $source -match 'CreateHardLinkW' -or
    $source -notmatch 'CreatePreparedDraft\([\s\S]{0,240}?std::atomic<bool>\* cancel' -or
    $source -notmatch 'cancelledBeforeCommit' -or
    $source -notmatch 'DeletePersistedJob\(id, rollbackError\)' -or
    $source -notmatch 'Single-pass project snapshot self-test failed') {
    throw 'Job creation must move a prepared copy, copy and hash other projects in one cancellable pass, and never hard-link user files.'
}

'LARGE_PROJECT_TRANSFER_POLICY_OK'
