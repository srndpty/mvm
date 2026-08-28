# P2-D5-2 B3 — Counter-free Required-intent Completion Correction

## 1. Scope / status

- 対象: P2 formal Playbackのrequired-intent issuance / completion control
- 前提: B2は`EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE`でCLOSED
- phase: **DESIGN CLOSED / IMPLEMENTATION I0 + I1 + I5A + I5B(+amendment 2) DONE / I5B runtime smoke PASS** (実装状況は§10、§11、§15、§16)
- production code: I0 exact qualified commit joinとI1 required-intent queueを接続済み
- test / capture: I0/I1 targeted test済み。live captureは**NOT RUN**
- canonical W3 verdict: **UNCHANGED / FAIL** (fresh 3/3試行はrun 1 acquisition failureで不成立。§17)
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
production implementation           I0 DONE / I1 DONE (§10, §11)
test / capture                      I0/I1 TARGETED TEST DONE / LIVE CAPTURE NOT RUN
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

## 11. B3-I1 実装結果 — Required-intent Queue State Machine

I1ではstart時のimmutable `[0,N)`を`RequiredIntentQueue`へ固定し、actual issuance identityを
queue headのreserveからのみ生成する。`selectForRender()`はreserve後に初めてsource targetを計算するため、
DWM counter/QPC/callback index/source frame/physical observer/previous ordinalはintent identityへ関与しない。

```text
reserve head -> identity確定 -> source mapping -> render complete (consume 0)
             -> I0 QUALIFIED_COMMIT evidence (consume 0)
             -> swap全failure条件をvalidate -> failure-free commit point -> exactly 1 dequeue
```

duplicate callbackはschedulerの同一pending decision、queue単体では同一active reservationを返し、
新規issue/transport/dequeueを行わない。missing/mismatch/unrendered transactionはfail-closeしdequeue 0、
source coverage不足は`SOURCE_COVERAGE_INSUFFICIENT`で失敗する。required setのskip・縮小は行わない。

planned endではactive reservationとunissued tailを保持する。online snapshotは`required/issued/rendered/
qualified_commit/dequeued/active/unissued_tail`とconservation verdictをJSONへ出力するが、
`display_satisfaction_imported=false`を固定し、後段FinalState satisfactionとは分離する。
normal completion ownerは`PLANNED_WINDOW_END`だけであり、queue emptyや`past_source_domain`、
`DOMAIN_TERMINAL`をsuccessful completionへ使わない。

targeted testはqueue state machine、既存opportunity scheduler regression、architecture guard、
guard mutation 13件を追加した。I0 exact native Present joinのsemanticsとABIは変更していない。
このsliceではcanonical W3 captureを実行しておらず、canonical W3 verdictとP5-E4 blockは不変である。

### 11.1 amendment — swap commitとのlogical atomicity

I0 `QUALIFIED_COMMIT`はpending qualified evidenceを確定するだけで、required queueをdequeueしない。
`commitSwap()`はQPC、render/swap ordinal、authority continuity、actual opportunity、pending finalizeを
すべて副作用なしでvalidateする。pending finalizeはprepare/applyへ分離し、queue dequeueより後に
failure pathを残さない。全検証後のfailure-free commit pointでqueue headを1件dequeueし、同じ経路で
scheduler swap stateをapplyする。rollbackによる救済は行わない。

`NegativeSwapCommitFailureConsumesIntent`はpost-I0のauthority regression、VBlank/QPC regression、
swap ordinal mismatchで`dequeued_count`が増えず、active rendered reservationが保持されることを固定する。
source coverage fatal後は元のfatal reasonを維持したnon-normal cleanupでqueueをclosedにし、active
reservationとunissued tailを保存する。architecture guardはdequeueをswap validationより前へ移すmutationを
拒否する。canonical W3/live captureは引き続き未実施である。

## 12. B3-I2 Composition Token Runtime Attribution

I2はdiagnostic-only sliceである。I0/I1 queue semantics、joinのaccept/reject、FATAL挙動、product counter、
required set、FinalState/display authorityは変更しない。nearest/latest/fallback、QPC proximity、serial推定も
追加しない。

### 12.1 `COMPOSITION_TOKEN_MISMATCH` predicate inventory

source上で同errorを生成できるpredicateは次の4個に閉じている。

| phase | predicate | 比較 |
|---|---|---|
| `BIND_NATIVE_PRESENT` | `NATIVE_TOKEN_PRESENT` | native recordにcomposition tokenがある |
| `BIND_NATIVE_PRESENT` | `NATIVE_INTENT_ORDINAL_VALID` | native recordのintent ordinal provenanceがvalid |
| `BIND_NATIVE_PRESENT` | `NATIVE_TOKEN_SERIAL_EQUALS_RESERVATION` | native token serialとactive reservationが一致 |
| `COMMIT_FRAME_SWAPPED` | `RECEIPT_TOKEN_SERIAL_EQUALS_RESERVATION` | one-shot receipt token serialとactive reservationが一致 |

従来は最初の2 predicateが1分岐へ畳まれていた。I2ではreturn/errorを変えずに分岐を分け、phaseとpredicateを
raw evidenceへ記録する。reservation、native record、frameSwapped receiptの各id、token presence/validity、
swapchain identity、HRESULTに加え、setToken/Present/frameSwapped threadとQPCをartifactへ保存する。

token lifetimeのsource-level順序は次である。

```text
render callback
  -> NativePresentTokenCaptureでtokenを構築
  -> destructorが同じrender threadでsetToken exportを同期呼出し
  -> Qt thread_local pending tokenへcopy
  -> actual Present直前にone-shot consumeしnative recordへcopy、pendingをreset
  -> Present return直後にrecordからthread_local receiptへcopy
  -> DirectConnection frameSwappedが同じthreadでreceiptをone-shot consume/reset
```

pending tokenが残った状態の再set、receipt未consumeで次Present、capture end時のpending/receiptは既存counterで
duplicate/staleとしてfail-closeする。boundary/preroll Presentも同じringへ入り、intent scope ledgerのexact
producer recordとraw token serialで区別する。

### 12.2 noncanonical runtime attribution

同条件の短時間formal Playbackを実施したが、I2後の2 runはいずれも元の
`COMPOSITION_TOKEN_MISMATCH`を再現せず、join前の次のFATALへ到達した。

```text
boundary swapがactive reservationを破棄しようとしました
```

両runともcapture envelopeは`FOREIGN_PRE_MEASUREMENT` 1 Presentの次に
`CURRENT_MEASUREMENT` reservation付きPresentを記録した。measurement開始時の
`formalOpportunityIgnoreNextSwap`がactive reservationのあるcallbackでconsumeされ、BIND/COMMITへ入る前に
停止した。このためI2 attributionの`failure_phase` / `failure_predicate`は`NONE`であり、元runの4候補を
推定で1個へ狭めない。queueは`qualified=0 / dequeued=0 / active=1`、conservation validを保持した。

pre-join fatalにも同じraw snapshotを適用した最終確認runでは、次をexactに記録した。

```text
failure phase/predicate  PRE_JOIN_BOUNDARY_SWAP /
                         BOUNDARY_SWAP_REQUIRES_NO_ACTIVE_RESERVATION
active reservation       id=1 intent=0 token=84
native Present record    present=2 intent=0(valid) token=84(present)
frameSwapped receipt     present=2 intent=0(valid) token=84(present)
swapchain / HRESULT      native == receipt / 0 == 0
thread                    setToken == Present == frameSwapped == 9692
QPC order                 setToken < Present enter < Present return < frameSwapped
capture sequence          present 1 token 83 FOREIGN_PRE_MEASUREMENT
                         present 2 token 84 CURRENT_MEASUREMENT
receipt counters          missing/duplicate/stale = 0/0/0
queue                     qualified=0 dequeued=0 active=1 tail=179 conservation valid
```

このrunではsetTokenからreceiptまでtoken identityは一致し、intervening Present、one-shot loss/reset failure、
preroll tokenのcurrent Presentへの混入は観測されなかった。停止点はtoken comparisonより前であるため、
元runの`COMPOSITION_TOKEN_MISMATCH` predicateの証明には使わない。

これはretryで成功cohortを選別せず、別のexact runtime orderingとして保存する。元のmismatch predicateは
**未確定**であり、production fixは設計しない。fresh W3 canonical 3/3にも進まない。

## 13. B3-I3 Boundary-swap Ownership Attribution

I3もdiagnostic-only sliceである。`formalOpportunityIgnoreNextSwap`の値、publication位置、consume位置、
active reservation時のFATAL、I0/I1 queue/join semanticsは変更していない。

### 13.1 flag source inventory

```text
initial/reset     CompositorSpikeState::formalOpportunityIgnoreNextSwap{false}
producer         measurement-start render callbackのstore(true)       exactly 1 site
consume/reset    recordFrameSwapped()のexchange(false)                 exactly 1 site
```

明示的なepoch reset writerは存在しない。初期値falseから始まり、各publicationを最初に通過した
`frameSwapped` callbackがpositionだけでconsumeする。present serial、token serial、scope、reservationは
flag自身に束縛されていない。

I3 ledgerはGUI threadのmeasurement-start request publication、render threadのrequest consume、required queue
start、first reservation、ignore publication、および全`frameSwapped`をglobal diagnostic event serialへ記録する。
event serial/QPC/callback位置は順序説明専用でidentity authorityではない。callback identityはone-shot receiptの
present/token serialをexact scope producer recordへ1件一致させて確定する。

