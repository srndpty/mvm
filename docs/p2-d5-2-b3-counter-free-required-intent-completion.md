# P2-D5-2 B3 — Counter-free Required-intent Completion Correction

## 1. Scope / status

- 対象: P2 formal Playbackのrequired-intent issuance / completion control
- 前提: B2は`EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE`でCLOSED
- phase: **DESIGN CLOSED / IMPLEMENTATION I0 DONE** (実装状況は§10)
- production code: I0 (exact qualified commit join) のみ変更済み。queue semanticsとordinal issuanceは**UNCHANGED**
- test / capture: I0 targeted test済み。live captureは**NOT RUN**
- canonical W3 verdict: **UNCHANGED / FAIL**
- P5-E4: **BLOCKED**

本designはexternal refresh counterの代替を発明しない。callback index、QPC、NULL DWM counter、
physical observer、source frame、previous ordinal単独をintent identity authorityにせず、current schedulerに
既に存在するlocal transaction boundaryだけからrequired intentの完了を定義する。

## 2. Frozen input facts

### 2.1 W4-C3が証明済みのactual chain

W4-C3 formal 3/3 EXACTが証明したのは、current runtimeでNULL DWM `cRefresh`由来のcompleted ordinalが
invocation間で`+2/+3`前進し、その値から`completed + 1`で次intent ordinalを作り、source target、
`past_source_domain`、required membership、`DOMAIN_TERMINAL`へ到達したactual causal chainである。
W3の29.033 fps / drop 51.611% FAILと約29秒terminalは不変の観測事実である。

次は未証明であり、本designでも事実へ昇格しない。

- sequential ordinal `+1`なら必ずW3 thresholdを満たすこと
- callbackごとにrequired intentを1件発行すれば正しいこと
- local commit数とphysical refresh数が一致すること
- counter-free correction後に全required intentがplanned window内でcommitされること

### 2.2 B2からの入力

supportedでsame-causal-pointなtarget-output completed-refresh counterは存在しない。したがってB3は
`SyncRefreshCount`、scanline、VBlank observer wake、derived cadence、nearest-QPCを使用しない。

### 2.3 Product invariant

次を継続してfreezeする。

```text
past_source_domain && required_intent_membership
  != successful measurement completion
```

`DOMAIN_TERMINAL`はnormal completion authorityではない。代替挙動は本designで選ぶが、まだ実装しない。

## 3. Current source-level inventory

### 3.1 Measurement start / required set

`PresentationOpportunityScheduler::start()`はobjectをresetし、configを検証してコピーした後、
`requiredIntentOrdinals_`へ`[0, requiredFrameCount)`を列挙する
(`presentation_opportunity_scheduler.cpp:18-35`)。したがってrequired set自体は既にstart時点で具体化される。
一方、current runtimeはこのvectorをissuance queueとして使わず、snapshot authorityとしてだけ保持する。

### 3.2 `selectForRender`

exact control flowは次である (`presentation_opportunity_scheduler.cpp:39-134`)。

```text
invalid scheduler/callback
  -> INVALID_FATAL

pendingRender_ == true
  -> pendingDecision_を複製
  -> duplicateCallback = true
  -> DUPLICATE_DECISION
  -> 新しいpending stateを作らない

authority invalid/regressed
  -> INVALID_FATAL

first decision
  -> ordinal = 0

anchored decision
  -> completed = DWM refreshCount - originRefreshCount
  -> ordinal = completed + 1

target = targetFor(ordinal)
required_intent_membership = ordinal in [0,N)

target >= source domain end
  -> pastSourceDomain_ = true
  -> OUTSIDE_SOURCE_DOMAIN_DECISION
  -> pendingRender_を作らない

otherwise
  -> pendingDecision_ = decision
  -> pendingRender_ = true
  -> pendingRenderCompleted_ = false
  -> PRIMARY_DECISION
```

現在のduplicate pathは同一pending decisionを返すが、app側の
`formalIntentTransportDisposition()`が`duplicateCallback`を
`SUPPRESS_DUPLICATE_CALLBACK`へ分類し、composition tokenへのtransport前にreturnする
(`presentation_opportunity_scheduler.h:85-94`, `compositor_rhi_item.cpp:408-419`)。よってduplicate callbackは
current control flowでも新しいformal intentをtransportしない。

### 3.3 `markRenderComplete`

