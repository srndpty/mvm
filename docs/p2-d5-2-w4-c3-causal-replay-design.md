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
amend 4: single atomic stop-cause arbitration（alternative exclusion authority）
amend 5: flag/publication serialをclosure authorityから降格
```

amend 1は`pre.explicit_stop_requested=false`だけでは別threadからのpublicationとのinterleavingを
排除できない欠陥を閉じる。amend 2はLink A→Bのordinalとterminal branchの間に残っていたedgeを
exact replayで埋める。amend 3はamend 1のserialをC++ memory modelとpublication site先頭という
実装契約まで下ろし、amend 2のreplay範囲とoverflow semanticsの曖昧さを消す。amend 4は
alternative stop exclusionのauthorityをserial観測から単一atomicのCAS ownershipへ移す。amend 5は
その帰結として、flagとpublication serialをclosureの必須条件から外す。いずれかを満たさないcaptureは
`W4_C3_PARTIAL`とする。

## 追加する唯一の stop witness

stop witnessのwriterはrender threadの`finishMeasurement()`だけとする。controllerやcheckerが
QPCからcauseを再構築してはならない。

```text
schema = mvm-p2-d5-2-w4-c3-stop-witness-3

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

stop_arbitration:
  previous                        # DOMAIN_TERMINAL成立時はNONE
  claimed                         # DOMAIN_TERMINAL
  claim_succeeded
  reset_count_during_measurement  # 0でなければ無効
  measurement_start_state         # NONE

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
serial incrementはarbitration claim直後、flag storeとside effectより前に置く
serialはmeasurement期間中resetしない
reader（stop witness）はserialを読むだけで、publicationを再構築しない
```

serialの主張範囲（amend 4で確定）:

```text
serial equalityは ordering / inventory diagnostic であり、
alternative-stop exclusion authorityではない
```

`seq_cst`は全`seq_cst` operationへ単一のtotal orderを与えるが、そのSC orderは異なるthreadの
wall-clock上の「開始した/していない」ではない。controller threadの`fetch_add`とrender threadの
`load`の間にsynchronizationがない以上、serial equalityだけから「その区間にalternative publicationが
物理的に開始していない」とは主張しない。exclusion authorityはamend 4のCAS arbitrationが持ち、
serial equalityはその補強証拠として扱う。

### memory ordering（instrumentation前にfreeze）

serialは補強diagnosticだが、観測値の比較を意味あるものにするためC++ memory modelまで固定する。

```text
writer:
  publish_serial.fetch_add(1, std::memory_order_seq_cst)
  その後にflag publication（既存のrelease store等）

reader（stop witness）:
  publish_serial.load(std::memory_order_seq_cst)
```

`relaxed`および`acquire/release`だけのserial ordering は W4-C3 では採らない。publication siteは
低頻度なので、`seq_cst`で

```text
measurement_start serial
pre serial
at_gate_close serial
```

を単一のSC total order上に並べる方が単純である。ただしこれはserialの読み値を整合させるための
規約であって、これ自体がalternative publicationの先行排除を与えるわけではない。

「publication開始」の定義はamend 4のarbitration claimとし、serial incrementはその直後に置く。
したがって次を禁止する。

```cpp
// NG: arbitration claim / serial incrementより前に外部可視なstop side effectがある
someStopSideEffect();
stopCause.compare_exchange_strong(expected, EXPLICIT_STOP, std::memory_order_seq_cst);
serial.fetch_add(1, std::memory_order_seq_cst);
flag.store(true, std::memory_order_release);
```

architecture testで、classified publication siteが`memory_order_seq_cst`のarbitration claimで
始まり、serial incrementがそれに続き、いずれより前に外部可視なstop side effectを持たないことを
検査する。

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

snapshot点を固定するのは、比較対象の観測点を曖昧にしないためである。

DOMAIN_TERMINAL closure条件へ次のserial比較を補強diagnosticとして追加する。

```text
pre.explicit_stop_publish_serial == at_gate_close.explicit_stop_publish_serial
pre.fatal_publish_serial         == at_gate_close.fatal_publish_serial