### 13.2 exact runtime ordering

noncanonical smoke `boundary-ownership-b3-i3-20260828T094430Z`は次を記録した。

| event | QPC | thread | exact identity / state |
|---|---:|---:|---|
| 1 `FRAME_SWAPPED` | 1207680030718 | 27852 | Present 1 / token 88 / `FOREIGN_PRE_MEASUREMENT` / ignore pre=false / preroll reservation 1 active |
| 2 `MEASUREMENT_START_REQUEST_PUBLISHED` | 1207680194965 | 48976 | GUI measurement arm |
| 3 `MEASUREMENT_START_CONSUMED` | 1207680195028 | 27852 | render measurement start |
| 4 `REQUIRED_QUEUE_STARTED` | 1207680195167 | 27852 | current immutable queue start |
| 5 `IGNORE_NEXT_SWAP_PUBLISHED` | 1207680195167 | 27852 | publication serial 1 |
| 6 `FIRST_RESERVATION` | 1207680195225 | 27852 | reservation 1 / intent 0 / token 89 |
| 7 `FRAME_SWAPPED` | 1207680199391 | 27852 | Present 2 / token 89 / `CURRENT_MEASUREMENT` / ignore pre=true・consumed / reservation 1 active |

Present 1はenter 1207680029634、return 1207680030062、frameSwapped 1207680030718であり、ignore publication
より前にcallbackまで完了している。publication後にFOREIGN callbackがconsume siteを迂回したのではない。
Present 2はenter 1207680198061、return 1207680199361、frameSwapped 1207680199391で、flagの唯一のconsumerに
なった。両callbackはpresent/token serialとexact scope recordで結合済みである。

```text
foreign_callback_relation                    FOREIGN_CALLBACK_BEFORE_IGNORE_PUBLICATION
foreign_after_publication_without_consume    0
consumer                                     Present 2 / token 89 / CURRENT_MEASUREMENT
positional_contract_expresses_identity       false
verdict                                      POSITIONAL_BOUNDARY_OWNERSHIP_NOT_EXPRESSED
```

したがってpositional `ignore-next-swap`は「publication後の次callback」を一意に選べても、意図したboundary
Present identityを表現できない。このrunでは既に完了したFOREIGN Present 1を所有できず、CURRENT Present 2へ
所有権が移った。

### 13.3 identity-bound replacement候補（design only）

production fixはまだ行わない。候補は次の2つである。

1. **Exact boundary reservation**: measurement transition時に未完了のpreroll reservationが実在する場合だけ、
   `(reservation_id, intent_ordinal, token_serial, FOREIGN_PRE_MEASUREMENT)`をboundary ownerとしてfreezeする。
   frameSwappedはreceipt present/token serialとexact scope recordがすべて一致した場合だけconsumeする。
   既にcallback/commit済みならownerを発行しない。
2. **Preroll receipt closure handshake**: exact preroll receiptのconsume/commit完了をtransition前提にし、その後に
   current queueをstartする。positionによるignore flag自体を不要にする。

どちらもnearest QPC、callback index、latest Present、serial推定を使わない。選定にはtransition中の未完了
preroll reservation/receipt状態を追加artifactで確定する必要があるため、I3では実装しない。

元の`COMPOSITION_TOKEN_MISMATCH`はhistorical runtime failureとして未解決のまま保持し、今回のboundary
failureへ再分類しない。production fixとfresh W3 canonical 3/3はattribution closure後まで保留する。

## 14. B3-I4 Preroll Transition Quiescence Design

I4はdesign-only sliceである。production behavior、I0/I1 queue semantics、join accept/reject、Qt ABI v5、
canonical W3を変更しない。機械可読な設計契約は
`docs/p2-d5-2-b3-i4-preroll-transition-quiescence.json`に固定した。

### 14.1 lifecycle inventoryと現行gap

prerollの1 transactionは次の順で進む。

```text
Started/Active
  -> queue head reservation
  -> join Reserved
  -> render completion / join RenderCompleted
  -> Qt pending composition token
  -> actual Present record / token one-shot consume
  -> pending frameSwapped receipt
  -> exact receipt consume
  -> join NativePresentBound / Committed
  -> scheduler pending qualified evidence
  -> failure-free commitSwap + queue exactly 1 dequeue
```

`formalOpportunityEnvelopePrerollStarted`は開始履歴、`Active`は新しいrender decisionのscope分類、
`Completed`は現行のproducer close履歴にすぎない。現行measurement-start callbackは`Active=false`にして
`closeWithoutNormalCompletion()`を呼んだ直後に`Completed=true`をpublishする。しかしI1 contractでは
non-normal closeがactive reservationとunissued tailを保持する。さらに同じscheduler objectの次の`start()`は
stateを初期化する。このため3個のphase boolから旧epoch transactionのclosureを証明できない。

Qt ABI v5のone-shot stateも、tokenは`set_token`からactual Present enterまで、receiptはPresent returnから
`frameSwapped`のtakeまで存続する。capture endは残存stateをstaleとしてFATALにできるが、transition handshake用の
read-only pending snapshotは公開していない。将来実装では同じrender threadでtoken/receipt pendingを含むexact
snapshot/ackを追加する必要がある。counter差分、QPC、latest record、serial推定で代用しない。

### 14.2 exact quiescence predicate

`PREROLL_TRANSACTION_FULLY_QUIESCENT`は同一`FOREIGN_PRE_MEASUREMENT` epoch snapshotで次をすべて満たすこととする。

```text
preroll admission closed
&& scheduler pending render == false
&& scheduler pending qualified evidence == false
&& scheduler pending opportunity == false、またはexact finalize済み
&& queue active reservation count == 0
&& join active reservation == false
&& Qt pending composition token == false
&& Qt pending frameSwapped receipt == false
&& issued == rendered == qualified_commit == dequeued
&& queue conservation valid
&& issued prefixの全要素が
   reservation -> token publication -> native Present -> receipt
   -> qualified join -> swap commit/dequeueへ1:1 exact join済み
&& preroll scope ledgerのissued/suppressed全recordがexact terminal partition済み
&& transport failure counters == 0
```

identityはreservation id / intent ordinal / token serialと、native record↔receiptのpresent serial /
swapchain identity / HRESULTだけを使う。unissued tailはtransactionではないためquiescenceの空条件に含めないが、
immutable required setのtailとして保持する。phase bool単独はどの項も置き換えない。

### 14.3 transition handshake

将来のproduction transitionは、preroll admissionを閉じたあと、既に発行済みのFOREIGN transactionだけを通常の
exact join/commit pointまでdrainする。admission closeは新規`selectForRender`を止めるgateであり、scheduler
`close()`とは分離する。先にschedulerをcloseすると未完了transactionのcommitを拒否するため、active transaction
drainとpending opportunityのexact finalize後に旧schedulerをcloseする。quiescence snapshot/ack成立後にcurrent required queueをstartし、その後に
canonical measurementをarmしてissuance gateとmeasurement windowを開始する。順序は次で固定する。

```text
preroll admission close request
  -> exact active FOREIGN transaction drain
  -> PREROLL_TRANSACTION_FULLY_QUIESCENT ack
  -> current required queue start
  -> canonical measurement arm
  -> current issuance gate open + measurement window start
```

handshake waitはmeasurement armより前でありrequired-intent windowへ算入しない。未解決ならcurrent intentを発行せず
待機する。timeoutは`PROTOCOL_FAIL_CLOSE_NOT_PERFORMANCE_DROP`であり、表示dropへ分類せず、required setのskip/dequeue/
縮小も行わない。

### 14.4 boundary ownershipの選定

selected designは**Preroll receipt closure handshake**である。quiescence後にはFOREIGN callbackが残らないため、
positional `formalOpportunityIgnoreNextSwap`は設計上削除可能であり、CURRENT Presentをboundaryとしてconsumeする
必要もない。

I3のExact boundary reservation候補は、handshake開始時に実在する未完了FOREIGN reservationだけへ
`(reservation_id, intent_ordinal, token_serial, FOREIGN_PRE_MEASUREMENT)`をprospectiveに束縛し、そのtransactionを
closureへ進める補助手段として採用できる。quiescenceの代用ではない。既にframeSwapped/commit済みのhistorical
Presentへownerを後付けせず、CURRENT scopeとは決して一致させない。

negative contractはcurrent queue start before closure、measurement window before handshake、completed FOREIGNへの
retroactive owner、CURRENT Present consumed as boundary、timeout as performance dropをすべて`PROTOCOL_FATAL`に固定した。
加えてphase bool authority、required-set shrink、positional flag残存、serial inference、historical mismatch再分類を
mutation testで拒否する。

元の`COMPOSITION_TOKEN_MISMATCH`は`UNRESOLVED_HISTORICAL_RUNTIME_FAILURE`のまま保持し、I3 boundary failureへ
再分類しない。production fix、runtime smoke、fresh W3 canonical 3/3はI4の範囲外である。

## 15. B3-I5A Qt One-shot Exact Snapshot ABI

I5AではQt native Present hook ABIをv5からv6へ上げ、I4 quiescenceが必要とするone-shot stateの
read-only取得境界だけを追加した。handshake本体、measurement transition、required queue、qualified join、
canonical W3には接続しない。

