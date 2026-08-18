# Phase 5 — PreviewEngine Productization Plan

- 状態: **Frozen before implementation**
- validated baseline: `f310e27a27f1d54fb79abe055d81a41ec8c00b39`
- adoption verdict: **ADOPT**
- 採用記録: [Preview backend 採用決定](preview-backend-adoption.md)
- 製品契約: [PreviewEngine 製品契約](preview-engine-contract.md)

## 1. 前提

Phase 1～4のarchitecture validationとD3 regression closureは完了した。

```text
P2-D4-2 FAIL
P3-C-1  FAIL
P2-D5-1 PASS
P3-C-2  PASS
Phase 4 FINAL PASS
```

歴史的なFAILを後続PASSで上書きしない。Phase 4 FINAL PASSは採用architectureの検証完了であり、
full NLE preview subsystemの完成ではない。

P5-Aはarchitecture/product contractの調査だけを行い、source/docsを変更していない。
P5-A.1はその結果を三文書へ固定するdocumentation sliceであり、C++実装を行わない。

結論は次のとおりである。

> PreviewEngine productization can begin without another backend spike.

## 2. Phase 5 goal

Phase 1～4で検証したbackend semanticsを弱めず、spike stateを公開しない製品向け`PreviewEngine`
boundaryへ移す。

Phase 5で閉じるもの:

- product value typeとstructured `Result`
- lifecycle/state machine
- source登録と安全な削除
- immutable composition submission
- configurable output timebase
- video-only vertical sliceからaudio-master play/pause/exact seekまでの段階導入
- private Qt render bridge
- bounded callback、status、device、telemetry surface
- product runtimeとformal instrumentationの分離

## 3. Non-goals

Phase 5では次を統合しない。

- Project Model、timeline/track model
- timeline editor UI
- undo/redo、serialization
- export/encode
- transition、effect、keyframe authoring
- text/subtitle editor
- proxy生成
- device lostからの自動recovery
- 任意track数のqualification
- 複数audio source mixing

Phase 5にproject/timeline modelを入れず、`PreviewEngine`は解決済みcompositionだけを受け取る。

## 4. Preserved semantics

Refactorまたはwrapper導入後も次を維持する。

- D3D11VA hardware frameとQt Quickが同一D3D11 deviceを使う
- per-frame full-frame CPU readbackを行わない
- software decode/CPU compositionへ黙ってfallbackしない
- exact source frameとgenerationが揃わなければ提示しない
- stale/latest frameでmissing frameを代用しない
- `ResourceEpoch`、`SourceGeneration`、`CompositionEpoch`のownerを混ぜない
- GPU completion serialまでframe/SRV/lifetime payloadを保持する
- normal render pathでGPU completionをblocking waitしない
- audio統合後は`IAudioClock`を唯一のmasterとし、QPC masterへfallbackしない
- worker join前にrender/device teardownしない
- shutdown drainは有限timeoutでfail-closedにする

## 5. Component strategy

| disposition | component | Phase 5で行うこと |
| --- | --- | --- |
| Reuse | `FFmpegD3D11Decoder` | 既存hardware decodeをそのまま利用 |
| Reuse | `SourceFrameBuffer` | bounded source-local bufferをそのまま利用 |
| Reuse | `GpuCompositor` | validated GPU composition pathをそのまま利用 |
| Reuse | GPU completion/retirement | serial lifetimeとfinite drainをそのまま利用 |
| Reuse | `AudioMasterClock` | validated clock projectionをそのまま利用 |
| Wrap | `SharedD3D11Device` | private render-device attachとengine ownershipで包む |
| Wrap | `SourceDecodeWorker` | public source mappingとengine lifecycleで包む |
| Wrap | `AudioDecodeWorker` | public source mappingとengine lifecycleで包む |
| Wrap | `WasapiAudioSink` | structured errorとendpoint metadataで包む |
| Refactor | `ExactFramePairer` | product snapshot/source tableへ接続 |
| Refactor | `CompositorCoordinator` | accepted composition tokenへ接続 |
| Refactor | audio/video scheduler | configurable product timebaseから算術を導出 |
| Test-only | `CompositorRhiItem` | P1～P4 regression用に維持 |
| Test-only | `CompositorSpikeState` | product codeから参照しない |
| Test-only | Phase 4 catalog/reference/probes | formal regression用に維持 |

