# P2-D5-2 B1 — Target-output Physical Authority Attribution Design

- 状態: **AMENDED DESIGN FROZEN / EXACT PHYSICAL JOIN UNAVAILABLE / CAPTURE NOT STARTED**
- 前提: W4-A / W4-B / W4-C1 / W4-C2 / W4-C3 CLOSED、B0 design CLOSED
- 目的: target-output physical VBlank authorityとcurrent NULL DWM authorityを同一fresh captureで
  exact比較し、W4-C3の`delta intentOrdinal = +2/+3`と約29秒のpremature terminalへの帰属を判定する
- 非目的: production scheduler変更、performance PASS、threshold変更、required set変更、
  `DwmGetCompositionTimingInfo(target HWND)`の再候補化

## 1. Static authority inventory

### 1.1 target output binding

B1はB0のidentity contractをそのまま再利用し、別のresolverやfallbackを作らない。

```text
target HWND
  -> IsWindow
  -> MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) -> HMONITOR
  -> GetMonitorInfoW(MONITORINFOEXW::szDevice)
  -> DXGI EnumAdapters1 / EnumOutputs
  -> exact DXGI_OUTPUT_DESC::Monitor == HMONITOR
  -> IDXGIOutput

MONITORINFOEXW::szDevice
  -> QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)
  -> DISPLAYCONFIG_SOURCE_DEVICE_NAME::viewGdiDeviceName exact equality
  -> source adapter/id + target adapter/id + target refresh rational
```

identity equalityはB0の全field、すなわちHMONITOR、adapter LUID、DXGI output index、両device
name、desktop rect、DisplayConfig source/target adapter/id、refresh numerator/denominatorのexact equalityで
決める。`MONITOR_DEFAULTTONEAREST`、primary fallback、device nameだけのjoinは認めない。

### 1.2 physical VBlank observer

既存`WindowOutputVBlankObserver`は解決済み`IDXGIOutput`を保持し、専用threadを
`THREAD_PRIORITY_TIME_CRITICAL`へ昇格して`IDXGIOutput::WaitForVBlank()`を呼ぶ。成功ごとに
`{ordinal,qpc}`をfixed ringへsingle-writer publishする。render threadで`WaitForVBlank()`を呼ばない。

```text
observer thread: WaitForVBlank -> {ordinal, qpc} -> fixed ring
render thread:   invocation serial / callback QPC / product inputをPOD ringへ記録
checker:         observer停止・successor取得後にoffline exact join
```

`VBlankRing::publishSerial`はresetでも戻らない。sequenceはobserverがpublishしたsampleについてordinal
strictly consecutiveかつQPC strictly increasingを要求する。

ただしsample QPCの命令位置と意味は次のとおりである。

source位置は`src/media/gpu_preview/window_output_vblank_observer.cpp`の
`WindowOutputVBlankObserver::run()`、QPC wrapperは
`src/media/gpu_preview/qpc_clock.h::qpcTicks()`である。

```cpp
if (FAILED(output->WaitForVBlank())) {
    // failure処理
}
ring_.capture({ordinal, qpcTicks()});
```

`qpcTicks()`は`QueryPerformanceCounter()`をその場で呼ぶ。実際の順序は次である。

```text
target IDXGIOutput::WaitForVBlank()が成功return
  -> observer threadが次のC++ statementを実行
  -> QueryPerformanceCounter()
  -> returned QPCとself ordinalをringへpublish
```

Windows API contractが保証するのは「次のvertical blankが発生するまでthreadを停止する」ことまでであり、
return時刻、thread再開遅延、VBlank boundaryのQPC、returnとboundaryの同時性は保証しない。
`QueryPerformanceCounter()`が返すのは呼出し時点のcurrent performance-counter valueである。ゆえにsample QPCは
**VBlank発生時刻ではなく、WaitForVBlank return後にobserver threadが実行再開してQPCを読めた時刻**である。

このQPCにはreturn pathとthread schedulingの非負かつ上限不明な遅延が入る。observerが遅延中にrender callbackが
走り得るため、隣接するsample QPCの半開区間はphysical VBlank境界間の区間と同値ではない。またself ordinalは
成功したwaitの回数であり、遅延時に物理VBlankを取りこぼしたことをcounter自身からは識別できない。

したがってB1初版の次の主張を**撤回**する。