capture開始時にpatched Qtが単調な`captureEpoch`をmintし、その時点のrender thread IDを固定する。snapshot callerは
expected capture epoch、ABI version、snapshot size、snapshot layout signatureを入力し、Qtはcurrent capture active、
epoch一致、`GetCurrentThreadId()`一致をすべて検査する。不一致は専用result codeで拒否する。

成功snapshotは次のraw stateをcopyする。

```text
capture epoch / capture active / capture thread / caller thread / exactness
pending token valid / MvmNativePresentCompositionToken
pending receipt valid / MvmNativePresentFrameSwappedReceipt
```

exportは`mvmPendingTokenValid`、`mvmFrameSwappedReceiptValid`およびraw TLS objectを変更しない。actual Present enter、
set token、frameSwapped receipt take、capture endもcapture threadと異なるcallerをfail-closeする。pending stateは
latest Present、ring counter差分、QPC、callback index、serial arithmeticから復元しない。

app wrapperはexport availability、ABI v6、ring/snapshot layout handshake、expected epoch、caller threadを再検査する。
I5Aでは`readOneShotSnapshot()`のcallerをmeasurement codeへ追加していない。元のhistorical
`COMPOSITION_TOKEN_MISMATCH`も未解決・未再分類のままである。

I5A amendmentではsnapshot layout signatureを、snapshot直下の全semantic fieldと、raw token内のsource identityを
含む全semantic field、raw receiptの全semantic fieldまで拡張した。reserved paddingはsemantic fieldではないため
署名対象外とする。同一sizeを保ったまま`captureActive`と`captureThreadId`、またはreceiptの`hresult`と
`tokenPresent`のoffset authorityを差し替えるmutationは、独立期待値によるABI unitとarchitecture guardの両方で
拒否する。このamendmentもproduction transitionへは接続せず、I5B開始条件はI5A gateのgreenである。

## 16. B3-I5B 実装結果 — Admission-close / Drain Handshake

I5Bはpreroll(FOREIGN)からcurrent(CANONICAL)へのtransitionをproduction実装したsliceである。I4 designの
`ordered_steps`と`PREROLL_TRANSACTION_FULLY_QUIESCENT`をそのまま実行時契約にし、I5A ABI v6のexact one-shot
snapshotをquiescence checkのQt側入力として接続した。I0/I1のqueue semantics、join accept/reject、required set、
FinalState satisfactionは変更していない。

### 16.1 明示state machine

`src/media/gpu_preview/preroll_transition_handshake.{h,cpp}`を追加した。transitionのauthorityはphase boolでも
callback位置でもなく、この列挙だけである。

```text
OPEN -> DRAIN_REQUESTED -> DRAINING -> QUIESCENCE_CHECK -> QUIESCENT
     -> CURRENT_READY -> MEASUREMENT_ARMED -> CURRENT_RUNNING
```

全failureは`PROTOCOL_FATAL`へ落ちる。timeoutも同じであり、performance dropやrequired setのskip/縮小へは
決して変換しない。handshake waitは`admission close -> quiescence ack`の区間として別に記録し、
`armMeasurement()`はcanonical startがack QPCより前になるcaseを`CANONICAL_WINDOW_MUTATED`で拒否する。

### 16.2 admission closeとscheduler closeの分離

```text
admission close   新規FOREIGN reservationだけを禁止する
                  既存active FOREIGN transactionのrender / Present / receipt /
                  join / commit / dequeue / finalizeはそのまま許可する
scheduler close   active transaction drain (active count == 0) と
                  pending opportunityのexact finalizeの後にだけ行う
```

render callbackの`selectForRender`前段に`foreignAdmissionOpen`を追加し、preroll scopeで閉じている間は
新しいFOREIGN reservationを発行しない。`PresentationOpportunityScheduler::finalizePendingOpportunityExact()`を
追加し、closeより前にpending opportunityを確定させる。accept/reject判定とledger内容は変更していない。

### 16.3 quiescence check

drain完了後、同一render callback内で1つのlogical snapshotを採取する。ABI v6 one-shot snapshot、
required queue snapshot、join active reservation、scheduler pending state、preroll scope ledger、
transport counterを同じ区間から読み、間にPresentもcallback境界も挟まない。

I4 §14.2の全predicateを`PrerollQuiescenceVerdict`の個別fieldとして保存する。1つでもfalseなら
current queue startとmeasurement armを許可せず、次callbackまで待機する。capture epoch不一致と
render thread不一致は待って解消する条件ではないため、その場で`CAPTURE_EPOCH_MISMATCH` /
`RENDER_THREAD_MISMATCH`として停止する。

`ISSUED_PREFIX_EXACT_IDENTITY_CLOSED`はscope ledgerのtoken serialとnative Present recordの
`token.tokenSerial`のexact一致だけで結合し、present serial、swapchain identity、HRESULT、
intent ordinal provenanceを個別に検査する。QPC近接、latest Present、callback index、serial推定は使わない。

### 16.4 ack後の順序

```text
quiescence ack
  -> current required queue start (issuance gateは閉じたまま)
  -> canonical measurement start/end authority freeze
  -> current issuance gate open + measurement window start
```

`formalOpportunityCaptureActive`はissuance gate openの1点でだけtrueになる。measurement-start callbackは
handshakeが成立するまでarmせず、`update()`で次callbackを予約して待つ。したがってhandshake waitは
canonical measurement windowへ算入されない。

### 16.5 positional flagの削除

`formalOpportunityIgnoreNextSwap`はstate、producer、consumerごと削除した。quiescence成立後はFOREIGN
callbackが残らないため、位置でboundary swapを読み飛ばす必要がない。ack後にFOREIGN scopeのframeSwappedが
到達した場合は、I2と同じraw evidenceを保存して`PRE_JOIN_BOUNDARY_SWAP`のPROTOCOL_FATALで停止する。
exact boundary reservationはdrain中のactive FOREIGN reservationにだけ束縛でき、完了済みFOREIGN Presentへの
後付け(`RETROACTIVE_FOREIGN_OWNER`)とCURRENT Presentのboundary化(`CURRENT_PRESENT_AS_BOUNDARY`)は拒否する。

B3-I3のpositional ownership guardは対象が消えたため廃止し、artifactも
`boundary_swap_ownership_attribution`から`preroll_transition_handshake`
(`mvm-p2-d5-2-b3-i5b-preroll-transition-handshake-1`)へ置き換えた。I3 §13の観測結果自体はhistorical
recordとして本documentに残す。

### 16.6 targeted test

```text
p2_b3_i5b_preroll_transition_handshake              state machine unit (positive / negative)
p2_b3_i5b_admission_close_drain_architecture         source-level契約guard
p2_b3_i5b_admission_close_drain_guard_*              guard mutation (Good + negative 18)
```

unitのpositiveはactive FOREIGN 0件caseと1件drain caseの両方を通す。negativeはquiescence predicate 13件を
1 fieldずつ壊し、いずれも`quiescent=false`・当該fieldのfalse・ack拒否・current queue start拒否・
measurement arm拒否を個別に固定する。mutation guardはmixed epoch、wrong render thread、early queue start、
early measurement arm、issuance before arm、admission close後の新規FOREIGN reservation、drain前scheduler close、
pending opportunity finalize省略、retroactive owner、CURRENT boundary、timeoutのperformance drop化、
handshake waitのwindow算入、canonical window startのcallback begin化、canonical startのcurrent queue
準備前sample、positional flag残存、one-shot snapshot切断、nearest QPC join、historical mismatch再分類を
拒否する。

ordinary CTest (ucrt64-release、`-LE 'performance|stability'`) は**1259/1266 PASS**である。残り7件は本slice前の
clean worktreeでも同じく失敗する既存failureであり、`p2_c3_a3_t2_startup_order` negative 3件、
`p2_present_id_oracle_live`、`p2_d5_2_w2c21_required_intent_domain_architecture`、
`p2_d5_2_w4c0_static_control_flow_goodstaticinventory`、`p2_d5_2_w4c3_stop_arbitration_architecture_good`である。
後半3件は本slice以前からのguard driftである。w2-c2.1はrequired setがI1でqueueへ移動した後もscheduler側の
`push_back`を要求し、w4-c0はanchored ordinal advancement式、w4-c3は`publishStopRequest`の行折り返しに一致
しない。いずれも本sliceでは修正していない。本sliceが実際に変更したassertionだけは更新した
(w4-c0のinvocation gate、w4-c3のmeasurement-start snapshot window、w2-c01のpreroll close順序、
I5Aのone-shot snapshot接続方向)。


### 16.7 ABI v6 patched Qt rebuild と noncanonical runtime smoke 1回目

patched Qt sourceをupstream v6.11.1へ戻したうえで更新済み`0001` -> `0002`を再適用し、
`scripts/prepare-p2-c0-qt-source.ps1`のprovenance検査をPASSさせてから再buildした。

```text
qtbase upstream          59c81a3c2247b821b9b84b4eb8d939b77e07e276
qtdeclarative upstream   a02bed441965ee1f18f856352c7d5ee5ba35d795
Qt6Gui.dll exports       ..._abi_version / _begin / _set_token / _end
                         ..._take_frame_swapped_receipt
                         ..._one_shot_snapshot          ← ABI v6で追加
```

同条件のnoncanonical formal Playback smoke (`warmup 1s / measure 3s / hook on / formal preflight`) を
**1回だけ**実行した。artifactは`build/handshake-b3-i5b-20260828T202738Z/formal-playback-smoke.json`である。
retryによる成功cohortの選別は行っていない。

