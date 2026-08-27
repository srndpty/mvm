# P2-D5-2 B0 — Same-output Authority Attribution Design

- 状態: **DESIGN FROZEN / CAPTURE NOT STARTED / PRODUCTION BEHAVIOR UNCHANGED**
- 前提: W4-A / W4-B / W4-C1 / W4-C2 / W4-C3 CLOSED
- 目的: mixed output provenanceがW4-C3の`+2/+3` ordinal advancementと
  約29秒のsource-domain terminalを説明するか、exactに判定する
- 非目的: production scheduler変更、performance PASS、threshold変更、historical verdict更新

## 1. Static inventory

### 1.1 current wiring

current render pathの`PresentationAuthoritySample`は次の混成である。

```text
cRefresh / qpcVBlank
  DwmGetCompositionTimingInfo(NULL)
  src/app/preview/compositor_rhi_item.cpp::capturePresentationAuthority()

refresh rational
  target HWND
    -> MonitorFromWindow
    -> GetMonitorInfoW(MONITORINFOEXW::szDevice)
    -> QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)
    -> DISPLAYCONFIG_SOURCE_DEVICE_NAME::viewGdiDeviceName join
    -> DISPLAYCONFIG_PATH_TARGET_INFO::refreshRate
  apps/compositor_spike/compositor_spike_controller.cpp::captureDwmTiming()

sample construction
  NULL DWM counter/QPC + stored target-output rational
```

counter/QPCとrationalが同じoutput authorityに属することを示すfieldは
`PresentationAuthoritySample`に無い。`presentationAuthorityUsable()`はavailable、正値、
rational equalityだけを検査し、output identityは検査しない。

### 1.2 Windows API semantics

Microsoftの`DwmGetCompositionTimingInfo`契約では、Windows 8.1以降`hwnd`は`NULL`必須であり、
非NULLなら`E_INVALIDARG`である。したがって**HWND-bound DWM timingは現行target OSで利用可能な
production authorityではない**。B0は非NULL呼出しを成功すると仮定せず、HRESULTを診断事実として
保存する。`NULL`へのfallbackでHWND sampleを捏造しない。

target output identityは既存`resolveWindowOutput()`と同じchainでbindする。

```text
target HWND
  -> IsWindow == true
  -> MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) -> HMONITOR
  -> GetMonitorInfoW -> MONITORINFOEXW::szDevice

HMONITOR
  -> CreateDXGIFactory1
  -> EnumAdapters1 -> DXGI_ADAPTER_DESC1::AdapterLuid
  -> EnumOutputs -> DXGI_OUTPUT_DESC
  -> exact DXGI_OUTPUT_DESC::Monitor == HMONITOR

GDI device name
  -> QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)
  -> DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME)
  -> exact viewGdiDeviceName == MONITORINFOEXW::szDevice
  -> path.sourceInfo(adapterId,id)
  -> path.targetInfo(adapterId,id,refreshRate)
```

identity keyは次の全fieldのexact equalityである。handleまたはdevice name単独ではbindしない。

```text
HMONITOR
DXGI adapter LUID high/low
DXGI output index
DXGI_OUTPUT_DESC::DeviceName
DXGI_OUTPUT_DESC::DesktopCoordinates
MONITORINFOEXW::szDevice
DISPLAYCONFIG source adapterId / source id
DISPLAYCONFIG target adapterId / target id
DISPLAYCONFIG target refresh numerator / denominator
```

`MonitorFromWindow`は`MONITOR_DEFAULTTONULL`だけを使う。nearest/primary fallbackは認めない。
`QueryDisplayConfig`の`ERROR_INSUFFICIENT_BUFFER`を別callbackの結果で救済せず、そのrecordを
identity unresolvedへ分類する。

API contractの一次資料:

- [DwmGetCompositionTimingInfo](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo)
- [MonitorFromWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromwindow)
- [MONITORINFOEXW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-monitorinfoexw)
- [QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig)
- [DISPLAYCONFIG_PATH_TARGET_INFO](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_target_info)
- [DISPLAYCONFIG_SOURCE_DEVICE_NAME](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_source_device_name)
- [DXGI_OUTPUT_DESC](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_output_desc)
- [IDXGIAdapter::EnumOutputs](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-enumoutputs)

### 1.3 existing evidenceとの境界

F3はhistorical runでNULL DWM `cRefresh`が約123.7 Hz、target outputが59950/1000、
swap callbackが約59.98/sだったことを既に観測している。またwindow-output
`IDXGIOutput::WaitForVBlank`は59.9502 Hzだった。これはmixed authority defectの強い先行evidenceだが、
W4-C3 checkpointのinvocation sequenceとsame-callbackでjoinしたevidenceではない。
B0はhistorical F3値をW4-C3 rawへspliceせず、新しいdiagnostic schemaのexact joinだけで判定する。

## 2. Diagnostic-only acquisition schema

schemaは`mvm-p2-d5-2-b0-same-output-authority-1`とする。production pathの
`PresentationAuthoritySample`、scheduler入力、decision、stop arbitration、counter、thresholdを変更しない。

```json
{
  "schema": "mvm-p2-d5-2-b0-same-output-authority-1",
  "stage": "P2-D5-2-B0",
  "diagnostic_only": true,
  "production_authority_changed": false,
  "production_behavior_changed": false,
  "canonical_performance_authority": false,
  "checkpoint_sha": "...",
  "binary_sha256": {},
  "os_version": {},
  "qpc_frequency": 0,
  "config": {
    "measurement_start_qpc": 0,
    "measurement_end_qpc_exclusive": 0,
    "source_fps_numerator": 60,
    "source_fps_denominator": 1,
    "required_intent_count": 3600,
    "source_frame_count_a": 0,
    "source_frame_count_b": 0,
    "qualified_source_frame_count": 0
  },
  "initial_output_identity": {},
  "render_callback_records": [],
  "post_swap_records": [],
  "summary": {}
}
```

### 2.1 output identity object

```json
{
  "resolved": true,
  "hwnd_value": "0x...",
  "is_window": true,
  "hmonitor_value": "0x...",
  "monitor_gdi_device_name": "\\\\.\\DISPLAY1",
  "dxgi_adapter_luid_high": 0,
  "dxgi_adapter_luid_low": 0,
  "dxgi_output_index": 0,
  "dxgi_output_device_name": "\\\\.\\DISPLAY1",
  "desktop_rect": [0, 0, 0, 0],
  "displayconfig_source_adapter_luid_high": 0,
  "displayconfig_source_adapter_luid_low": 0,
  "displayconfig_source_id": 0,
  "displayconfig_target_adapter_luid_high": 0,
  "displayconfig_target_adapter_luid_low": 0,
  "displayconfig_target_id": 0,
  "refresh_numerator": 0,
  "refresh_denominator": 0,
  "resolve_status": "EXACT"
}
```

start、各sample、stopで同じidentityを再取得する。全field exact equalityを要求し、
producerの`stable=true`を信用しない。

### 2.2 DWM sample object

```json
{
  "attempted": true,
  "argument_kind": "NULL_OR_TARGET_HWND",
  "call_begin_qpc": 0,
  "call_end_qpc": 0,
  "hresult": "0x00000000",
  "succeeded": true,
  "c_refresh": 0,
  "qpc_vblank": 0,
  "rate_refresh_numerator": 0,
  "rate_refresh_denominator": 0,
  "qpc_refresh_period": 0
}
```

失敗時はtiming fieldを0で成功扱いせずJSON `null`にする。

### 2.3 same-callback record

各formal scheduler invocationとexactにjoinする。QPC nearest joinは禁止し、W4-C3と同じ
`scheduler_invocation_serial`をprimary keyにする。