```text
V_k.qpc <= callback_qpc < V_(k+1).qpc
  -> callback時点のcompleted target physical VBlank ordinal = V_k.ordinal
```

このbracketはobserver wake-QPC intervalへの分類にすぎず、physical VBlank boundaryのexact authorityではない。
nearest-QPC、nominal cadence、midpoint、interval tolerance、QPC interpolationでphysical boundaryを推定しない。

既存のinterval/cadence reportはobserver健全性の診断としてraw保存してよいが、B1 delta一致、join、
replayの救済には使わない。B1はcadence toleranceを新設・緩和せず、gapやmiss疑いをVALIDへ変換しない。

API contractの一次資料:

- [IDXGIOutput::WaitForVBlank](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank)
- [QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)
- [IDXGIOutput::GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getframestatistics)
- [DXGI_FRAME_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics)

`DXGI_FRAME_STATISTICS.SyncRefreshCount/SyncQPCTime`はpaired timing情報を持つが、
`IDXGIOutput::GetFrameStatistics`はAPI contract上full-screen時だけsupportedである。current windowed formal runの
代替authorityとしてB1へ採用しない。`IDXGISwapChain::GetFrameStatistics`もmulti-monitor等でstatisticsが
unreliableと明記されており、別のsupport/provenance設計なしにfallbackしない。

この候補inventoryは[B2 Supported Exact Target-output Counter Authority](p2-d5-2-b2-supported-exact-target-output-counter-authority.md)
で完了した。4候補はすべてstatic rejectionとなり、verdictは
`EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE`である。

### 1.3 current scheduler authority

current product inputは次のmixed authorityである。

```text
DwmGetCompositionTimingInfo(NULL).cRefresh/qpcVBlank
  + target output DisplayConfig refresh rational
  -> completed refresh
  -> first render ? 0 : completed + 1
  -> actual intent ordinal
  -> target
  -> past_source_domain / required_intent_membership
```

W4-C3はactual pathについて`delta intent ordinal == delta completed NULL cRefresh`、terminal invocationの
predicate、`OUTSIDE_SOURCE_DOMAIN_DECISION -> DOMAIN_TERMINAL -> gate close`を3/3 exactに閉じた。
physical ordinalを代入したcounterfactualはまだ証明していないため、B1 shadowで初めて検査する。

## 2. Diagnostic-only acquisition schema

schemaは`mvm-p2-d5-2-b1-target-output-physical-authority-1`とする。productの
`PresentationAuthoritySample`、scheduler入力/decision、stop arbitration、measurement windowを変更しない。

```json
{
  "schema": "mvm-p2-d5-2-b1-target-output-physical-authority-1",
  "stage": "P2-D5-2-B1",
  "diagnostic_only": true,
  "production_authority_changed": false,
  "production_behavior_changed": false,
  "canonical_performance_authority": false,
  "checkpoint_sha": "...",
  "binary_sha256": {},
  "qpc_frequency": 0,
  "config": {
    "measurement_start_qpc": 0,
    "measurement_end_qpc_exclusive": 0,
    "source_frame_offset": 0,
    "source_fps_numerator": 60,
    "source_fps_denominator": 1,
    "refresh_numerator": 59950,
    "refresh_denominator": 1000,
    "required_intent_count": 3600,
    "source_frame_count_a": 0,
    "source_frame_count_b": 0,
    "qualified_source_frame_count": 0
  },
  "output_identity_start": {},
  "output_identity_end": {},
  "observer_diagnostic": {},
  "exact_target_physical_authority": null,
  "physical_vblank_samples": [],
  "scheduler_invocations": [],
  "formal_present_records": [],
  "terminal_witness": {},
  "summary": {}
}
```

`summary`は非authorityであり、checkerはrawから再計算する。capture/checker/schema SHAをmanifestへbindし、
別runやW4-C3 historical rawをspliceしない。

### 2.1 observer diagnostic objectとraw sequence

```json
{
  "thread_kind": "DEDICATED_OBSERVER",
  "render_thread_wait_for_vblank_count": 0,
  "priority_state": "TIME_CRITICAL",
  "publish_serial_before_start": "0",
  "publish_serial_after_preroll": "0",
  "prestart_preroll_completed": true,
  "prestart_sample_ordinal": 0,
  "prestart_sample_qpc": 0,
  "successor_completed": true,
  "successor_ordinal": 0,
  "successor_qpc": 0,
  "wait_failure_count": 0,
  "ring_overflow_count": 0,
  "sequence_status": "OK",
  "qpc_semantics": "POST_WAIT_RETURN_THREAD_RESUME_QPC",
  "physical_boundary_timestamp_authority": false,
  "callback_physical_join_authority": false
}
```

