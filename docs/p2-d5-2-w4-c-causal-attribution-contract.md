# P2-D5-2 W4-C Scheduler Causal Attribution — Contract (FROZEN)

W4-A は unsatisfied intent が primary scheduler decision の生成前に集中することを閉じ、
W4-B は primary decision の `intent_ordinal` が主として `+2`、時折 `+3` 進み、
decision stream が canonical 60 秒窓の約 29.06 秒で終了することを exact な相関として
確定した。W4-C は、この二つを scheduler の実際の state transition / control-flow へ
帰属する段である。

**本 contract は W4-C の instrumentation または causal replay 実装より先に freeze する。**

## 問いと段の分離

W4-C が答える問いは次の一つである。

> primary decision の `intent_ordinal` が主として `+2`（時折 `+3`）進む原因と、
> primary decision stream が約 29.06 秒で終了する原因を、scheduler の実際の
> state transition / control-flow に exact に帰属できるか。

ただし、次の三命題は別々に閉じる。

```text
Cause A: なぜ primary decision ordinal が +2 / +3 進むのか
Cause B: なぜ primary decision generation が 29.06s で止まるのか
Link A -> B: A が required domain exhaustion を早め、B を起こしたか
```

Cause A だけを根拠に Link A -> B を成立させてはならない。

## staged contract

```text
W4-C0  static producer control-flow attribution
W4-C1  existing sealed W3 cohort causal-compatibility replay
W4-C2  scheduler invocation ledger（C1で実行branchを識別できない場合だけ）
W4-C3  exact causal replay と root-cause verdict
```

### W4-C0

新規 capture と product 変更を行わず、source から次を完全列挙する。

```text
opportunity / intent ordinal の producer
last finalized opportunity ordinal の writer
ordinal advancement の式
primary / duplicate / outside-domain decision return
invalid decision return
required-domain end と measurement stop
target frame advancement との関係
```

C0 は code path の存在を示す段であり、W3 cohort でその branch が実行されたとは言わない。
未分類 writer、return site、second producer、間接的な ordinal 再構築が一つでもあれば
`W4_C0_BLOCKED` とする。

### W4-C1

W4-B が bind した sealed producer records だけを使い、observed transition と C0 path の
整合可能性を調べる。branch discriminator が既存 ledger に無い場合の上限は
`EXACT_CAUSAL_COMPATIBILITY` であり、`BRANCH_EXECUTION_EXACT` へ昇格しない。

### W4-C2

C1 で execution を識別できない場合だけ producer instrumentation を追加する。
decision record だけでなく、**全 `selectForRender` invocation** を記録する。

```text
scheduler_invocation_serial
invocation_qpc
pre: anchored / origin_refresh_count / completed_refresh_ordinal /
     last_finalized_opportunity_ordinal / required-domain state
result: PRIMARY_DECISION / DUPLICATE_DECISION / OUTSIDE_SOURCE_DOMAIN_DECISION /
        INVALID_FATAL
reason: source branch と 1:1 の enum
decision: opportunity_ordinal / target_frame / repeat / past_source_domain /
          required_intent_membership / formal transport disposition
post: scheduler state
state_transition_exact = true
```

上記 result の domain は scheduler が判定する source-frame domain である。required-intent
domain は transport 層の別 authority なので、result と混ぜず次の field に分離する。

```text
result: PRIMARY_DECISION / DUPLICATE_DECISION / OUTSIDE_SOURCE_DOMAIN_DECISION /
        INVALID_FATAL
required_intent_membership: true / false
formal_transport_disposition:
  TRANSPORT / SUPPRESSED_OUTSIDE_REQUIRED_INTENT_DOMAIN / SUPPRESSED_DUPLICATE / INVALID
```

source-domain resultへrequired-domainを意味するenum名を付けることを禁止する。source-domain
terminalの観測だけからrequired ordinal domain exhaustionを結論してはならない。

現実装の `selectForRender()` は正常な `NO_DECISION` を返さない。`return {}` の全経路は
error を latch する fatal path である。このため、C2 で一般的な `NO_DECISION` enum を
発明せず、現存 branch を上記 result/reason へ 1:1 で割り当てる。将来正常な no-decision
branch を追加した場合は、contract と enum を先に更新する。

instrumented capture は root-cause 診断専用であり、次を固定する。

```text
diagnostic_root_cause_capture = true
canonical_performance_authority = false
historical_w3_verdict_rewritten = false
historical_w4a_rewritten = false
historical_w4b_rewritten = false
```

fresh checkpoint、clean worktree、source SHA、binary hash、instrumentation schema version を
capture に bind する。

### W4-C3 closure

Cause A の PASS は、全 observed `+2` / `+3` transition を exact invocation sequence、
pre/post state、executed branch/reason から再生し、その結果が primary decision ordinal
sequence と完全一致することを要求する。

Cause B の PASS は、last primary decision の後を terminal witness まで追い、次を区別する。

```text
scheduler が呼ばれ続け、分類済み result を返した
capture gate が閉じ、scheduler invocation 自体が止まった
別の measurement stop / fatal path が先に成立した
```

Link A -> B の PASS は次の全条件を要求する。

```text
1. ordinal advancement rule が exact
2. required-domain termination condition が exact
3. termination branch の実行が exact
4. termination input state を先行 advancement sequence から再構築可能
5. alternative stop reason = 0
```

一つでも欠ければ次のままとする。

```text
root_cause_determined = false
attribution = PARTIAL
```

## W4-C0 static inventory（source SHAへbindする対象）

### ordinal producer と advancement

