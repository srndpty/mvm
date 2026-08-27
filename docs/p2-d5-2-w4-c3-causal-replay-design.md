# P2-D5-2 W4-C3 exact causal replay — Design (FROZEN)

## 目的

W4-C2 formal captureは全3 runでsource-domain terminal branchの実行と、その後のscheduler
invocation 0件をexactに観測した。ただし、同一QPCやsource上の直接呼出しだけでは、実行時に
terminal decisionがcapture gate closeを起こしたこと、および別のexplicit stopが先行していないことを
証明しない。W4-C3はこの二つを単一render callback内のbranch-exact witnessで閉じる。

W4-C3 instrumentationもdiagnostic-onlyであり、canonical performance authorityへ昇格しない。

## 追加する唯一の stop witness

stop witnessのwriterはrender threadの`finishMeasurement()`だけとする。controllerやcheckerが
QPCからcauseを再構築してはならない。

```text
schema = mvm-p2-d5-2-w4-c3-stop-witness-1

cause:
  DOMAIN_TERMINAL
  PLANNED_WINDOW_END
  EXPLICIT_STOP

render_callback_begin_qpc
scheduler_invocation_serial       # DOMAIN_TERMINALだけ必須、他causeはnull
scheduler_result                  # DOMAIN_TERMINALではOUTSIDE_SOURCE_DOMAIN_DECISION
scheduler_reason                  # DOMAIN_TERMINALではPAST_SOURCE_DOMAIN
terminal_past_source_domain
terminal_required_intent_membership

pre:
  capture_gate_open
  explicit_stop_requested
  planned_window_end_reached
  fatal_latched

action:
  formal_opportunity_domain_reached_published
  finish_measurement_entered
  capture_gate_exchange_closed
  measurement_stop_published

post:
  capture_gate_open
  measurement_stop_qpc
```

`cause`は呼出側が明示して`finishMeasurement()`へ渡す。現在の二つのterminal branchは
`DOMAIN_TERMINAL`、`callbackBegin >= measurementEndQpc`は`PLANNED_WINDOW_END`、
`measurementStopRequested.exchange(false)`は`EXPLICIT_STOP`とする。複数条件が同じcallbackで
成立した場合もpre fieldをすべて保存し、cause enumだけでalternativeを隠さない。

## exact join

DOMAIN_TERMINAL witnessは次をすべて同じscheduler invocation serialへ結ぶ。

```text
C2 invocation record
  -> result = OUTSIDE_SOURCE_DOMAIN_DECISION
  -> reason = PAST_SOURCE_DOMAIN
  -> scheduler_invocation_serial
  -> W4-C3 stop witness.scheduler_invocation_serial
  -> capture_gate_exchange_closed = true
  -> measurement_stop_published = true
```

nearest QPC、同一QPC、token serialの類似、terminal recordの配列末尾という位置関係をjoin keyに
使わない。QPCはjoin後の順序検査にだけ使う。

## causal replay

Cause AはC2 invocation全件について、input authorityからcompleted ordinalを計算し、
`ordinal = completed + 1`、pre/post state、result/reasonを再生する。全primary間で
`delta intent ordinal = delta completed refresh ordinal`を要求し、欠損やoverflowを補間しない。

Cause BとLink A→Bは各runで次をすべて要求する。

```text
terminal witness count = 1
cause = DOMAIN_TERMINAL
scheduler invocation join count = 1
pre.capture_gate_open = true
pre.explicit_stop_requested = false
pre.planned_window_end_reached = false
pre.fatal_latched = false
action.formal_opportunity_domain_reached_published = true
action.finish_measurement_entered = true
action.capture_gate_exchange_closed = true
action.measurement_stop_published = true
post.capture_gate_open = false
post-terminal scheduler invocation count = 0
INVALID_FATAL count = 0
```

さらにterminal invocationのpre stateを、それ以前のC2 invocation replayの最終post stateとexactに
一致させる。required-intent domainとsource-frame domainは引き続き別fieldであり、terminal
`past_source_domain=true`からrequired-domain exhaustionを推論しない。

## verdict

```text
W4_C3_CAUSAL_REPLAY_EXACT
  Cause A、Cause B、Link A→Bの全条件が全runで成立

W4_C3_PARTIAL
  branch executionは観測したがstop witness、alternative exclusion、またはreplay入力が不足

W4_C3_INCOMPATIBLE
  exact witnessまたはreplay結果が一件でも矛盾
```

`W4_C3_PARTIAL`では`root_cause_determined=false`を固定する。instrumented captureはhistorical W3、
W4-A、W4-Bのverdictを書き換えない。

## negative contract

```text
NegativeMissingStopWitness
NegativeStopCauseMutation
NegativeTerminalInvocationJoinMutation
NegativeCaptureGatePreMutation
NegativeCaptureGateExchangeMutation
NegativeExplicitStopPreexisting
NegativePlannedWindowEndPreexisting
NegativeFatalPreexisting
NegativePostTerminalInvocation
NegativeRequiredSourceDomainConflation
NegativeNearestQpcJoin
NegativeRootCauseDeclaredWithAlternativeStop
NegativePerformanceAuthorityPromotion
```