Refactorは既存semanticsの再実装ではない。exact pairing、generation validation、composition identity、
GPU lifetimeを既存testで固定したまま、ownerとinput boundaryを製品向けに変える。

## 6. Slice overview

```text
P5-B  Product contract types / state / events / render seam
  -> P5-C  Qt bridge + one-video executable slice
  -> P5-D  WASAPI / IAudioClock transport + exact seek + timebase
  -> P5-E  immutable multi-layer composition + qualified two-source path
  -> P5-F  product callback / QML / status surface
  -> P5-G  error / device metadata / bounded telemetry
  -> P5-H  spike isolation + full regression closure
```

各sliceは小さく完結させ、次のsliceの機能を先回りして束ねない。

## 7. P5-B — Product foundation

### 7.1 Scope

- `PreviewSourceId`
- `PreviewFrameRate`、`PreviewPosition`、`PreviewOutputConfig`
- `CompositionSnapshot`とengine-owned `AcceptedComposition`
- `PreviewError`、`Result<T>`
- `PreviewStatus`、`PreviewCapabilities`
- `PreviewSourceDescriptor` validation
- explicit state machine
- `initialize(config, dispatcher)`とevent sink attach/detach contract
- bounded/coalescing event mailboxのpure state
- private `PreviewRenderPort` seam
- `PreviewEngine` façadeのlogical initialization

media playback、FFmpeg open、D3D11 attach、QQuick presentationは要求しない。

### 7.2 Required tests

- valid/invalid rational frame rate
- empty path、audio/video両方disabledのinvalid `PreviewSourceDescriptor`
- state transition positive test
- illegal operation per stateのnegative test
- `initialize()` failureがtransactionalに`Uninitialized`へrollbackし、dispatcher/resourceを保持しないtest
- engine-owned revisionがrejectで進まないtest
- latest desiredと同一compositionのno-op token test
- capability超過のnegative test
- destinationとsourceRectの双方について、full-frame `{0, 0, 1, 1}`、interior
  `{0.25, 0.25, 0.5, 0.5}`、edge-touching `{0.5, 0.5, 0.5, 0.5}`を受理するtest
- destinationとsourceRectの双方について、負の`x`/`y`、positive width/heightを伴う`x == 1`/`y == 1`、
  0または負のwidth/height、1より大きいwidth/height、`x + width > 1`、`y + height > 1`をrejectするtest
- destinationとsourceRectの各fieldについてNaN、正Infinity、負Infinityをrejectするtest
- opacityの0、1、interior valueを受理し、0未満、1超、NaN、正負Infinityをrejectするtest
- opacity 0のlayerが構造比較、layer count、distinct source countに残るtest
- empty snapshotを`CompositionFailure`としてrejectするtest
- invalid rect/opacityまたはempty snapshotのrejectがID/revisionとlatest desiredを変更しないtest
- composition validatorを使わず独立したliteralから上記期待値を検査するtest
- event ordering/coalescing/capacity test
- event sink attach/detach lifecycle test
- sink detach後にcallbackが開始しないtest
- dispatcherをterminal acknowledgementまで保持するlifetime test
- sink無しでもstate/correctnessが進行するtest
- fatal detectionから`ShuttingDown`を経てteardown完了後に`Error`となるtest
- active stateから`ShuttingDown`へ遷移した`requestShutdown()` return直後、またはfatal検出直後は
  destruction-safeでないnegative test
