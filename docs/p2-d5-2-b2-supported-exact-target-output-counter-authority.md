# P2-D5-2 B2 — Supported Exact Target-output Counter Authority

- 状態: **STATIC INVENTORY CLOSED / NO SUPPORTED CANDIDATE / IMPLEMENTATION NOT STARTED**
- 前提: B1 amendment review CLOSED
- verdict: **EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE**
- 非目的: production変更、test追加、capture、API runtime probe、single-monitorへのworkload縮小

## 1. Acceptance requirements

B1 shadowを再有効化できるcandidateは、次をすべて満たさなければならない。

| requirement | 内容 |
| --- | --- |
| supported | current Windows / windowed QRhi D3D11 swapchain構成でAPI contract上supported |
| reliable | current target workloadのmulti-monitor可能性を含むsupport envelopeでreliable |
| target-bound | B0 identityのHMONITOR / adapter LUID / output / DisplayConfig source-targetへexact bind |
| direct counter | completed target refreshのmonotonic countを直接返す |
| pre-render sample | `selectForRender()`へ渡すauthority取得点でdirect countをsample可能 |
| anchor sample | first successful `commitSwap()`のpost-swap authority取得点で同じcounter domainをsample可能 |
| causal equality | current NULL DWM pathと同じpre-render / first post-swap causal boundaryを表す |
| nonblocking | render threadをVBlankまでblockしない |
| fail-closed | unsupported、disjoint、migration、regression、wrap、identity mismatchを検出可能 |

observer wake timestamp、nearest-QPC、derived cadence、scanlineからのcycle推定、counter clamp、previous ordinalへの
sequential `+1`はdirect counterではない。

## 2. Current QRhi / swapchain source inventory

### 2.1 backendとnative handle exposure

`CompositorRhiRenderer::initialize()`は`QRhi::D3D11`を要求し、`QRhi::nativeHandles()`を
`QRhiD3D11NativeHandles`へcastする。Qtが公開するD3D11 native handlesは`ID3D11Device*`と
`ID3D11DeviceContext*`、adapter LUID、feature levelであり、swapchain pointerを含まない。

```text
src/app/preview/compositor_rhi_item.cpp
  QRhiD3D11NativeHandles -> dev / context
  swapchainは非公開
```

### 2.2 actual swapchain access

patched Qt 6.11.1は`QRhiD3D11::endFrame()`内で、Qt内部の
`QRhiD3D11SwapChain::swapChain`を`IDXGISwapChain::Present()`へ渡す直前にnative hookへ渡す。

```text
qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch

swapChainD->swapChain
  -> mvmBeginPresentCapture(actual IDXGISwapChain*)
  -> record.swapchainIdentity = raw pointer identity
  -> IDXGISwapChain::Present(...)
```

`PresentationEligibilityPreflight`は記録されたactual pointerからrender-owner contextで
`QueryInterface(IDXGISwapChain1)`し、`GetDesc1()`と`GetContainingOutput()`を呼べる。したがってunderlying
swapchain objectへの到達経路自体は存在する。ただしこれはQt public QRhi contractではなくlocal patch contractで、
raw identityのlifetimeを越えて保持する権利はない。pre-renderから利用するにはactual object lifetime / owning thread / 
AddRef-releaseを別途固定する必要がある。

### 2.3 actual swapchain configuration

current preflightはactual `GetDesc1()`値をauthorityとする。保存済みcurrent-path evidenceでは次である。

```text
windowed HWND swapchain
DXGI_SWAP_EFFECT_FLIP_DISCARD (numeric 4)
DXGI_USAGE_RENDER_TARGET_OUTPUT
BufferCount 2
sample count 1
SyncInterval 1 in formal path
```

exact Qt 6.11.1 sourceの`QD3D11SwapChain::BUFFER_COUNT`は2である。`createOrResize()`は
`QT_D3D_NO_FLIP`が非0ならlegacy `DISCARD`、それ以外は`FLIP_DISCARD`を選び、通常pathは
`CreateSwapChainForHwnd()`を使う。current artifactのactual `GetDesc1()`も`BufferCount=2`、
`SwapEffect=4`を記録している。環境変数からの再構成ではなくactual descを最終authorityとする。

## 3. Candidate matrix

