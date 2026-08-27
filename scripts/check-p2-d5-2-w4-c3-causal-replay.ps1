[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Json,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2 W4-C3 exact causal replay checker。
# authorityは保存済みwitness/ledgerのbranch-exact fieldだけであり、QPC近傍・tolerance・
# 配列末尾からcauseやjoinを再構築しない。flagとpublication serialはdiagnostic表示のみで、
# verdict条件には使わない。
$script:Verdict='W4_C3_CAUSAL_REPLAY_EXACT'
class W4C3Failure : System.Exception {
    [string]$Kind
    W4C3Failure([string]$kind,[string]$message):base($message){$this.Kind=$kind}
}
function Fail-Partial([string]$Message){throw [W4C3Failure]::new('W4_C3_PARTIAL',$Message)}
function Fail-Incompatible([string]$Message){throw [W4C3Failure]::new('W4_C3_INCOMPATIBLE',$Message)}
function Need([object]$Object,[string]$Name,[string]$Context){
    if($null-eq$Object){Fail-Partial "${Context} がありません"}
    if($Object.PSObject.Properties.Name-notcontains$Name){Fail-Partial "${Context}.${Name} がありません"}
    return $Object.$Name
}
# producerと同じ64-bit signed checked演算。中間積のoverflowも一致させる。
# PowerShellの'/'と[math]::Floorはdouble経路に入るため、整数除算はDivRemだけを使う。
$script:Int64Max=[int64]::MaxValue
function Exact-Int64Divide([int64]$Numerator,[int64]$Denominator,[string]$Context){
    if($Denominator-le0){Fail-Incompatible "${Context}: denominatorが正ではありません"}
    if($Numerator-lt0){Fail-Incompatible "${Context}: 負のnumeratorはproducer domain外です"}
    $remainder=[int64]0
    # 非負domainなのでtruncation toward zeroとfloorは一致する。
    return [System.Math]::DivRem($Numerator,$Denominator,[ref]$remainder)
}
function Checked-Multiply([int64]$A,[int64]$B,[string]$Context){
    if($A-lt0-or$B-lt0){Fail-Incompatible "${Context}: 負のoperandはproducer domain外です"}
    if($A-ne0){
        $limit=Exact-Int64Divide $script:Int64Max $A $Context
        if($B-gt$limit){Fail-Incompatible "${Context}: checked multiplyがoverflowします"}
    }
    return [int64]([int64]$A*[int64]$B)
}
function Checked-Add([int64]$A,[int64]$B,[string]$Context){
    if($B-gt($script:Int64Max-$A)){Fail-Incompatible "${Context}: checked addがoverflowします"}
    return [int64]($A+$B)
}
function Replay-Target([int64]$Ordinal,$Config,[string]$Context){
    # target = source_frame_offset + floor(ordinal * fpsNum * refreshDen / (fpsDen * refreshNum))
    $numerator=Checked-Multiply $Ordinal ([int64]$Config.source_fps_numerator) $Context
    $numerator=Checked-Multiply $numerator ([int64]$Config.refresh_denominator) $Context
    $denominator=Checked-Multiply ([int64]$Config.source_fps_denominator) `
        ([int64]$Config.refresh_numerator) $Context
    $relative=Exact-Int64Divide $numerator $denominator $Context
    return Checked-Add ([int64]$Config.source_frame_offset) $relative $Context
}
try{
    if(-not(Test-Path -LiteralPath $Json)){Fail-Partial "W4-C3 captureがありません: $Json"}
    $app=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json

    # ---- 1. stop witness ----
    $witness=Need $app 'formal_stop_witness' 'formal_stop_witness'
    if([string](Need $witness 'schema' 'formal_stop_witness')-ne'mvm-p2-d5-2-w4-c3-stop-witness-3'){
        Fail-Partial 'stop witness schemaが一致しません'
    }
    if(-not[bool](Need $witness 'diagnostic_root_cause_capture' 'formal_stop_witness')){
        Fail-Partial 'diagnostic captureが有効ではありません'
    }
    if([bool](Need $witness 'canonical_performance_authority' 'formal_stop_witness')){
        Fail-Incompatible 'stop witnessがcanonical performance authorityへ昇格しています'
    }
    if(-not[bool](Need $witness 'captured' 'formal_stop_witness')){
        Fail-Partial 'stop witnessが記録されていません'
    }
    if([int64](Need $witness 'witness_count' 'formal_stop_witness')-ne1){
        Fail-Incompatible 'stop witnessが1件ではありません'
    }
    if([int64](Need $witness 'duplicate_witness_count' 'formal_stop_witness')-ne0){
        Fail-Incompatible 'duplicate stop witnessがあります'
    }
    if([string](Need $witness 'cause' 'formal_stop_witness')-ne'DOMAIN_TERMINAL'){
        Fail-Incompatible 'stop causeがDOMAIN_TERMINALではありません'
    }

    # ---- 2. arbitration authority ----
    $arbitration=Need $witness 'stop_arbitration' 'formal_stop_witness'
    if(-not[bool](Need $arbitration 'claim_recorded' 'stop_arbitration')){
        Fail-Partial 'claim結果が記録されていません'
    }
    if([string](Need $arbitration 'claim_source' 'stop_arbitration')-ne'THIS_CALL_SITE'){
        Fail-Incompatible 'DOMAIN_TERMINALのclaimがそのcall site由来ではありません'
    }
    if([string](Need $arbitration 'measurement_start_state' 'stop_arbitration')-ne'NONE'){
        Fail-Incompatible 'measurement開始時のarbitrationがNONEではありません'
    }
    if([int64](Need $arbitration 'reset_count_during_measurement' 'stop_arbitration')-ne0){
        Fail-Incompatible 'measurement中にarbitrationがresetされています'
    }
    if([string](Need $arbitration 'previous' 'stop_arbitration')-ne'NONE'){
        Fail-Incompatible 'DOMAIN_TERMINAL claim時に別causeがownershipを持っていました'
    }
    if([string](Need $arbitration 'claimed' 'stop_arbitration')-ne'DOMAIN_TERMINAL'){
        Fail-Incompatible 'claimされたcauseがDOMAIN_TERMINALではありません'
    }
    if(-not[bool](Need $arbitration 'claim_succeeded' 'stop_arbitration')){
        Fail-Incompatible 'DOMAIN_TERMINALがarbitrationに勝っていません'
    }

    # ---- 3. capture gate（同一gateのbefore / exchange実return / after）----
    $pre=Need $witness 'pre' 'formal_stop_witness'
    $action=Need $witness 'action' 'formal_stop_witness'
    $post=Need $witness 'post' 'formal_stop_witness'
    $gateClose=Need $witness 'at_gate_close' 'formal_stop_witness'
    if(-not[bool](Need $gateClose 'snapshot_captured' 'at_gate_close')){
        Fail-Partial 'capture gate close時のsnapshotがありません'
    }
    if(-not[bool](Need $pre 'capture_gate_open' 'pre')){
        Fail-Incompatible 'terminal entry時にcapture gateが開いていません'
    }
    if([bool](Need $pre 'planned_window_end_reached' 'pre')){
        Fail-Incompatible 'terminal callbackでplanned window endも成立しています'
    }
    if(-not[bool](Need $action 'formal_opportunity_domain_reached_published' 'action')-or
       -not[bool](Need $action 'finish_measurement_entered' 'action')-or
       -not[bool](Need $action 'capture_gate_exchange_closed' 'action')-or
       -not[bool](Need $action 'measurement_stop_published' 'action')){
        Fail-Incompatible 'terminal actionがexactに成立していません'
    }
    if([bool](Need $post 'capture_gate_open' 'post')){
        Fail-Incompatible 'capture gateがcloseされていません'
    }

    # ---- 4. exact serial join ----
    $ledger=Need $app 'formal_scheduler_invocation_ledger' 'formal_scheduler_invocation_ledger'
    if([string](Need $ledger 'schema' 'formal_scheduler_invocation_ledger')-ne
       'mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1'){
        Fail-Partial 'C2 invocation ledger schemaが一致しません'
    }
    $records=@(Need $ledger 'records' 'formal_scheduler_invocation_ledger')
    if($records.Count-eq0){Fail-Partial 'invocation recordがありません'}
    $witnessSerial=[int64](Need $witness 'scheduler_invocation_serial' 'formal_stop_witness')
    if($witnessSerial-le0){Fail-Partial 'terminal invocation serialがありません'}
    # joinはserialだけ。QPC近傍・配列末尾・token類似は使わない。
    $joined=@($records|Where-Object{[int64]$_.scheduler_invocation_serial-eq$witnessSerial})
    if($joined.Count-eq0){Fail-Partial 'witness serialへjoinできるinvocation recordがありません'}
    if($joined.Count-ne1){Fail-Incompatible "serial joinが1件ではありません: $($joined.Count)"}
    $terminal=$joined[0]
    if([string]$terminal.result-ne'OUTSIDE_SOURCE_DOMAIN_DECISION'-or
       [string]$terminal.reason-ne'PAST_SOURCE_DOMAIN'){
        Fail-Incompatible 'joined recordがsource-domain terminal branchではありません'
    }
    # witnessのterminal factsとjoined recordを1:1で一致させる。
    if([int64](Need $witness 'terminal_intent_ordinal' 'formal_stop_witness')-ne
       [int64]$terminal.intent_ordinal){
        Fail-Incompatible 'witnessとjoined recordのintent ordinalが一致しません'
    }
    if([int64](Need $witness 'terminal_target_frame' 'formal_stop_witness')-ne
       [int64]$terminal.target_frame){
        Fail-Incompatible 'witnessとjoined recordのtarget frameが一致しません'
    }
    if([bool](Need $witness 'terminal_past_source_domain' 'formal_stop_witness')-ne
       [bool]$terminal.past_source_domain-or-not[bool]$terminal.past_source_domain){
        Fail-Incompatible 'witnessとjoined recordのpast_source_domainが一致しません'
    }
    if([bool](Need $witness 'terminal_required_intent_membership' 'formal_stop_witness')-ne
       [bool]$terminal.required_intent_membership){
        Fail-Incompatible 'witnessとjoined recordのrequired intent membershipが一致しません'
    }
    # source-frame domainのterminal predicateが成立した時点で、required-intent domain上は
    # まだrequired intentであること。両domainの非同一性を保ったcausal chainを要求する。
    if(-not[bool]$terminal.required_intent_membership){
        Fail-Incompatible 'terminal decisionがrequired intent domain外です（W4-C3 closure対象外）'
    }

    # ---- 5. scheduler config（artifact直読み。再構成しない）----
    $config=Need $ledger 'scheduler_config' 'formal_scheduler_invocation_ledger'
    foreach($field in 'source_frame_offset','source_fps_numerator','source_fps_denominator',
                      'refresh_numerator','refresh_denominator','required_frame_count'){
        $null=Need $config $field 'scheduler_config'
    }
    if([int64]$config.required_frame_count-le0-or[int64]$config.source_fps_numerator-le0-or
       [int64]$config.source_fps_denominator-le0-or[int64]$config.refresh_numerator-le0-or
       [int64]$config.refresh_denominator-le0-or[int64]$config.source_frame_offset-lt0){
        Fail-Incompatible 'scheduler configがproducerのvalid domain外です'
    }

    # ---- 6. valid decision replay ----
    $validResults=@('PRIMARY_DECISION','DUPLICATE_DECISION','OUTSIDE_SOURCE_DOMAIN_DECISION')
    $stateFields=@('started','closed','anchored','origin_refresh_count',
                   'last_finalized_opportunity_ordinal','pending_render','past_source_domain')
    # markRenderComplete()/commitSwap()はinvocation ledgerの外でstateを進めるため、
    # invocation間のstate transitionはexactに再構築できない。ここで検証するのは
    # 「観測されたinvocation境界のstateがfrozen monotone/immutability invariantと
    # compatibleであること」であり、exact continuityとは呼ばない。
    $fatalCount=0;$terminalIndex=-1;$replayedCount=0;$previous=$null
    $frozenOrigin=$null
    $previousValid=$null
    for($index=0;$index-lt$records.Count;++$index){
        $record=$records[$index]
        $context="record[$index]"
        foreach($field in 'scheduler_invocation_serial','result','reason','decision_valid',
                          'state_transition_exact','pre','post'){
            $null=Need $record $field $context
        }
        foreach($field in $stateFields){
            $null=Need $record.pre $field "$context.pre"
            $null=Need $record.post $field "$context.post"
        }
        $serial=[int64]$record.scheduler_invocation_serial
        if(-not[bool]$record.state_transition_exact){
            Fail-Incompatible "pre/post stateがexactではありません: serial=${serial}"
        }
        # per invocation invariant
        if(-not[bool]$record.pre.started-or-not[bool]$record.post.started){
            Fail-Incompatible "schedulerがstartedではありません: serial=$serial"
        }
        if([bool]$record.pre.closed-or[bool]$record.post.closed){
            Fail-Incompatible "measurement中にschedulerがclosedです: serial=$serial"
        }
        if(-not[bool]$record.pre.anchored-and[int64]$record.pre.origin_refresh_count-ne0){
            Fail-Incompatible "unanchored stateにorigin refresh countがあります: serial=$serial"
        }
        # anchor成立後のoriginはfrozen。以降の全pre/postで不変。
        if([bool]$record.pre.anchored){
            if($null-eq$frozenOrigin){$frozenOrigin=[int64]$record.pre.origin_refresh_count}
            elseif([int64]$record.pre.origin_refresh_count-ne$frozenOrigin){
                Fail-Incompatible "anchored originが変化しました: serial=$serial"
            }
        }
        if([bool]$record.post.anchored){
            if($null-eq$frozenOrigin){$frozenOrigin=[int64]$record.post.origin_refresh_count}
            elseif([int64]$record.post.origin_refresh_count-ne$frozenOrigin){
                Fail-Incompatible "anchored originが変化しました: serial=$serial"
            }
        }
        if([bool]$record.pre.anchored-and-not[bool]$record.post.anchored){
            Fail-Incompatible "anchored stateが後退しています: serial=$serial"
        }
        if([bool]$record.pre.past_source_domain-and-not[bool]$record.post.past_source_domain){
            Fail-Incompatible "past_source_domainが後退しています: serial=$serial"
        }
        if([int64]$record.post.last_finalized_opportunity_ordinal-lt
           [int64]$record.pre.last_finalized_opportunity_ordinal){
            Fail-Incompatible "last finalized ordinalが後退しています: serial=$serial"
        }
        # invocation境界のinvariant
        if($null-ne$previous){
            if([bool]$previous.post.anchored-and-not[bool]$record.pre.anchored){
                Fail-Incompatible "anchored stateが後退しています: serial=$serial"
            }
            if([int64]$record.pre.last_finalized_opportunity_ordinal-lt
               [int64]$previous.post.last_finalized_opportunity_ordinal){
                Fail-Incompatible "last finalized ordinalが後退しています: serial=$serial"
            }
            if([bool]$previous.post.past_source_domain-and
               -not[bool]$record.pre.past_source_domain){
                Fail-Incompatible "past_source_domainが後退しています: serial=$serial"
            }
        }
        $previous=$record
        if($serial-ne($index+1)){Fail-Incompatible "invocation serialが連続していません: $serial"}
        $result=[string]$record.result
        if($result-eq'INVALID_FATAL'){
            # target arithmetic成立前に終わり得る。target/predicateを捏造しない。
            ++$fatalCount
            if([bool]$record.decision_valid){
                Fail-Incompatible "INVALID_FATALにvalid decisionがあります: serial=$serial"
            }
            continue
        }
        if($result-notin$validResults){Fail-Incompatible "未知のresultです: $result"}
        foreach($field in 'intent_ordinal','target_frame','past_source_domain',
                          'required_intent_membership','input_authority'){
            $null=Need $record $field $context
        }
        $null=Need $record.input_authority 'refresh_count' "$context.input_authority"
        if(-not[bool]$record.decision_valid){
            Fail-Incompatible "valid resultのdecisionがvalidではありません: serial=${serial}"
        }
        $ordinal=[int64]$record.intent_ordinal
        if($ordinal-lt0){Fail-Incompatible "intent ordinalが負です: serial=$serial"}
        # completed refresh -> ordinal
        if([bool]$record.pre.anchored){
            $refresh=[int64]$record.input_authority.refresh_count
            $origin=[int64]$record.pre.origin_refresh_count
            if($refresh-lt$origin-or$ordinal-ne($refresh-$origin+1)){
                Fail-Incompatible "completed refresh + 1をordinalへ再生できません: serial=$serial"
            }
        }elseif($ordinal-ne0){
            Fail-Incompatible "unanchored ordinalが0ではありません: serial=$serial"
        }
        # ordinal -> target -> past_source_domain
        $replayedTarget=Replay-Target $ordinal $config "serial=${serial}"
        if($replayedTarget-ne[int64]$record.target_frame){
            Fail-Incompatible ("target replayが一致しません: serial={0} replayed={1} recorded={2}" -f `
                $serial,$replayedTarget,[int64]$record.target_frame)
        }
        $replayedPast=$replayedTarget-ge[int64]$config.required_frame_count
        if($replayedPast-ne[bool]$record.past_source_domain){
            Fail-Incompatible "past_source_domain replayが一致しません: serial=$serial"
        }
        # required-intent membershipもC0定義からordinalで再生する。
        $null=Need $record 'required_intent_membership_exact' $context
        if(-not[bool]$record.required_intent_membership_exact){
            Fail-Incompatible "required intent membershipがexactではありません: serial=$serial"
        }
        $replayedMembership=($ordinal-ge0-and$ordinal-lt[int64]$config.required_frame_count)
        if($replayedMembership-ne[bool]$record.required_intent_membership){
            Fail-Incompatible "required intent membership replayが一致しません: serial=$serial"
        }
        ++$replayedCount
        if($result-eq'OUTSIDE_SOURCE_DOMAIN_DECISION'){
            if($terminalIndex-ge0){Fail-Incompatible 'source-domain terminalが複数あります'}
            if(-not$replayedPast){
                Fail-Incompatible "terminal predicateがreplayで成立しません: serial=$serial"
            }
            if(-not$replayedMembership){
                Fail-Incompatible 'terminalのrequired intent membership replayがtrueではありません'
            }
            if($null-eq$previousValid){
                Fail-Partial 'terminal直前のvalid decisionがありません'
            }
            if([bool]$previousValid.past_source_domain){
                Fail-Incompatible 'terminal直前decisionが既にsource domain外です'
            }
            if(-not[bool]$record.pre.anchored){
                Fail-Incompatible 'terminal invocationがanchored stateではありません'
            }
            if($null-eq$frozenOrigin-or
               [int64]$record.pre.origin_refresh_count-ne$frozenOrigin){
                Fail-Incompatible 'terminal originがanchor確立時のfrozen originと一致しません'
            }
            if([bool]$record.pre.past_source_domain-or
               -not[bool]$record.post.past_source_domain){
                Fail-Incompatible 'terminalのpast_source_domain遷移がexactではありません'
            }
            if([int64]$record.pre.last_finalized_opportunity_ordinal-lt
               [int64]$previousValid.post.last_finalized_opportunity_ordinal){
                Fail-Incompatible 'terminalのlast finalizedが直前valid invocationから後退しています'
            }
            $terminalIndex=$index
        }
        $previousValid=$record
    }
    if($fatalCount-ne0){Fail-Incompatible "INVALID_FATALがあります: $fatalCount"}
    if($terminalIndex-lt0){Fail-Partial 'source-domain terminal decisionがありません'}
    if($terminalIndex-ne$records.Count-1){
        Fail-Incompatible 'terminal後にscheduler invocationがあります'
    }
    if([int64]$terminal.scheduler_invocation_serial-ne
       [int64]$records[$terminalIndex].scheduler_invocation_serial){
        Fail-Incompatible 'joined terminalとreplay terminalが別recordです'
    }

    # ---- 7. C2 ledger authority envelope ----
    # C2 ledger自身のauthority envelopeは既存C2 checkerをprerequisiteとして再実行する。
    # C3側へ条件を複製してdriftさせない。
    $c2Checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w4-c2-invocation-ledger.ps1'
    if(-not(Test-Path -LiteralPath $c2Checker)){Fail-Partial 'W4-C2 checkerが見つかりません'}
    $c2Output=& pwsh -NoProfile -File $c2Checker -Json $Json 2>&1
    if($LASTEXITCODE-ne0){
        $c2Message=(($c2Output|Out-String).Trim()-split"`n"|Select-Object -Last 1).Trim()
        Fail-Incompatible ("W4-C2 invocation ledger prerequisiteが失敗しました: {0}" -f $c2Message)
    }

    # ---- 8. flag / publication serialはdiagnostic表示のみ ----
    $diagnostic=[ordered]@{
        pre_explicit_stop_requested=[bool]$pre.explicit_stop_requested
        pre_fatal_latched=[bool]$pre.fatal_latched
        pre_explicit_stop_publish_serial=[int64]$pre.explicit_stop_publish_serial
        at_gate_close_explicit_stop_publish_serial=[int64]$gateClose.explicit_stop_publish_serial
        measurement_start_explicit_stop_publish_serial=
            [int64]$witness.measurement_start.explicit_stop_publish_serial
        losing_stop_claim_count=[int64]$witness.losing_stop_claim_count
        coalesced_stop_publication_count=[int64]$witness.coalesced_stop_publication_count
        used_as_authority=$false
    }
    $summary=[ordered]@{
        schema='mvm-p2-d5-2-w4-c3-causal-replay-check-1';stage='P2-D5-2-W4-C3'
        source_json=(Resolve-Path -LiteralPath $Json).Path
        source_json_sha256=(Get-FileHash -LiteralPath $Json -Algorithm SHA256).Hash.ToLowerInvariant()
        diagnostic_root_cause_capture=$true
        canonical_performance_authority=$false
        stop_cause='DOMAIN_TERMINAL'
        arbitration_previous='NONE'
        arbitration_claim_source='THIS_CALL_SITE'
        terminal_invocation_serial=$witnessSerial
        terminal_intent_ordinal=[int64]$terminal.intent_ordinal
        terminal_target_frame=[int64]$terminal.target_frame
        terminal_required_intent_membership=[bool]$terminal.required_intent_membership
        replayed_decision_count=$replayedCount
        invalid_fatal_count=$fatalCount
        post_terminal_invocation_count=0
        join_method='scheduler_invocation_serial'
        inter_invocation_state='INVARIANT_COMPATIBLE_NOT_EXACTLY_RECONSTRUCTED'
        qpc_used_for_join=$false
        alternative_stop_fields=$diagnostic
        root_cause_determined=$true
        verdict='W4_C3_CAUSAL_REPLAY_EXACT'
    }
}catch{
    $kind=if($_.Exception-is[W4C3Failure]){$_.Exception.Kind}else{'W4_C3_INCOMPATIBLE'}
    $script:Verdict=$kind
    $summary=[ordered]@{
        schema='mvm-p2-d5-2-w4-c3-causal-replay-check-1';stage='P2-D5-2-W4-C3'
        source_json=$Json
        diagnostic_root_cause_capture=$true
        canonical_performance_authority=$false
        root_cause_determined=$false
        failure=$_.Exception.Message
        verdict=$kind
    }
    if(-not[string]::IsNullOrWhiteSpace($Output)){
        if(Test-Path -LiteralPath $Output){throw "既存W4-C3 checkを上書きしません: $Output"}
        $summary|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
    }
    Write-Host ("P2-D5-2 W4-C3 causal replay: {0} {1}" -f $kind,$_.Exception.Message)
    exit $(if($kind-eq'W4_C3_PARTIAL'){2}else{3})
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    if(Test-Path -LiteralPath $Output){throw "既存W4-C3 checkを上書きしません: $Output"}
    $summary|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
}
Write-Host ("P2-D5-2 W4-C3 causal replay: {0} terminal_serial={1} replayed={2}" -f `
    $summary.verdict,$summary.terminal_invocation_serial,$summary.replayed_decision_count)
return $summary