`markRenderComplete()`は次をすべて要求する
(`presentation_opportunity_scheduler.cpp:137-160`)。

- pending renderが1件ある
- まだrender completeされていない
- render end QPCがrender beginより前でない
- render ordinalがpending decisionと一致する
- rendered source frameがpending targetと一致する

成功時は`pendingRenderCompleted_`、end QPC、rendered source frameを記録するだけである。
`pendingRender_`をclearせず、required intent、opportunity、required setのいずれもconsumeしない。
appではrepeat/preroll/normal composeの各成功経路から呼ばれる
(`compositor_rhi_item.cpp:479`, `:688`, `:924`)。

### 3.4 `commitSwap`

`frameSwapped`のdirect connectionから`recordFrameSwapped()`が呼ばれ、formal capture中に
`commitSwap()`へ入る (`apps/compositor_spike/main.cpp:146-147`,
`compositor_rhi_item.cpp:1703-1750`)。current `commitSwap()`は次を要求する
(`presentation_opportunity_scheduler.cpp:163-266`)。

```text
pendingRender_ == true
pendingRenderCompleted_ == true
swap/render QPCとlocal render/swap ordinalのpairingがvalid
post-swap DWM authorityがusableかつmonotonic

then:
  DWM actual ordinalを計算
  pendingOpportunity candidateをinstall / supersede / finalize
  swappedCompositionCount_++
  last swap/render stateを更新
  pendingRender_とpending completion stateをclear
```

current実装ではDWM authority検査とactual-opportunity groupingが混在する。しかし、成功returnの末尾にある
「1件のreserved renderをcompleted renderとswapにexactly pairし、次のprimary decisionを許可する」という
local state transitionは、次の`selectForRender`より前に因果的に確定している既存境界である。

native Presentのformal identityはcomposition token serialを通じて既存ledgerへ運ばれる
(`compositor_rhi_item.cpp:448-471`)。実装時には単なる`frameSwapped`回数ではなく、reservation/token serial、
matching render completion、successful native Present record、matching swap commitのexact joinをqualified条件にする。
ETW `Presented` / `FinalState`をこのonline判断へ逆輸入しない。

このjoinに必要なsource schemaは既に存在する。`MvmNativePresentRecord`は`presentSerial`、actual swapchain
identity、HRESULT、Present enter/return QPC、token presence、intent ordinal、composition tokenを保持する
(`native_present_hook_abi.h:36-51`)。patched Qtはactual `IDXGISwapChain::Present()`の直前にrecordを開始し、
return直後にHRESULTを記録してから`recordCount`をpublishする
(`qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch:207-210`, `:100-113`)。
したがって`frameSwapped` callback時点で、直前の新規recordが同一token serialを持ち`SUCCEEDED(hresult)`であることを
exactに検査できるsource materialはある。ただしcurrent `commitSwap()`引数にはrecord identityが無く、このjoinは
**未配線**である。latest recordを無条件採用せず、expected next `presentSerial`とtoken serialの双方を一致させる。

### 3.5 `pendingOpportunity_` / `lastFinalizedOpportunityOrdinal_`

`pendingOpportunity_`はDWM actual ordinalごとのlatest candidateを保持し、次のDWM ordinal前進または`close()`で
`finalizePendingOpportunity()`される (`presentation_opportunity_scheduler.cpp:230-249`, `:269-348`)。
`lastFinalizedOrdinal_`はこの遅延finalize済みDWM ordinalであり、次required intentのlocal identityではない。
`Decision::lastFinalizedOpportunityOrdinal`はdiagnostic provenanceで、issuance authorityにしてはならない。

### 3.6 `close`

current `close()`は`pendingRender_`があればfailし、pending DWM opportunityをfinalizeしてからsource-frame側の
tail dropを計算する (`presentation_opportunity_scheduler.cpp:354-389`)。required setはsnapshotへ残るが、
未発行required tailをqueue残数として直接accountするstateはまだ無い。

### 3.7 Current termination

`OUTSIDE_SOURCE_DOMAIN_DECISION`はtransport suppressionへ入り、`pastSourceDomain`ならrender callback内で
`DOMAIN_TERMINAL`をclaimして`finishMeasurement()`を呼ぶ
(`compositor_rhi_item.cpp:421-446`)。同じterminal branchが後段にも存在する (`:494-512`)。
これはW4-C3がexactに帰属したcurrent premature completion pathである。