`PresentationOpportunityScheduler::selectForRender()` が formal decision ordinal の唯一の
producer である。

```text
unanchored: ordinal = 0
anchored:
  completed = presentationOpportunityOrdinal(originRefreshCount, preRenderAuthority)
  ordinal   = completed + 1
decision.opportunityOrdinal = ordinal
```

`presentationOpportunityOrdinal()` は refresh count authority から `completed` を作る。
QPC、source frame、`lastFinalizedOrdinal_` は ordinal 式へ入力されない。呼出側での唯一の
intent transport producer は
`nativePresentToken.setFormalIntentOrdinal(formalDecision.opportunityOrdinal)` であり、
`targetFrame` 等から intent identity を再構築する経路は無い。

### target frame

```text
target = sourceFrameOffset
       + floor(ordinal * sourceFpsNumerator * refreshDenominator
               / (sourceFpsDenominator * refreshNumerator))
```

`targetFrame` は ordinal から派生する。逆向きに target/source frame から ordinal を作らない。
したがって source frame delta は Cause A の authority ではない。

### last finalized writer

初期値は `-1`。実行時の唯一の mutation site は `finalizePendingOpportunity()` の
`lastFinalizedOrdinal_ = ordinal` であり、ここでの `ordinal` は post-swap refresh authority
から得た `pendingOpportunityOrdinal_` である。`selectForRender()` は値を
`decision.lastFinalizedOpportunityOrdinal` へ snapshot するだけで、更新しない。

### `selectForRender()` return inventory

```text
INVALID_FATAL x 6
  INVALID_CONFIGURATION x 1
  AUTHORITY_DISCONTINUITY x 2
  OPPORTUNITY_REGRESSION x 1
  ARITHMETIC_OVERFLOW x 2

DUPLICATE_DECISION x 1
  pendingRender_ == true
  pending decision と同じ ordinal、duplicateCallback=true

OUTSIDE_SOURCE_DOMAIN_DECISION x 1
  target >= requiredFrameCount
  valid=true、pastSourceDomain=true

PRIMARY_DECISION x 1
  valid=true、pendingDecision_ と pendingRender_ を設定
```

`requiredIntentMembership` は `0 <= ordinal < requiredFrameCount` で決まる。
`pastSourceDomain` は `target >= requiredFrameCount` で決まるため、一般の fps ratio では
両条件を同一視しない。

### decision stream を止められる path

formal scheduler invocation の直接 gate は renderer の
`output < 0 && formalOpportunityCaptureActive` である。measurement 中の gate close authority は
`finishMeasurement()` が行う `formalOpportunityCaptureActive.exchange(false)` である。

`finishMeasurement()` へ到達する分類済み path:

```text
DOMAIN_TERMINAL
  outside-required transport branch または past-source-domain branch
  -> formalOpportunityDomainReached=true
  -> native capture envelope有効時に finishMeasurement(callbackBegin)

PLANNED_WINDOW_END
  callbackBegin >= measurementEndQpc

EXPLICIT_STOP
  measurementStopRequested（controller timeout / domain reached / fatal shutdownを含む）
```

さらに measurement lifecycle の preroll/main 切替は一度 capture gate を false にして scheduler を
再初期化した後 true に戻す。これは main decision stream termination と混同しない。
`measurementResetRequested` と envelope stop は lifecycle control path だが、現実装ではそれ自体が
main scheduler の `finishMeasurement()` を呼ばない。fatal error は個々の callback を return させ、
controller の fatal shutdown が `EXPLICIT_STOP` を要求する。

以上は **可能な control-flow の列挙**である。W3 cohort で `DOMAIN_TERMINAL` が実行されたこと、
または ordinal advancement がその入力を作ったことは C1/C2/C3 の証拠なしに主張しない。

## 明示的禁止事項

```text
source frame == intent identity とみなす
nearest QPC で invocation と decision を結ぶ
timing tolerance で branch を推定する
missing ordinal へ state を補間する
29.06s ~= 60s/2 を原因とみなす
30fps source を +2 の原因とみなす
legacy measurement_elapsed を authority へ戻す
instrumented capture を W3 canonical performance authority に使う
正常な NO_DECISION branch/reason を source に無いまま発明する
```

## negative contract

```text
NegativeUnclassifiedOrdinalWriter
NegativeUnclassifiedLastFinalizedWriter
NegativeUnclassifiedNoDecisionReturn
NegativeSecondIntentProducer
NegativeIndirectOrdinalReconstruction
NegativeDecisionReasonMutation
NegativeInvocationSequenceGap
NegativePrePostStateMutation
NegativeDecisionWithoutInvocation
NegativeNoDecisionWithoutExactReason
NegativeNearestQpcInvocationBinding
NegativeMissingStateInterpolation
NegativeAlternativeStopReasonIgnored
NegativeInstrumentedCapturePromotedToPerformanceAuthority
NegativeRootCauseDeclaredWithoutTerminalWitness
```

C0 では先頭 5 件を静的契約テストで固定する。残りは該当 stage の実装と同時に追加する。

## 現在の verdict

```text
w4_c_contract_frozen = true
w4_c0_static_inventory_complete = true
w4_c1_started = true
w4_c1_complete = true
formal_c2_capture_performed = false
non_authoritative_runtime_smoke_performed = true
producer_instrumentation_changed = true
root_cause_determined = false
attribution = EXACT_CAUSAL_COMPATIBILITY_PARTIAL_COVERAGE
c2_instrumentation_required = true
c2_instrumentation_implemented = true
```