| candidate | current support | target bind | direct completed count | same causal sample | nonblocking | reliability | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `IDXGISwapChain::GetFrameStatistics` `SyncRefreshCount/SyncQPCTime` | windowed flip-modelなので呼出し形態はsupported | swapchain→containing outputをB0 identityへ照合可能 | **NO**。schedulerが最後にQPCをsampleした時点のpairで、API call時点のcurrent countではない | **NO**。pre/post call時点とscheduler sample pointが同一とは保証されない | YES | **NO**。multi-monitor等でunreliable | **STATIC REJECT** |
| `IDXGIOutput::GetFrameStatistics` | **NO**。full-screen時だけsupported | YES | field自体はrefresh countを含む | current windowed pathでは評価不能 | YES | current path outside support envelope | **STATIC REJECT** |
| `D3DKMTGetScanLine` | desktopからsupported | GDI display name→adapter/LUID/VidPnSourceIdへexact bind可能 | **NO**。`InVerticalBlank`とcurrent `ScanLine`だけ | call時点のphaseは得るがcompleted countを得ない | YES | status取得としてsupported | **STATIC REJECT** |
| `D3DKMTWaitForVerticalBlankEvent` / `Event2` | supported | adapter handle + VidPnSourceIdへbind可能 | **NO**。event wait resultだけ | **NO**。return後sampleはwake boundary。pre-renderで呼べばblocking | **NO** | wait APIとしてsupported | **STATIC REJECT** |

## 4. Candidate details

### 4.1 `IDXGISwapChain::GetFrameStatistics`

flip-model swapchainなので、bitblt-windowed禁止条件には該当しない。actual swapchain pointerもlocal Qt patch経由で
取得可能である。この2点だけならruntime call candidateにはなる。

しかし`DXGI_FRAME_STATISTICS`のcontractは次である。

```text
SyncRefreshCount
  schedulerが最後にQueryPerformanceCounterでmachine timeをsampleしたときのVBlank running count

SyncQPCTime
  そのscheduler sampleに対応するQPC value
```

これは`GetFrameStatistics()`を呼んだpre-render / post-swap時点のcompleted countではない。値が更新された時刻を
QPC nearest、cadence、pollingで推定するとB1禁止事項へ戻る。

さらにMicrosoftはstatisticsがmany multiple-monitor scenariosおよび他のfullscreen appがあるscenarioで
reliableではないと明記する。B0 target identity bindingやoutput migration fail-closeは、APIが返したstatisticsの
reliability保証を新たに作らない。current product/protocolはsingle active monitor専用ではなく、別monitorの存在、
別fullscreen app不在、hardware flip queue挙動をproduct invariantとして固定していない。

したがって今回のtarget workloadでは`reliable` requirementを満たさず**static rejection**とする。single-monitorへ
protocolを縮小して候補を救済しない。これはperformance条件の変更になり、production一般性も失うためである。

### 4.2 `IDXGIOutput::GetFrameStatistics`

target `IDXGIOutput`へB0 chainでexact bindできる。しかしMicrosoft contractはこのmethodをfull-screen mode中だけ
supportedとする。current QRhi pathはHWND windowed flip-modelでありfull-screen exclusiveではない。
`DXGI_ERROR_INVALID_CALL`をretry、swapchain statistics fallback、DWM fallbackで成功扱いしない。

### 4.3 `D3DKMTGetScanLine`

`D3DKMTOpenAdapterFromGdiDisplayName`はB0のexact GDI device nameをadapter handle、adapter LUID、
`VidPnSourceId`へmapできる。返却LUIDとDisplayConfig/DXGI adapter LUID、source idを再照合すればtarget bindingは
構成可能である。

`D3DKMTGetScanLine`が返すのは対象VidPN sourceの`InVerticalBlank`とcurrent scan lineであり、monotonic completed
refresh countではない。呼出しを繰り返してVBlank transitionを数える案はpolling miss、derived cadence、
sequential counterでありauthority requirementに違反する。scan line wrapからcountを推定する案も同じである。

### 4.4 `D3DKMTWaitForVerticalBlankEvent(2)`

adapter / VidPnSourceIdへのtarget bindingは可能だが、APIはvertical blank eventを待ってからreturnするblocking waitで
ある。countやboundary QPCを返さない。render threadでの使用は禁止であり、別threadでreturn後QPC/counterを
publishすればB1で棄却済みのobserver wake semanticsと同一になる。Event2の複数wait object対応はこの欠落を
補わない。

## 5. Multi-monitor reliability contract

exact authorityに対し「通常は正しい」「このmachineでは過去に動いた」を認めない。API specificationがcurrent
support envelopeでreliabilityを否定または保証していない場合は、次でfail-closeする。