runtime ABI handshakeは成立した。

```text
app ABI / Qt ABI                 6 / 6
snapshot layout handshake        accepted
capture epoch                    1 (exact)
observer thread == render thread true
```

quiescence handshakeもack成立まではdesignどおりに進んだ。

```text
event 1-2  FRAME_SWAPPED (FOREIGN_PRE_MEASUREMENT, active reservation)
event 3    MEASUREMENT_START_REQUEST_PUBLISHED   (GUI)
event 4    MEASUREMENT_START_CONSUMED            (render)
event 5    PREROLL_ADMISSION_CLOSED              -> DRAINING
event 6    PREROLL_DRAIN_OBSERVED                scheduler closed (active 0 / finalize済み)
event 7    PREROLL_QUIESCENCE_ACK                -> QUIESCENT
event 8    REQUIRED_QUEUE_STARTED                -> CURRENT_READY
```

`PREROLL_TRANSACTION_FULLY_QUIESCENT`は同一evaluation (evaluation_count=1) で13 predicate全てtrueであり、
`same_capture_epoch` / `same_render_thread`も成立した。active FOREIGNは0件でdrainした。

その直後の`MEASUREMENT_ARM`で停止した。

```text
state    PROTOCOL_FATAL
error    CANONICAL_WINDOW_MUTATED
reason   P2-D5-2 B3-I5B measurement arm拒否: CANONICAL_WINDOW_MUTATED
```

原因は本sliceのimplementation defectである。`armMeasurement()`はcanonical window startが
quiescence ack QPCより前であることを拒否するが、arm siteはcallback begin QPCを渡していた。ack QPCは
同じcallback内でcallback beginより後にsampleされるため、`start < ack`が常に成立する
(`handshake_wait_qpc = 72` ticks)。unit testは`start=240 / ack=230`を手で与えていたため検出できなかった。

fail-close自体は設計どおりに機能した。current required queueは`issued=0 / dequeued=0 / tail=180`で
conservation validのまま保持され、issuance gateは開かず、measurement windowも開始せず、performance dropへは
一切変換されていない。teardownも成功している。

修正はarm siteでquiescence ack後に`measurementArmQpc = gpu::qpcTicks()`をsampleし、
`scheduler_.start` / `measurementStartQpc` / `measurementEndQpc` / `measurementStart.qpc` /
`armMeasurement()`すべてをその値で統一した。architecture guardへ
`armMeasurement(callbackBegin ...)`を拒否するassertionと、mutation case
`NegativeCanonicalStartFromCallbackBegin`を追加した。W4-C3 guardのmeasurement-start snapshot patternも
同じ変数名へ更新した。

**この修正後のsmokeは未実行である。** 失敗した1 runはattribution authorityとしてそのまま保存し、
次のsmokeは別runとして記録する。canonical W3は引き続きHOLDである。


### 16.8 noncanonical runtime smoke 2回目 (post-fix validation)

r1 (`build/handshake-b3-i5b-20260828T202738Z`) はpre-fix checkpointのvalid failureとしてそのまま保存する。
r2は同一条件・**exactly 1 run**のpost-fix validationであり、retryによる成功cohort選別ではない。

```text
parent failure artifact   handshake-b3-i5b-20260828T202738Z (PROTOCOL_FATAL / CANONICAL_WINDOW_MUTATED)
change under validation   canonical arm authority: callbackBegin -> post-quiescence measurementArmQpc
r2 artifact               build/handshake-b3-i5b-r2-20260829T000543Z/formal-playback-smoke.json
```

#### I5B handshake

```text
app ABI / Qt ABI                             6 / 6
snapshot layout handshake                    accepted
capture epoch                                1 (exact)
observer thread == render thread             true (25940)

state                                        CURRENT_RUNNING
error                                        NONE
handshake_step_order_exact                   true
quiescence evaluation_count                  1
quiescence predicate 13件                     all true
foreign_callback_after_quiescence_ack_count  0
current_callback_before_issuance_open_count  0
wait_charged_to_measurement_window           false
canonical_window_frozen                      true
current_issuance_open                        true
verdict                                      PREROLL_TRANSITION_QUIESCENCE_HANDSHAKE_OBSERVED
```

exact orderingは次である。canonical startはack QPCより後であり、handshake wait 72 ticks (約7.2µs) は
measurement windowの外にある。

```text
QPC 1400409378592  MEASUREMENT_START_REQUEST_PUBLISHED  thread 41196 (GUI)
QPC 1400409545288  MEASUREMENT_START_CONSUMED           thread 25940 (render)
QPC 1400409545305  PREROLL_ADMISSION_CLOSED             -> DRAINING
QPC 1400409545361  PREROLL_DRAIN_OBSERVED               scheduler closed
QPC 1400409545362  PREROLL_QUIESCENCE_ACK               -> QUIESCENT
QPC 1400409545365  canonical measurement start          (>= ack)
QPC 1400409545395  REQUIRED_QUEUE_STARTED               -> CURRENT_READY
QPC 1400409545398  MEASUREMENT_ARMED                    -> MEASUREMENT_ARMED
QPC 1400409545408  CURRENT_ISSUANCE_OPENED              -> CURRENT_RUNNING
QPC 1400409545456  FIRST_RESERVATION                    reservation 1 / intent 0
```

FOREIGN callbackはPresent 1 (token 91) とPresent 2 (token 92) の2件で、いずれもack前に完了している。
本runのactive FOREIGN transactionはdrain poll時点で0件だった。1件drainのlive evidenceは未取得である
(1件になるまでretryはしない)。

#### I0 exact join / transport

```text
missing / duplicate / stale token             0 / 0 / 0
missing / duplicate / stale receipt           0 / 0 / 0
failed present / ring overflow / thread mismatch  0 / 0 / 0
token set failure                             0
native present record count                   180
authority_pass                                true
composition token join failure                captured=false / phase=NONE / predicate=NONE
nearest-latest fallback used                  false
capture envelope authority_pass               true
```

capture envelopeのみ`missing_token_count = 2`だが、これはW2-C0.1 supersetがdomain外prestart Presentの
token欠損をcandidate-level gateへ委ねる既存契約どおりであり、projected counterは0である。

#### I1 required-intent queue / termination

```text
required / issued / rendered / qualified / dequeued   180 / 180 / 180 / 180 / 180
unissued tail                                        0
active reservation count                             0
conservation_valid                                   true
required_set_immutable                               true
display_satisfaction_imported                        false
planned_window_ended                                 true

stop cause                                           PLANNED_WINDOW_END (witness 1件 / duplicate 0)
DOMAIN_TERMINAL successful completion                0
source_coverage_ok                                   true
fatal                                                なし / process exit 0 / teardown success
```

intent scope ledgerはFOREIGN 2件・CURRENT 180件にexact partitionされ、missing/ambiguous/mutation/unmatched
はすべて0である。CURRENT intent ordinalは0..179のcontiguousで、reservation idは1..180だった。これは記録で
あってverdict authorityではない。正しさは
`queue head reserve -> exact token transport -> Present -> receipt -> qualified commit -> dequeue`が
成立したことによる。

#### 本runで到達していないもの

`effective_fps = 59.946` / `drop_rate = 0` / `formal_true_opportunity_drop_count = 0`は
**noncanonical 3秒smokeの記録**であり、canonical W3 verdictではない。W3 thresholdの判定には使わない。
`formal_lost_opportunity_count = 191`はopportunity ordinal側のgap accountingであり、本sliceでは解釈しない。

historical `COMPOSITION_TOKEN_MISMATCH`は本runで再現しなかった。これはroot causeがI3 boundaryやI5B
transitionであったことの証明ではない。historicalは`UNRESOLVED_HISTORICAL_RUNTIME_FAILURE`のまま保持し、
本runは`new corrected cohort / not reproduced`として別に記録する。

既知のdiagnostic gapが1件ある。`FIRST_RESERVATION` eventだけがtransition snapshotを取らずに記録されるため、
`transition_state`が既定値`OPEN`のまま出る (実際の状態は`CURRENT_RUNNING`)。ordering自体はevent serialとQPCで
確定しており、handshake判定には使っていない。r2 artifactを現行codeのexact checkpointとして残すため、本slice
では修正していない。


### 16.9 amendment 2 — canonical measurement start authorityをcurrent queue準備完了後へ移す

r2はhandshake / I0 / I1 runtime smokeとしてPASSしたが、exact orderingが

```text
QUIESCENCE_ACK -> canonical_start -> REQUIRED_QUEUE_STARTED -> MEASUREMENT_ARMED -> ISSUANCE_OPEN
```

となり、I4 frozen ordering
(`ACK -> current queue start -> measurement arm/window -> issuance`) と一致していなかった。canonical startを
current queue初期化より前にsampleしていたためである。r1 / r2は変更も無効化もしない。

amendment 2では`measurementArmQpc`のsample点を、`startCurrentRequiredQueue()`と
`startFormalOpportunityScheduler()`の成功後・`armMeasurement()`直前へ移した。canonical
`scheduler_.start` / `measurementStartQpc` / `measurementEndQpc` / `measurementStart.qpc` /
`armMeasurement()`はすべてこの同一sampleを使う。W4-C3のmeasurement-start arbitration snapshotも同じ点で撮る。
current queue initializationはissuance disabledのままcanonical windowの外で完了する。