- active stateの`requestShutdown()`だけが`ShuttingDown`へ遷移するtest
- `ShuttingDown`、`Shutdown`、`Error`での再呼び出しがidempotent success・state不変となるtest
- `Uninitialized`の`requestShutdown()`が`InvalidState`・state不変となるtest
- `Uninitialized`、`Shutdown`、teardown-complete `Error`がdestruction-safeとなるtest
- public headerにFFmpeg/D3D11/QRhi/spike型が無いcontract test

## 8. P5-C — One-video executable vertical slice

### 8.1 Scope

- thin `PreviewRhiItem` / `PreviewRenderBridge`
- render-thread-only native device attach
- `WaitingForRenderDevice -> ReadyPaused`
- 一つのvideo source登録
- 一layer immutable compositionの受理
- D3D11VA decode
- existing validated D3D11/QRhi presentation path
- video-only scheduling/presentationの開始
- video-only pause/hold
- frozen orderingによるshutdown

audio、WASAPI、`IAudioClock`、audio-master exact seekを含めない。

### 8.2 Pause claim

P5-Cのpauseはvideo scheduler/presentationをstopまたはholdするだけである。P3-C-2で検証した
audio-master pause semanticsの成立を主張しない。P5-Cの測定値やtestをP3 audio evidenceへ読み替えない。

### 8.3 Acceptance

`PreviewEngine`が次を、`CompositorSpikeState`をpublic/product経路へ出さずに実行できること。

1. logical initialize
2. private Qt render-device attach
3. 一つの既存video sourceをregister/open
4. 一layersnapshotをaccept
5. decodeを開始
6. actual render targetへ提示
7. video-only pause/hold
8. worker join、GPU drain、device releaseを順序どおり完了

### 8.4 Required negative tests

- device attach前のplay拒否
- duplicate/incompatible device attach拒否
- unknown sourceを参照するsnapshot拒否
- capability外layer/source拒否
- worker join未確認時のrender teardown拒否
- drain timeoutをsuccessにしないtest

## 9. P5-D — Audio-master transport and exact seek

### 9.1 Scope

- `AudioDecodeWorker`
- `WasapiAudioSink`
- `AudioMasterClock`
- WASAPI shared event-driven rendering
- authoritative product `play()` / `pause()`
- exact `seek()`
- audio/video generation alignment
- configurable `PreviewFrameRate`
- audio sample positionとoutput frame間の有理数変換
- actual requested frame提示によるseek completion

### 9.2 Timebase requirement

現行のformal 48 kHz / 60 fps固定値からproduct schedulerを分離する。

```text
output_time(frame) = frame * denominator / numerator
audio_sample(frame) = output_time(frame) * configured_sample_rate
```

丸め規則を一箇所に固定し、scheduler、seek、statusが同じhelperを使用する。test期待値は実装helperを
呼ばず独立に計算する。

初期qualified rateは`60/1`、audio formatは48000 Hz / stereo / float32である。これ以外をsupportする
根拠が無ければ`UnsupportedCapability`で拒否し、暗黙resample/fallbackで成功へ変えない。

### 9.3 Required tests

- frame/time/sample rational conversionのpositive/negative/boundary test
- audio clock以外のmaster拒否
- play/pause state transition
- seek request acceptanceとcompletionの分離
- decode readyだけではseek completeにならないnegative test
- stale generation提示拒否
- QPC fallback count 0
- audio sink/worker/video worker join ordering

### 9.4 Sub-slice分割

P5-Dは一度に閉じない。§9.1のscopeを次の4 sliceへ分け、各sliceが単独で§14 gateを満たす。

| slice | 範囲 | 状態 |
| --- | --- | --- |
| P5-D1 | `CheckedOutputTimebase`による換算authorityの一本化 | 済 |
| P5-D2 | audio source登録、`AudioDecodeWorker` / `WasapiAudioSink` / `AudioMasterClock`のengine所有、audio-master `play()` / `pause()`、shutdown ordering拡張 | 済 |
| P5-D3 | exact `seek()`、audio/video generation alignment、actual requested frameによるseek completion | 未 |
| P5-D4 | P5-D closure (capability確定、frozen P3 regression再走、§9.3全項目の突き合わせ) | 未 |

