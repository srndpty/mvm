param(
    [Parameter(Mandatory)][string]$Guard,
    [Parameter(Mandatory)][string]$RepositoryRoot,
    [Parameter(Mandatory)][string]$GitExe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Guard

$before = Get-P2Provenance $RepositoryRoot $GitExe
$inside = Join-Path $RepositoryRoot 'build\formal-output-negative'
$rejected = $false
try {
    Assert-P2FormalOutputDirectory $inside $RepositoryRoot $false | Out-Null
} catch {
    $rejected = $true
}
if (-not $rejected) { throw 'non-dry formalのrepo内出力先が拒否されませんでした' }

$externalRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "mvm-p2-output-path-$([Guid]::NewGuid().ToString('N'))"
try {
    $resolved = Assert-P2FormalOutputDirectory $externalRoot $RepositoryRoot $false
    New-Item -ItemType Directory -Path $resolved | Out-Null
    Set-Content -LiteralPath (Join-Path $resolved 'artifact.json') -Value '{}'
    $after = Get-P2Provenance $RepositoryRoot $GitExe
    if ($before.git_commit -ne $after.git_commit -or
        $before.source_fingerprint_sha256 -ne $after.source_fingerprint_sha256) {
        throw 'worktree外へのartifact生成でprovenanceが変化しました'
    }
} finally {
    if (Test-Path -LiteralPath $externalRoot) {
        Remove-Item -LiteralPath $externalRoot -Recurse -Force
    }
}
Write-Host 'P2 formal output path contract: PASS'