architecture guardには次を追加した。

```text
Require  startCurrentRequiredQueue -> startFormalOpportunityScheduler -> measurementArmQpc sample -> armMeasurement
Reject   measurementArmQpc sample -> startCurrentRequiredQueue        (canonical startがqueue準備前)
Require  scheduler_.start(measurementArmQpc) が同一sampleを使う
mutation NegativeCanonicalStartBeforeCurrentQueueReady
```

runtime artifactにもclosureを追加した。controllerは
`ack <= queue_started <= canonical_start <= measurement_armed <= issuance_open`をQPCで検査し、
`canonical_start_order_exact`をhandshake verdictの必須条件へ組み込む。`quiescence_ack_qpc`、
`current_queue_start_event_qpc`、`measurement_armed_event_qpc`、`current_issuance_open_event_qpc`を
artifactへ出力する。

`FIRST_RESERVATION` eventのtransition snapshotも実際の値を採るよう修正した。diagnostic-onlyであり、
ordering authorityはevent serialとQPCのままである。

### 16.10 amendment 2 noncanonical smoke (exactly 1 run)

新checkpointで**1 runだけ**実行した。artifactは
`build/handshake-b3-i5b-a2-20260829T002803Z/formal-playback-smoke.json`である。

```text
QPC 1413788090142  MEASUREMENT_START_REQUEST_PUBLISHED  thread 35316 (GUI)
QPC 1413788256837  MEASUREMENT_START_CONSUMED           thread 33688 (render)
QPC 1413788256855  PREROLL_ADMISSION_CLOSED             -> DRAINING
QPC 1413788256996  PREROLL_DRAIN_OBSERVED               scheduler closed
QPC 1413788256998  PREROLL_QUIESCENCE_ACK               -> QUIESCENT
QPC 1413788257058  REQUIRED_QUEUE_STARTED               -> CURRENT_READY
QPC 1413788257058  canonical measurement start          (queue ready後の同一sample点)
QPC 1413788257065  MEASUREMENT_ARMED                    -> MEASUREMENT_ARMED
QPC 1413788257074  CURRENT_ISSUANCE_OPENED              -> CURRENT_RUNNING
QPC 1413788257116  FIRST_RESERVATION                    reservation 1 / intent 0 / CURRENT_RUNNING
```

```text
state / error                                CURRENT_RUNNING / NONE
handshake_step_order_exact                   true
canonical_start_order_exact                  true
canonical_start_after_current_queue_ready    true
quiescence 13 predicate                      all true (evaluation_count 1)
foreign_callback_after_quiescence_ack_count  0
current_callback_before_issuance_open_count  0
wait_charged_to_measurement_window           false
handshake_wait_qpc                           155 (約15.5µs / window外)
verdict                                      PREROLL_TRANSITION_QUIESCENCE_HANDSHAKE_OBSERVED

I0  missing/duplicate/stale token / receipt  0 / 0 / 0
    failed present / overflow / token set failure  0 / 0 / 0
    join failure captured                    false (phase NONE / predicate NONE)
    nearest-latest fallback used             false
    capture envelope authority_pass          true (env missing_token=2はW2-C0.1 superset契約どおり)

I1  required/issued/rendered/qualified/dequeued  180 / 180 / 180 / 180 / 180
    tail 0 / active 0 / conservation valid / required_set_immutable / planned_window_ended
    display_satisfaction_imported            false
    scope partition                          FOREIGN 2 / CURRENT 180、missing/ambiguous/mutation/unmatched 0

終端 stop cause PLANNED_WINDOW_END (witness 1 / duplicate 0)
    DOMAIN_TERMINAL successful completion    0
    source_coverage_ok true / fatal なし / exit 0 / teardown success
```

CURRENT intent ordinalは0..179 contiguousだったが、これは記録でありverdict authorityではない。
`effective_fps = 59.951` / `drop_rate = 0` / `formal_lost_opportunity_count = 191`も
noncanonical 3秒smokeの記録であり、canonical W3 verdictではない。

r1 (pre-fix VALID FAIL) と r2 (post-fix PASS / ordering不一致) はそのまま保存する。本runはamendment 2
checkpointの単発validationである。historical `COMPOSITION_TOKEN_MISMATCH`は本runでも再現せず、
`UNRESOLVED_HISTORICAL_RUNTIME_FAILURE`のまま保持する。

### 16.11 未実施 / 次slice

```text
ABI v6 patched Qt rebuild          DONE (§16.7)
noncanonical runtime smoke r1      VALID FAIL / CANONICAL_WINDOW_MUTATED (§16.7)
noncanonical runtime smoke r2      PASS / ordering不一致 (§16.8)
amendment 2 smoke                  PASS / I4 frozen ordering一致 (§16.10)
canonical W3                       NOT RUN / UNCHANGED / FAIL
P5-E4                              BLOCKED
```

r2 artifact auditはhandshake / I0 / I1 / terminationのschemaを個別に確認済みである。fresh W3 canonical 3/3は
別sliceとして計画する。ordinary CTestの既存failure 7件 (§16.6) は最終P5-E4 closureまでの技術的負債として
持ち越す。

historical `COMPOSITION_TOKEN_MISMATCH`は`UNRESOLVED_HISTORICAL_RUNTIME_FAILURE`のまま保持し、I3 boundary
failureにもI5B transition failureにも再分類しない。次はABI v6でpatched Qtを再buildし、noncanonical runtime
smokeでhandshakeの実runtime orderingを採取する。canonical W3はその後である。

## 17. fresh W3 canonical 3/3 試行 — acquisition stage FAIL

B3-I5B amendment 2 checkpoint (`45910320d7d8`、clean worktree) でfresh W3 canonical 3/3を試行した。
historical W3 contract / threshold / denominator / required population / FinalState authorityは一切変更していない。
frozen thresholdは`effective_fps >= 55` / `drop_rate <= 2%`のままである。

```powershell
pwsh scripts/acquire-p2-d5-2-w3-fresh.ps1 `
  -OutputDirectory build/p2-d5-2-w3-cohort-20260829 `
  -Runs 3 -WarmupSeconds 5 -MeasureSeconds 60 -TimeoutSeconds 240
```

**run 1がacquisition/protocol stageで失敗し、cohortは成立しなかった**。acquisition scriptはrun 1のapp
異常終了(exit 3)でthrowするため、run 2 / run 3は取得していない。replacement retryも行っていない。
canonical W3 verdictは`NOT_ACHIEVED_ACQUISITION_STAGE_FAILURE`であり、historical W3 verdictは**FAIL / UNCHANGED**のままである。

### 17.1 B3-I5B handshakeはこのrunでも成立している

```text
state / error                                CURRENT_RUNNING / NONE
handshake_step_order_exact                   true
canonical_start_order_exact                  true
quiescence 13 predicate                      all true
wait_charged_to_measurement_window           false
handshake_wait_qpc                           126
```

したがってrun 1の失敗はhandshake / admission close / drain / quiescenceのいずれでもない。

### 17.2 primary failure — `SOURCE_COVERAGE_INSUFFICIENT`

```text
required_frame_count      3600   (measure 60s x source 60fps)
source_a / source_b       3600 frames
refresh                   59950 / 1000  (59.95Hz)
source fps                60 / 1
failing intent ordinal    3597
```

`targetFor(ordinal) = ordinal * 60000 / 59950`はordinalより約0.083%速く進む。ordinal 3597で
source targetが3600に達し、3600 frameのsource domain外へ出るため、B3-I1の
`target >= requiredFrameCount`判定が`SOURCE_COVERAGE_INSUFFICIENT`でfail-closeした。

これはI5Bで新設した経路ではなく、I1時点から存在するsource coverage fail-closeである。59.95Hzの
60秒windowはrequired set 3600件を発行し切れるが、source domainは最後の約3 ordinal分を持たない。

required count / denominator / thresholdの調整でPASSへ寄せることはしない。sizingの是非は別途判断する。

### 17.3 secondary failure — fatal callbackのPresentがjoinへ到達する

surfaceしたshutdown reasonはprimaryではなくI0 joinのものだった。

```text
shutdown_reason   P2-D5-2 B3-I0 exact qualified commit失敗: RENDER_COMPLETION_MISSING
phase / predicate BIND_NATIVE_PRESENT / STATE_IS_RENDER_COMPLETED
active reservation  id 3597 / intent 3596 / token 3929  (既にcommit済み)
native record       present 3600 / token 3930 / intent_ordinal_valid=false / HRESULT 0
receipt             present 3600 / token 3930   (recordとexact一致)
scope ledger        token 3930のrecordなし (missing_scope_count=1)
```

source coverage fatalとなったcallbackは、`selectForRender`でqueue headをreserve済み(issued 3598)、
composition token 3930をmint済みだが、transport siteへ到達する前に`fail()`でreturnした。その
callbackのPresentはQt側で実行され、receiptが`frameSwapped`へ届き、joinは既にCommitted状態だったため
`RENDER_COMPLETION_MISSING`で停止した。

identity自体は壊れていない (record↔receiptのpresent/token/swapchain/HRESULTはexact一致、
`nearest_latest_fallback_used=false`)。問題は**fatal returnとPresentの間にtoken publicationが残る**
という順序であり、historical `COMPOSITION_TOKEN_MISMATCH`とは別のpredicateである。再分類はしない。

### 17.4 このrunの数値をcanonical判定に使わない