#### P5-D1 exit criteria

- frame / media time / audio sampleの有理数変換がoverflow checkedであること
- scheduler、seek、statusが同じ換算へ委譲していること
- test期待値を実装helperから生成していないこと

#### P5-D2 exit criteria

- `audioEnabled == true`のsourceを受理し、2件目を`UnsupportedCapability`で拒否する
- 製品`play()` / `pause()`が`IAudioClock`をmasterとして動作する
- audio master projectionが成立しない場合にQPC masterへ退避せず、`AudioFailure`として
  fail-closedにする (projection失敗が記録され、`Playing`へ戻らない)
- **`pause()`はaudio sinkの停止を確認するまで`ReadyPaused`をcommitしない。** sink停止に
  失敗した場合は`FatalToSession`として`ShuttingDown -> Error`へ落とす。
  「videoは停止したがaudioが鳴り続けている」状態を`ReadyPaused`として公開しない
- **audio sink自身のruntime failureをproduct側が検知して`AudioFailure` /
  `FatalToSession`へ昇格する。** negative testは完成errorをengineへ注入するのではなく、
  `WasapiAudioSink`にdevice failureを起こさせ、通常のpolling経路を通すこと
- shutdown orderingが`DisableSchedulers -> StopAudioSink -> StopAudioDecodeWorker ->
  StopVideoWorkers -> VerifyJoins -> RequestRenderTeardown ->
  FiniteGpuRetirementDrain -> ReleaseRenderTarget/Device -> PublishShutdownComplete`である。
  **最終状態だけでなく、実行されたstepの順序そのものをassertionで固定する**
  (`P5CRuntimeDiagnostics::shutdownSequence`)
- audio sink / audio workerのjoinを確認できなければrender teardownを要求しない
- `PreviewCapabilities`がqualified audio domain (48000 Hz / stereo / float32 / 1 source) を報告する
- qualified audio domainの検査に期待値そのものを渡さない。engine configから導出した実際の値を
  使い、加えてpreroll後に`AudioFrameQueue`のinvalid rejectが0であることでworkerの実出力を確認する
- video-only経路 (P5-C) のregressionを変えない

#### P5-D3 exit criteria

- `seek()`のreturnがrequest acceptanceであり、completionと分離されている
- seek completionがactual requested frameのpresentationであること
- decode readyだけではseek completeにしないnegative testがあること
- stale generationのframeを提示しないこと

#### P5-D4 exit criteria

- §9.3の全項目に対応するtestが存在し、対象0件のgroupが無いこと
- frozen P3-C-2 regressionが変更前semanticsを維持していること

### 9.5 P5-Dで扱わないもの

audio endpointのidentity / friendly nameは`PreviewDeviceInfo`のdevice metadataであり、
§12 (P5-G) で閉じる。P5-Dが`PreviewDeviceInfo`へ載せるのはendpoint formatまでとする。

WASAPI出力段のdevice mix formatへの変換は、P3で検証済みの明示的な出力段変換であり、
§9.2が禁じている「暗黙のfallback」ではない。qualified domainの判定はinternal PCM domain
(48000 Hz / stereo / float32) に対して行う。

## 10. P5-E — Product composition

### 10.1 Scope

- immutable multi-layer `CompositionSnapshot`
- engine-owned acceptance ID/revision
- current qualified two-source composition
- current qualified two-layer composition
- accepted tokenを`PresentedFrameInfo`へ固定
- public source IDからvideo/audio internal IDへのmapping
- active/pending snapshotから参照が外れたsourceのremoval
- `ExactFramePairer`と`CompositorCoordinator`のproduct boundary適合