## 4. Qualified formal opportunity completion

counter-free online authorityを次のtransactionとしてfreezeする。

```text
RESERVED
  immutable required setのhead intentを1件だけreserve
  reservation_idとintent_ordinalをcomposition token scopeへexactにbind

RENDER_COMPLETED
  同じreservation_id / intent_ordinal / targetのmarkRenderCompleteが1回成功
  まだconsumeしない

COMMITTED
  同じreservation/tokenに属するsuccessful native Present recordと
  matching frameSwapped commitがexact joinされる
  ここでのみqualified completionが1件成立
  ここでのみrequired intentを1件consumeする
```

いずれかのidentity欠損、不一致、Present failure、swap欠損、二重commit、順序違反はfail-closeする。
callback QPCやordinalの近接でjoinしない。ETW display outcomeは後段のsatisfaction/accounting authorityとして維持し、
次intentをonline発行する根拠にはしない。

用語を分離する。

```text
issued       queue headをreserveしformal tokenへtransportした
rendered     matching markRenderCompleteが成功した
committed    matching successful Present/swap transactionが確定した
satisfied    後段のformal token -> Present -> FinalState authorityでrequired intentを満たした
```

`committed != satisfied`である。online schedulerはfuture display outcomeを待たず、final accountingはcommitを
display successへ読み替えない。

## 5. Candidate comparison

### A. Local completed-opportunity counter

変更するもの:

- start時に`qualifiedCompletedCount = 0`を固定する
- pendingが無いとき、次ordinalを`qualifiedCompletedCount`から選ぶ
- qualified commit成功時だけcounterをexactly 1増やす
- pending中のduplicateは同じdecisionを返しcounterを変えない

変更しないもの:

- required set `[0,N)`
- intent決定後のsource mapping式
- final token / Present / FinalState satisfaction accounting
- thresholds、planned duration、source domain

これは「previous ordinalへ無条件に`+1`」するpolicyではない。値は成功したqualified transactionのcardinalityで
あり、invalid/uncommitted transactionを数えない。ただしordinal identityがcountの位置へ暗黙化されるため、
reserve/dequeueの保存則を別途実装しないとoff-by-oneや二重incrementを監査しにくい。

W3 FAIL回復については、W4-C3で確定したNULL DWM `+2/+3` jumpをissuanceから除去するには十分である。
planned window内に必要数のqualified commitが成立することまでは証明しないため、W3 PASSの十分条件ではない。

### B. Required-intent queue / reserve / commit-dequeue

変更するもの:

- start時のimmutable `[0,N)`を実際のrequired-intent queue authorityにする
- pendingが無い`selectForRender`はqueue headを**reserve**する。まだdequeueしない
- duplicate callbackは同じreservationを参照し、新規reserve/transport/consumeしない
- matching `markRenderComplete`はreservation stateだけを進める
- qualified commit成功時だけreserved headを1件dequeueする
- planned endでqueueのreserved entryとunissued tailをすべてunsatisfiedとしてaccountする

変更しないもの:

- required setの要素数とordinal `[0,N)`
- intent issuance後のsource mapping
- final display satisfaction authority
- P3 audio-master scheduler、P4 composition semantics、P5 source/layer capability

保存則は次である。

```text
N == satisfied + issued_but_unsatisfied + unissued_tail
dequeued_count == qualified_commit_count
0 <= dequeued_count <= issued_count <= N
active_reservation_count in {0,1}
```

display accounting後は、committedだがnot displayedのintentも`issued_but_unsatisfied`へ残す。
required setを縮小して式を成立させてはならない。

Bもknown `+2/+3` chainを除去するには十分だが、W3 PASSを保証しない。Aよりidentity、状態遷移、negativeの
観測点が明示的であり、**production correctionの第一候補をBとする**。

### C. Fail-close only

変更するもの:

- `past_source_domain && required_intent_membership`をfatal contract failureにする
- `DOMAIN_TERMINAL`によるnormal completionを禁止する
- normal stop ownerをplanned measurement endだけにする

変更しないもの:

- current NULL DWM ordinal selection
- current source mappingとtransport
- required intentの発行方法

Cはfalse successful completionを止めるが、`+2/+3`によるordinal/target jumpと未発行intentを回復しない。
したがってW3 FAIL correctionとしては**不十分**であり、Bを安全に実装できない場合のfail-close baselineに限る。

