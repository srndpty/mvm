[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Json,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Json)){throw "W4-C2 captureがありません: $Json"}
$app=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
if($app.PSObject.Properties.Name-notcontains'formal_scheduler_invocation_ledger'){
    throw 'W4-C2 scheduler invocation ledgerがありません'
}
$ledger=$app.formal_scheduler_invocation_ledger
if([string]$ledger.schema-ne'mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1'){
    throw 'W4-C2 invocation schemaが不正です'
}
if(-not[bool]$ledger.diagnostic_root_cause_capture-or
   [bool]$ledger.canonical_performance_authority){
    throw 'W4-C2 captureのdiagnostic/performance authority分離が不正です'
}
if([bool]$ledger.physical_vblank_successor_required-or
   [bool]$ledger.physical_mapping_support_authority){
    throw 'W4-C2がphysical VBlank mapping authorityへ昇格されています'
}
if([int64]$ledger.measurement_stop_qpc-le0-or[int64]$ledger.native_envelope_close_qpc-le0){
    throw 'W4-C2 capture gate closeのraw QPCがありません'
}
$records=@($ledger.records)
if($records.Count-eq0-or$records.Count-ne[int64]$ledger.record_count){
    throw 'W4-C2 invocation record countが不正です'
}
$allowedResults=@('PRIMARY_DECISION','DUPLICATE_DECISION','OUTSIDE_SOURCE_DOMAIN_DECISION',
    'INVALID_FATAL')
$reasonByResult=@{
    PRIMARY_DECISION=@('PRIMARY')
    DUPLICATE_DECISION=@('PENDING_RENDER')
    OUTSIDE_SOURCE_DOMAIN_DECISION=@('PAST_SOURCE_DOMAIN')
    INVALID_FATAL=@('INVALID_CONFIGURATION','AUTHORITY_UNUSABLE','CALLBACK_QPC_REGRESSION',
        'COMPLETED_ORDINAL_UNAVAILABLE','COMPLETED_ORDINAL_OVERFLOW','TARGET_ARITHMETIC_OVERFLOW')
}
$resultCounts=[ordered]@{}
foreach($name in $allowedResults){$resultCounts[$name]=0L}
$previousQpc=0L;$terminalIndex=-1
for($index=0;$index-lt$records.Count;++$index){
    $record=$records[$index]
    $serial=[uint64]$record.scheduler_invocation_serial
    if($serial-ne[uint64]($index+1)){throw "invocation serial gapがあります: index=$index serial=$serial"}
    $qpc=[int64]$record.invocation_qpc
    if($qpc-le0-or($previousQpc-gt0-and$qpc-lt$previousQpc)){
        throw "invocation QPCが単調ではありません: serial=$serial"
    }
    $previousQpc=$qpc
    if(-not[bool]$record.state_transition_exact){
        throw "pre/post stateがexactではありません: serial=$serial"
    }
    $result=[string]$record.result
    $reason=[string]$record.reason
    if($result-notin$allowedResults-or$reason-notin@($reasonByResult[$result])){
        throw "result/reasonがbranch-exact enumと一致しません: serial=$serial $result/$reason"
    }
    $resultCounts[$result]=[int64]$resultCounts[$result]+1
    if([int64]$record.pre.last_finalized_opportunity_ordinal-ne
       [int64]$record.post.last_finalized_opportunity_ordinal){
        throw "select中にlast finalizedが変更されました: serial=$serial"
    }
    if($result-eq'INVALID_FATAL'){
        if([bool]$record.decision_valid-or[bool]$record.formal_transport_disposition_exact){
            throw "fatal invocationにvalid decision/transport dispositionがあります: serial=$serial"
        }
        continue
    }
    if(-not[bool]$record.decision_valid-or
       -not[bool]$record.formal_transport_disposition_exact){
        throw "valid invocationのdecision/transport dispositionがexactではありません: serial=$serial"
    }
    if($result-eq'PRIMARY_DECISION'-or$result-eq'OUTSIDE_SOURCE_DOMAIN_DECISION'){
        if([bool]$record.pre.anchored){
            $refresh=[uint64]$record.input_authority.refresh_count
            $origin=[uint64]$record.pre.origin_refresh_count
            if($refresh-lt$origin-or[uint64]$record.intent_ordinal-ne($refresh-$origin+1)){
                throw "completed refresh + 1をinvocationから再生できません: serial=$serial"
            }
        }elseif([int64]$record.intent_ordinal-ne0){
            throw "unanchored primary ordinalが0ではありません: serial=$serial"
        }
    }
    if($result-eq'PRIMARY_DECISION'-and-not[bool]$record.post.pending_render){
        throw "primary decision後にpending renderが立っていません: serial=$serial"
    }
    if($result-eq'DUPLICATE_DECISION'-and
       (-not[bool]$record.pre.pending_render-or-not[bool]$record.post.pending_render-or
        -not[bool]$record.duplicate_callback)){
        throw "duplicate decisionのpre/post stateが不正です: serial=$serial"
    }
    if($result-eq'OUTSIDE_SOURCE_DOMAIN_DECISION'){
        if($terminalIndex-ge0-or-not[bool]$record.past_source_domain-or
           -not[bool]$record.post.past_source_domain){
            throw "source-domain terminal stateが不正です: serial=$serial"
        }
        $terminalIndex=$index
    }
}
if($terminalIndex-lt0){throw 'OUTSIDE_SOURCE_DOMAIN_DECISIONがありません'}
if($terminalIndex-ne$records.Count-1){throw 'source-domain terminal後にもscheduler invocationがあります'}
$terminal=$records[$terminalIndex]
$terminalMembership=[bool]$terminal.required_intent_membership
$terminalDisposition=[string]$terminal.formal_transport_disposition
$expectedTerminalDisposition=$(if($terminalMembership){'TRANSPORT'}else{'SUPPRESS_OUTSIDE_REQUIRED_SET'})
if($terminalDisposition-ne$expectedTerminalDisposition){
    throw 'source-domain resultとrequired-intent transport dispositionが独立fieldとして整合しません'
}
$summary=[ordered]@{
    schema='mvm-p2-d5-2-w4-c2-invocation-ledger-check-1';stage='P2-D5-2-W4-C2'
    source_json=(Resolve-Path -LiteralPath $Json).Path
    source_json_sha256=(Get-FileHash -LiteralPath $Json -Algorithm SHA256).Hash.ToLowerInvariant()
    diagnostic_root_cause_capture=$true
    canonical_performance_authority=$false
    invocation_count=$records.Count
    result_counts=$resultCounts
    invocation_sequence_gap_count=0
    state_transition_exact=$true
    source_domain_required_domain_separated=$true
    terminal_invocation_serial=[string]$terminal.scheduler_invocation_serial
    terminal_intent_ordinal=[string]$terminal.intent_ordinal
    terminal_required_intent_membership=$terminalMembership
    terminal_past_source_domain=[bool]$terminal.past_source_domain
    terminal_formal_transport_disposition=$terminalDisposition
    post_terminal_invocation_count=0
    branch_execution_exact=$true
    verdict='SCHEDULER_INVOCATION_CONTROL_FLOW_EXACT'
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    if(Test-Path -LiteralPath $Output){throw "既存W4-C2 checkを上書きしません: $Output"}
    $summary|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
}
Write-Host ("P2-D5-2 W4-C2 invocation ledger: PASS invocations={0} terminal={1}" -f `
    $summary.invocation_count,$summary.terminal_invocation_serial)
return $summary