API typeは二layerへhard-codeしない。runtime capabilityは
`maxQualifiedActiveVideoSources == 2`と`maxQualifiedCompositionLayers == 2`を別々に報告する。
初期capabilityでは同一sourceの複数layer配置を`UnsupportedCapability`で拒否する。

### 10.2 Required tests

- 一layerと二layersnapshot acceptance
- latest accepted desiredと同一contentのstructural no-opでtokenを再利用
- last presented A / latest desired pending B / submit Aでnew tokenと`revision + 1`
- supersedeされたtokenを再利用しない
- content変更時だけrevisionが1増える
- reject時にrevisionが進まない
- max active video source count超過のnegative test
- max composition layer count超過のnegative test
- duplicate source layer policyのnegative test
- layer count/order/source ID/各rect field/opacityのstructural equality boundary test
- NaN/Infinity rejectと`-0.0` canonicalizationのtest
- destinationとsourceRectの双方について、full-frame `{0, 0, 1, 1}`、interior
  `{0.25, 0.25, 0.5, 0.5}`、edge-touching `{0.5, 0.5, 0.5, 0.5}`を受理するtest
- destinationとsourceRectの双方について、負の`x`/`y`、positive width/heightを伴う`x == 1`/`y == 1`、
  0または負のwidth/height、1より大きいwidth/height、`x + width > 1`、`y + height > 1`をrejectするtest
- destinationとsourceRectの各fieldについてNaN、正Infinity、負Infinityをrejectするtest
- opacityの0、1、interior valueを受理し、0未満、1超、NaN、正負Infinityをrejectするtest
- opacity 0のlayerを削除せず、構造比較とsource/layer capability countに含めるtest
- empty snapshotを`CompositionFailure`としてrejectするtest
- invalid rect/opacityまたはempty snapshotのrejectがID/revisionとlatest desiredを変更しないtest
- composition validatorを使わず独立したliteralから上記期待値を検査するtest
- `PresentedFrameInfo`がactual accepted tokenを保持
- unknown/removed source拒否
- active/pending snapshot参照中のremove拒否
- exact pair不足時にold/latest frameを使わないnegative test
- stale composition epochを提示しないnegative test

## 11. P5-F — Product event and Qt/QML surface

### 11.1 Scope

- `stateChanged`
- `positionChanged`
- `framePresented`
- `errorOccurred`
- `deviceChanged`
- Qt/QML adapter
- position/frame event coalescing
- bounded event mailbox
- `PreviewStatus`と`PreviewDeviceInfo`のUI surface
- sink detachとshutdown後callback停止

QMLへtexture、frame lifetime、layer graph、formal ledgerを渡さない。

### 11.2 Required tests

- state/error/device event order
- position/frame latest-value coalescing
- bounded pending event count
- callbackをengine lock保持中に実行しないtest
- sink expiration/detach test
- final Shutdown acknowledgement後にcallbackが無いtest

## 12. P5-G — Error, device metadata, telemetry

### 12.1 Scope

- structured error category/severity/operation/source
- recoverableとfatal-to-sessionの明示
- adapter value information
- audio endpoint identity/name/format
- `PreviewCapabilities`
- `maxQualifiedActiveVideoSources`、`maxQualifiedCompositionLayers`、
  `maxQualifiedActiveAudioSources`の独立report
- bounded `PreviewTelemetry`
- last error一件
- device lost時の`ShuttingDown -> safe teardown -> Error` lifecycle

automatic device recoveryは実装しない。

### 12.2 Required telemetry

- presented count
- drop count/reasons
- underflow count
- decode failures
- current source queue depth
- GPU retirement current/peak
- last error
- current adapter
- current audio endpoint
- state、position
- latest accepted desired/last presented composition token

### 12.3 Explicit exclusions

- formal display ledger
- A/V delta全履歴
- render timing全履歴
- probe result vector
- fixture/catalog provenance
- measurement baseline
- native pointer

### 12.4 Required negative tests