### 5.1 Risk comparison

| candidate | known W3 causeの除去 | W3 PASS保証 | P3 risk | P4 risk | P5 risk |
|---|---|---|---|---|---|
| A local counter | 十分 | しない | 低。P2 formal専用なら非接続 | 低 | 中。render/swap stateの二重incrementに注意 |
| B queue/dequeue | 十分 | しない | 低。P2 formal専用なら非接続 | 低 | 中。token/reservation exact joinを維持する必要 |
| C fail-close only | 不十分 | しない | 低 | 低 | 低 |

A/BともP2 formal feature scope外へ接続しない。P3/P4/P5への最大リスクはsource mappingではなく、共有render
callback上のpending state、composition token、native Present / frameSwapped pairingを壊すことである。

## 6. Planned end / source mapping contract

normal completionをclaimできるonline eventは`PLANNED_WINDOW_END`だけとする。

```text
past source domain          -> normal completionではない
required queue empty        -> 単独ではnormal completionではない
DOMAIN_TERMINAL             -> normal completionではない
controller/manual/fatal     -> normal completionではない
PLANNED_WINDOW_END          -> normal stopをclaim可能
```

planned end後は新しいintentを発行しない。end時に未commit reservationがあれば
`WINDOW_END_WITH_INFLIGHT_REQUIRED_INTENT`としてfail-closeし、post-end commitでmeasurement内のsatisfactionを
遡及生成しない。unissued tailはunsatisfiedのまま残す。planned endは「正常な停止原因」であり、結果PASSを
単独で意味しない。全required setのfinal accountingがthresholdを満たして初めてPASSになり得る。

source mappingは次の順序を維持する。

```text
immutable required intentをqueueからidentify/reserve
  -> intent ordinalを確定
  -> 独立したsource mappingでtargetを計算
  -> source-domain predicateを評価
```

required memberがsource domain外へmapされた場合、そのintentをconsumeせずcontract failureにする。
source domainへ合わせてrequired setを縮小せず、source frameをintent identityとして使わない。

## 7. Frozen negative contract

B3ではtestを実装しない。implementationと同時に、検査が無ければ落ちないnegativeを最低限次で固定する。

```text
NegativeRequiredSetMutableAfterStart
NegativeRequiredTailShrinkAtEnd
NegativeSelectConsumesBeforeCommit
NegativeMarkRenderCompleteConsumes
NegativeDuplicateReservesSecondIntent
NegativeDuplicateTransportsSecondIntent
NegativeDuplicateConsumesIntent
NegativeInvalidDecisionConsumesIntent
NegativeUncommittedRenderConsumesIntent
NegativePresentFailureConsumesIntent
NegativeFrameSwappedWithoutReservationConsumesIntent
NegativeCommitTwiceConsumesTwice
NegativeMismatchedReservationCommit
NegativeOutOfOrderQueueDequeue
NegativeCallbackIndexIntentAuthority
NegativeQpcIntentAuthority
NegativeDwmCounterIntentAuthority
NegativePhysicalObserverIntentAuthority
NegativeSourceFrameIntentIdentity
NegativePreviousOrdinalIncrementAuthority
NegativeEtwFutureOutcomeOnlineAuthority
NegativePresentedFinalStateOnlineAuthority
NegativePastRequiredSuccessfulCompletion
NegativeDomainTerminalNormalCompletion
NegativeNonPlannedStopNormalCompletion
NegativePostEndIntentIssuance
NegativePostEndCommitSatisfiesWindow
NegativeSourceMappingBeforeIntentIdentity
NegativeSourceDomainMutatesRequiredSet
NegativePrerollConsumesCurrentRequiredIntent
NegativeQueueCounterConservation
NegativeUnsatisfiedTailOmitted
NegativeSummaryTrustedWithoutRawLedger
```

positive側では少なくとも、primary reserve、duplicate suppression、matching render complete、matching successful
Present/swap commit、exactly-one dequeue、planned-end tail accountingを個別に観測可能にする。

## 8. Production correction entry conditions

次をB3 design closureとしてfreezeし、candidate Bのproduction correction implementationへ進める。