参考として記録するが、planned window endへ到達しておらず(`planned_window_ended=false`)、
fatalで終了しているため**canonical W3 metricではない**。

```text
                          historical W3 run-1 (20260826)   本run-1 (INVALID)
formal swapped                     1741                          3597
formal displayed unique            1741                          3596
formal true opportunity drop       1859                             2
drop_rate (app)                  0.516                             0
effective_fps                   59.916                        59.934
formal_opportunity_error          NONE          SOURCE_COVERAGE_INSUFFICIENT
```

この差をB3の成果として主張しない。canonical verdictは3/3 PASSのcohortからしか出さない。

### 17.5 sealed artifacts

```text
build/p2-d5-2-w3-cohort-20260829/run-1/                     raw artifacts (9 files)
build/p2-d5-2-w3-cohort-20260829/w3-partial-cohort-manifest.json
    checkpoint sha / primary+secondary attribution / SHA-256 manifest
```

`w3-acquisition-provenance.json`はacquisition scriptがrun 1 failureでthrowしたため生成されていない。
代替としてpartial cohort manifestへcheckpoint sha、planned/completed run数、replacement retry
無しの事実、両failureの識別子、SHA-256を固定した。

### 17.6 現在位置

```text
B3-I5B                         CLOSED / runtime smoke PASS
fresh W3 canonical 3/3         NOT ACHIEVED (run 1 acquisition failure)
canonical W3 verdict           UNCHANGED / FAIL
B3-I6A source mapping authority DESIGN CLOSED (§18)
B3-I6B fatal/token atomicity   CLOSED WITH DEFERRED INTEGRATION-NEGATIVE (§19、§20、§21)
B3-I6C mapping / preflight修正  DONE / positive smoke PASS (§22)
deferred I6B integration test  fresh W3より前に必ず閉じる (§21.1)
P3-C-2 / P4再検証               HOLD (W3 3/3 PASSが前提)
P5-E4                          BLOCKED
historical COMPOSITION_TOKEN_MISMATCH   UNRESOLVED_HISTORICAL_RUNTIME_FAILURE
```

## 18. B3-I6A — Source Mapping Authority (design only)

§17のW3 run 1が露出した2件は別sliceへ分ける。本節はそのうちmapping semanticsだけを扱う。
design本体は`docs/p2-d5-2-b3-i6a-source-mapping-authority.md`、機械可読契約は
`docs/p2-d5-2-b3-i6a-source-mapping-authority.json`である。

production codeは変更していない。mapping、source coverage preflight、queue、join、threshold、
denominator、required population、canonical source fixtureのいずれも触っていない。canonical W3はHOLD、
historical W3 verdictはFAILのままである。

### 18.1 確定した問題

required intent ordinalの時間軸authorityが2つ存在していた。

```text
required_intent_count   measureSeconds * 60        Layer 1A / workload軸
required intent ordinal RequiredIntentQueue head   Layer 1A / workload軸
targetFor(ordinal)      refresh比を適用            Layer 1B / display軸  ← 混在
source coverage preflight  source >= required count Layer 1A / workload軸
```

pre-B3では`opportunityOrdinal`がDWM refresh count由来のdisplay opportunity序数だったため、
`targetFor`のrefresh比は正しかった。B3-I1でordinal producerをqueue head identityへ移した際に
`targetFor`が据え置かれ、意味の異なるordinalへdisplay rateが適用され続けている。

preflightも`required count`としか比較しておらず、mappingが実際に要求する最大source frame
(`max target = 3602`) を検査していないため、preflightは通り runtimeだけが末尾でfail-closeした。

### 18.2 選定 — `WORKLOAD_INTENT_TIME_AXIS`

```text
target(i) = sourceFrameOffset + floor(i * sourceFps / requiredIntentRate)
canonical: target(i) = i、max target = 3599、source fixture 3600 frameで充足
display refresh非依存
```

根拠は既に凍結済みの契約である。

1. W1 formal accounting contract v2 §1が`intent_ordinal`をLayer 1A、`physical_vblank_ordinal`を
   Layer 1Bとし、「1Aと1Bを同一視しない」と無修飾`ordinal`の禁止を凍結している
2. 同§1が`required_intent_count`を「test contract由来の分母」と定義している
3. B3-I1でordinal producerがrequired-intent queue headになった
4. display軸を採るとcanonical input assetのsizingが測定機のrefreshに依存し再現しない
   (59.95Hzなら3603、60.000Hzなら3600)

したがって**source fixtureを3603へ延長することは修正ではなく症状の隠蔽**であり、禁止解法として
固定した。required set 3600、drop-rate分母、frozen threshold 55fps/2%はいずれも変更しない。

### 18.3 凍結invariantと禁止解法

```text
frozen    REQUIRED_INTENT_ORDINAL_IS_LAYER_1A
          TARGET_MAPPING_INDEPENDENT_OF_DISPLAY_REFRESH
          INTENT_RATE_HAS_SINGLE_PRODUCER
          SOURCE_COVERAGE_PREFLIGHT_USES_MAX_TARGET_OVER_REQUIRED_SET
          MAX_TARGET_OVER_REQUIRED_SET_LESS_THAN_SOURCE_FRAME_COUNT
          REQUIRED_SET_SIZE_UNCHANGED / DROP_RATE_DENOMINATOR_UNCHANGED
          SOURCE_COVERAGE_FAILURE_IS_PROTOCOL_FATAL_NOT_PERFORMANCE_DROP

prohibited EXTEND_SOURCE_FIXTURE_TO_HIDE_MAPPING_DEFECT / SHRINK_REQUIRED_SET
          CHANGE_FROZEN_THRESHOLD / CHANGE_DROP_RATE_DENOMINATOR / SKIP_TAIL_INTENTS
          CLAMP_TARGET_TO_LAST_SOURCE_FRAME
          TREAT_SOURCE_COVERAGE_FATAL_AS_PERFORMANCE_DROP
          DERIVE_INTENT_ORDINAL_FROM_PHYSICAL_VBLANK_ORDINAL
```

### 18.4 targeted test

```text
p2_b3_i6a_source_mapping_design_*   Good + negative 14
```

Goodは設計契約に加えて、現行sourceのinventory (targetForのrefresh比、`target >= requiredFrameCount`判定、
controllerの`measureSeconds * 60`、preflightの比較対象、queueのrequired set producer) が
design記述と一致することを固定する。negativeはdisplay軸選定、refresh依存mappingの許可、
source fixture延長の許可、required set縮小、threshold/分母変更、preflight invariant削除、
target clamp許可、coverage fatalのperformance drop化、intent ordinalのphysical VBlank由来化、
intent rate single producer削除、historical mismatch再分類を個別に拒否する。

### 18.5 acquisition runner robustness (product fixではない)

`scripts/acquire-p2-d5-2-w3-fresh.ps1`が失敗時にもprovenanceを封止するようにした。live gateの
失敗をcatchし、`w3-acquisition-partial-provenance.json`へcheckpoint sha、binary hash、
planned/completed run数、replacement retry無しの事実、`run_metrics_are_canonical=false`、
raw artifactのSHA-256を書いてからfail-closeする。成功時のprovenanceとはfile名もschemaも分けており、
失敗cohortをcanonicalと誤認させない。parseとPSScriptAnalyzerで検証済みで、実際のfailure pathは
次のcanonical attemptで初めて実行される。

### 18.6 未実施 / 次slice

```text
B3-I6B  fatal-before-Present publication atomicity   NOT STARTED (§17.3)
B3-I6C  target mapping / coverage preflight修正       NOT STARTED
fresh W3 canonical 3/3                               HOLD
canonical W3 verdict                                 UNCHANGED / FAIL
P5-E4                                                BLOCKED
```

I6Cはselected semanticsに従いmappingとpreflightを修正する。source fixtureは変更しない。
I6BはI6Aのsemanticsに依存しない独立のcorrectness sliceであり、`SOURCE_COVERAGE_INSUFFICIENT`を
再現可能なnegative test vectorとして使えるうちに先に実施する。

historical `COMPOSITION_TOKEN_MISMATCH`は`UNRESOLVED_HISTORICAL_RUNTIME_FAILURE`のまま保持し、
I6 mapping failureへ再分類しない。

## 19. B3-I6B — Fatal-before-Present Publication Atomicity

§17.3で露出したlatent correctness bugを、I6A mapping semanticsとは独立に塞ぐsliceである。
I6A selected semantics、required set、source fixture、threshold、denominatorは変更していない。
I6Cはまだ行っていない。canonical W3はI6B + I6C closureまでHOLDである。

### 19.1 exact negative baseline

W3 run 1が記録したchainをそのままbaselineにする。

```text
queue reserve (issued 3598)
-> composition token 3930 publication
-> SOURCE_COVERAGE_INSUFFICIENT (primary)
-> callback return
-> actual Present
-> frameSwapped receipt
-> formal join (Committed状態)
-> RENDER_COMPLETION_MISSING (secondary)
-> shutdown reasonがsecondaryで上書きされる
```

### 19.2 変更

**composition token publicationのgate**。`NativePresentTokenCapture`に
`publicationAllowed()`を追加し、destructorはこのgateを通過した場合だけ
`setCompositionToken`を呼ぶ。gateは`active_ && valid_ && !fatal`であり、callback-localの
fail-able pre-Present validation (source mapping / source coverageを含む) が1つでも失敗して
fatalが立った場合はpublicationを行わない。publication siteはdestructorのexactly 1箇所である。