```text
API_RELIABILITY_NOT_GUARANTEED_FOR_TARGET_WORKLOAD
  -> candidate STATIC_REJECTED
```

single-monitorだけを有効runとして選別するretry、別monitorをdisableするcapture protocol、failure runだけを
discardする運用は採らない。multi-monitor topologyが無かったことをcaptureで示しても、pre-render/post-swapの
same causal sample defectは解消しない。

## 6. Static verdict

4 candidateすべてが少なくとも`direct counter`または`same causal sample`を満たさず、swapchain statisticsは
追加で`reliable`、output statisticsは`current support`を満たさない。

```text
IDXGISwapChain::GetFrameStatistics   STATIC_REJECTED
IDXGIOutput::GetFrameStatistics      STATIC_REJECTED
D3DKMTGetScanLine                    STATIC_REJECTED
D3DKMTWaitForVerticalBlankEvent(2)   STATIC_REJECTED

EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE
```

したがってB1のphysical join / same-causal-origin / shadow replayは再有効化しない。runtime captureでstatic support
defectを覆す試行も行わない。

## 7. Production correction handoff

production correctionはtarget-output pre-render counter置換系統を終了し、別系統
`B3 Counter-free Required-intent Completion Correction`へ切り替える。後続の
[B3 corrective design](p2-d5-2-b3-counter-free-required-intent-completion.md)では、immutable required-intent
queueのreserve / render-complete / qualified commit-dequeueを第一候補として選定した。B2からは最低限、次を
入力contractとして渡す。

```text
past_source_domain && required_intent_membership
  != successful measurement completion

NULL DWM counterをtarget-output physical counterと呼ばない
observer wake / QPC / cadence / sequential +1を代替counterにしない
required_intent_countとqualified_source_frame_countを分離する
final displayed accountingはformal token -> Present -> FinalState authorityのまま保つ
counterなしでrequired intentをどう発行・失敗終了するかを別designで比較する
```

B3の候補比較には少なくとも「planned required intentをtransportへ発行するcounter-free control」と
「source coverage不足を開始前またはruntime contract failureとしてfail-closeするcontrol」を含める。
これらをtarget physical counterfactualの証明済み結果として扱わない。

## 8. Negative contract

B2ではtestを実装しない。将来のstatic checker/architecture guard向けに次をfreezeする。

```text
NegativeQrhiNativeHandlesInventsSwapchain
NegativeReconstructedSwapEffectTrusted
NegativeRawSwapchainPointerLifetimeIgnored
NegativeBitbltWindowedStatisticsAccepted
NegativeSyncRefreshCountTreatedAsCallTimeCounter
NegativeSyncQpcNearestJoin
NegativeMultiMonitorReliabilityIgnored
NegativeSingleMonitorProtocolNarrowing
NegativeOutputStatisticsAcceptedWindowed
NegativeOutputStatisticsFailureFallsBackToSwapchain
NegativeScanLineTreatedAsRefreshCount
NegativeScanLineWrapSequentialCounter
NegativeScanLineCadenceDerivedCounter
NegativeRenderThreadD3dkmtVblankWait
NegativeD3dkmtWaitReturnQpcAuthority
NegativeD3dkmtObserverPublicationAuthority
NegativeEvent2UserObjectWakeAcceptedAsVblank
NegativeCandidateRetryUntilSuccess
NegativeUnsupportedCandidateEnablesB1Shadow
NegativeExactAuthorityUnavailableReportedAsCausalReject
```

## 9. Primary API references

- [IDXGISwapChain::GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)
- [IDXGIOutput::GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getframestatistics)
- [DXGI_FRAME_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics)
- [D3DKMTOpenAdapterFromGdiDisplayName](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtopenadapterfromgdidisplayname)
- [D3DKMTGetScanLine](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtgetscanline)
- [D3DKMTWaitForVerticalBlankEvent2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtwaitforverticalblankevent2)
- [QRhiD3D11NativeHandles](https://doc.qt.io/qt-6/qrhid3d11nativehandles.html)

## 10. B2 exit

```text
B2 static inventory                 CLOSED
B2 candidate implementation         NOT STARTED / NOT AUTHORIZED
B2 capture                          NOT STARTED / NOT REQUIRED
exact target-output counter         UNAVAILABLE
B1 shadow replay                    NOT EVALUABLE
next production design lineage      B3 COUNTER-FREE CORRECTION / DESIGN CLOSED
production behavior                 UNCHANGED
canonical W3 verdict                UNCHANGED / FAIL
P5-E4                               BLOCKED
```
