[CmdletBinding()]
param(
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2-W2-E.2 retirement completeness。
#
# 目的は legacy field / log / code を消すことではない。retirement = deletion ではない。
# 証明したいのは 1 点だけである。
#
#     legacy presentation authority が canonical verdict の入力として使われていない
#
# そのため次を定義で分ける。
#
#   CANONICAL_PERFORMANCE  run の presentation / performance verdict を決める判定。
#                          fps / drop / threshold の PASS/FAIL がこれにあたる。
#                          W2-E 後は 0 件でなければならない。
#   DIAGNOSTIC_INTEGRITY   diagnostic ledger が自己整合であることの検査。
#                          「記録値が再計算値と一致するか」しか主張しないため、
#                          presentation authority を主張していない。残してよい。
#
# canonical checker 側は disposition を machine-readable に宣言し、legacy metric を
# 参照する失敗地点には注記を置く。注記の無い失敗地点は UNCLASSIFIED として
# fail-close する。これにより「新しい fps threshold FAIL をうっかり足す」ことを防ぐ。
#
# 走査対象を列挙で固定すると、登録されていない checker に threshold を足すだけで
# false-PASS できてしまう。そのため対象は scripts/check-*.ps1 から discovery する。
# legacy metric を失敗判定に使う checker は、登録の有無に関係なくすべて分類対象になる。

function Fail([string]$Message){throw $Message}

# legacy presentation authority に属する performance metric 名。
$legacyPerformanceMetrics=@('effective_fps','drop_rate','effective_video_fps')
# 失敗を発生させる呼び出し。checker 内の fail helper も含める
# (Close / Require-Equal / Require-Zero は不一致で FAIL する)。
$failureEmitters=@('Add-Failure','Fail ','Close ','Require-Equal ','Require-Zero ')
$expectedDisposition=[ordered]@{
    presentation_authority='FORMAL_V2'
    legacy_presentation_metrics='DIAGNOSTIC'
    canonical_performance_verdict='DEFERRED_TO_W3'
}
# 走査対象は discovery する。ハードコードした列挙は使わない。
# legacy presentation authority を produce しているが verdict を出さない場所。
# diagnostic として存在してよいことを positive に記録する。
$legacyDiagnosticSources=@(
    @('src/app/preview/compositor_rhi_item.cpp','recordFrameSwapped'),
    @('src/media/gpu_preview/presentation_opportunity_scheduler.h','commitSwap'))

function Remove-PowerShellComments([string]$Text){
    return [regex]::Replace($Text,'(?m)#.*$','')
}

$scriptDirectory=Join-Path $SourceRoot 'scripts'
if(-not(Test-Path -LiteralPath $scriptDirectory)){Fail "scripts directoryがありません: $scriptDirectory"}
$discovered=@(Get-ChildItem -LiteralPath $scriptDirectory -Filter 'check-*.ps1' -File|Sort-Object Name)
if($discovered.Count-eq0){Fail 'checkerを1件もdiscoveryできませんでした'}

$checkers=@();$canonicalCount=0L;$diagnosticCount=0L;$unclassifiedCount=0L
$undeclaredCheckers=@()
foreach($file in $discovered){
    $relative='scripts/'+$file.Name
    $path=$file.FullName
    $text=Get-Content -LiteralPath $path -Raw -Encoding utf8
    $lines=@($text-split"`r?`n")

    # legacy performance metric を参照する失敗地点の分類。
    $sites=@()
    for($index=0;$index-lt$lines.Count;++$index){
        $line=$lines[$index]
        $code=Remove-PowerShellComments $line
        $emits=$false
        foreach($emitter in $failureEmitters){if($code-match[regex]::Escape($emitter)){$emits=$true}}
        if(-not$emits){continue}
        $metrics=@($legacyPerformanceMetrics|Where-Object{$code-match[regex]::Escape($_)})
        if($metrics.Count-eq0){continue}
        # 注記は同じ行か直前の行に置く。
        $annotationScope=$line
        if($index-gt0){$annotationScope=$lines[$index-1]+"`n"+$line}
        $classification='UNCLASSIFIED'
        if($annotationScope-match'W2-E:\s*DIAGNOSTIC_INTEGRITY'){$classification='DIAGNOSTIC_INTEGRITY'}
        elseif($annotationScope-match'W2-E:\s*CANONICAL_PERFORMANCE'){$classification='CANONICAL_PERFORMANCE'}
        switch($classification){
            'CANONICAL_PERFORMANCE'{$canonicalCount+=1}
            'DIAGNOSTIC_INTEGRITY'{$diagnosticCount+=1}
            default{$unclassifiedCount+=1}
        }
        $sites+=,[ordered]@{
            line=$index+1
            metrics=@($metrics)
            classification=$classification
            statement=$code.Trim()
        }
    }

    # legacy metric を失敗判定に使っていない checker は presentation authority の
    # 当事者ではない。走査対象からは外すが、discovery したことは数える。
    if($sites.Count-eq0){continue}

    # legacy metric を使う checker は disposition を宣言していなければならない。
    # 宣言が無ければ UNCLASSIFIED として fail-close する。登録漏れで通り抜けない。
    $disposition=[ordered]@{}
    $declared=$true
    foreach($field in @($expectedDisposition.Keys)){
        $pattern=[regex]::Escape($field)+"\s*=\s*'([^']+)'"
        $match=[regex]::Match($text,$pattern)
        if(-not$match.Success){$declared=$false;break}
        $disposition[$field]=$match.Groups[1].Value
    }
    if(-not$declared){
        $undeclaredCheckers+=$relative
        $unclassifiedCount+=$sites.Count
        $checkers+=,[ordered]@{
            checker=$relative
            sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            disposition=$null
            authority_disposition_declared=$false
            legacy_performance_threshold_reaches_verdict=$true
            legacy_metric_failure_sites=$sites
        }
        $canonicalCount+=1
        continue
    }
    foreach($field in @($expectedDisposition.Keys)){
        if([string]$disposition[$field]-ne[string]$expectedDisposition[$field]){
            Fail "$relative のauthority dispositionがW2-E契約と一致しません: $field=$($disposition[$field])"
        }
    }

    # threshold 定数が失敗地点へ到達していないこと。W2-E 前の形へ戻ると引っかかる。
    $thresholdReintroduced=$false
    foreach($index in 0..($lines.Count-1)){
        $code=Remove-PowerShellComments $lines[$index]
        if($code-notmatch'(?i)(fps|drop)'){continue}
        if($code-notmatch'-lt\s*55|-gt\s*0\.02|<\s*55\.0|>\s*0\.02'){continue}
        $emits=$false
        $scope=$code
        if($index+1-lt$lines.Count){$scope=$code+"`n"+(Remove-PowerShellComments $lines[$index+1])}
        foreach($emitter in $failureEmitters){if($scope-match[regex]::Escape($emitter)){$emits=$true}}
        if($emits){$thresholdReintroduced=$true}
    }
    if($thresholdReintroduced){$canonicalCount+=1}

    $checkers+=,[ordered]@{
        checker=$relative
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        disposition=$disposition
        authority_disposition_declared=$true
        legacy_performance_threshold_reaches_verdict=$thresholdReintroduced
        legacy_metric_failure_sites=$sites
    }
}

# retirement = deletion ではないことの positive 記録。
$diagnosticSources=@()
foreach($source in $legacyDiagnosticSources){
    $path=Join-Path $SourceRoot $source[0]
    $present=(Test-Path -LiteralPath $path)-and
        ((Get-Content -LiteralPath $path -Raw -Encoding utf8)-match[regex]::Escape($source[1]))
    $diagnosticSources+=,[ordered]@{
        source=$source[0];symbol=$source[1];present=$present
        authoritative=$false;used_for_canonical_verdict=$false
    }
}
$diagnosticsRetained=@($diagnosticSources|Where-Object{[bool]$_.present}).Count-eq$diagnosticSources.Count

$blockers=@{}
if($canonicalCount-ne0){$blockers['LEGACY_METRIC_FEEDS_CANONICAL_VERDICT']=$true}
if($undeclaredCheckers.Count-ne0){$blockers['LEGACY_METRIC_CHECKER_AUTHORITY_UNDECLARED']=$true}
if($unclassifiedCount-ne0){$blockers['LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED']=$true}
if(-not$diagnosticsRetained){$blockers['LEGACY_DIAGNOSTIC_SOURCE_MISSING']=$true}
$blockerList=@($blockers.Keys|Sort-Object)

$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-e-legacy-authority-retirement-1';stage='P2-D5-2-W2-E.2'
    presentation_authority_schema='FORMAL_V2'
    legacy_presentation_authority='FRAME_SWAPPED_AND_DISPLAY_LEDGER'
    retirement_means_deletion=$false
    checker_discovery='scripts/check-*.ps1'
    discovered_checker_count=$discovered.Count
    legacy_metric_consumer_count=$checkers.Count
    authority_undeclared_checkers=@($undeclaredCheckers)
    canonical_checker_count=$checkers.Count
    legacy_metric_canonical_decision_count=$canonicalCount
    legacy_metric_diagnostic_integrity_count=$diagnosticCount
    legacy_metric_unclassified_count=$unclassifiedCount
    legacy_diagnostics_retained=$diagnosticsRetained
    canonical_performance_verdict_evaluated=$false
    canonical_performance_verdict_deferred_to='W3'
    retirement_exact=$blockerList.Count-eq0
    blockers=$blockerList
    canonical_checkers=$checkers
    legacy_diagnostic_sources=$diagnosticSources
    verdict=$(if($blockerList.Count-eq0){'LEGACY_PRESENTATION_AUTHORITY_RETIRED'}else{'LEGACY_PRESENTATION_AUTHORITY_STILL_CANONICAL'})
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    $outputDirectory=Split-Path -Parent $Output
    if(-not[string]::IsNullOrWhiteSpace($outputDirectory)-and-not(Test-Path -LiteralPath $outputDirectory)){
        New-Item -ItemType Directory -Path $outputDirectory|Out-Null
    }
    $result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
}
if(-not[bool]$result.retirement_exact){Fail "W2-E.2 retirementが不成立です: $($blockerList-join', ')"}
Write-Output ("P2-D5-2 W2-E.2 legacy authority retirement: PASS (canonical={0} diagnostic={1})" -f `
    $canonicalCount,$diagnosticCount)
