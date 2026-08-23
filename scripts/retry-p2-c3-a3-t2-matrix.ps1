[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,20)][int]$MaxAttempts=5
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$matrix=Join-Path $PSScriptRoot 'p2-c3-a3-t2-matrix.ps1'
if(-not(Test-Path -LiteralPath $matrix)){throw "T2-B matrix scriptがありません: $matrix"}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent();$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T2-B matrix採取には管理者権限が必要です'}
if(-not(Test-Path -LiteralPath $OutputDirectory)){throw "resume対象のmatrix artifactがありません: $OutputDirectory"}
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$parent=Split-Path -Parent $OutputDirectory
$leaf=Split-Path -Leaf $OutputDirectory
$quarantined=@()
# retry-until-successはcohortを条件付けうる。ここでのretryは環境要因
# (ETW session衝突・VBlank observer preemption)に限る前提で、条件固有の
# 起動失敗が観測された場合はretryせず原因修正へ回すこと。AGENTS.md参照。
Write-Host ''
Write-Host '【操作停止必須】取得中は desktop を操作しないでください。'
Write-Host '条件固有の起動失敗が続く場合は retry せず中止して原因を調査してください。'
Write-Host ''
for($attempt=1;$attempt-le$MaxAttempts;++$attempt){
    Write-Host "F3-C3-A3-T2-B retry: attempt $attempt/$MaxAttempts"
    foreach($session in @('MvmP2TargetedPresentHistory')){
        & logman stop $session -ets *> $null
        if($LASTEXITCODE-eq0){Write-Host "F3-C3-A3-T2-B retry: 残留ETW sessionを停止しました ($session)"}
    }
    $global:LASTEXITCODE=0
    & pwsh -NoProfile -File $matrix -OutputDirectory $OutputDirectory -Resume
    if($LASTEXITCODE-eq0){
        Write-Host "F3-C3-A3-T2-B retry: PASS (attempts=$attempt quarantined=$($quarantined.Count))"
        foreach($entry in $quarantined){Write-Host "  quarantined: $entry"}
        exit 0
    }
    Write-Warning "F3-C3-A3-T2-B retry: attempt $attempt が失敗しました"
    # 完了していない条件artifactのみを隔離する。t2-summary.jsonを持つrunは決して触らない。
    $partial=@(Get-ChildItem -LiteralPath $OutputDirectory -Directory|Where-Object{-not(Test-Path -LiteralPath (Join-Path $_.FullName 't2-summary.json'))})
    if($partial.Count-eq0){throw "未完了runが無いのにmatrixが失敗しました。手動診断が必要です: $OutputDirectory"}
    foreach($directory in $partial){
        $stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
        $destination=Join-Path $parent ('{0}-invalid-{1}-attempt{2}-{3}'-f$leaf,$directory.Name,$attempt,$stamp)
        Move-Item -LiteralPath $directory.FullName -Destination $destination
        $quarantined+=$destination
        Write-Warning "F3-C3-A3-T2-B retry: 未完了runを隔離しました -> $destination"
    }
}
throw "F3-C3-A3-T2-B matrixが $MaxAttempts 回連続で失敗しました。隔離済み: $($quarantined -join '; ')"