at_gate_close.explicit_stop_publish_serial == measurement_start.explicit_stop_publish_serial
at_gate_close.fatal_publish_serial         == measurement_start.fatal_publish_serial
```

serial equalityが意味するのは次だけである。

```text
measurement_start / pre / at_gate_close の観測値に
publication-countの変化が見えなかった
```

alternative publicationの先行排除はserial equalityからは主張しない。そのauthorityは
amend 4のStopArbitration CAS ownershipのみが持つ。serial不一致はcaptureをPARTIAL/INCOMPATIBLE
側へ落とす材料としてだけ使う。

`planned_window_end`は`callbackBegin >= measurementEndQpc`という純粋なexact predicateなので
serialを持たない。

## amend 4: single atomic stop-cause arbitration

alternative stop exclusionのauthorityはserial観測ではなく、単一atomicのownership claimとする。

```text
StopArbitration =
    NONE
    DOMAIN_TERMINAL
    PLANNED_WINDOW_END
    EXPLICIT_STOP
    FATAL
```

classified publication siteは、外部可視なstop side effectより前にこのatomicへclaimする。

```cpp
// explicit stop publication site
auto expected = StopArbitration::None;
const bool claimed = stopCause_.compare_exchange_strong(
    expected, StopArbitration::ExplicitStop, std::memory_order_seq_cst);

// DOMAIN_TERMINAL（render thread, terminal branch）
auto expected = StopArbitration::None;
const bool claimed = stopCause_.compare_exchange_strong(
    expected, StopArbitration::DomainTerminal, std::memory_order_seq_cst);
```

publication siteの順序は次に固定する。

```text
arbitration claim (CAS, seq_cst)
        v
publication serial fetch_add
        v
flag store / その他のstop side effect
```

terminal callbackでのclaim成功そのものが、この単一atomicのmodification order上で

```text
それ以前にEXPLICIT_STOP / FATAL / PLANNED_WINDOW_ENDがstop ownershipを獲得していない
```

というbranch-exact arbitration witnessになる。timing/visibilityの議論を経由しない。

claim結果はpublication siteからconsumerへexactに運ぶ。EXPLICIT_STOP / FATALのようにpublication
siteとfinishMeasurement call siteが別threadの場合、publication siteは

```text
stop_publication_record:
  valid / claimed / previous / succeeded / publish_serial
```

をstop request flagと同じlock下で保存し、consumerもflagとrecordを同じlock下で受け取る。pending
stop requestが既にある場合、後着publicationはrecordを上書きせず`coalesced_stop_publication_count`
へ落とす。これによりconsumeしたrecordは、そのflagをpublishしたsiteのものであることが保証される。

consumerが`stopArbitration.load()`からclaim結果を逆算してはならない。recordが無い場合は
`claim_recorded=false`を記録し、defaultのclaim結果を観測値のように出さない。witnessには
`claim_source`（`THIS_CALL_SITE` / `PUBLICATION_RECORD` / `NONE`）を残す。

claimに失敗したsiteは推測せず、結果をそのまま記録する。

```text
claim_succeeded = false
previous        = 実際に勝っていたcause（例: DOMAIN_TERMINAL）
```

これによりexplicit stopが「先行していた」のか「後から来て負けた」のかが識別される。後着の
EXPLICIT_STOPは`LOST_TO_DOMAIN_TERMINAL`として扱い、root cause判定を汚さない。

### arbitration epoch / reset lifetime

CAS winnerをcausal authorityにした以上、そのatomicのepochもauthority contractの一部である。
`previous = NONE`は「measurement epoch中に一度もNONEへ戻されていない」ことを前提にしてのみ
exclusionを意味する。次のlifecycleをfreezeする。

```text
StopArbitration lifecycle:

pre-measurement reset:
  exactly once -> NONE
  measurement start publicationより前（同一thread上でsequenced-before）

measurement-start authority established:
  arbitration == NONE を exact に確認

measurement active:
  reset-to-NONE 禁止

terminal / stop winner established:
  winnerをmeasurement終了まで保持

next measurement:
  lifecycle reset siteは1箇所だけで NONE へ戻す
