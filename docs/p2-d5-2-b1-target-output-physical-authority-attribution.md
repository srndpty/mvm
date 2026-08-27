# P2-D5-2 B1 — Target-output Physical Authority Attribution Design

- 状態: **DESIGN FROZEN / INSTRUMENTATION NOT IMPLEMENTED / CAPTURE NOT STARTED**
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

`VBlankRing::publishSerial`はresetでも戻らない。lower boundaryはmeasurement arm前の新規preroll、upper
boundaryはfrozen measurement end以後のsuccessorで閉じる。sequenceはordinal strictly consecutiveかつQPC
strictly increasingを要求する。

既存のinterval/cadence reportはobserver健全性の診断としてraw保存してよいが、B1 delta一致、join、
replayの救済には使わない。B1はcadence toleranceを新設・緩和せず、gapやmiss疑いをVALIDへ変換しない。

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
  "observer": {},
  "physical_vblank_samples": [],
  "scheduler_invocations": [],
  "formal_present_records": [],
  "terminal_witness": {},
  "summary": {}
}
```

`summary`は非authorityであり、checkerはrawから再計算する。capture/checker/schema SHAをmanifestへbindし、
別runやW4-C3 historical rawをspliceしない。

### 2.1 observer objectとraw sequence

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
  "sequence_status": "OK"
}
```

各`physical_vblank_samples`は`{sample_index,publish_serial,physical_vblank_ordinal,qpc}`を持つ。
`sample_index`と`publish_serial`をordinalの代用にしない。capture終了後、successorまでpublish済みのringを
sealしてからsnapshotし、snapshot中にpublish範囲が変わった、必要範囲がoverwrite/overflowした、または
同じpublish serialから異なるsampleが読めた場合は`PUBLICATION_AMBIGUITY`とする。

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
  "physical_join": {
    "status": "EXACT_BRACKET",
    "lower_sample_publish_serial": "0",
    "lower_ordinal": 0,
    "lower_qpc": 0,
    "upper_sample_publish_serial": "0",
    "upper_ordinal": 1,
    "upper_qpc": 0,
    "completed_target_vblank_ordinal": 0
  }
}
```

NULL DWM sampleはproductが実際に読んだ同一sampleを複製せず参照する。diagnostic用に別callを挿入して
sample時点を動かさない。actual intent/decisionもproduct recordそのものへserial joinする。

### 2.3 exact join graph

joinは次だけを許す。

```text
scheduler_invocation_serial
  -> W4-C2 invocation input / actual decision
  -> W4-C3 terminal witness（terminal invocationだけ）

composition_token_serial
  -> native Present embedded token serial
  -> exact PresentEvent identity
  -> FinalState / DisplayedQPC
  -> physical_vblank_ordinal

callback_begin_qpc
  -> unique strict bracket V_k.qpc <= callback_begin_qpc < V_(k+1).qpc
  -> completed_target_vblank_ordinal = V_k.ordinal
```

最後はnearest-QPCではなく、連続する実sample 2本が定める半開区間へのexact membershipである。lower/upper
ordinalが連続しない、upper sampleが無い、境界が複数候補になる場合はjoinしない。QPCはserial/token joinの
代替にはせず、join後の順序とphysical bracketにだけ使う。

first committed formal swapに対応するcomposition tokenから、existing formal chainで一意に得た
`physical_vblank_ordinal`を`origin_target_vblank_ordinal`とする。source frame、近いQPC、配列位置からoriginを
推測しない。

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
NULL_DWM_ACQUISITION_FAILED
NULL_COUNTER_REGRESSION
ACTUAL_INTENT_REGRESSION
FORMAL_SERIAL_JOIN_INVALID
TOKEN_PRESENT_JOIN_INVALID
PHYSICAL_JOIN_INVALID
DELTA_POPULATION_INCOMPLETE
EXACT_COMPARABLE
```

output identityはstart、measurement arm、各formal boundary、stopで再検査し、全field exact equalityを要求する。
refresh rationalだけの変化もmigrationから隠さず専用reasonで失敗させる。regressionを0 clamp、record drop、
run再試行で救済しない。INVALID runはcandidate rejection evidenceやperformance FAILへ転用せず、取得契約FAIL
として保存する。

## 4. Exact raw-delta checker

checkerは各runの隣接するvalid primary decision pairについてraw値を引く。

```text
N_i = invocation iでproductが使用したNULL DWM cRefresh
V_i = invocation iのcompleted_target_vblank_ordinal
I_i = invocation iのactual intent ordinal

delta NULL_i          = N_i - N_(i-1)
delta target_vblank_i = V_i - V_(i-1)
delta intent_i        = I_i - I_(i-1)
```

first render、duplicate、invalid、terminalを暗黙に落とさず、全invocationを次へexact partitionする。

```text
ORIGIN_ONLY
PRIMARY_DELTA_COMPARABLE
DUPLICATE_NON_DELTA
TERMINAL_DELTA_COMPARABLE
INVALID_FATAL_NON_DELTA
INVALID_ACQUISITION
```

