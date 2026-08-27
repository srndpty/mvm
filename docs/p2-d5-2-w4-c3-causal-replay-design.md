# P2-D5-2 W4-C3 exact causal replay — Design (FROZEN v2, amended)

## 目的

W4-C2 formal captureは全3 runでsource-domain terminal branchの実行と、その後のscheduler
invocation 0件をexactに観測した。ただし、同一QPCやsource上の直接呼出しだけでは、実行時に
terminal decisionがcapture gate closeを起こしたこと、および別のexplicit stopが先行していないことを
証明しない。W4-C3はこの二つを単一render callback内のbranch-exact witnessで閉じる。

W4-C3 instrumentationもdiagnostic-onlyであり、canonical performance authorityへ昇格しない。

## v2 amendment（instrumentation着手前に固定）

```text
amend 1: alternative stop publication ordering serial
amend 2: ordinal -> target -> past_source_domain predicate replay
amend 3: publication serial memory ordering / replay scope / overflow semantics
```

amend 1は`pre.explicit_stop_requested=false`だけでは別threadからのpublicationとのinterleavingを
排除できない欠陥を閉じる。amend 2はLink A→Bのordinalとterminal branchの間に残っていたedgeを
exact replayで埋める。amend 3はamend 1のserialをC++ memory modelとpublication site先頭という
実装契約まで下ろし、amend 2のreplay範囲とoverflow semanticsの曖昧さを消す。いずれかを満たさない
captureは`W4_C3_PARTIAL`とする。

## 追加する唯一の stop witness

stop witnessのwriterはrender threadの`finishMeasurement()`だけとする。controllerやcheckerが
QPCからcauseを再構築してはならない。

```text
schema = mvm-p2-d5-2-w4-c3-stop-witness-2

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

measurement_start:
  explicit_stop_publish_serial
  fatal_publish_serial

pre:
  capture_gate_open
  explicit_stop_requested
  planned_window_end_reached
  fatal_latched
  explicit_stop_publish_serial
  fatal_publish_serial

action:
  formal_opportunity_domain_reached_published
  finish_measurement_entered
  capture_gate_exchange_closed
  measurement_stop_published

at_gate_close:
  explicit_stop_publish_serial
  fatal_publish_serial

post:
  capture_gate_open
  measurement_stop_qpc
```

`cause`は呼出側が明示して`finishMeasurement()`へ渡す。現在の二つのterminal branchは
`DOMAIN_TERMINAL`、`callbackBegin >= measurementEndQpc`は`PLANNED_WINDOW_END`、
`measurementStopRequested.exchange(false)`は`EXPLICIT_STOP`とする。複数条件が同じcallbackで
成立した場合もpre fieldをすべて保存し、cause enumだけでalternativeを隠さない。

## amend 1: alternative stop publication ordering serial

`measurementStopRequested`とfatal latchはrender threadの外（controller thread、shutdown path）
からもpublishされ得る。したがって`pre.explicit_stop_requested=false`は「pre snapshot時点で未成立」
しか意味せず、

```text
render thread                 controller thread

pre snapshot: false
                              publish explicit stop
capture gate exchange
```

というinterleavingを排除しない。ここでQPC近傍やthread時刻の比較へ逃げてはならない。

writer-side monotonic publication serialを追加する。

```text
explicit_stop_publish_serial   monotonic, writer-side
fatal_publish_serial           monotonic, writer-side
```

規約:

```text
publish siteはflag storeより前にserialをfetch_add(1)する
serial incrementはclassified publication siteのfirst externally visible operationである
serialはmeasurement期間中resetしない
reader（stop witness）はserialを読むだけで、publicationを再構築しない
```

flagより先にserialを進めるので、readerがserialの不変を観測したwindowでは、そのwindow内で
publicationが開始してもいない。

### memory ordering（instrumentation前にfreeze）

serialはdiagnostic counterではなくnegative causal witnessなので、C++ memory modelまで固定する。

```text
writer:
  publish_serial.fetch_add(1, std::memory_order_seq_cst)
  その後にflag publication（既存のrelease store等）

reader（stop witness）:
  publish_serial.load(std::memory_order_seq_cst)
```

`relaxed`および`acquire/release`だけのserial ordering は W4-C3 では採らない。W4-C3は診断専用で
publication siteは低頻度なので、未観測incrementを排除するhappens-before/coherence argumentを
書き下すより、`seq_cst`で単一のSC total order上に

