[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodStaticInventory','NegativeUnclassifiedOrdinalWriter',
        'NegativeUnclassifiedLastFinalizedWriter','NegativeUnclassifiedNoDecisionReturn',
        'NegativeSecondIntentProducer','NegativeIndirectOrdinalReconstruction')][string]$Case,
    [Parameter(Mandatory=$true)][string]$SourceRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){
    $path=Join-Path $SourceRoot $RelativePath
    if(-not(Test-Path -LiteralPath $path)){throw "W4-C0対象sourceがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}

function Count-Pattern([string]$Text,[string]$Pattern){
    return [regex]::Matches($Text,$Pattern).Count
}

function Require-Count([string]$Text,[string]$Pattern,[int]$Expected,[string]$Message){
    $actualCount=Count-Pattern $Text $Pattern
    if($actualCount-ne$Expected){throw "$Message (actual=$actualCount expected=$Expected)"}
}

function Get-Function([string]$Text,[string]$Signature){
    $pattern='(?ms)^'+[regex]::Escape($Signature)+'[\s\S]*?^\}'
    $result=[regex]::Match($Text,$pattern)
    if(-not$result.Success){throw "W4-C0対象functionを抽出できません: $Signature"}
    return $result.Value
}

$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$transport=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h'
$document=Read-Source 'docs/p2-d5-2-w4-c-causal-attribution-contract.md'

switch($Case){
    'NegativeUnclassifiedOrdinalWriter'{
        $scheduler=$scheduler.Replace('decision.opportunityOrdinal = ordinal;',
            "decision.opportunityOrdinal = ordinal;`n    decision.opportunityOrdinal = ordinal + 1;")
    }
    'NegativeUnclassifiedLastFinalizedWriter'{
        $scheduler=$scheduler.Replace('lastFinalizedOrdinal_ = ordinal;',
            "lastFinalizedOrdinal_ = ordinal;`n    lastFinalizedOrdinal_ = ordinal + 1;")
    }
    'NegativeUnclassifiedNoDecisionReturn'{
        $scheduler=$scheduler.Replace('long long target = -1;',
            "if (renderOrdinal == 42)`n        return {};`n    long long target = -1;")
    }
    'NegativeSecondIntentProducer'{
        $renderer=$renderer.Replace(
            'nativePresentToken.setFormalIntentOrdinal(formalDecision.opportunityOrdinal)',
            'nativePresentToken.setFormalIntentOrdinal(formalDecision.targetFrame)')
    }
    'NegativeIndirectOrdinalReconstruction'{
        $renderer=$renderer.Replace('bool formalDecisionObserved = false;',
            "bool formalDecisionObserved = false;`n        const auto reconstructedIntentOrdinal = formalDecision.targetFrame;")
    }
}

$select=Get-Function $scheduler `
    'PresentationOpportunityDecision PresentationOpportunityScheduler::selectForRender('
$finalize=Get-Function $scheduler 'bool PresentationOpportunityScheduler::finalizePendingOpportunity()'

$rejected=$false
try{
    # ordinal producerとadvancement式。writer追加・別identityへの差し替えを受理しない。
    Require-Count $select 'decision\.opportunityOrdinal\s*=\s*ordinal\s*;' 1 `
        'formal decision ordinalのproducerが単一ではありません'
    Require-Count $select 'opportunityOrdinal\s*=' 1 `
        '未分類のopportunity ordinal writerがあります'
    Require-Count $select 'ordinal\s*=\s*completed\s*\+\s*1\s*;' 1 `
        'anchored ordinal advancement式が変更されています'
    Require-Count $select 'presentationOpportunityOrdinal\(originRefreshCount_,' 1 `
        'ordinalがrefresh authority以外から作られています'

    # last finalizedの実行時writerはfinalize時の1箇所だけ。
    Require-Count $scheduler 'lastFinalizedOrdinal_\s*=' 1 `
        '未分類のlast finalized writerがあります'
    Require-Count $finalize 'lastFinalizedOrdinal_\s*=\s*ordinal\s*;' 1 `
        'last finalized writerがfinalize pathから外れています'

    # C2 instrumentation後も全returnはbranch-exact finishInvocationを通る。
    # 正常NO_DECISIONやledgerを迂回するreturnを暗黙追加できないよう固定する。
    Require-Count $select 'return\s+\{\s*\}\s*;' 0 `
        'invocation ledgerを迂回するinvalid/no-decision returnがあります'
    Require-Count $select 'return\s+finishInvocation\(' 9 `
        '未分類またはinstrumentationを迂回するreturnがあります'
    Require-Count $select 'PresentationSchedulerInvocationResult::InvalidFatal' 6 `
        'fatal resultのbranch数が分類と一致しません'
    Require-Count $select 'PresentationSchedulerInvocationResult::DuplicateDecision' 1 `
        'duplicate decision resultが変更されています'
    Require-Count $select 'PresentationSchedulerInvocationResult::OutsideSourceDomainDecision' 1 `
        'source-domain resultが変更されています'
    Require-Count $select 'PresentationSchedulerInvocationResult::PrimaryDecision' 1 `
        'primary decision resultが変更されています'
    Require-Count $select 'fail\(PresentationOpportunityError::InvalidConfiguration\)' 1 `
        'INVALID_CONFIGURATION fatal returnが分類と一致しません'
    Require-Count $select 'fail\(PresentationOpportunityError::AuthorityDiscontinuity\)' 2 `
        'AUTHORITY_DISCONTINUITY fatal returnが分類と一致しません'
    Require-Count $select 'fail\(PresentationOpportunityError::OpportunityRegression\)' 1 `
        'OPPORTUNITY_REGRESSION fatal returnが分類と一致しません'
    Require-Count $select 'fail\(PresentationOpportunityError::ArithmeticOverflow\)' 2 `
        'ARITHMETIC_OVERFLOW fatal returnが分類と一致しません'

    # required-domainとtargetは別式。intent transportのproducerはordinal直結の1箇所だけ。
    Require-Count $select 'requiredIntentMembership\s*=\s*ordinal\s*>=\s*0\s*&&\s*ordinal\s*<\s*config_\.requiredFrameCount' 1 `
        'required intent membership式が変更されています'
    Require-Count $select 'target\s*>=\s*config_\.requiredFrameCount' 1 `
        'past source domain式が変更されています'
    Require-Count $renderer `
        'setFormalIntentOrdinal\(formalDecision\.opportunityOrdinal\)' 1 `
        'intent transport producerが単一ordinal直結ではありません'
    if($renderer-match '(?i)(reconstruct|derive)[A-Za-z_]*IntentOrdinal\s*='){
        throw 'target/source fieldからintent ordinalを間接再構築しています'
    }

    # invocation gateと全measurement stop authorityをsource上で固定する。
    Require-Count $renderer 'if \(output < 0 && formalOpportunityActive\)' 1 `
        'formal scheduler invocation gateが変更されています'
    Require-Count $renderer 'formalOpportunityCaptureActive\.exchange\(false,' 1 `
        'measurement中のcapture gate close authorityが単一ではありません'
    Require-Count $renderer 'if \(intervalEnded \|\| stopRequested\)' 1 `
        'planned/explicit measurement stop pathが変更されています'
    Require-Count $renderer 'formalOpportunityDomainReached\.store\(true,' 2 `
        'domain terminal branchの分類数が変更されています'

    if($document-notmatch 'Contract \(FROZEN\)' -or
       $document-notmatch 'w4_c0_static_inventory_complete = true' -or
       $document-notmatch 'root_cause_determined = false'){
        throw 'W4-C freezeまたはC0 verdictが文書に固定されていません'
    }
    if($document-match 'OUTSIDE_REQUIRED_DOMAIN_DECISION' -or
       $document-notmatch 'OUTSIDE_SOURCE_DOMAIN_DECISION' -or
       $document-notmatch 'SUPPRESSED_OUTSIDE_REQUIRED_INTENT_DOMAIN'){
        throw 'source domain resultとrequired intent transport dispositionが分離されていません'
    }
}catch{
    $rejected=$true
    if($Case-eq'GoodStaticInventory'){throw}
}

if($Case-ne'GoodStaticInventory' -and -not$rejected){
    throw "$Case が静的inventoryに受理されました"
}
Write-Output "W4-C0 static control-flow contract $Case : PASS"