各`physical_vblank_samples`は`{sample_index,publish_serial,observer_wait_ordinal,post_return_qpc}`を持つ。
`sample_index`と`publish_serial`をordinalの代用にしない。capture終了後、successorまでpublish済みのringを
sealしてからsnapshotし、snapshot中にpublish範囲が変わった、必要範囲がoverwrite/overflowした、または
同じpublish serialから異なるsampleが読めた場合は`PUBLICATION_AMBIGUITY`とする。

このrawはtarget outputへのbinding、wait return sequence、counter advancementのdiagnosticには使えるが、
callbackへcompleted physical ordinalを付与するauthorityには使わない。

### 2.2 scheduler invocation record

W4-C2/C3と同じ`scheduler_invocation_serial`をprimary keyにする。

```json
{
  "scheduler_invocation_serial": 0,
  "render_ordinal": 0,
  "callback_begin_qpc": 0,
  "callback_end_qpc": 0,
  "null_dwm": {
    "hresult": "0x00000000",
    "c_refresh": 0,
    "qpc_vblank": 0
  },
  "actual_input": {
    "origin_completed_refresh": 0,
    "completed_refresh": 0
  },
  "actual_decision": {
    "intent_ordinal": 0,
    "target_frame": 0,
    "required_intent_membership": true,
    "past_source_domain": false,
    "result": "PRIMARY_DECISION",
    "reason": "PRIMARY"
  },
  "composition_token_serial": "0",
  "native_present_serial": "0",
  "physical_join": null,
  "physical_join_status": "EXACT_AUTHORITY_UNAVAILABLE"
}
```

NULL DWM sampleはproductが実際に読んだ同一sampleを複製せず参照する。diagnostic用に別callを挿入して
sample時点を動かさない。actual intent/decisionもproduct recordそのものへserial joinする。

### 2.3 exact join graph amendment

joinは次だけを許す。

```text
scheduler_invocation_serial
  -> W4-C2 invocation input / actual decision
  -> W4-C3 terminal witness（terminal invocationだけ）

composition_token_serial
  -> native Present embedded token serial
  -> exact PresentEvent identity
  -> FinalState / DisplayedQPC
```

このformal chainはPresentのdisplay outcomeをexactに追うが、pre-render callback時点で完了済みだったtarget
physical VBlank counterを与えない。observer post-return QPCを足してもその欠落は埋まらない。

B1のexact physical joinを成立させる将来authorityは、少なくとも次をAPI/runtime contractとして満たす必要がある。

```text
target output identityへexact bindされる
pre-render sample pointでcompleted target refresh countを直接返す
first post-swap anchor sample pointで同じcounter domainを直接返す
counterとsample causal pointの関係がAPI contractで定義される
render threadをblockしない
migration / unsupported / regression / wrapをfail-closeできる
```

別threadのlatest publicationは「そのwaitがreturn済み」というlower boundしか示さず、sample point時点のlatest
completed refreshを示さないため、この契約を満たさない。

### 2.4 current anchor establishment inventory

current schedulerは`recordFrameSwapped()`で先に`swapQpc = qpcTicks()`を取得し、続いて
`capturePresentationAuthority(state_)`を評価してから`commitSwap()`へ入る。最初のsuccessful commitで次を実行する。

source位置は`src/app/preview/compositor_rhi_item.cpp::recordFrameSwapped()`と
`src/media/gpu_preview/presentation_opportunity_scheduler.cpp::commitSwap()`である。

```cpp
if (!anchored_) {
    originRefreshCount_ = postSwapAuthority.refreshCount;
    anchored_ = true;
}
```

つまりcurrent originのcausal boundaryは、**first committed formal swapのframeSwapped callback中に実行した
post-swap NULL DWM sample**である。B1初版の「first committed Presentをformal display chainで後からphysical
ordinalへ結んだ値」は、物理表示完了側の別boundaryであり、current anchor establishmentと一致しない。

