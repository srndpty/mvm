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
# 走査対象を列挙で固定すると、登録されていない checker に threshold を足すだけで
# false-PASS できる。そのため対象は scripts/check-*.ps1 から discovery する。
#
# さらに failure site の検出を「metric名とFAILが同一行にある」に頼ると、
#
#     $fps = Require-Property $raw 'effective_fps'
#     $minimum = 55
#     if ($fps -lt $minimum) { Fail '...' }
#
# のように行が分かれた瞬間にすり抜ける。行 regex を増やすのではなく PowerShell AST を
# 使い、legacy metric から taint を伝播させて判定式を追う。
# 加えて file-level fallback として、legacy metric を参照し失敗しうる checker が
# authority disposition を宣言していなければ、site 検出の結果に関係なく fail-close する。

function Fail([string]$Message){throw $Message}

# legacy presentation authority に属する performance metric 名。
# 部分一致にすると canonical_effective_fps / canonical_drop_rate のような
# formal-v2 側の別 metric まで legacy 扱いになる。identifier 境界で照合する。
$legacyPerformanceMetrics=@('effective_fps','drop_rate','effective_video_fps')
function Get-MvmELegacyMetricPattern([string]$Metric){
    return '(?<![A-Za-z0-9_])'+[regex]::Escape($Metric)+'(?![A-Za-z0-9_])'
}
$expectedDisposition=[ordered]@{
    presentation_authority='FORMAL_V2'
    legacy_presentation_metrics='DIAGNOSTIC'
    canonical_performance_verdict='DEFERRED_TO_W3'
}
# legacy presentation authority を produce しているが verdict を出さない場所。
# diagnostic として存在してよいことを positive に記録する。
$legacyDiagnosticSources=@(
    @('src/app/preview/compositor_rhi_item.cpp','recordFrameSwapped'),
    @('src/media/gpu_preview/presentation_opportunity_scheduler.h','commitSwap'))

function Remove-PowerShellComments([string]$Text){
    return [regex]::Replace($Text,'(?m)#.*$','')
}

# node の配下に legacy metric literal または taint 済み変数への参照があるか。
function Test-MvmELegacyReference($Node,[hashtable]$Tainted,[string[]]$Metrics){
    if($null-eq$Node){return $false}
    $text=Remove-PowerShellComments $Node.Extent.Text
    foreach($metric in $Metrics){if($text-match(Get-MvmELegacyMetricPattern $metric)){return $true}}
    foreach($variable in @($Node.FindAll({param($n)
        $n-is[System.Management.Automation.Language.VariableExpressionAst]},$true))){
        if($Tainted.ContainsKey($variable.VariablePath.UserPath)){return $true}
    }
    return $false
}

# 失敗を発生させうる node。checker ごとの fail helper 名は揃っていないため、
# 名前規約 (*Fail* / Close / Require-* / Assert-*) と throw / 非0 exit を拾う。
function Get-MvmEFailureEmitters($Ast){
    $emitters=@()
    foreach($command in @($Ast.FindAll({param($n)
        $n-is[System.Management.Automation.Language.CommandAst]},$true))){
        $name=$command.GetCommandName()
        if([string]::IsNullOrWhiteSpace($name)){continue}
        if($name-match'Fail'-or$name-eq'Close'-or$name-match'^(Require|Assert)-'){
            $emitters+=,$command
        }
    }
    $emitters+=@($Ast.FindAll({param($n)
        $n-is[System.Management.Automation.Language.ThrowStatementAst]},$true))
    foreach($exitStatement in @($Ast.FindAll({param($n)
        $n-is[System.Management.Automation.Language.ExitStatementAst]},$true))){
        if($null-ne$exitStatement.Pipeline-and$exitStatement.Pipeline.Extent.Text-match'^\s*[1-9]'){
            $emitters+=,$exitStatement
        }
    }
    return $emitters
}