```

したがって次のsequenceはcontract違反であり、`previous=NONE`かつ`claim_succeeded=true`の
witnessが出ても無効とする。

```text
NONE -> EXPLICIT_STOP    // 先行claim
EXPLICIT_STOP -> NONE    // reset（禁止）
NONE -> DOMAIN_TERMINAL  // 見かけ上のterminal claim成功
```

architecture testで、reset siteが1箇所であること、そのsiteがmeasurement activeな区間から
到達しないことを検査する。

```text
NegativeArbitrationResetDuringMeasurement
NegativeSecondArbitrationResetSite
```

### losing claimのside effect semantics

CASに失敗したsiteはstop ownershipを変えない。

```text
claim failure does not alter stop ownership
```

loserが既存flag（`measurementStopRequested`等）をpublishする実装を残す場合も、artifact上の
flagとownershipを混同しない。

```text
flag = true は causal ownership を意味しない
causal ownership = arbitration winner のみ
```

stop witnessはloserのclaim結果（`claim_succeeded=false` / `previous=勝者`）をそのまま記録し、
checkerはflag値からcauseを推定しない。

役割分担:

```text
stop arbitration atomic  = causal authority（alternative exclusion）
publication serials      = independent ordering / inventory diagnostic
```

DOMAIN_TERMINAL closure条件へ次を追加する。

```text
stop_arbitration.previous = NONE
stop_arbitration.claimed = DOMAIN_TERMINAL
stop_arbitration.claim_succeeded = true
stop_arbitration.reset_count_during_measurement = 0
```

`NegativeExplicitStopPublishedBetweenPreAndGateClose`のfixtureは、先に
`NONE -> EXPLICIT_STOP`のclaimを成功させてからDOMAIN_TERMINAL claimを試し、後者が失敗して
root cause PASSにならないことをcheckerへ要求する。

## amend 5: flag / publication serialのclosure降格

amend 4を実装すると、正しいDOMAIN_TERMINAL runでも次が起こる。

```text
render:     claim DOMAIN_TERMINAL 成功（previous=NONE）
render:     formalOpportunityDomainReached.store(true)
controller: domainReachedを観測 -> claim EXPLICIT_STOP 失敗（previous=DOMAIN_TERMINAL）
            -> explicit_stop_publish_serial++ / measurementStopRequested=true
render:     finishMeasurementのpre snapshot
```

このとき`pre.explicit_stop_requested=true`、`pre serial != measurement_start serial`となるが、
causal ownershipはDOMAIN_TERMINALのままである。reset-to-NONEはmeasurement中禁止なので、
claim成功後に来るEXPLICIT_STOP/FATALはownershipを奪えない。

したがって次をCause B / Link A→Bの必須条件から外す。

```text
pre.explicit_stop_requested = false
pre.fatal_latched = false
pre.explicit_stop_publish_serial == at_gate_close.explicit_stop_publish_serial
pre.fatal_publish_serial         == at_gate_close.fatal_publish_serial
at_gate_close.* == measurement_start.*
```

これらはartifactに残すが位置づけは次に固定する。

```text
diagnostic only
do not infer causal ownership from these fields
```

「loser claimへ帰属できた場合だけ許容する」という例外ロジックは採らない。`losing_stop_claim_count`は
aggregate counterであり、個々のflag変化を特定のloser claimへexact joinするには弱い。ownershipは
CAS winnerだけで決まるので、その帰属はroot cause判定に不要である。

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
duplicate witness count = 0
cause = DOMAIN_TERMINAL
scheduler invocation join count = 1
stop_arbitration.measurement_start_state = NONE
stop_arbitration.reset_count_during_measurement = 0
stop_arbitration.previous = NONE
stop_arbitration.claimed = DOMAIN_TERMINAL
stop_arbitration.claim_succeeded = true
pre.capture_gate_open = true
pre.planned_window_end_reached = false
action.formal_opportunity_domain_reached_published = true
action.finish_measurement_entered = true
action.capture_gate_exchange_closed = true      # capture gate exchangeの実return
post.capture_gate_open = false
action.measurement_stop_published = true
post-terminal scheduler invocation count = 0
INVALID_FATAL count = 0
replayed_target_frame == recorded target_frame（全valid decision invocation）
replayed_past_source_domain == recorded past_source_domain（全valid decision invocation）
terminal直前decisionのpast_source_domain = false
```