```text
current control-flow inventory             FROZEN
online causal boundary                     QUALIFIED LOCAL COMMIT
future ETW outcomeのonline逆輸入           FORBIDDEN
required set                               IMMUTABLE [0,N)
duplicate/invalid/uncommitted consumption   FORBIDDEN
source mapping order                        INTENT ISSUANCE AFTER IDENTITY
normal completion owner                     PLANNED_WINDOW_END ONLY
candidate comparison                        A/B/C COMPLETE
selected candidate                          B QUEUE/RESERVE/COMMIT-DEQUEUE
negative contract                           FROZEN
```

source schemaとpatched Qtのpublication順序からexact native Present joinに必要なmaterialは存在するが、current
`commitSwap()`には未配線である。実装ではexpected next `presentSerial` / reservation token serial / intent ordinal /
HRESULT / swapchain identityを同一recordで検査する。どれかを同一causal callbackで確定できなければcallback/QPCで
救済せず、Bを実装せずCへfail-closeする。

implementation後のclosureには以下が必要であり、本design完了だけでは満たさない。

1. queue transactionと全negativeのtargeted test
2. ordinary / P5-C / P5-D / P5-E regression
3. P2 correctness/Seek 6/6
4. fresh W3 canonical 3/3とfrozen 55 fps / drop 2% threshold PASS
5. P3-C-2 9/9、P4 formal 3/3
6. raw / summary / exact provenance / SHA-256 manifest / docs audit

## 9. B3 design exit

```text
B3 static inventory                 CLOSED
B3 corrective design               CLOSED
selected production candidate       B REQUIRED-INTENT QUEUE
production implementation           I0 DONE / I1 NOT STARTED (§10)
test / capture                      I0 TARGETED TEST DONE / LIVE CAPTURE NOT RUN
canonical W3 verdict                UNCHANGED / FAIL
P5-E4                               BLOCKED
```

## 10. B3-I0 実装結果 — Exact Qualified Commit Join

I0はB3 candidate Bのうち**join provenanceだけ**を確定するsliceである。required-intent queue semantics、
ordinal issuance、threshold、required set、W2/W3 historical authority、FinalState satisfactionは変更していない。

### 10.1 expected_present_serialのauthority

`expected_present_serial`のauthorityはpatched Qtが`IDXGISwapChain::Present`直前にmintし、そのPresentの
`MvmNativePresentRecord`自身が保持する`presentSerial`だけである。同じ値をPresent return直後に
one-shot `MvmNativePresentFrameSwappedReceipt`へ複製し、DirectConnectionの`frameSwapped` callbackが
1回だけconsumeする。

```text
scheduler decision      reservation_id   (++reservationSerial_、schedulerが唯一のproducer)
composition token       token_serial / intent_ordinal
patched Qt Present      present_serial / swapchain_identity / HRESULT   ← authority
one-shot receipt        上の値のexact copy。app側で再構成しない
frameSwapped callback   receipt -> exact record lookup -> qualified join
```

`last + 1`、latest record、ring array position、QPC proximityからは生成しない。
`ExactQualifiedCommitJoin::bindNativePresent()`の`expectedPresentSerial_ = evidence.presentSerial;`が
唯一の代入箇所であり、`NativePresentHook::recordForPresentSerial()`はpresent serialが一意に一致する
recordだけを返す(0件も2件以上もfail-close)。

### 10.2 1 transactionとして検証するidentity

```text
RESERVED            reserve(reservation_id, intent_ordinal, token_serial)
RENDER_COMPLETED    markRenderComplete(同一 reservation_id / intent_ordinal / token_serial)
NATIVE_PRESENT      bindNativePresent(record: token present / intent ordinal exact /
                    present_serial != 0 / swapchain_identity != 0 / HRESULT >= 0 /
                    capture envelope内でswapchain identity固定)
COMMITTED           commitFrameSwapped(receipt: 上の全idを一致比較)
                    -> QUALIFIED_COMMIT
```

`QUALIFIED_COMMIT`はsuccessful native Present + matching frameSwapped + matching render completionが
すべて成立した場合だけ返す。**queue dequeueはまだ行わない**。
identity欠損、不一致、Present failure、swap欠損、二重commit、順序違反はすべてfail-closeし、
`compositor_rhi_item.cpp`の`recordFrameSwapped()`はfatal latchへ落とす。

### 10.3 変更範囲