# emitter を囲む if の条件をすべて遡る。入れ子の外側が legacy 依存なら、
# 内側の条件が clean でもその判定は legacy 由来である。
function Test-MvmEDecisionUsesLegacy($Node,[hashtable]$Tainted,[string[]]$Metrics){
    if(Test-MvmELegacyReference $Node $Tainted $Metrics){return $true}
    $current=$Node.Parent
    while($null-ne$current){
        if($current-is[System.Management.Automation.Language.IfStatementAst]){
            foreach($clause in $current.Clauses){
                if(Test-MvmELegacyReference $clause.Item1 $Tainted $Metrics){return $true}
            }
        }
        $current=$current.Parent
    }
    return $false
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
    $strippedText=Remove-PowerShellComments $text

    # legacy metric を一切参照しない checker は presentation authority の当事者ではない。
    $referencesLegacy=@($legacyPerformanceMetrics|
        Where-Object{$strippedText-match(Get-MvmELegacyMetricPattern $_)}).Count-ne0
    if(-not$referencesLegacy){continue}

    $tokens=$null;$parseErrors=$null
    $ast=[System.Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$parseErrors)
    if($null-ne$parseErrors-and$parseErrors.Count-ne0){
        Fail "$relative をAST解析できません (parse error $($parseErrors.Count)件)"
    }

    # legacy metric から taint を伝播させる。alias 経由の判定を追うため
    # 変化が無くなるまで繰り返す。
    $assignments=@($ast.FindAll({param($n)
        $n-is[System.Management.Automation.Language.AssignmentStatementAst]},$true))
    $tainted=@{}
    do{
        $changed=$false
        foreach($assignment in $assignments){
            if($assignment.Left-isnot[System.Management.Automation.Language.VariableExpressionAst]){continue}
            $name=$assignment.Left.VariablePath.UserPath
            if($tainted.ContainsKey($name)){continue}
            if(Test-MvmELegacyReference $assignment.Right $tainted $legacyPerformanceMetrics){
                $tainted[$name]=$true;$changed=$true
            }
        }
    }while($changed)

    $emitters=@(Get-MvmEFailureEmitters $ast)
    $sites=@()
    foreach($emitter in $emitters){
        if(-not(Test-MvmEDecisionUsesLegacy $emitter $tainted $legacyPerformanceMetrics)){continue}
        $line=$emitter.Extent.StartLineNumber
        # 注記は同じ行か直前の行に置く。
        $annotationScope=$lines[$line-1]
        if($line-gt1){$annotationScope=$lines[$line-2]+"`n"+$annotationScope}
        $classification='UNCLASSIFIED'
        if($annotationScope-match'W2-E:\s*DIAGNOSTIC_INTEGRITY'){$classification='DIAGNOSTIC_INTEGRITY'}
        elseif($annotationScope-match'W2-E:\s*CANONICAL_PERFORMANCE'){$classification='CANONICAL_PERFORMANCE'}
        $sites+=,[ordered]@{
            line=$line
            classification=$classification
            statement=(Remove-PowerShellComments $emitter.Extent.Text).Trim()
        }
    }

    # file-level fallback。legacy metric を参照し、かつ失敗しうる checker は
    # site 検出の結果に関係なく authority disposition を宣言していなければならない。
    # 未登録 checker のすり抜けはここで止まる。
    $disposition=[ordered]@{}
    $declared=$true
    foreach($field in @($expectedDisposition.Keys)){
        $pattern=[regex]::Escape($field)+"\s*=\s*'([^']+)'"
        $match=[regex]::Match($text,$pattern)
        if(-not$match.Success){$declared=$false;break}
        $disposition[$field]=$match.Groups[1].Value
    }
    if(-not$declared-and$emitters.Count-ne0){
        $undeclaredCheckers+=$relative
        $unclassifiedCount+=[Math]::Max($sites.Count,1)
        $canonicalCount+=1
        $checkers+=,[ordered]@{
            checker=$relative
            sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            disposition=$null
            authority_disposition_declared=$false
            failure_emitter_count=$emitters.Count
            legacy_metric_failure_sites=$sites
        }
        continue
    }
    if(-not$declared){continue}
    foreach($field in @($expectedDisposition.Keys)){
        if([string]$disposition[$field]-ne[string]$expectedDisposition[$field]){
            Fail "$relative のauthority dispositionがW2-E契約と一致しません: $field=$($disposition[$field])"
        }
    }
    foreach($site in $sites){
        switch([string]$site.classification){
            'CANONICAL_PERFORMANCE'{$canonicalCount+=1}
            'DIAGNOSTIC_INTEGRITY'{$diagnosticCount+=1}
            default{$unclassifiedCount+=1}
        }
    }
    $checkers+=,[ordered]@{
        checker=$relative
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        disposition=$disposition
        authority_disposition_declared=$true
        failure_emitter_count=$emitters.Count
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
    schema='mvm-p2-d5-2-w2-e-legacy-authority-retirement-2';stage='P2-D5-2-W2-E.2'
    presentation_authority_schema='FORMAL_V2'
    legacy_presentation_authority='FRAME_SWAPPED_AND_DISPLAY_LEDGER'
    retirement_means_deletion=$false
    checker_discovery='scripts/check-*.ps1'
    failure_site_analysis='POWERSHELL_AST_WITH_LEGACY_METRIC_TAINT'
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
Write-Output ("P2-D5-2 W2-E.2 legacy authority retirement: PASS (consumers={0} canonical={1} diagnostic={2})" -f `
    $checkers.Count,$canonicalCount,$diagnosticCount)