```json
{
  "scheduler_invocation_serial": 0,
  "render_ordinal": 0,
  "callback_begin_qpc": 0,
  "identity_before": {},
  "null_sample_before": {},
  "hwnd_sample_attempt": {},
  "null_sample_after": {},
  "identity_after": {},
  "existing_decision": {
    "intent_ordinal": 0,
    "target_frame": 0,
    "required_intent_membership": true,
    "past_source_domain": false,
    "result": "PRIMARY_DECISION",
    "reason": "PRIMARY"
  }
}
```

call順は固定する。

```text
callback begin QPC
identity before
DwmGetCompositionTimingInfo(NULL) before
DwmGetCompositionTimingInfo(target HWND) attempt
DwmGetCompositionTimingInfo(NULL) after
identity after
existing product selectForRender()は従来の入力・順序のまま
```

NULL before/afterの`cRefresh`と`qpcVBlank`が一致しないrecordは、API呼出し窓がcomposition tickを
跨いだため`INTRA_CALLBACK_NULL_TICK_SPLIT`とする。片側を選ばず、nearest-QPCでも救済しない。
diagnostic instrumentationがproduct decisionの前後関係を変えないよう、実装段階ではfixed POD ring、
single writer、allocation/mutex/I/O/log無しを要求する。DXGI factory/outputとDisplayConfig bufferは
measurement開始前に確保する。callback内では`MonitorFromWindow`、既存DXGI outputの`GetDesc`、
preallocated bufferへの`QueryDisplayConfig`だけを行い、buffer不足はその場でfail-closeする。

post-swapにも同じdual sampleとidentityを保存し、既存render ordinal / swap ordinal / token serialで
render recordへexact joinする。shadow originは最初のexact post-swap sampleから固定する。

## 3. Fail-closed classification

分類は優先順位順に一意化する。

```text
B0_SCHEMA_INVALID
B0_PROVENANCE_INVALID
HWND_INVALID
OUTPUT_IDENTITY_UNRESOLVED
OUTPUT_MIGRATION
REFRESH_RATIONAL_CHANGED
DISPLAYCONFIG_TOPOLOGY_RACE
NULL_DWM_ACQUISITION_FAILED
HWND_DWM_API_UNSUPPORTED
HWND_DWM_ACQUISITION_FAILED
INTRA_CALLBACK_NULL_TICK_SPLIT
NULL_COUNTER_REGRESSION
HWND_COUNTER_REGRESSION
NULL_QPC_VBLANK_REGRESSION
HWND_QPC_VBLANK_REGRESSION
RENDER_SWAP_JOIN_INVALID
W4_C3_JOIN_INVALID
EXACT_COMPARABLE
```

- non-NULL callが`E_INVALIDARG`なら`HWND_DWM_API_UNSUPPORTED`。
- HWND失効、`MonitorFromWindow(...DEFAULTTONULL)==NULL`、identity解決不能は別分類にする。
- start/current/stop identityのいずれかが違えば`OUTPUT_MIGRATION`。
- identity keyが同じでもrationalだけが変われば`REFRESH_RATIONAL_CHANGED`を優先する。
- counter/QPC regressionは0 clampやrecord dropをせずINVALID。
- invalid runをperformance FAILまたは候補B rejection evidenceへ変換しない。ただし
  `HWND_DWM_API_UNSUPPORTED`はMicrosoft API contractと一致するため、literalな
  HWND-bound DWM authority候補の棄却条件になる。

## 4. Exact delta checker contract

checkerはproducer summaryを信じずraw recordから次を構築する。

```text
N_i = exact comparable NULL cRefresh at invocation i
H_i = exact comparable HWND cRefresh at invocation i
W_i = W4-C3 existing intent ordinal at invocation i

ΔN_i = N_i - N_(i-1)
ΔH_i = H_i - H_(i-1)
ΔW_i = W_i - W_(i-1)
```

比較対象は同じrun、隣接するvalid non-duplicate scheduler invocationである。run境界、duplicate、
terminal recordを暗黙に除外せず、各dispositionをrawへ残して契約どおりpartitionする。