- unsupported capabilityを別pathへfallbackしない
- active decode/audio/device failureをsuccessにしない
- device lost後にplayへ戻らない
- telemetry storageが時間とともに無制限増加しない
- fatal error後もsafe teardown orderingを維持

## 13. P5-H — Isolation and regression closure

### 13.1 Scope

- product targetから`CompositorSpikeState`依存を除去
- product targetからPhase 4 catalog/reference/probe依存を除去
- spike appとformal harnessはtest-support targetとして維持
- product/public header layering検査
- obsoleteなproduct-side bridge storageを整理
- P1～P4とPhase 5のregression closure

spike instrumentation自体を削除するsliceではない。historical contractを再現できるtest infrastructureとして
隔離して残す。

### 13.2 Closure

- ordinary CTestが対象0件でなく通過
- affected targeted testsが通過
- Phase 5 positive/negative testsが通過
- frozen P1～P4 regressionが変更前semanticsを維持
- formal-only type/fixture/reportがproduct dependency graphに無い
- public product headerにFFmpeg/D3D11/QRhi/spike型が無い
- source/docsに未説明のfallbackが無い

## 14. Gate policy

P5-A.1より後の各implementation sliceは次を満たす。

1. 新しいpositive test
2. その検査が無ければ落ちるnegative/fail-closed test
3. 変更componentに対応するtargeted test
4. ordinary CTest
5. closure riskに応じたfrozen P1～P4 regression

対象0件のtest groupを成功扱いにしない。通常testでは`performance`と`stability`を区別し、formal matrixを
ordinary gateへ混ぜない。

過去のthreshold、historical verdict、contract versionを再解釈しない。formal evidenceはhistoricalなまま
保持し、sliceのexit criteriaが明示的に要求した場合だけformal contractを再実行する。

## 15. Formal run policy

- P5-A.1ではREAL/formal matrixを実行しない
- documentation changeだけを理由にP1～P4 formal evidenceを再取得しない
- implementation sliceが共有hot pathまたはfrozen invariantを変更する場合、影響に対応するclosure gateを
  slice開始前に明示する
- formal runが必要な場合も既存thresholdを変更しない
- debug performance値をexit criteriaに使わない

## 16. First executable productization target

最初の実行可能targetはP5-Cとする。意図的に次だけを閉じる。

```text
PreviewEngine logical initialize
  -> private Qt render-device attach
  -> register one video source
  -> accept one-layer immutable composition
  -> D3D11VA decode
  -> existing validated D3D11/QRhi presentation
  -> video-only pause/hold
  -> deterministic shutdown
```

このtargetにaudio、二source composition、Project Model、editing state、effectを同時に統合しない。
目的はAPI、ownership、thread、shutdown boundaryを最初に実行可能な形で閉じることである。

## 17. Documentation and code discipline

- 人間向け文書、comment、errorは日本語で書く
- Qt/QRhi dependencyは`src/app/preview`へ限定する
- `src/media/gpu_preview`と`src/media/audio_preview`は可能な限りQt非依存を維持する
- 同じvalidationやshutdown orderingを複数箇所へ複製しない
- fallbackは明示contractなしに追加しない
- test expectationをproduction helperから生成しない
- Phase 1～4のformal fixture/reportをproduct runtimeへ移さない
- source count、resolution、DPR、audio count、adapterの現行envelopeをpermanent API limitと書かない

## 18. Phase 5 exit boundary

Phase 5完了はfull NLE completionを意味しない。Phase 5で判定できるのは次である。

- validated backendが製品`PreviewEngine`境界から利用できる
- public identity/time/snapshot/error/event semanticsが実装とtestで固定されている
- 一source videoからqualified二source/audio-master pathまで段階的に成立している
- deterministic shutdownとfail-closed policyが製品経路でも維持されている
- product runtimeがspike instrumentationに依存していない

Project/timeline integration、effect authoring、proxy、export、device recoveryは後続の独立計画で扱う。