`pre.capture_gate_open` / `action.capture_gate_exchange_closed` / `post.capture_gate_open`は同一
gate（`formalOpportunityCaptureActive`）のbefore / exchange実return / afterでなければならない。
`explicit_stop_requested`、`fatal_latched`、publication serialはartifactに残るがclosure条件では
ない（amend 5）。

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
NegativeAlternativeStopFieldUsedAsAuthority
NegativeStopPublishSerialRelaxedOrdering
NegativeWitnessCapturedBeforePayloadPublish
NegativeCaptureGateExchangeReturnIgnored
NegativeExplicitStopClaimDefaulted
NegativeExplicitStopClaimReconstructedFromWinner
NegativeAlternativeStopWinsArbitration
NegativeDomainTerminalClaimWithoutNonePrestate
NegativeArbitrationResetDuringMeasurement
NegativeSecondArbitrationResetSite
NegativeStopSideEffectBeforeArbitrationClaim
NegativePlannedWindowEndPreexisting
NegativeFatalPreexisting
NegativePostTerminalInvocation
NegativeTerminalTargetPredicateMutation
NegativeRequiredSourceDomainConflation
NegativeNearestQpcJoin
NegativeRootCauseDeclaredWithAlternativeStop
NegativePerformanceAuthorityPromotion
```

`NegativeAlternativeStopFieldUsedAsAuthority`は、checkerが`explicit_stop_requested`や
publication serialからcausal ownershipを導く実装を拒否する（amend 5）。alternative stopの排除は
arbitration winnerだけが与える。
`NegativeWitnessCapturedBeforePayloadPublish`は`captured=true`がpayload保存より先に見える実装を、
`NegativeCaptureGateExchangeReturnIgnored`は`capture_gate_exchange_closed`をgate exchangeの実
return以外（interval activeなど別条件）から立てる実装を拒否する。
`NegativeTerminalTargetPredicateMutation`はterminalの`target_frame`または
`past_source_domain`を改変したcaptureをreplay不一致として拒否する。
`NegativeStopPublishSerialRelaxedOrdering`と`NegativeStopSideEffectBeforeArbitrationClaim`は
architecture testとして、publication siteのserial/claimが`seq_cst`でないもの、およびarbitration
claimより前に外部可視なstop side effectを置いたものを拒否する。
`NegativeAlternativeStopWinsArbitration`はEXPLICIT_STOP/FATALがarbitrationに勝ったcaptureで
root cause PASSを出させず、`NegativeDomainTerminalClaimWithoutNonePrestate`は
`previous != NONE`のまま DOMAIN_TERMINAL をclaimedとして記録したcaptureを拒否する。
`NegativeArbitrationResetDuringMeasurement`と`NegativeSecondArbitrationResetSite`は
measurement中のreset-to-NONE、およびlifecycle reset siteの複数化を拒否する。

## publication site inventory（step 2で列挙、architecture testで固定）

source上のwriterを完全列挙し、全siteを`claimStopCause()`単一helperへ結んだ。未分類writerは無い。

```text
EXPLICIT_STOP
  compositor_spike_controller.cpp  measurement stop request（domain reached / 経過時間）

FATAL
  compositor_spike_controller.cpp  beginShutdown由来のmeasurement stop request
  compositor_rhi_item.cpp          fail()のfatal latch
  compositor_rhi_item.cpp          commitSwap失敗のfatal latch（fail()を経由しない）

PLANNED_WINDOW_END
  compositor_rhi_item.cpp          captureMeasurementBoundary()のintervalEnded分岐

DOMAIN_TERMINAL
  compositor_rhi_item.cpp          SuppressOutsideRequiredSet terminal branch
  compositor_rhi_item.cpp          transport terminal branch

EXPLICIT_STOP consumption（publication siteではない）
  compositor_rhi_item.cpp          measurementStopRequested.exchange(false)

lifecycle reset（1箇所のみ）
  compositor_spike_controller.cpp  armMeasurementAfterCaptureEnvelopeOpen()
```

architecture testは、reset site数=1、helper外のinline CAS/store無し、
`formalOpportunityDomainReached`storeの全件がclaim直後であること、fatal latch数<=claim数、
`scheduler_config`がinstance config由来であることを検査する。

## 実装順序

```text
W4-C3 before implementation
  amend 1: alternative stop publication ordering serial   [固定済み]
  amend 2: ordinal -> target -> past_source_domain replay [固定済み]
  amend 3: publication serial memory ordering            [固定済み]
  amend 3: replay対象 = valid decision invocation         [固定済み]
  amend 3: producerと同じoverflow semantics               [固定済み]
  amend 4: single atomic stop-cause arbitration           [固定済み]

then
  1. scheduler_config emit
  2. stop arbitration atomic + publication serial instrumentation
     （claim seq_cst / site先頭、serialはclaim直後）
  3. stop witness v3
  4. checker / replay
  5. negatives（architecture testを含む）
  6. diagnostic-only fresh capture
  7. exact causal replay
```