terminalがvalidな隣接primary pairを持つなら必ず比較に含める。expected pair数をraw invocation partitionから
独立計算し、actual comparison数と一致しなければ`DELTA_POPULATION_INCOMPLETE`とする。

B1の中心命題は各runの**全比較点**で次が同時成立することである。

```text
delta_null_matches_intent_i = (delta NULL_i == delta intent_i)
delta_target_differs_i      = (delta target_vblank_i != delta intent_i)

all_null_matches_intent = AND_i(delta_null_matches_intent_i)
all_target_differs      = AND_i(delta_target_differs_i)
```

absolute offset、平均、rate、中央値、多数決では代用しない。`delta target_vblank`を1へ固定せずrawのまま保存する。
差が1点でも一致するなら「全点で別authority」というB1命題は成立しない。nearest-QPC、QPC interpolation、
cadence tolerance、counter clamp、previous shadow ordinalへのsequential `+1`は禁止する。

## 5. Target-output physical shadow replay

product pathを変えず、sealed rawだけから次を再生する。

```text
origin_target_vblank
  = first committed formal swapのexact physical_vblank_ordinal

completed_target_vblank_i
  = pre-render exact bracket ordinal_i - origin_target_vblank

shadow_intent_ordinal_i
  = first render ? 0 : completed_target_vblank_i + 1

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

ここで`completed + 1`は「観測済みtarget-output VBlankの次のopportunity」というcurrent formulaの
counterfactual入力であり、前回shadow ordinalへ機械的に1を足すpolicyではない。physical ordinalが同じなら
同じshadow ordinalになり、飛べばrawどおり飛ぶ。

各recordで整数overflow、recorded configとの不一致、originより前、join欠損をfail-closeする。actual resultを
shadowへコピーせず、shadow target/predicateを独立再計算する。ただしshadowは診断であり、frame selection、
token publication、Present、stop claim、gate stateを変更しない。

### 5.1 premature terminal消失の判定

W4-C3 actual terminal witnessと同じinvocation serialまでreplayし、次をすべて要求する。

```text
actual terminal invocation:
  actual past_source_domain = true
  actual required_intent_membership = true
  actual result = OUTSIDE_SOURCE_DOMAIN_DECISION
  stop winner = DOMAIN_TERMINAL

same invocation under shadow:
  NOT (shadow_past_source_domain && shadow_required_intent_membership)

full planned measurement shadow:
  約29秒以前またはactual terminal serial以前に
  shadow_past_source_domain && shadow_required_intent_membership が0件
  required intent setを縮小していない
  frozen measurement endまでreplay coverageが完全
```

「約29秒」はthresholdにしない。authorityはexact terminal serial/QPCとfrozen measurement windowであり、秒値は
human-readable diagnosticに限る。shadowでintersectionが出た場合、それをsuccessful completionへ変換しない。

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
observer wait failure / overflow / gap / regression / publication ambiguityが0
formal serial / token / Present / physical joinが全件exact
delta populationが全runで完全
全比較点で delta NULL == delta intent
全比較点で delta target_vblank != delta intent
W4-C3の+2/+3 actual advancementをNULL rawが全件再現
physical shadowがactual約29秒terminalを消し、frozen endまで完全
required set、source domain、formula、threshold、measurement windowを変更していない
render thread WaitForVBlank count = 0
```

これはproduction correctionの**設計へ進む**十分条件であり、production PASSではない。次段階でnonblocking
publication、startup/teardown、migration handling、failure behaviorをproduct contractとしてreviewし、実装後に
fresh W3およびP3/P4/P5 regressionを通す必要がある。

### 7.2 candidate rejection / non-closure

次のいずれかならtarget-output physical ordinalを今回のcausal correction候補として棄却するか、authority未成立
として閉じない。

```text
TARGET_OUTPUT_PHYSICAL_NOT_CAUSAL
  exact 3/3で delta NULL == delta intent が1点でも崩れる
  または delta target_vblank != delta intent が1点でも崩れる
  またはphysical shadowでも同じpremature terminalが残る

TARGET_OUTPUT_PHYSICAL_AUTHORITY_INVALID
  identity migration、observer gap/regression/failure/overflow、publication ambiguity、
  boundary不足、serial/token/physical join欠損のいずれか

TARGET_OUTPUT_PHYSICAL_CORRECTION_UNSAFE
  render threadのWaitForVBlank、nearest/tolerance/interpolation/clamp、sequential +1、
  threshold変更、required-set縮小のいずれかが必要
```

INVALIDをNOT_CAUSALへ読み替えない。1/3や2/3の一致、多数決、retry-until-successではclosureしない。

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
NegativeCrossRunSplice
NegativeNearestQpcJoin
NegativeQpcInterpolation
NegativeCadenceToleranceRescue
NegativeCounterClamp
NegativeSequentialShadowOrdinal
NegativeDeltaNullMutation
NegativeDeltaTargetVblankMutation
NegativeDeltaIntentMutation
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
B1 instrumentation               NOT STARTED
B1 negative tests                NOT STARTED
B1 fresh capture                 NOT STARTED
production behavior              UNCHANGED
canonical W3 verdict              UNCHANGED / FAIL
P5-E4                             BLOCKED
```