```text
null_existing_exact
  iff 全比較点で ΔN_i == ΔW_i

hwnd_existing_exact
  iff 全比較点で ΔH_i == ΔW_i

null_hwnd_exact
  iff 全比較点で ΔN_i == ΔH_i
```

absolute counter offsetは比較しないが、deltaをQPCで補間・量子化・clampしない。欠落recordが1件でもあれば
PARTIALではなくauthority INVALIDとする。W4-C3 source artifact SHAとchecker SHAをbindし、別cohortを
spliceしない。

## 5. Coherent HWND shadow replay

HWND sampleがAPI上取得可能な環境に限り、product pathへ接続せず次をoffline replayする。

```text
origin_hwnd = 最初のexact joined post-swap HWND cRefresh
completed_hwnd = pre-render HWND cRefresh - origin_hwnd
shadow_intent_ordinal = first render ? 0 : completed_hwnd + 1
shadow_target = sourceFrameOffset + floor(
  shadow_intent_ordinal * sourceFpsNumerator * refreshDenominator /
  (sourceFpsDenominator * refreshNumerator))
shadow_required_intent_membership =
  0 <= shadow_intent_ordinal < required_intent_count
shadow_past_source_domain =
  shadow_target < sourceFrameOffset ||
  shadow_target >= qualified_source_frame_count
```

これは保存済みsampleからのshadowであり、frame selection、token、Present、stop arbitration、measurement
endを変更しない。中間積overflow、origin regression、join欠損はfail-closed。`completed + 1`はcurrent
「完了済みrefreshの次」のreplayだけに使い、previous ordinalからのsequential `+1`を導入しない。

## 6. Attribution verdict

```text
SAME_OUTPUT_MIXING_CAUSAL_EXACT
  3/3 runがEXACT_COMPARABLE
  output identity / rationalが全runで不変
  null_existing_exact = true
  hwnd_existing_exact = false
  null_hwnd_exact = false
  NULL側の全+2/+3がW4-C3 ordinal advancementとexact一致
  HWND shadow replayが同じ約29秒地点でpast source terminalにならない
  HWND shadowでpast&&requiredをsuccessful completionへ変換していない

SAME_OUTPUT_MIXING_NOT_CAUSAL
  exact comparable 3/3でnull_hwnd_exact = true
  またはHWND shadowでも同じordinal/target/terminal chainをexact再現

SAME_OUTPUT_AUTHORITY_UNAVAILABLE
  HWND-bound sampleがAPI contract上またはruntimeで取得不能

SAME_OUTPUT_ATTRIBUTION_INVALID
  identity、provenance、sequence、joinのいずれかがINVALID
```

多数決、tolerance、retry-until-successを使わない。

## 7. Production fixへのclosure / rejection

literalな候補B「`DwmGetCompositionTimingInfo(target HWND)`をproduction authorityにする」は、
Windows 8.1以降のAPI contractにより**static rejection**である。captureで非NULL callが成功しない限り、
production fixへ進めない。成功したとしてもtarget OS/API support envelopeを別途固定する必要がある。

B0から別のsame-output production fix設計へ進めるのは、次をすべて満たした場合だけである。

```text
SAME_OUTPUT_MIXING_CAUSAL_EXACT 3/3
candidate authorityがtarget output identityへexact bind可能
candidate authorityがproduction環境でsupported
shadow ordinal/target/predicate replayが全record exact
約29秒のpremature source terminalを除去
required set / source domain / threshold / measurement windowを変更しない
nearest-QPC / tolerance / clamp / sequential +1を使わない
```

候補Bを棄却する条件:

```text
HWND_DWM_API_UNSUPPORTED
SAME_OUTPUT_MIXING_NOT_CAUSAL
identityをtarget outputへbindできない
coherent replayでも約29秒terminalが残る
explanationにnearest-QPC / cadence tolerance / clamp / sequential +1が必要
required intent countまたはsource frame countの縮小が必要
```