同じcausal boundaryでtarget physical counterを直接sampleできない限り、authorityだけを置換したcounterfactual
originは定義できない。displayed physical ordinalを新originにする案はscheduler anchor semantics自体の変更であり、
B1 attribution shadowへ混ぜず、必要なら別production designとして扱う。

既存W2/F3 artifactは削除・書換えない。ただしobserver post-return QPCをphysical boundaryとして使用した
historical mapping claimはB1のexact authorityへimportしない。そのclaim自体をcanonical physical boundary
authorityとして今後使う場合は、本amendmentを前提に別途再監査する。

## 3. Fail-closed classification

run classificationは次の優先順位で一意にする。

```text
B1_SCHEMA_INVALID
B1_PROVENANCE_INVALID
HWND_INVALID
OUTPUT_IDENTITY_UNRESOLVED
OUTPUT_MIGRATION
REFRESH_RATIONAL_CHANGED
OBSERVER_UNAVAILABLE
OBSERVER_PRIORITY_INVALID
OBSERVER_WAIT_FAILURE
OBSERVER_RING_OVERFLOW
OBSERVER_SEQUENCE_EMPTY_OR_INVALID
OBSERVER_ORDINAL_REGRESSION
OBSERVER_ORDINAL_GAP
OBSERVER_QPC_REGRESSION
OBSERVER_BOUNDARY_UNCLOSED
PUBLICATION_AMBIGUITY
OBSERVER_QPC_NOT_PHYSICAL_BOUNDARY
NULL_DWM_ACQUISITION_FAILED
NULL_COUNTER_REGRESSION
ACTUAL_INTENT_REGRESSION
FORMAL_SERIAL_JOIN_INVALID
TOKEN_PRESENT_JOIN_INVALID
PHYSICAL_JOIN_INVALID
PHYSICAL_SAMPLE_AUTHORITY_UNAVAILABLE
SHADOW_ORIGIN_CAUSAL_BOUNDARY_MISMATCH
DELTA_POPULATION_INCOMPLETE
EXACT_COMPARABLE
```

output identityはstart、measurement arm、各formal boundary、stopで再検査し、全field exact equalityを要求する。
refresh rationalだけの変化もmigrationから隠さず専用reasonで失敗させる。regressionを0 clamp、record drop、
run再試行で救済しない。INVALID runはcandidate rejection evidenceやperformance FAILへ転用せず、取得契約FAIL
として保存する。

## 4. Exact raw-delta checker amendment

NULL sequenceとactual sequenceはcurrent product recordだけでexact比較できる。

```text
N_i = invocation iでproductが使用したNULL DWM cRefresh
I_i = invocation iのactual intent ordinal

delta NULL_i   = N_i - N_(i-1)
delta intent_i = I_i - I_(i-1)

null_actual_all_point_exact
  = 全actual comparable pointで delta NULL_i == delta intent_i
```

first render、duplicate、invalid、terminalを暗黙に落とさず、全invocationを`ORIGIN_ONLY`、
`PRIMARY_DELTA_COMPARABLE`、`DUPLICATE_NON_DELTA`、`TERMINAL_DELTA_COMPARABLE`、
`INVALID_FATAL_NON_DELTA`、`INVALID_ACQUISITION`へpartitionする。terminalがvalid pairを持つなら必ず含め、
expected pair数と比較数が違えば`DELTA_POPULATION_INCOMPLETE`である。

target physical sequenceについて初版の`all delta target_vblank != delta intent`を**撤回**する。causal authorityが
異なっても一部のdeltaが偶然一致し得るため、全点不一致は必要条件ではない。exact physical sample/joinが将来
成立した場合の命題は次へ修正する。

```text
V_i = invocation iと同じcausal sample pointのexact target physical completed count
delta target_i = V_i - V_(i-1)

target_sequence_nonidentical
  = common exact comparison domainが完全
    AND EXISTS i (delta target_i != delta intent_i)
```

必要なのはNULL sequenceのactual sequenceへの全点exact一致、target physical sequenceの**非同一性**、後述する
full shadow causal outcome differenceの3点である。一部または大半のtarget delta一致は非同一性を否定しない。
反対に平均rate差だけでは非同一性を証明しない。

現行observerには`V_i`をexactに作るauthorityがないため、target comparison populationは作成せず
`PHYSICAL_SAMPLE_AUTHORITY_UNAVAILABLE`とする。observer wake QPC bracket、absolute offset、平均、rate、中央値、
nearest-QPC、QPC interpolation、cadence tolerance、counter clampで補完しない。