| 対象 | 変更 |
|---|---|
| `src/media/gpu_preview/qualified_present_commit_join.{h,cpp}` | 新規。join state machineとerror分類 |
| `src/app/preview/native_present_hook_abi.h` | ABI v4 -> **v5**。receipt構造体、receipt loss counter 3種、layout signature |
| `qt-patches/qtbase-6.11.1/0001-*.patch` | Present return直後のreceipt mint、`..._take_frame_swapped_receipt` export、one-shot consume、begin/endでのreset・stale計上 |
| `src/app/preview/native_present_hook.{h,cpp}` | `takeFrameSwappedReceipt()` / `recordForPresentSerial()`、receipt counterのauthority検査への編入 |
| `src/app/preview/compositor_rhi_item.{h,cpp}` | envelope開始でjoin start、decisionでreserve、render完了でjoin前進、`frameSwapped`でreceipt -> record -> bind -> commit |
| `src/media/gpu_preview/presentation_opportunity_scheduler.{h,cpp}` | `decision.reservationId`。duplicate callbackは同じreservationを返す |
| `apps/compositor_spike/compositor_spike_controller.cpp` | `reservation_id`とreceipt counterをJSONへ出力 |

ABI v5化に伴い、W2-B1 intent transport contractのversion negativeは
`NegativeAppV4QtV5` / `NegativeAppV5QtV4`へ更新した。

### 10.4 I0 targeted test

```text
p2_qualified_present_commit_join                   join positive + join-specific negatives
p2_b3_i0_exact_qualified_join_architecture         source-level provenance guard
p2_b3_i0_exact_qualified_join_guard_*              guard自身のmutation test (Good + negative 12)
```

positiveは`QUALIFIED_COMMIT`、`expected_present_serial`のrecord由来、swapchain identityのenvelope固定を
個別に観測する。negativeはnative側(record欠損 / intent mismatch / token mismatch / token欠損 /
intent provenance欠損 / present serial 0 / swapchain identity 0 / Present failure)、commit側
(frameSwapped欠損 / reservation mismatch / intent mismatch / token mismatch / present serial mismatch /
swapchain identity mismatch / HRESULT mismatch / duplicate commit)、lifecycle
(envelope外reserve / reservation id 0 / 二重reserve / render完了なしのPresent / reservationなしのswap /
native Present未結合のswap / envelope内swapchain migration)を個別に落とす。

ABI v5化に伴い、W2-B2 terminal shadow contractのfixtureも`abi_version=5`へ更新した。
同fixtureはB1 transport checkerへ委譲するため、更新しないとGood 4件と`NegativeIntentJoinMutation`が落ちる。

ordinary CTest (ucrt64-release、`-LE 'performance|stability'`) は**1196/1200 PASS**である。
残り4件は本sliceより前 (`379c274`のclean worktree) でも同じく失敗する既存failureであり、
`p2_c3_a3_t2_startup_order` negative 3件 (contract ps1がCRLF、対象sourceがLFのためmutation anchorが
一致しない) と`p2_present_id_oracle_live` (本機で2026-08-23以降の全runが`ORACLE_SAMPLING_GAP`) である。
どちらも本sliceでは修正していない。

architecture guardは`AGENTS.md`の規約どおりmutation testを併設した。実sourceの変異コピーに対し、
receipt ABIの削除、receipt serialのglobal counter由来化、one-shot exportの削除、consume省略、
app側receipt取得APIの削除、exact record lookupの削除、commit順序のbypass、reservation identityの
非scheduler化、`expected_present_serial`のlast+1推定、latest record authority、sequential serial推定、
QPC近接joinの12件をguardが検出することを固定する。

### 10.5 未変更 / 次slice

```text
required-intent queue semantics      UNCHANGED (I1)
ordinal issuance semantics           UNCHANGED (I1)
W2/W3 historical authority           UNCHANGED
FinalState satisfaction / threshold  UNCHANGED
required set                         UNCHANGED
canonical W3 verdict                 UNCHANGED / FAIL
P5-E4                                BLOCKED
```

ABI v5はpatched Qtの再build後にだけ実行時互換になる。`scripts/prepare-p2-c0-qt-source.ps1` と
`scripts/build-p2-c0-patched-qt.ps1`の再実行はI0のexit条件ではなく、live captureを行うsliceで実施する。
patch自体はpristine v6.11.1 sourceへ0001 -> 0002の順で適用可能であることを確認済みである。
