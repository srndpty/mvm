[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=5,
    [ValidateRange(30,600)][int]$TimeoutSeconds=120,
    [string]$LiveRunner=(Join-Path $PSScriptRoot 'p2-d5-2-w2-c01-live.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2-W3 fresh acquisition。
#
# W2-E で canonical presentation authority を formal-v2 exact chain へ切り替えた。
# W3 は「切替後の HEAD / binary」で初めて fresh capture を取る段である。
# したがって acquisition 時点の provenance を必ず artifact として残す。
# 後段の W3 evaluator は、この checkpoint と一致しない capture を canonical にしない。
#
# 本 script は ETW session を張るため管理者権限が必要である (live runner が検査する)。

function Fail([string]$Message){throw $Message}
$repo=Split-Path -Parent $PSScriptRoot

$headSha=(& git -C $repo rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or[string]::IsNullOrWhiteSpace($headSha)){Fail 'HEADを解決できません'}
$status=& git -C $repo status --porcelain
$worktreeClean=[string]::IsNullOrWhiteSpace(($status|Out-String))
if(-not$worktreeClean){
    # dirty tree の capture は「どの binary で取ったか」を後から再現できない。
    Fail 'W3 fresh acquisitionはclean worktreeから取得してください (未コミットの変更があります)'
}

$binaries=[ordered]@{
    compositor_spike='build\ucrt64-release\bin\mvm_compositor_spike.exe'
    decoder='build\p2-etw-decoder\mvm_present_history_decoder.exe'
    qt_gui='build\qtbase-c0\bin\Qt6Gui.dll'
    qt_quick='build\qtquick-t2-runtime\Qt6Quick.dll'
}
$binaryHashes=[ordered]@{}
foreach($name in @($binaries.Keys)){
    $path=Join-Path $repo $binaries[$name]
    if(-not(Test-Path -LiteralPath $path)){Fail "W3 acquisition必須binaryがありません: $($binaries[$name])"}
    $binaryHashes[$name+'_sha256']=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

if(Test-Path -LiteralPath $OutputDirectory){Fail "既存W3 cohortを上書きしません: $OutputDirectory"}
if(-not(Test-Path -LiteralPath $LiveRunner)){Fail "live runnerがありません: $LiveRunner"}

Write-Host ("P2-D5-2 W3 fresh acquisition: checkpoint={0} runs={1} measure={2}s" -f `
    $headSha.Substring(0,12),$Runs,$MeasureSeconds)
& pwsh -NoProfile -File $LiveRunner -OutputDirectory $OutputDirectory -Runs $Runs `
    -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
if($LASTEXITCODE-ne0){Fail 'W3 fresh acquisitionのC0.1.1 live gateが失敗しました'}

$summaryPath=Join-Path $OutputDirectory 'w2-c01-live-summary.json'
if(-not(Test-Path -LiteralPath $summaryPath)){Fail "live summaryがありません: $summaryPath"}
$summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json

# acquisition 後も HEAD が動いていないこと。capture 中に別 binary へ入れ替わっていない。
$headAfter=(& git -C $repo rev-parse HEAD).Trim()
if($headAfter-ne$headSha){Fail 'acquisition中にHEADが変化しました'}
foreach($name in @($binaries.Keys)){
    $path=Join-Path $repo $binaries[$name]
    $after=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if($after-ne[string]$binaryHashes[$name+'_sha256']){Fail "acquisition中に$name が変化しました"}
}

$provenance=[ordered]@{
    schema='mvm-p2-d5-2-w3-acquisition-provenance-1';stage='P2-D5-2-W3'
    fresh_acquisition=$true
    acquisition_mode='CanonicalPresentMonLive'
    checkpoint_sha=$headSha
    worktree_clean=$true
    cohort_directory=(Resolve-Path -LiteralPath $OutputDirectory).Path
    run_count=$Runs
    warmup_seconds=$WarmupSeconds
    measure_seconds=$MeasureSeconds
    coverage_complete=[bool]$summary.coverage_complete
    intent_scope_exact=[bool]$summary.intent_scope_exact
    missing_scope_count=[int64]$summary.missing_scope_count
    ambiguous_scope_count=[int64]$summary.ambiguous_scope_count
    mutated_scope_count=[int64]$summary.mutated_scope_count
    live_summary_sha256=(Get-FileHash -LiteralPath $summaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    acquired_at_utc=(Get-Date).ToUniversalTime().ToString('o')
}
foreach($key in @($binaryHashes.Keys)){$provenance[$key]=$binaryHashes[$key]}
$provenancePath=Join-Path $OutputDirectory 'w3-acquisition-provenance.json'
$provenance|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $provenancePath -Encoding utf8
Write-Host ("P2-D5-2 W3 fresh acquisition: PASS cohort={0}" -f $provenance.cohort_directory)
Write-Host ("  provenance: {0}" -f $provenancePath)