## 5. Target-output physical shadow replay amendment

初版のobserver QPC bracketとdisplayed physical ordinal originに基づくshadow replayを**撤回**する。現時点の判定は
`SHADOW_NOT_EVALUABLE_EXACT_PHYSICAL_AUTHORITY_UNAVAILABLE`であり、shadow ordinal/target/predicateを出力しない。

将来、§2.3のexact authorityと§2.4のsame causal anchorが成立した場合だけ次を再び有効化できる。

```text
origin_target_count
  = first committed formal swapのpost-swap authority sample pointで直接得たtarget count

completed_target_i
  = pre-render authority sample pointで直接得たtarget count - origin_target_count

shadow_intent_ordinal_i
  = first render ? 0 : completed_target_i + 1

shadow_target_i
  = source_frame_offset
    + floor(
        shadow_intent_ordinal_i * source_fps_numerator * refresh_denominator
        / (source_fps_denominator * refresh_numerator)
      )

shadow_required_intent_membership_i
  = 0 <= shadow_intent_ordinal_i < required_intent_count

shadow_past_source_domain_i
  = shadow_target_i < source_frame_offset
    || shadow_target_i >= qualified_source_frame_count
```

`completed + 1`はcurrent formulaの再生でありsequential policyではない。full shadow causal outcome differenceは、
W4-C3 actual terminal witnessと同じinvocation serialでintersectionが消えるだけでなく、frozen measurement endまで
全invocationをexact replayし、prematureな`past_source_domain && required_intent_membership`が0件であることをいう。
required set、source domain、measurement windowは変更しない。

「約29秒」はthresholdにせず、actual terminal serial/QPCの説明表示に限る。exact authorityが無い状態で
`premature terminal disappeared`を主張しない。

## 6. Frozen product invariant

B0で固定した次をB1でも変更不能とする。

```text
past_source_domain && required_intent_membership
  != successful measurement completion
```

この状態を`DOMAIN_TERMINAL`正常完了として扱うcurrent semanticsは**妥当ではない**。required intentが未消化の
ままmeasurementを成功終了させるためである。B1は代替behaviorを選ばず、将来のcorrectionでplanned accounting
継続または独立contract failureのどちらかを明示する。

`required_intent_count`はformal measurement contract、`source_frame_count_a/b`はdecoder metadata、
`qualified_source_frame_count`はsource-domain authorityである。数値が偶然同じでもfield、計算、checkerを共有しない。

## 7. Attribution verdictとproduction handoff

### 7.1 production correction designへ進む十分条件

次をすべて満たしたときだけ`TARGET_OUTPUT_PHYSICAL_CAUSAL_EXACT`とする。

```text
fresh clean capture 3/3がEXACT_COMPARABLE
checkpoint / binary / schema / checker provenanceがexact
B0 output identityが全boundaryでstable、refresh rational不変
target physical authorityがpre-renderとfirst post-swap anchorの同じcausal sample pointを直接表す
observer post-return QPC、nearest、tolerance、interpolationをphysical joinに使っていない
formal serial / token joinとtarget physical sample joinが全件exact
delta populationが全runで完全
全比較点で delta NULL == delta intent
target physical delta sequenceがactual intent sequenceと非同一（exact pointが少なくとも1点異なる）
W4-C3の+2/+3 actual advancementをNULL rawが全件再現
same-causal-boundary physical shadowがactual約29秒terminalを消し、frozen endまで完全
required set、source domain、formula、threshold、measurement windowを変更していない
render thread WaitForVBlank count = 0
```

これはproduction correctionの**設計へ進む**十分条件であり、production PASSではない。現行observerは最初の
physical sample条件を満たさないため、現在このclosureは実行不能である。まずsupportedなexact target-output
counter/sample authorityを別design reviewで確立する必要がある。実装後にはfresh W3およびP3/P4/P5 regressionを
通す。

### 7.2 candidate rejection / non-closure

次のいずれかならtarget-output physical ordinalを今回のcausal correction候補として棄却するか、authority未成立
として閉じない。

