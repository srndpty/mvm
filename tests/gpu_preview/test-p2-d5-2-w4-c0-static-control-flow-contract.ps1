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
$contract=Read-Source 'docs/p5-e4-s2-d-w4-c0-current-control-flow-contract.md'
$originalSources=@($scheduler,$renderer)
$expectedViolations=@{
    NegativeUnclassifiedOrdinalWriter='未分類のopportunity ordinal writerがあります (actual=2 expected=1)'
    NegativeUnclassifiedLastFinalizedWriter='未分類のlast finalized writerがあります (actual=2 expected=1)'
    NegativeUnclassifiedNoDecisionReturn='invocation ledgerを迂回するinvalid/no-decision returnがあります (actual=1 expected=0)'
    NegativeSecondIntentProducer='intent transport producerが単一ordinal直結ではありません (actual=0 expected=1)'
    NegativeIndirectOrdinalReconstruction='target/source/callback/QPCからintent ordinalを間接再構築しています'
}

switch($Case){
    'NegativeUnclassifiedOrdinalWriter'{
        $scheduler=$scheduler.Replace('decision.opportunityOrdinal = ordinal;',
            "decision.opportunityOrdinal = ordinal;`n    decision.opportunityOrdinal = ordinal + 1;")
    }
    'NegativeUnclassifiedLastFinalizedWriter'{
        $scheduler=$scheduler.Replace('lastFinalizedOrdinal_ = prepared.record.actualOpportunityOrdinal;',
            "lastFinalizedOrdinal_ = prepared.record.actualOpportunityOrdinal;`n    lastFinalizedOrdinal_ = prepared.record.actualOpportunityOrdinal + 1;")
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

$mutatedSources=@($scheduler,$renderer)
if($Case-ne'GoodStaticInventory'){
    $mutationApplied=$false
    for($sourceIndex=0;$sourceIndex-lt$originalSources.Count;++$sourceIndex){
        if($mutatedSources[$sourceIndex]-cne$originalSources[$sourceIndex]){
            $mutationApplied=$true
            break
        }
    }
    if(-not$mutationApplied){throw "mutation対象が見つかりません: $Case"}
}

$select=Get-Function $scheduler `
    'PresentationOpportunityDecision PresentationOpportunityScheduler::selectForRender('
$finalize=Get-Function $scheduler 'bool PresentationOpportunityScheduler::finalizePendingOpportunity()'
$apply=Get-Function $scheduler `
    'void PresentationOpportunityScheduler::applyPendingOpportunityFinalization('

$intendedViolationDetected=$false
try{
    # B3 current authority。queue reservationが唯一のordinal producerである。
    Require-Count $select 'const auto queueDecision\s*=\s*requiredIntentQueue_\.reserveHead\(\)\s*;' 1 `
        'required intent queue reservationが単一producerではありません'
    Require-Count $select `
        'const long long ordinal\s*=\s*queueDecision\.reservation\.intentOrdinal\s*;' 1 `
        'intent ordinalがqueue reservation identityから作られていません'
    Require-Count $select '\bordinal\s*=' 1 `
        'queue reservation以外のordinal producerがあります'
    Require-Count $select 'decision\.opportunityOrdinal\s*=\s*ordinal\s*;' 1 `
        'formal decision ordinalのproducerが単一ではありません'
    Require-Count $select 'opportunityOrdinal\s*=' 1 `
        '未分類のopportunity ordinal writerがあります'
    Require-Count $select 'presentationOpportunityOrdinal\(' 0 `
        'physical refresh ordinalからintent ordinalを再構築しています'
    Require-Count $select 'completed\s*\+\s*1' 0 `
        'completed refresh countからintent ordinalを再構築しています'

    # writerはapply関数内の1箇所だけ。finalize wrapperも必ず同じapplyへ流す。
    Require-Count $scheduler 'lastFinalizedOrdinal_\s*=' 1 `
        '未分類のlast finalized writerがあります'
    Require-Count $apply `
        'lastFinalizedOrdinal_\s*=\s*prepared\.record\.actualOpportunityOrdinal\s*;' 1 `
        'last finalized writerがapply pathから外れています'
    Require-Count $finalize 'preparePendingOpportunityFinalization\(prepared\)' 1 `
        'finalize pathがpending opportunityのprepareを通りません'
    Require-Count $finalize 'applyPendingOpportunityFinalization\(prepared\)' 1 `
        'finalize pathが単一apply writerを通りません'

    # 全returnをbranch-exact finishInvocationで閉じる。
    Require-Count $select 'return\s+\{\s*\}\s*;' 0 `
        'invocation ledgerを迂回するinvalid/no-decision returnがあります'
    Require-Count $select 'return\s+finishInvocation\(' 9 `
        '未分類またはinstrumentationを迂回するreturnがあります'
    Require-Count $select 'PresentationSchedulerInvocationResult::InvalidFatal' 6 `
        'fatal resultのbranch数が分類と一致しません'
    Require-Count $select 'PresentationSchedulerInvocationResult::DuplicateDecision' 1 `
        'duplicate decision resultが変更されています'
    Require-Count $select 'PresentationSchedulerInvocationResult::RequiredQueueExhaustedDecision' 1 `
        'required queue exhausted resultが変更されています'
    Require-Count $select 'PresentationSchedulerInvocationResult::OutsideSourceDomainDecision' 0 `
        'source coverage errorをsuccessful decisionへ戻しています'
    Require-Count $select 'PresentationSchedulerInvocationResult::PrimaryDecision' 1 `
        'primary decision resultが変更されています'

    # source coverage不足はqueue reservation後のprotocol fatalである。
    Require-Count $select `
        'if \(target >= config_\.requiredFrameCount\) \{[\s\S]{0,500}fail\(PresentationOpportunityError::SourceCoverageInsufficient\);[\s\S]{0,300}decision\.valid = false;[\s\S]{0,300}PresentationSchedulerInvocationResult::InvalidFatal,[\s\S]{0,200}PresentationSchedulerInvocationReason::PastSourceDomain' 1 `
        'source coverage errorがprotocol fatalとして分類されていません'

    # required-domainとtargetは別式。transportはdecision ordinal直結の1箇所だけ。
    Require-Count $select 'requiredIntentMembership\s*=\s*ordinal\s*>=\s*0\s*&&\s*ordinal\s*<\s*config_\.requiredFrameCount' 1 `
        'required intent membership式が変更されています'
    Require-Count $select 'target\s*>=\s*config_\.requiredFrameCount' 1 `
        'past source domain式が変更されています'
    Require-Count $renderer `
        'setFormalIntentOrdinal\(formalDecision\.opportunityOrdinal\)' 1 `
        'intent transport producerが単一ordinal直結ではありません'
    if($renderer-match '(?i)\b[A-Za-z_][A-Za-z0-9_]*IntentOrdinal\s*=\s*(formalDecision\.(targetFrame|renderOrdinal|renderBeginQpc)|callbackBegin|output)'){
        throw 'target/source/callback/QPCからintent ordinalを間接再構築しています'
    }

    # invocation gateとmeasurement stop authority。DOMAIN_TERMINALは成功にしない。
    Require-Count $renderer 'if \(output < 0 && formalOpportunityActive && foreignAdmissionOpen\)' 1 `
        'formal scheduler invocation gateが変更されています'
    Require-Count $renderer 'formalOpportunityCaptureActive\.exchange\(false,' 1 `
        'measurement中のcapture gate close authorityが単一ではありません'
    Require-Count $renderer 'if \(intervalEnded \|\| stopRequested\)' 1 `
        'planned/explicit measurement stop pathが変更されています'
    Require-Count $renderer 'finishMeasurement\(callbackBegin,\s*StopArbitration::DomainTerminal' 0 `
        'DOMAIN_TERMINALをsuccessful completionにしています'
    Require-Count $renderer 'formalOpportunityDomainReached\.store\(true,' 0 `
        'DOMAIN_TERMINALをnormal completion side effectにしています'
    Require-Count $renderer 'StopArbitration cause = StopArbitration::PlannedWindowEnd;' 1 `
        'planned window endがnormal completion ownerではありません'
    Require-Count $renderer 'closePlannedWindow\(\)' 1 `
        'normal completion ownerが単一ではありません'
    Require-Count $renderer `
        'cause == StopArbitration::PlannedWindowEnd\s*\?\s*state_->formalOpportunityScheduler\.closePlannedWindow\(\)\s*:\s*state_->formalOpportunityScheduler\.closeWithoutNormalCompletion\(\)' 1 `
        'PLANNED_WINDOW_END以外がnormal completionになり得ます'

    if($transport-notmatch 'RequiredQueueExhaustedDecision' -or
       $contract-notmatch 'w4_c0_current_contract\s+true' -or
       $contract-notmatch 'ordinal_authority\s+REQUIRED_INTENT_QUEUE_RESERVATION' -or
       $contract-notmatch 'source_coverage_failure\s+PROTOCOL_FATAL' -or
       $contract-notmatch 'domain_terminal_success\s+false' -or
       $contract-notmatch 'normal_completion_owner\s+PLANNED_WINDOW_END'){
        throw 'W4-C0 current control-flow contractが文書に固定されていません'
    }
}catch{
    if($Case-eq'GoodStaticInventory'){throw}
    $actualViolation=$_.Exception.Message
    if(-not$expectedViolations.ContainsKey($Case)){
        throw "negative caseの期待違反が定義されていません: $Case"
    }
    $expectedViolation=$expectedViolations[$Case]
    if($actualViolation-cne$expectedViolation){
        throw "negative caseが意図しない契約違反を検出しました: case=$Case expected='$expectedViolation' actual='$actualViolation'"
    }
    $intendedViolationDetected=$true
    Write-Output "expected violation: $actualViolation"
}
if($Case-ne'GoodStaticInventory'-and-not$intendedViolationDetected){
    throw "$Case が静的inventoryに受理されました"
}
Write-Output "W4-C0 static control-flow contract $Case : PASS"