抑止時はQt pending formal tokenを残さず、その後のPresentはformal tokenを持たないため
formal join candidateにならない。抑止は`nativePresentTokenSuppressedBeforePresentCount`へ
記録し、**transport failure counterへは混ぜない**。Present自体の抑止は前提にしていない。
nearest/latest/QPC/present-serial推定によるtoken cancelやtransaction recoveryは実装せず、
guardで拒否する。

**post-fatal Presentのjoin除外**。`recordFrameSwapped`は先頭で
`protocolFatalLatched`を観測し、first protocol fatal後は`formalEnvelopeActive`をfalseにして
receipt take / bind / commitのいずれにも入らない。post-fatal callbackはboundary attribution
eventとしてだけ記録する。

**first protocol fatalのimmutable化**。join failureとrender↔swap authority failureの
`fatalReason` writerを`fail()`と同じfirst-writer-winsにした。post-fatal diagnosticsが
primary fatalを上書きしない。

**queue historyのrollback禁止**。failed reservationはdequeueせず、active/tail/conservationを
保持する既存契約 (B3-I1) をそのまま維持し、post-fatal操作でも前進しないことを新規testで固定した。

### 19.3 targeted test

```text
p2_presentation_opportunity_scheduler
    postSourceCoverageFatalDoesNotMutateTransaction を追加
p2_b3_i6b_fatal_publication_atomicity_architecture
p2_b3_i6b_fatal_publication_atomicity_guard_*   Good + negative 12
```

unit positiveは、source coverage fatal後にcallback / render完了 / qualified evidence / swapを
すべて拒否し、issued / rendered / qualified / dequeued / active / tail / conservationが
fatal時点から1つも変化せず、first protocol fatal (`SOURCE_COVERAGE_INSUFFICIENT`) も
上書きされないことを固定する。

guard mutationはtoken-before-validation、fatal後のpending token残存、抑止のtransport failure化、
publication siteの二重化、fatal後のformal receipt生成、post-fatal joinへの到達、
primary fatalのjoin/swap側からの上書き、queue rollback、source coverage fail-closeの除去、
抑止counterの隠蔽、nearest token recoveryを個別に拒否する。

ordinary CTestは既存failure 6件のみで、それ以外はgreenである。

### 19.4 noncanonical negative smoke — negative vectorは再現しなかった

既存configで**exactly 1 run**取得した
(`build/i6b-negative-smoke-20260829T011354Z/negative-source-coverage-smoke.json`、
warmup 5s / measure 60s / hook on / formal preflight、ETWなし)。

期待していた`SOURCE_COVERAGE_INSUFFICIENT`は**この runでは発生しなかった**。

```text
issued / rendered / qualified / dequeued   3596 / 3596 / 3596 / 3596
unissued tail                             4
最大intent ordinal                         3595  (target 3597 < 3600)
fatal ordinal                             3597  (target 3600) には未到達
stop cause                                PLANNED_WINDOW_END
formal_opportunity_error                  NONE
process exit                              0
```

W3 run 1は60秒windowへ3598件のissuanceが入りordinal 3597へ到達したが、本runは3596件で
planned window endに達した。source coverage fatalは**window終端とordinal 3597到達の競合**であり、
この2件差の範囲に入っている。したがってI6Bのruntime negative validationは本runでは成立していない。
retryして再現runを選別することはしない。

同runはI6Bがsuccess pathに対して**behavior-neutral**であることは示している。

```text
token publication suppressed count        0
token set failure                         0
missing / duplicate / stale token         0 / 0 / 0
missing / duplicate / stale receipt       0 / 0 / 0
native present authority_pass             true
capture envelope authority_pass           true
handshake                                 CURRENT_RUNNING / canonical_start_order_exact
queue conservation                        valid / planned_window_ended
```

`effective_fps 59.915` / `drop_rate 0.111%` / `true drop 4`はnoncanonical 1 runの記録であり、
canonical W3 metricではない。

### 19.5 現在位置

```text
B3-I6B implementation                 DONE
B3-I6B targeted gate                  GREEN
B3-I6B runtime negative validation    NOT ESTABLISHED (negative vector未再現)
B3-I6C                                NOT STARTED
canonical W3                          HOLD
canonical W3 verdict                  UNCHANGED / FAIL
historical COMPOSITION_TOKEN_MISMATCH UNRESOLVED_HISTORICAL_RUNTIME_FAILURE
```

runtime negative validationを成立させるには、window終端との競合に依存しない決定的な
negative vectorが要る。それはcanonical inputかconfigのどちらかを触ることになるため、
I6Bの範囲では選択しない。

## 20. B3-I6B-V — Deterministic Fatal Publication Runtime Vector (設計失敗 / 1 run)

diagnostic-only のnoncanonical vectorを設計し、**exactly 1 run**実施した。production I6B semantics、
I6A selected mapping semantics、canonical fixture、canonical required set、threshold、denominatorは
一切変更していない。期待した`SOURCE_COVERAGE_INSUFFICIENT`は発生せず、**vector設計が不成立**である。
retryは行っていない。

artifact: `build/i6b-v-deterministic-vector-20260829T012213Z/`
runner: `scripts/p2-d5-2-b3-i6b-v-negative-vector.ps1` (diagnostic-only)

### 20.1 設計した vector と結果

legacy mappingでfatalがplanned window endより前に来る条件は、後述のとおり
**issuance rate > display refresh rate**だけである。vsync待ちを外してissuance rateを上げる
workload cadenceを選び、authorityは何も偽造しない構成にした。

結果はapp側のproduct guardによる拒否だった。

```text
app exit                 6
stderr                   formal presentation pathではQSG_NO_VSYNCを使用できません
producer                 apps/compositor_spike/main.cpp:116-118
measurement              未開始 (traced-app.json未生成)
```

これはproductのfail-closedが正しく働いた結果である。guardを外して測ることはしない
(formal presentation pathのworkload契約を壊すため)。

### 20.2 なぜ config だけでは margin を作れないか

現行の3つの固定値から、marginは構造的に作れない。

```text
required        = measureSeconds * 60                     controller:462
scheduler config sourceFps = 60/1 (hardcoded)             startFormalOpportunityScheduler
target(i)       = floor(i * 60 * refreshDen / refreshNum) targetFor
fatal condition = target(i) >= required
```

refresh rate を R、window を T とすると

```text
fatal ordinal      i_f = ceil(T * R)
window内issuance   <= R * T           (vsync下では1 Present = 1 issuance が上限)
```

`i_f`と上限issuance数がどちらも`R*T`であり、**Rを上げても下げてもTを変えても打ち消し合う**。
したがってvsyncが必須である限り、fatalはrequired setの末尾1〜3件目にしか現れず、
「全window中refresh cadenceの100%を維持できたか」というcoin flipになる。

実測もこれと一致する。

```text
W3 run 1 (T=60)                3598 issuance -> ordinal 3597 到達 -> fatal
I6B smoke (T=60, §19.4)        3596 issuance -> ordinal 3595 止まり -> fatal無し
```

### 20.3 margin を作るには production surface が要る

次のいずれかが必要であり、いずれも「production semanticsを変更しない」という本sliceの制約に
反するため選択しない。

```text
(a) test専用の intent rate authority
    I6Aの INTENT_RATE_HAS_SINGLE_PRODUCER を導入する際に、rateをdiagnostic runで
    差し替え可能にする。required = r * T と mapping の 60/R が分離されmarginが生まれる。
(b) scheduler configのsourceFps (現在hardcoded 60) をdiagnostic-onlyで上書きする経路
(c) I6C後にlegacy mappingが消えるため、runtime vectorではなくinjection/unit levelで
    fatal-before-Present orderingを固定する
```

I6C後は`SOURCE_COVERAGE_INSUFFICIENT`自体が正常経路から到達不能になるため、(c)が最も筋が良い。

### 20.4 同 run で実検証できたもの — acquisition failure path sealing

vectorはPASSしなかったが、**failure path provenance sealingは実runで検証できた**。
`acquire-p2-d5-2-w3-fresh.ps1`へ`-LiveRunner`としてvector runnerを渡し、live gate失敗を
経由させた。

```text
w3-acquisition-partial-provenance.json   生成された
acquisition_gate                         FAILED
planned_runs / completed_runs            1 / 0
replacement_retry_performed              false
run_metrics_are_canonical                false
canonical_w3_verdict                     NOT_ACHIEVED_ACQUISITION_STAGE_FAILURE
checkpoint_sha / binary 4点のSHA-256      記録された
files                                    run-1/app-stderr.txt, run-1/app-stdout.txt
                                         (path正規化とSHA-256が正しく出力された)
```

この実行で§18.5実装の2件の欠陥が実証的に潰れた。path正規化の`Replace`引数が空文字列で
実行時例外になっていた件と、completed run数をartifactの存在で数えていた件である。後者は
`process_exit_code == 0`だけをcompletedとする形へ直し、本runで`completed_runs = 0`を確認した。

既知のcaveatが1件ある。sealerはW3 acquisition専用のため、`-LiveRunner`にdiagnostic runnerを
与えた本runでも`acquisition_mode`は`CanonicalPresentMonLive`と記録される。この1件は
canonical W3 cohortではない (directory名とrun_metrics_are_canonical=falseで区別できる)。
sealerへ実際のlive runnerを記録するfieldを足すのは次sliceで行う。**本runで検証済みのsealerを
後から書き換えないため、本sliceでは変更していない。**