```text
TARGET_OUTPUT_PHYSICAL_NOT_CAUSAL
  exact 3/3で delta NULL == delta intent が1点でも崩れる
  またはtarget physical delta sequenceがactual intent sequenceと全点同一
  またはfull physical shadowでも同じpremature terminal/outcomeが残る

TARGET_OUTPUT_PHYSICAL_AUTHORITY_INVALID
  identity migration、counter regression/wrap、publication ambiguity、
  causal sample boundary不足、serial/token/physical join欠損のいずれか

TARGET_OUTPUT_PHYSICAL_NOT_EVALUABLE
  current observerのpost-Wait return QPCしかなく、exact callback physical sampleまたは
  same-causal-boundary originを構築できない

TARGET_OUTPUT_PHYSICAL_CORRECTION_UNSAFE
  render threadのWaitForVBlank、nearest/tolerance/interpolation/clamp、sequential +1、
  threshold変更、required-set縮小のいずれかが必要
```

INVALIDをNOT_CAUSALへ読み替えない。1/3や2/3の一致、多数決、retry-until-successではclosureしない。
現在のstatic verdictは`TARGET_OUTPUT_PHYSICAL_NOT_EVALUABLE`であり、候補のcausal rejectionではない。

## 8. Negative contract

B1 design段階ではtest codeを追加しない。実装前に次のnegative名と単一故障を固定する。

```text
NegativeB0IdentityFieldMutation
NegativeMonitorDefaultToNearest
NegativeOutputMigrationIgnored
NegativeRefreshRationalChangedIgnored
NegativeRenderThreadWaitForVBlank
NegativeObserverPriorityFailureAccepted
NegativeObserverWaitFailureAccepted
NegativeObserverRingOverflowAccepted
NegativeObserverOrdinalRegression
NegativeObserverOrdinalGap
NegativeObserverQpcRegression
NegativePostWaitQpcAcceptedAsVblankBoundary
NegativeObserverWakeBracketAcceptedAsPhysicalJoin
NegativeLatestPublicationAcceptedAsLatestCompletedVblank
NegativeMissingPrerollPredecessor
NegativeMissingEndSuccessor
NegativePublishSerialRegression
NegativePublicationSnapshotAmbiguity
NegativeNullCounterRegression
NegativeIntentOrdinalRegression
NegativeMissingInvocationSerialJoin
NegativeCompositionTokenSerialMutation
NegativeNativePresentSerialMutation
NegativePhysicalPresentJoinMutation
NegativeDisplayedPhysicalOrdinalUsedAsPostSwapOrigin
NegativeShadowOriginCausalBoundaryMismatch
NegativeUnsupportedOutputFrameStatisticsFallback
NegativeUnreliableSwapchainStatisticsFallback
NegativeCrossRunSplice
NegativeNearestQpcJoin
NegativeQpcInterpolation
NegativeCadenceToleranceRescue
NegativeCounterClamp
NegativeSequentialShadowOrdinal
NegativeDeltaNullMutation
NegativeDeltaTargetVblankMutation
NegativeDeltaIntentMutation
NegativeSingleTargetDeltaEqualityRejectsNonidentity
NegativeAllTargetDeltaEqualityAcceptedAsNonidentity
NegativeTerminalPairSilentlyExcluded
NegativeComparisonPopulationShrink
NegativeShadowOriginFromArrayPosition
NegativeShadowTargetMutation
NegativeRequiredIntentUsesSourceCount
NegativePastSourceUsesRequiredIntentCount
NegativeRequiredSetShrink
NegativeThresholdMutation
NegativePastRequiredSuccessfulCompletion
NegativeProducerSummaryTrusted
```

各negativeは1 fieldまたは1 ruleだけを壊し、同じcheckerで対照fixtureをPASSさせる。negativeが期待したexact
reason以外（parse errorやcrash）で落ちた場合はPASSにしない。

## 9. B1 exit

B1 design closureは、本書のstatic inventory、schema、exact join/delta、shadow replay、fail-close classification、
production handoff/rejection、frozen invariant、negative contractをreviewで固定した時点で成立する。

```text
B1 design                         CLOSED
B1 amendment review               CLOSED
B1 exact physical sample authority UNAVAILABLE
B1 shadow replay                  NOT EVALUABLE
B2 supported authority inventory CLOSED / NO CANDIDATE
B1 instrumentation               NOT STARTED
B1 negative tests                NOT STARTED
B1 fresh capture                 NOT STARTED
production behavior              UNCHANGED
canonical W3 verdict              UNCHANGED / FAIL
P5-E4                             BLOCKED
```