棄却時は既にformal authorityとなっているwindow-output physical VBlank pathの設計へ戻り、
DWM sampleをdiagnostic-onlyのまま維持する。

## 8. Frozen product invariant

次をB0開始前にfreezeする。

```text
past_source_domain && required_intent_membership
  != successful measurement completion
```

このintersectionはrequired intentが残っているため、`DOMAIN_TERMINAL`による正常完了へ変換しない。
B0では代替behaviorを実装しない。将来のproduction correctionは、少なくとも次のどちらかを明示する。

```text
planned windowまでrequired intent accountingを継続する
独立したcontract failureとしてfail-closeする
```

required set縮小、短いmeasurement windowへの再定義、後続PASSによるhistorical W3/W4書換えは禁止する。

## 9. required intent / source frame authority separation

current `requiredFrameCount`を将来次の別fieldへ分ける。B0ではschema/shadowだけに追加し、product structは
変更しない。

| field | authority | 用途 |
| --- | --- | --- |
| `required_intent_count` | formal measurement contract。`measure_seconds * 60/1`を開始前にexact整数演算しfreeze | required set `[0, count)`、canonical satisfied/unsatisfied conservation |
| `source_frame_count_a` | source A decoder metadata | source Aの実frame domain |
| `source_frame_count_b` | source B decoder metadata | source Bの実frame domain |
| `qualified_source_frame_count` | exact-pair requirementでは`min(A, B)`。単sourceではA | `target`が全required sourceに存在するかの上限 |

将来のscheduler predicateは次とする。

```text
requiredIntentMembership =
  0 <= intentOrdinal < requiredIntentCount

pastSourceDomain =
  targetFrame < sourceFrameOffset ||
  targetFrame >= qualifiedSourceFrameCount
```

数値がfixture上で偶然3600/3600に一致しても、field、authority、checkerを共有しない。
source coverage preflightも`sourceFrameCount >= requiredIntentCount`という現行の代理比較から分離し、
planned opportunity domainから導いた最大required targetが各source domain内にあることを独立に検査する。

## 10. Negative contract

実装前に次のnegative名と失敗理由を固定する。B0 design段階ではtest codeをまだ追加しない。

```text
NegativeHwndDwmUnsupportedAccepted
NegativeHwndFailureFallsBackToNull
NegativeMonitorDefaultToNearest
NegativeOutputMigrationIgnored
NegativeRefreshRationalMutation
NegativeDisplayConfigPathSplice
NegativeAdapterLuidMutation
NegativeDxgiOutputIndexMutation
NegativeDesktopRectMutation
NegativeNullCounterRegression
NegativeHwndCounterRegression
NegativeQpcVBlankRegression
NegativeIntraCallbackTickSplitNearestRescue
NegativeMissingInvocationJoin
NegativeCrossRunRecordSplice
NegativeCounterClamp
NegativeSequentialOrdinalIncrement
NegativeCadenceToleranceMatch
NegativeShadowTargetMutation
NegativeRequiredIntentUsesSourceCount
NegativePastSourceUsesRequiredIntentCount
NegativePastRequiredSuccessfulCompletion
NegativeRequiredSetShrinkAtTerminal
```

各negativeは1 fieldまたは1 ruleだけを壊し、対照群を同時にPASSさせる。

## 11. B0 exit

B0 design closureは本書のstatic/API inventory、schema、classification、checker、shadow replay、
closure/rejection、product invariant、authority分離、negative contractをfreezeした時点で成立する。
capture開始には別checkpointでdiagnostic-only instrumentation reviewを通す必要がある。

現時点の判定:

```text
B0 design                         CLOSED
B0 capture                        NOT STARTED
literal HWND-bound DWM candidate  STATICALLY UNSUPPORTED on Windows 8.1+
production behavior               UNCHANGED
canonical W3 verdict              UNCHANGED / FAIL
P5-E4                             BLOCKED
```