### 20.5 現在位置

```text
B3-I6B implementation                 DONE / targeted gate GREEN
B3-I6B runtime negative validation    NOT ESTABLISHED (vector構成不能 / §20.2)
B3-I6B runtime closure                NOT CLOSED
acquisition failure sealing           VERIFIED (§20.4)
B3-I6C                                NOT STARTED
canonical W3                          HOLD / verdict UNCHANGED / FAIL
historical COMPOSITION_TOKEN_MISMATCH UNRESOLVED_HISTORICAL_RUNTIME_FAILURE
```

I6B runtime closureの成立には production surface の追加が要るため、その可否判断まで
`NOT CLOSED`のまま保持する。

## 21. B3-I6B status の二層固定と runner sealing verdict

I6B-V (§20) の結論に従い、I6Bのstatusを二層で固定する。runtime negativeが無いことを理由に
implementationをopenへ戻さない一方、runtime failure pathまでexactに証明済みとも言わない。

```text
B3-I6B production implementation        CLOSED
B3-I6B targeted correctness gate        CLOSED / GREEN
B3-I6B success-path runtime neutrality  OBSERVED (§19.4)
B3-I6B natural runtime negative         NOT ESTABLISHED
B3-I6B deterministic natural vector     UNAVAILABLE UNDER VALID FORMAL CONFIG (§20.2)
B3-I6B overall product-fix status       CLOSED WITH DEFERRED INTEGRATION-NEGATIVE EVIDENCE

acquisition failure-path sealing        VERIFIED (§20.4)
```

production surfaceへdiagnostic-onlyのintent rate / source fps overrideを足してruntime negativeを
作る案は採らない。I6Aで閉じた`INTENT_RATE_HAS_SINGLE_PRODUCER`の実装前に別のrate authorityを
足すことになり、目的と手段が逆になるためである。

### 21.1 deferred integration-negative の凍結条件

置き換えとなるclosure testは**fresh W3 canonical 3/3より前に必ず閉じる**。scheduler unitの延長では
不足であり、renderer/Qt境界のpublication lifecycleを実際に通すintegration-level testとする。

```text
1. NativePresentTokenCapture を active/valid にする
2. token publication前に protocol fatal を注入する
3. capture objectのdestructorを実際に通す
4. setCompositionToken call count == 0
5. suppressed_before_present_count == 1
6. post-fatal frameSwapped相当を送る
7. receipt take / bind / commit / join に入らない
8. fatalReasonは注入前のfirst fatalのまま
9. queueはdequeue / rollbackしない
```

injection authorityは**publication直前にfatal latchを立てることだけ**に限定する。fake token serial、
fake present serial、fake QPC、fake VBlank ordinal、nearest/latest reconstruction、production config
overrideはいずれも禁止する。`SOURCE_COVERAGE_INSUFFICIENT`の再現は不要であり、証明する不変量は
一般形の「token publication前に生じたcallback-local protocol fatalは、そのtransactionをformal Present
transactionにしない」である。

`acquisition_mode`がdiagnostic runnerでも`CanonicalPresentMonLive`と記録される件は provenance
metadata gapとして残す。`requested_live_runner` / `resolved_live_runner`の追加は次sliceで行い、
§20.4でsealed済みのartifactは書き換えない。

## 22. B3-I6C — Source Mapping / Coverage Preflight Correction

I6A selected semantics (`WORKLOAD_INTENT_TIME_AXIS`) を実装した。required set、canonical source
fixture、W2-A denominator、threshold、FinalState authority、queue semantics、join semanticsは
変更していない。

### 22.1 rate authority の single producer 化

```text
gpu::kFormalRequiredIntentRate  {60, 1}   required countとtarget mappingの唯一のrate authority
gpu::kFormalSourceFrameRate     {60, 1}   canonical workloadのsource frame rate
gpu::formalRequiredIntentCountForSeconds(seconds)   required set sizeのsingle producer
```

controllerの`measureSeconds * 60`リテラルと、renderer configのhardcoded `60`を両方この authority へ
置き換えた。`PresentationOpportunityConfig`へ`requiredIntentRateNumerator/Denominator`を追加し、
未設定 (0) は`start()`が`INVALID_CONFIGURATION`でfail-closeする。既存configが黙って別の意味へ
ずれないよう、fieldは末尾に追加してdefaultを0にした。

「single producer」は世界中で60固定という意味ではなく、**各formal workload configurationが参照する
required-intent rate authorityが1つだけ**という意味である。

### 22.2 mapping

```text
target(i) = sourceFrameOffset
          + floor(i * sourceFps / requiredIntentRate)
```

実装は`presentationTargetFrameFor()`のexactly 1箇所であり、
`PresentationOpportunityScheduler::targetFor()`はそこへ委譲するだけである (DRY)。
display refresh (`refreshNumerator/Denominator`) はauthority検査専用となり、mappingからは消えた。

canonicalでは

```text
requiredIntentRate = 60/1、sourceFps = 60/1
target(i) = i、required set = 0..3599、max target = 3599、source = 3600 frames
```

### 22.3 source coverage preflight

count比較を廃止し、required set全域のexact target rangeで判定する。

```text
requiredIntentSourceCoverage(config)
    required set [0, requiredCount) を全走査し min/max target と monotonicity を出す
requiredIntentSourceCoverageSatisfied(coverage, sourceFrameCount)
    coverage.valid && 0 <= minTarget && maxTarget < sourceFrameCount
```

単調性は別invariantとして証明せず、同じ走査で実測する。artifactへ
`required_intent_source_coverage` (rate、min/max target、monotonicity、
`target_mapping_uses_display_refresh=false`、`count_comparison_used=false`) を出力する。

### 22.4 targeted test

```text
p2_presentation_opportunity_scheduler
    targetMappingIsDisplayRefreshIndependent
    prerollTargetStaysOnRepeatedFrame            positive + negative
    requiredIntentSourceCoverageIsExact
    既存accountingをworkload rate軸へ更新 (intent rate 30 / 120)
p2_b3_i6c_source_mapping_correction_architecture
p2_b3_i6c_source_mapping_correction_guard_*      Good + negative 12
```

`targetMappingIsDisplayRefreshIndependent`は60 / 59.95 / 120 / 30 Hzで同一target列になることを、
実装式とは独立に`target(i) = i`を期待値として固定する。`prerollTargetStaysOnRepeatedFrame`は
preroll rangeでtargetが`repeatedFrame`のままであること (positive) と、intent rateをsource rateと
同じにすると repeat が壊れること (negative) を固定し、repeatを保っているのがrate authorityであることを示す。
`requiredIntentSourceCoverageIsExact`はcanonical (max 3599 / 3600 frameでcoverage成立、3599では不成立) と
legacy display軸 (max 3602 / 3600 frameで不成立) を対比し、count比較では両者を区別できないことも固定する。

guard mutationはmappingのrefresh依存化、targetForでの式複製、rate validation削除、第2のrate producer、
count比較preflightへの復帰、coverageのlast targetのみ評価、monotonicity未検査、
`maxTarget <= sourceFrameCount`への緩和、minTarget無視、rendererのhardcoded rate、
artifactのrefresh mapping主張、count比較主張を個別に拒否する。

ordinary CTestは既存failure 6件のみである。

### 22.5 noncanonical positive smoke (1 run)

`build/i6c-positive-smoke-20260829T013919Z/positive-smoke.json`

```text
coverage      rate 60/60 / display非依存 / valid / monotonic
              required 3600 / min 0 / max 3599 / source 3600 / count比較不使用
queue         issued=rendered=qualified=dequeued=3598 / tail 2 / active 0
              conservation valid / planned_window_ended / error NONE
終端           PLANNED_WINDOW_END / exit 0 / formal_opportunity_error NONE
handshake     CURRENT_RUNNING / canonical_start_order_exact
transport     token・receiptのmissing/duplicate/stale 0 / failed present 0 / authority_pass
publication   suppressed_before_present_count 0
current 3598件  target == intent ordinal がすべて成立
preroll 2件     target == repeatedFrame (301) がすべて成立
```

§17のW3 run 1はordinal 3597で`SOURCE_COVERAGE_INSUFFICIENT`へ落ちたが、本runは同じ3598 issuanceを
fatalなくplanned window endまで完走した。live artifactでもtarget == ordinalとpreroll repeatを確認できた。

`effective_fps 59.951` / `drop_rate 0.056%` / `true drop 2`は**noncanonical 1 runの記録であり
canonical W3 metricではない**。

### 22.6 現在位置

```text
I6A source mapping semantics            CLOSED
I6B publication atomicity implementation CLOSED WITH DEFERRED INTEGRATION-NEGATIVE EVIDENCE
I6B-V deterministic natural vector      CLOSED / IMPOSSIBLE UNDER VALID CONFIG
runner failure-path sealing             VERIFIED
I6C mapping + preflight correction      DONE / targeted green / positive smoke PASS

next  deferred I6B integration injection test   (fresh W3より前に必ず閉じる)
      fresh W3 canonical 3/3                    HOLD
canonical W3 verdict                    UNCHANGED / FAIL
historical COMPOSITION_TOKEN_MISMATCH   UNRESOLVED_HISTORICAL_RUNTIME_FAILURE
```