```text
measurement_start serial
pre serial
at_gate_close serial
```

を並べる方が証拠として単純である。release/acquireへ緩めるなら、その argument自体をこの契約へ
追記してからでなければならない。

「publication開始」の定義はserial incrementそのものとする。したがって次を禁止する。

```cpp
// NG: serial incrementより前に外部可視なstop side effectがある
someStopSideEffect();
serial.fetch_add(1, std::memory_order_seq_cst);
flag.store(true, std::memory_order_release);
```

architecture testで、classified publication siteが`memory_order_seq_cst`のfetch_addで始まり、
その前に外部可視なstop side effectを持たないことを検査する。

### measurement_start snapshotの位置

`measurement_start`のserialは「measurement中の適当な時点」ではなく、次の一点へbranch-exactに
固定する。

```text
measurement-start authority established
        v
snapshot alternative-publication serials   <- measurement_start
        v
formal measurement capture open / activate
```

この順序でsnapshotするので、`at_gate_close serial == measurement_start serial`が
measurement interval全体でalternative publicationなしを意味する。

DOMAIN_TERMINAL closure条件へ次を追加する。

```text
pre.explicit_stop_publish_serial == at_gate_close.explicit_stop_publish_serial
pre.fatal_publish_serial         == at_gate_close.fatal_publish_serial
```

これでterminal cause選択からcapture gate closeまでにalternative publicationが割り込んでいない
ことをexactに言える。さらにmeasurement開始時のserialとも比較する。

```text
at_gate_close.explicit_stop_publish_serial == measurement_start.explicit_stop_publish_serial
at_gate_close.fatal_publish_serial         == measurement_start.fatal_publish_serial
```

これが成立すればmeasurement中に先行explicit stop publicationもfatal publicationも無い。
`planned_window_end`は`callbackBegin >= measurementEndQpc`という純粋なexact predicateなので
serialを持たない。

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

## amend 2: ordinal -> target -> past_source_domain predicate replay

Cause A（ordinal advancement）とCause B（source-domain terminal）の間のedgeを、C0で静的に確定した
述語のexact replayで埋める。captureはreplay入力となるscheduler configをartifactへ出力する。

```text
scheduler_config:
  source_frame_offset
  source_fps_numerator
  source_fps_denominator
  refresh_numerator
  refresh_denominator
  required_frame_count
```

configはschedulerが実際に使用したinstance configを出力する。checkerが別fieldから再構成したり
既定値で補完したりしてはならない。

replay式:

```text
target =
  source_frame_offset
  + floor(
      ordinal
      * source_fps_numerator
      * refresh_denominator
      / (source_fps_denominator * refresh_numerator)
    )

past_source_domain = target >= required_frame_count
```

replay対象はvalid decision invocationに限る。

```text
PRIMARY_DECISION
DUPLICATE_DECISION
OUTSIDE_SOURCE_DOMAIN_DECISION

  replayed_target_frame        == recorded target_frame
  replayed_past_source_domain  == recorded past_source_domain

INVALID_FATAL

  target arithmetic成立前に終わり得るので、replay可能な入力までだけ検証する
  branch-exact fatal reasonとの一致を要求する
  target/past_source_domainを捏造・補完しない
```

今回のformal captureは`INVALID_FATAL=0`なので実測verdictは変わらないが、negative fixtureで
replay範囲が曖昧にならないようここで固定する。

### overflow semantics

checkerはproducerと同じ整数semanticsを再現する。

```text
producerと同じoperand domain（64-bit signed）
producerと同じchecked multiply / add precondition
producerと同じ切り捨て除算
```

`ordinal * source_fps_numerator * refresh_denominator`の中間積がproducer側でchecked
arithmeticである以上、checkerがPowerShell/.NETの多倍長やdoubleへ逃げて「計算できたからPASS」と
してはならない。producerがoverflowで`INVALID_FATAL`へ落ちる入力は、checkerでも同じ
overflow判定に落ちなければならない。

terminalでは加えて次を要求する。

```text
previous decision:
  past_source_domain == false

terminal decision:
  past_source_domain == true
  result             == OUTSIDE_SOURCE_DOMAIN_DECISION
  reason             == PAST_SOURCE_DOMAIN
```

replayは整数演算だけで行い、浮動小数、丸めの近似、tolerance比較を使わない。overflow検査に
失敗したinvocationは`INVALID_FATAL`として記録されている前提であり、replay側で補間しない。

これによりLink A→Bを次のchainとして再生する。

