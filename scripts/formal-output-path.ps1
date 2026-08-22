Set-StrictMode -Version Latest

function Test-PathWithin([string]$Candidate, [string]$Root) {
    $candidateFull = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    return $candidateFull.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $candidateFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
}

function Assert-P2FormalOutputDirectory([string]$OutputDirectory, [string]$RepositoryRoot,
                                        [bool]$DryRun) {
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        throw 'P2 matrixの出力先が空です'
    }
    $resolved = [IO.Path]::GetFullPath($OutputDirectory)
    if (-not $DryRun -and (Test-PathWithin $resolved $RepositoryRoot)) {
        throw "P2 formalのOutputDirectoryはgit worktree外を指定してください: $resolved"
    }
    return $resolved
}

function Invoke-P2GitText([string]$GitExe, [string]$RepositoryRoot,
                          [string[]]$Arguments) {
    $value = & $GitExe -C $RepositoryRoot @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') に失敗しました"
    }
    return ($value -join "`n")
}

function Get-P2Provenance([string]$RepositoryRoot, [string]$GitExe = 'git') {
    $status = Invoke-P2GitText $GitExe $RepositoryRoot @('status', '--porcelain')
    $diff = Invoke-P2GitText $GitExe $RepositoryRoot @('diff', '--no-ext-diff')
    $cachedDiff = Invoke-P2GitText $GitExe $RepositoryRoot @('diff', '--cached', '--no-ext-diff')
    $untrackedText = Invoke-P2GitText $GitExe $RepositoryRoot `
        @('ls-files', '--others', '--exclude-standard')
    $untracked = @($untrackedText -split "`r?`n" | Where-Object { $_ })
    $untrackedHashes = @($untracked | ForEach-Object {
        $absolute = Join-Path $RepositoryRoot $_
        "$_=$((Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant())"
    })
    $commit = Invoke-P2GitText $GitExe $RepositoryRoot @('rev-parse', 'HEAD')
    $fingerprintText = @($commit, $status, $diff, $cachedDiff,
        ($untrackedHashes -join "`n")) -join "`n--P2-PROVENANCE--`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($fingerprintText)
    $hash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    return [ordered]@{
        git_commit = $commit
        dirty_worktree = -not [string]::IsNullOrWhiteSpace($status)
        git_status_porcelain = @($status -split "`n" | Where-Object { $_ })
        git_diff_stat = Invoke-P2GitText $GitExe $RepositoryRoot @('diff', '--stat')
        source_fingerprint_sha256 = $hash
    }
}