```text
completed-refresh authority sequence
        v exact
intent ordinal sequence
        v exact target formula
target / source-domain predicate
        v exact
OUTSIDE_SOURCE_DOMAIN_DECISION
        v exact serial join
DOMAIN_TERMINAL stop witness
        v
capture gate closed
        v
post-terminal scheduler invocation = 0
```

## root cause statementのscope

W4-C3が主張してよいのはactual causal chainだけである。

```text
observed completed-refresh advancementがordinalへそのまま伝播し、
そのordinalからexactに導出されたtargetがsource-domain terminal predicateを成立させ、
DOMAIN_TERMINALがcapture gateを閉じた
```

「+2/+3で進んだので+1の場合より早くterminalになった」というcounterfactualはこのchainに含めない。
主張するならcounterfactual +1 ordinal sequenceを別途定義しなければならず、W4-C3のscope外とする。

## Cause B / Link A→B の要求

Cause BとLink A→Bは各runで次をすべて要求する。

```text
terminal witness count = 1
cause = DOMAIN_TERMINAL
scheduler invocation join count = 1
pre.capture_gate_open = true
pre.explicit_stop_requested = false
pre.planned_window_end_reached = false
pre.fatal_latched = false
pre.explicit_stop_publish_serial == at_gate_close.explicit_stop_publish_serial
pre.fatal_publish_serial == at_gate_close.fatal_publish_serial
at_gate_close.explicit_stop_publish_serial == measurement_start.explicit_stop_publish_serial
at_gate_close.fatal_publish_serial == measurement_start.fatal_publish_serial
action.formal_opportunity_domain_reached_published = true
action.finish_measurement_entered = true
action.capture_gate_exchange_closed = true
action.measurement_stop_published = true
post.capture_gate_open = false
post-terminal scheduler invocation count = 0
INVALID_FATAL count = 0
replayed_target_frame == recorded target_frame（全valid decision invocation）
replayed_past_source_domain == recorded past_source_domain（全valid decision invocation）
terminal直前decisionのpast_source_domain = false
```

さらにterminal invocationのpre stateを、それ以前のC2 invocation replayの最終post stateとexactに
一致させる。required-intent domainとsource-frame domainは引き続き別fieldであり、terminal
`past_source_domain=true`からrequired-domain exhaustionを推論しない。

## verdict

```text
W4_C3_CAUSAL_REPLAY_EXACT
  Cause A、Cause B、Link A→Bの全条件が全runで成立

W4_C3_PARTIAL
  branch executionは観測したがstop witness、alternative exclusion、predicate replay入力、
  またはreplay入力が不足

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
NegativeExplicitStopPublishedBetweenPreAndGateClose
NegativeStopPublishSerialRelaxedOrdering
NegativeStopSideEffectBeforeSerialIncrement
NegativePlannedWindowEndPreexisting
NegativeFatalPreexisting
NegativePostTerminalInvocation
NegativeTerminalTargetPredicateMutation
NegativeRequiredSourceDomainConflation
NegativeNearestQpcJoin
NegativeRootCauseDeclaredWithAlternativeStop
NegativePerformanceAuthorityPromotion
```

`NegativeExplicitStopPublishedBetweenPreAndGateClose`はpre snapshot後・gate close前に
explicit stopがpublishされたcaptureを拒否し、raceによるfalse root-cause PASSを直接防ぐ。
`NegativeTerminalTargetPredicateMutation`はterminalの`target_frame`または
`past_source_domain`を改変したcaptureをreplay不一致として拒否する。
`NegativeStopPublishSerialRelaxedOrdering`と`NegativeStopSideEffectBeforeSerialIncrement`は
architecture testとして、publication siteのserialが`seq_cst`でないもの、およびserial increment
より前に外部可視なstop side effectを置いたものを拒否する。

## 実装順序

```text
W4-C3 before implementation
  amend 1: alternative stop publication ordering serial   [固定済み]
  amend 2: ordinal -> target -> past_source_domain replay [固定済み]
  amend 3: publication serial memory ordering            [固定済み]
  amend 3: replay対象 = valid decision invocation         [固定済み]
  amend 3: producerと同じoverflow semantics               [固定済み]

then
  1. scheduler_config emit
  2. publication serial instrumentation（seq_cst / site先頭）
  3. stop witness v2
  4. checker / replay
  5. negatives（architecture testを含む）
  6. diagnostic-only fresh capture
  7. exact causal replay
```
