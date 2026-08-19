# PreviewEngine 製品契約

- 状態: **Frozen for Phase 5 implementation**
- baseline: `f310e27a27f1d54fb79abe055d81a41ec8c00b39`
- 採用決定: [Preview backend 採用決定](preview-backend-adoption.md)
- 実装計画: [Phase 5 計画](phase5-plan.md)

## 1. 目的

`PreviewEngine`は、Phase 1～4で検証したWindows preview architectureを製品から利用するための
安定した境界である。Project Modelやeditor stateを解釈せず、呼び出し側が解決済みのsourceと
immutable compositionを受け取って再生する。

この文書は製品contractを固定する。C++実装、spike componentのrefactor、未検証能力のqualificationは
別sliceで行う。

## 2. 責務

`PreviewEngine`は次を担う。

- lifecycleと明示的なstate machine
- sourceの登録と削除
- immutable composition snapshotの検証、受理、提示
- play、pause、exact seek
- `IAudioClock` masterによるaudio/video scheduling
- presentation statusとbounded event
- structured error
- GPU adapterとaudio endpointのdevice information
- bounded production telemetry
- worker joinとGPU retirementを含む決定論的shutdown

`PreviewEngine`は次を担わない。

- Project Model、timeline/track model
- editor UI、QML画面構成
- undo/redo
- project serialization
- export、encode、offline render
- effect、transition、keyframeのauthoringまたはgraph
- text/subtitle editor
- proxy生成またはproxy selection
- formal fixture、catalog、probe、ledger、matrix report

## 3. Public type boundary

次は概念上の最小public value typeである。最終的なnamespaceやminor namingはP5-Bで決められるが、
authorityと意味は変更しない。

```cpp
struct PreviewSourceId {
    std::uint64_t value = 0;
};

struct PreviewFrameRate {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

struct PreviewPosition {
    std::int64_t outputFrame = 0;
};

struct PreviewOutputConfig {
    PreviewFrameRate frameRate;
};

struct PreviewEngineConfig {
    PreviewOutputConfig output;
};

struct PreviewSourceDescriptor {
    std::filesystem::path mediaPath;
    bool videoEnabled = false;
    bool audioEnabled = false;
};
```

`PreviewSourceDescriptor`のvalidationを次に固定する。

- `mediaPath`が空ならrejectする
- `videoEnabled == false && audioEnabled == false`ならrejectする
- `PreviewSourceId`はengineだけが発行する
- media openとdescriptor validationが全て成功するまでpublic source IDを返さず、source tableへ成功登録しない
- open/validation failureはfailure `Result`を返し、失敗したsourceを後続APIから参照可能にしない
- source registrationは`ReadyPaused`だけで受理する
- `Playing`中のdynamic source registration、switching、removalは未contractである

public product headerに次を公開しない。

- `AVFrame*`を含むFFmpeg型
- `ID3D11Texture2D*`を含むD3D11 resource型
- `QRhi*`、`QQuickRhiItem*`を含むQt render型
- `SourceDecodeWorker`
- `CompositorCoordinator`
- `CompositorSpikeState`
- internal `SourceGeneration`、`ResourceEpoch`、`CompositionEpoch`
- GPU completion serial、mutex、condition variable

## 4. Output timebase

`PreviewPosition::outputFrame`は`PreviewEngineConfig::output.frameRate`のtimebaseに属する。

```text
time_seconds = outputFrame * denominator / numerator
```

`numerator`と`denominator`はともに0より大きくなければならない。invalidまたはoverflowする設定は
`initialize()`が`UnsupportedCapability`または入力に対応するstructured errorで拒否する。

public contractに暗黙の60 fpsを置かない。現在formalに検証済みのrateは`60/1`であるが、これは
型の上限ではない。P5-Dでは、現行schedulerにある48 kHz / 60 fpsの固定算術をproduct configから
導出する有理数変換へ置き換える。実装時は整数または有理数演算を使用し、長尺でframeがずれる
浮動小数点累積をmaster identityに使わない。

P5-A.1ではこの変更を実装しない。

## 5. Audio configuration

初期product capabilityとして検証・supportするaudio domainは次のとおりである。

- 48000 Hz
- stereo
- float32 internal PCM
- 一つのactive audio source
- WASAPI shared event-driven rendering
- `IAudioClock` master

これはarchitecture上の普遍的な制約ではない。対応能力は`PreviewCapabilities`で報告し、現在の
能力外のaudio source数、sample rate、channel layout、sample formatは
`UnsupportedCapability`としてfail-closedで拒否できる。

format conversionやresamplingを実装都合のfallbackとして黙って有効にしない。変換をsupportする場合は、
入力domain、出力domain、quality、clock/seek semanticsを別途contractで明示する。

初期Phase 5では、最初に登録成功した`audioEnabled == true`のsourceをauthoritative active audio sourceと
する。authoritative audio sourceが登録済みの間、二件目の`audioEnabled == true` sourceの登録は
`UnsupportedCapability`で拒否する。暗黙に先着sourceを切り替えたり、二sourceをmixしたりしない。

`CompositionSnapshot`はvideo compositionだけを表し、audio layer、audio graph、audio source selectionを
表現しない。authoritative audio sourceの切り替えと`Playing`中のdynamic audio source変更は将来の
独立contractとする。

## 6. Source identity

public `PreviewSourceId`は一つのengine session内でengineが発行し、sourceが登録されている間は安定する。
内部video/audio componentのIDやgenerationがopen、seek、flushで変化してもpublic IDは変えない。

engineはprivate mappingを持つ。

```text
PreviewSourceId
  -> optional gpu::SourceId
  -> optional audio::SourceId
```

`SourceGeneration`と`ResourceEpoch`はengine内部のcorrectness identityであり、public APIへ出さない。

source removalは、active compositionまたはacceptedだが未提示のpending compositionがそのsourceを
参照している場合に`InvalidState`で拒否する。Phase 5でsupportするのは安全に参照が外れたsourceの
削除である。authoritative audio sourceを`ReadyPaused`で安全に削除した場合はaudio authorityを空へ戻す。
再生中の一般的なdynamic removalは将来の独立contractとする。

## 7. Immutable composition contract

callerはimmutableなcomposition contentを提出する。callerはpublic revisionを採番しない。

```cpp
struct PreviewNormalizedRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

struct PreviewCompositionLayer {
    PreviewSourceId source;
    PreviewNormalizedRect destination;
    PreviewNormalizedRect sourceRect;
    float opacity = 1.0f;
};

struct CompositionSnapshot {
    std::vector<PreviewCompositionLayer> layers;
};

struct CompositionSnapshotId {
    std::uint64_t value = 0;
};

struct AcceptedComposition {
    CompositionSnapshotId id;
    std::uint64_t revision = 0;
};
```

layer vectorの先頭をback、末尾をfrontとして描画順を定義する。同順位という概念を持たず、vector順で
決定論的にする。現在のcontractが扱うpropertyはsource reference、destination rectangle、source
rectangle、opacity、layer orderだけである。

transition、keyframe、effect graph、S0/S1/S2/S3、formal boundary、fixture catalog、Phase 4 schedule
kindは含めない。

### 7.1 Composition value domain

初期Phase 5では、`destination`と`sourceRect`の両方に同じnormalized containment ruleを適用する。
validな`PreviewNormalizedRect`は全fieldが有限であり、次を全て満たす。

```text
0 <= x < 1
0 <= y < 1
0 < width <= 1
0 < height <= 1
x + width <= 1
y + height <= 1
```

すなわち`[x, x + width]`と`[y, y + height]`がそれぞれ`[0, 1]`に包含され、面積が正である
rectangleだけを受理する。`{0, 0, 1, 1}`はvalidなfull-frame rectangleである。負の座標、1以上の
`x`または`y`、0以下または1より大きい`width`または`height`、境界を越えるrectangle、NaN、正負Infinityは
rejectする。

この初期domainでは、destinationのoutput境界外配置、source境界外sampling、implicit clipping、wrap/repeat
sampling、negative coordinates、overscan coordinatesをsupportしない。これはpublic struct shapeの恒久的な
限界ではなく、初期Phase 5でsupportするsemantic domainである。将来domainを拡張する場合も、silent clampで
旧invalid inputをsuccessへ変えず、独立したcontractで扱う。

上記の数学的domainをauthorityとする。implementationは有限性を確認した後、例えば
`width <= 1 - x`と`height <= 1 - y`のようなoverflow/roundingを考慮した同値な検査を使ってよい。
validation用epsilonを導入して境界外値をacceptしてはならない。

`opacity`は有限かつ`0.0 <= opacity <= 1.0`を満たす場合だけvalidとし、両端を含む。範囲外、NaN、
正負Infinityはrejectし、暗黙clampしない。

`opacity == 0.0`のlayerもsnapshot内に存在するlayerとして扱う。canonicalizationで削除せず、構造比較、
layer count、distinct active video source count、token/no-op判定の全てに含める。したがってopacity 0のlayerと
layer自体が存在しないsnapshotは構造的に異なる。zero-opacity layer elisionを将来導入する場合も、public
composition identityとtoken semanticsを変えないinternal optimizationとして別途証明する。

`CompositionSnapshot::layers`がemptyのsnapshotは、blank/clear frame semanticsが未contractであるため
`CompositionFailure`のrecoverable errorとしてrejectする。rejectはIDとrevisionを消費せず、
`latestAcceptedDesiredComposition`を変更しない。blank/clear compositionは将来の明示的なcontractで扱う。

このdomain freezeはPhase 5のproduct input semanticsを定義するものであり、全normalized rectangleまたは
opacity値についてP1～P4のformal GPU correctnessを新たに証明するものではない。正式validated envelopeと
未検証能力は変更せず、clipping、HDR、effect等を追加でqualificationしたとは扱わない。

### 7.2 IDとrevisionのauthority

`CompositionSnapshotId`と`revision`のauthorityはengineだけが持つ。

engineは概念上、次の二つを区別する。

- `latestAcceptedDesiredComposition`: callerが最後にacceptさせ、今後の提示で実現したいtokenとcontent
- `lastPresentedComposition`: actual render targetへ最後に提示されたtoken

- callerはimmutable contentだけを提出する
- engineはvalidationを全て通したsnapshotだけをacceptする
- no-op比較対象は`latestAcceptedDesiredComposition`だけとする
- contentが`latestAcceptedDesiredComposition`と構造的に同一なら、そのtokenを返すno-opとする
- contentが変わるacceptごとにsession内で一意なIDを発行する
- revisionはengine session内で単調に1ずつ増加する
- rejectされたsubmissionはIDもrevisionも消費しない
- caller入力によるduplicateまたはrevision regressionは構造上発生しない
- supersedeされたaccepted snapshotのtokenは再利用しない
- `PresentedFrameInfo`は実際に提示したframeに対応するaccepted tokenを値で保持する

例えば、`lastPresentedComposition == A`、`latestAcceptedDesiredComposition == B`の状態でAと同じcontentを
再提出しても、old A tokenを再利用しない。この提出はpending BをsupersedeしてAへ戻す新しいacceptであり、
新しいIDと`revision + 1`を発行する。一方、latest desiredがBのときにBと同じcontentを提出した場合だけ、
no-opとしてBのtokenを返す。

internal `CompositionEpoch`はengine-privateのまま維持し、public revisionとは別概念とする。
public tokenをformal state名やinternal epochへ変換できることをcontractにしない。

`submitComposition`の概念上のsignatureは次のとおりである。

```cpp
Result<AcceptedComposition> submitComposition(
    std::shared_ptr<const CompositionSnapshot> snapshot);
```

### 7.3 Structural equality

no-op判定はpointer identityやepsilon比較ではなく、validation後のcanonical product valueの構造比較で行う。
最低限、次を全て比較する。

- layer count
- layer order
- 各layerの`PreviewSourceId`
- destination rectangleの全field
- source rectangleの全field
- opacity

全ての浮動小数値はNaNと正負Infinityをvalidationでrejectする。validationを通った値は`-0.0`を`+0.0`へ
canonicalizeし、それ以外を丸めたりepsilon bucketへ量子化したりしない。canonicalize後の各fieldをvalue
equalityで比較する。epsilon依存のno-op判定、暗黙clamp、caller objectのaddress比較は行わない。

production structural-equality helperとtest expectationを同じ実装から生成しない。testはlayer order、source
ID、各rectangle field、opacityの差を独立した期待値で検査する。

### 7.4 Capability policy

`CompositionSnapshot`は将来のqualificationで型を作り直さないためlayer vectorを持つ。ただしvectorで
あることは任意track数の性能・correctness qualificationを意味しない。

- API typeは二layerへhard-codeしない
- `PreviewCapabilities::maxQualifiedActiveVideoSources`と
  `PreviewCapabilities::maxQualifiedCompositionLayers`を別fieldとして報告する
- P5-D closure時点のproduct wiringの現在値は`maxQualifiedActiveVideoSources == 1`、
  `maxQualifiedCompositionLayers == 1`、`maxQualifiedActiveAudioSources == 1`である
- qualified audio domainは48000 Hz / stereoとして報告する。sample formatのfloat32は
  capabilityの報告項目ではなく`AudioChunk::PcmSample`の型不変条件で保証する (§5)
- `qualifiedOutputFrameRate`は`60/1`、`duplicateSourceLayersSupported`と
  `deviceRecoverySupported`はいずれもfalseである
- capabilityを超えるsnapshotはaccept前に`UnsupportedCapability`で拒否する
- 将来のqualificationはsnapshot formatを変えずにcapabilityを増やせる

source countはsnapshot内のvideo-enabled registered sourceを参照するdistinctな`PreviewSourceId`数、layer
countは`layers.size()`として独立に数える。opacity 0のlayerも両方のcount対象である。video-enabledでない
sourceはvideo compositionへ参加できない。同一sourceを複数layerへ重複配置する能力はformalにqualification
されていないため、初期capabilityでは`UnsupportedCapability`で拒否する。将来qualificationしてもpublic
snapshot formatを変更する必要はない。

二source/二layerはP5-Eでproduct wiringとformal qualificationを完了した後にcapabilityを引き上げる。
型が将来の値を表現できることを、現在利用可能な能力として報告しない。

### 7.5 Composition validation order

`submitComposition()`は次の概念順で、accept前に全validationを行う。

1. snapshot pointerがnon-nullであること
2. layer countが1以上であること
3. layer countがcurrent capability以内であること
4. 各sourceが登録済みかつvideo-enabledであること
5. duplicate-source policyを満たすこと
6. distinct source countがcurrent capability以内であること
7. 各`destination`がvalidであること
8. 各`sourceRect`がvalidであること
9. 各`opacity`がvalidであること
10. `-0.0`を`+0.0`へcanonicalizeすること
11. canonical valueを構造比較し、no-opを判定すること
12. contentが変わった場合だけ新しいtokenをacceptすること

unknownまたはvideo-enabledでないsourceは`InvalidSource`、capability超過とduplicate sourceは
`UnsupportedCapability`、null/empty snapshotとinvalid rectangle/opacityは`CompositionFailure`とする。
これらは全てsubmission前validationの`Recoverable` errorである。validation failureはstate/content/tokenを
変更せず、IDもrevisionも消費しない。

## 8. Public API shape

非同期操作の意味を名前で明確にするため、shutdownは`requestShutdown()`とする。

```cpp
class PreviewEngine {
public:
    Result<void> initialize(
        const PreviewEngineConfig& config,
        std::shared_ptr<PreviewEventDispatcher> dispatcher);

    Result<void> attachEventSink(std::weak_ptr<PreviewEventSink> sink);
    Result<void> detachEventSink();

    Result<PreviewSourceId> addSource(const PreviewSourceDescriptor& descriptor);
    Result<void> removeSource(PreviewSourceId source);

    Result<AcceptedComposition> submitComposition(
        std::shared_ptr<const CompositionSnapshot> snapshot);

    Result<void> play();
    Result<void> pause();
    Result<void> seek(PreviewPosition target);

    PreviewStatus status() const;
    PreviewCapabilities capabilities() const;
    PreviewTelemetry telemetry() const;
    PreviewDeviceInfo deviceInfo() const;

    Result<void> requestShutdown();
};
```

`Result<void>`は操作要求の同期validationと受理だけを表す。`seek()`、`requestShutdown()`の完了を
表さない。

### 8.1 `Result<T>` semantics

P5-Bで採用するconcrete typeによらず、`Result<T>`の意味を次に固定する。

- successは`T`のvalue、または`Result<void>`のsuccessを持つ
- failureはstructured `PreviewError`を持つ
- expected validation failureとrecoverable control rejectionをexceptionで表現しない
- rejectされたcontrol operationは、そのoperationについて文書化されたstate mutationを行わない
- fatalな非同期runtime failureは同期returnだけでなくeventとstatusにも反映する

C++20で`std::expected`相当を自作するか、別のimplementation typeを使うかはP5-Bで決める。

## 9. Render-device attach handshake

logical engine initializationとnative render device attachmentを二段階に分ける。

### 9.1 Stage 1: logical initialization

control/UI threadが`PreviewEngine::initialize()`を呼ぶ。configとevent contractをvalidationし、成功時に
stateを`WaitingForRenderDevice`へ進める。この時点ではnative D3D11 deviceはまだ存在しなくてよい。

`initialize()`はtransactionalである。configまたはdispatcherのvalidation、あるいはlogical initializationが
失敗した場合は、途中で確保したlogical resourceを解放し、stateを`Uninitialized`へrollbackする。失敗した
`initialize()`はdispatcher、event sink、render binding、worker、device referenceを保持せず、structured
`PreviewError`を返す。このrollback後のengineはdestruction-safeであり、破棄のために
`requestShutdown()`を呼ぶ必要はない。

### 9.2 Stage 2: private render attachment

Qt render threadでのみ、private/internal `PreviewRenderPort`または`PreviewRenderBridge`が概念上の
次操作を呼ぶ。

```cpp
attachRenderDevice(RenderDeviceBinding binding);
```

これはproduct public APIではない。`RenderDeviceBinding`がD3D11 native typeを含む場合も、
`src/app/preview`とengine内部実装の境界から外へ公開しない。

attachmentのcontractは次のとおりである。

- Qt render bridgeだけが呼ぶ
- render-thread affinityを持つ
- `SharedD3D11Device`はnative device/contextをvalidationし、成功時にAddRefして保持する
- Qtがnative objectの起点を所有し、engineはvalidated COM referenceを所有する
- adapter/device identity mismatchはfail-closed
- attach成功で`ReadyPaused`へ遷移する
- 同一bindingのduplicate attach、異なるdeviceの再attachは拒否する
- `play()`と`seek()`は`ReadyPaused`より前に`InvalidState`で拒否する
- device recovery/re-attachはこのcontractに含めない

## 10. State machine

製品stateを次に固定する。

| state | 意味 |
| --- | --- |
| `Uninitialized` | `initialize()`前 |
| `WaitingForRenderDevice` | logical initialization済み、render device未attach |
| `ReadyPaused` | device ready、transport停止中 |
| `Playing` | authoritative schedulerが進行中 |
| `Seeking` | exact seek要求を処理中 |
| `ShuttingDown` | shutdown受理済み、join/drain/release中 |
| `Shutdown` | 正常shutdown完了。resourceは解放済み |
| `Error` | session-fatal error後のsafe teardown完了済みterminal state |

主要なlegal operationは次のとおりである。

| operation | legal state | 結果 |
| --- | --- | --- |
| `initialize` | `Uninitialized` | `WaitingForRenderDevice` |
| `attachEventSink` | `WaitingForRenderDevice`, `ReadyPaused`, `Playing`, `Seeking` | sink generation更新。state維持 |
| `detachEventSink` | `Uninitialized`以外 | idempotent detach。state維持 |
| private device attach | `WaitingForRenderDevice` | `ReadyPaused` |
| `addSource` | `ReadyPaused` | state維持 |
| `removeSource` | `ReadyPaused`かつ未参照 | state維持 |
| `submitComposition` | `ReadyPaused`, `Playing` | accepted tokenを発行。state維持 |
| `play` | `ReadyPaused` | `Playing` |
| `pause` | `Playing` | `ReadyPaused` |
| `seek` | `ReadyPaused`, `Playing` | `Seeking` |
| `requestShutdown` | `WaitingForRenderDevice`, `ReadyPaused`, `Playing`, `Seeking` | request accepted、`ShuttingDown` |
| `requestShutdown` | `ShuttingDown`, `Shutdown`, `Error` | idempotent success、state不変 |
| `requestShutdown` | `Uninitialized` | `InvalidState`、state不変 |

未対応stateのoperationはside effectなしで`InvalidState`を返す。`requestShutdown()`は
`ShuttingDown`、`Shutdown`、`Error`で再度呼ばれてもidempotent successを返し、stateとresource ownershipを
変更しない。`Uninitialized`は既にresourceを持たずdestruction-safeであるため、shutdown requestを受理せず
`InvalidState`を返す。

seek開始時に直前transport stateを保持する。seekがactual presentationまで成功した後、元がPlayingなら
`Playing`、元がpausedなら`ReadyPaused`へ戻る。

### 10.1 Seek completion

`seek()`のreturnはrequest acceptanceである。seek completionは、要求した`outputFrame`が、現在accept済みの
source generationとcomposition tokenを使ってactual render targetへ提示された時点である。

decode ready、queue submit、exact pair formation、GPU command issueだけではseek completeにしない。
途中でidentity整合を保証できなくなった場合は`SeekFailure`としてfail-closedにする。

## 11. P5-CとP5-Dのtransport semantics

P5-Cはvideo-onlyの最初のvertical sliceである。P5-Cが提供するpauseは、video scheduling/presentationを
停止または保持するvideo-only behaviorであり、P3で検証したaudio-master pause semanticsの成立を
主張しない。

P5-Dで次を統合した時点から、製品`play()`、`pause()`、`seek()`はauthoritativeなP3-C-2 semanticsを持つ。
P5-Dはsub-sliceへ分割している (phase5-plan.md §9.4)。P5-D3で`seek()`を統合したため、P5-D closure
時点では`play()`、`pause()`、`seek()`のすべてがauthoritativeである。

`seek()`は引数検査をsource/compositionの有無より先に行う。呼び出し側の誤り (負のframe等) は
stateに関わらず`SeekFailure`であり、accepted compositionが無い状態でのseekは`InvalidState`である。
呼び出し側の誤りをstateの都合で別のerrorへすり替えない。

- audio decode
- WASAPI shared event-driven sink
- `IAudioClock` master
- configurable output timebase
- audio/video generation alignment
- actual requested frameによるexact seek completion

P5-Cの結果をaudio-master play/pauseのformal evidenceとして扱わない。

## 12. Shutdown contract

public API名は`requestShutdown()`とする。active stateでのreturnはshutdown request acceptanceであり、
resource解放完了ではない。`ShuttingDown`、`Shutdown`、`Error`でのreturnは既存stateを確認する
idempotent successであり、新しいshutdown処理を開始しない。

正常shutdown completionは次を全て満たし、state `Shutdown`が公開された時点である。

- 全worker threadがjoin済み
- render teardownが完了
- GPU retirementが有限timeout内にdrain済み
- render targetとengine所有device referenceが解放済み
- final status/telemetry snapshotが確定済み

順序を次に固定する。

```text
DisableSchedulers
StopAudioSink
StopAudioDecodeWorker
StopVideoWorkers
DetachRenderVisibleWorkerRefs
VerifyJoins
RequestRenderTeardown
FiniteGpuRetirementDrain
ReleaseRenderTarget/Device
PublishShutdownComplete
```

control/UI threadはshutdown completionをblocking waitしない。render threadは通常render callback内で
non-blocking pollを継続し、有限deadlineを超えたdrainを`ShutdownFailure`として表面化する。

joinを確認できなければrender teardownを要求しない。GPU completionを確認できないpayloadを成功扱いで
releaseしない。

session-fatal errorと正常shutdownで終端を次のように分ける。

```text
normal request:
  ShuttingDown -> frozen safe teardown -> Shutdown

fatal failure:
  record PreviewError
    -> errorOccurredを配送可能にする
    -> transportを停止
    -> ShuttingDown
    -> frozen safe teardown
    -> Error
```

fatality検出時点では`Error`を公開しない。`stateChanged(Error)`はworker join、render teardown、GPU drain、
resource releaseが完了したterminal acknowledgementである。`lastError`はfinal `Error` statusに残す。
これにより、初期化済みsessionのcallerは`Shutdown`または`Error`を観測すればengineを安全に破棄できる。
初期化前またはtransactional rollback後の`Uninitialized`もdestruction-safeである。

### 12.1 Engine lifetimeとdestructor

- active stateから`ShuttingDown`へ遷移させた`requestShutdown()` returnはdestruction-safeを意味しない
- fatal errorの検出または`errorOccurred` deliveryもdestruction-safeを意味しない
- ownerはfinal terminal acknowledgementまで`PreviewEngine`を保持する
- destruction-safeなのは`Uninitialized`、`Shutdown`、safe teardown完了済み`Error`である
- `ShuttingDown`でのidempotent `requestShutdown()` successはdestruction-safeを意味しない
- `Shutdown`または`Error`でのidempotent successは既存のdestruction-safe状態を変更しない
- destructorをUI thread blocking shutdownとして使用しない
- destructorへ通常shutdown semanticsを暗黙移譲しない

destruction-safeでない状態のdestructor misuseをどうfail-closedに表面化するかはP5-Bで決めるが、destructorが
worker joinやrender drainをUI thread上で待って正常終了に見せてはならない。

## 13. Threadingとevent delivery

### 13.1 Thread affinity

- control methodはengineを作成したcontrol/UI thread affinityを持つ
- `status()`、`capabilities()`、`telemetry()`、`deviceInfo()`はthread-safeなvalue snapshot read
- video/audio workerはsource-local queueとdecoderを所有する
- WASAPI render threadはendpoint writeと`IAudioClock` query/updateを担う
- Qt render threadはsnapshot adoption、exact pairing、GPU composition、presentationを担う
- callbackをengine内部lock保持中に実行しない

### 13.2 Dispatcher lifetime

callerは`initialize()`へnon-nullのdispatcherを渡す。engineはdispatcherを`shared_ptr`でterminal
acknowledgementまで保持し、dispatcherのtarget lifetimeもその間有効でなければならない。event sinkは
`attachEventSink()`で`weak_ptr`として登録する。attach/detachはcontrol/UI thread onlyである。
`initialize()`が失敗して`Uninitialized`へrollbackした場合はdispatcherを保持しない。

sinkが失効していればeventを配送しない。`detachEventSink()` return後に新しいcallbackが開始しないことを
保証し、pending closureは実行直前にsink generationとweak ownershipを再検査する。sinkが無い、失効した、
またはdispatcherがeventを配送できない場合も、engine内部のmedia correctness、state transition、safe
teardownは進行できる。event delivery failureをsoftware decode等のmedia fallbackへ変換しない。callerは
thread-safeなstatus snapshotをpollしてterminal stateを確認できる。

### 13.3 Orderingとbackpressure

最低限のproduct eventは次のとおりである。

- `stateChanged`
- `positionChanged`
- `framePresented`
- `errorOccurred`
- `deviceChanged`

event mailboxは有限容量とし、unbounded historical queueを持たない。

- state/error/device eventは発生順を維持する
- terminal event用の予約slotを持つ
- positionは未配送のlatest value一件へcoalesceできる
- framePresentedは未配送のlatest bounded metadata一件へcoalesceできる
- dispatcherへ同種taskを無制限にpostしない
- non-coalescible eventの容量不足を黙ってdropしない
- QMLへper-frame layer graph、texture、frame lifetime objectを渡さない

正常shutdownでは最終`stateChanged(Shutdown)` acknowledgement後にcallbackを発行しない。fatal pathでは
safe teardown後の最終`stateChanged(Error)` acknowledgement後にcallbackを発行しない。sink detach後も
同様である。

## 14. Presented frame metadata

`PresentedFrameInfo`はbounded valueである。

```cpp
struct PresentedFrameInfo {
    std::uint64_t presentationSequence = 0;
    PreviewPosition position;
    AcceptedComposition composition;
    std::uint32_t activeLayerCount = 0;
};
```

internal source generation、resource epoch、composition epoch、GPU serial、layer vectorを含めない。
formal verificationがそれらを必要とする場合はproduct callbackではなくtest instrumentationを使用する。

## 15. Error model

```cpp
enum class PreviewErrorCategory {
    InvalidState,
    InvalidSource,
    UnsupportedCapability,
    DecodeFailure,
    AudioFailure,
    DeviceFailure,
    SeekFailure,
    CompositionFailure,
    ShutdownFailure,
};

enum class PreviewErrorSeverity {
    Recoverable,
    FatalToSession,
};
```

errorはcategory、severity、operation、任意のpublic source ID、人間向けdetailを持つ。FFmpeg HRESULTや
COM pointerをpublic identityとして要求しない。必要なnative codeはbounded diagnostic fieldとして
保持できる。

| condition | severity |
| --- | --- |
| illegal operation、unknown source、capability超過 | `Recoverable`。状態を変更せず拒否 |
| source open前のdecode failure | 原則`Recoverable`。sourceを登録しない |
| active sourceのdecode failure | `FatalToSession`。exact pathを維持できない |
| active audio pathのfailure | `FatalToSession` |
| device lost/mismatch | `FatalToSession` |
| seek入力validation failure | `Recoverable` |
| generation変更開始後のseek failure | `FatalToSession` |
| composition採用前validation failure | `Recoverable` |
| GPU composition issue/completion failure | `FatalToSession` |
| join/drain/release invariant failure | `FatalToSession` |

## 16. Fallbackとdevice lost

採用済みpathで次へ黙ってfallbackしない。

- software decode
- CPU composition
- full-frame CPU readback
- QPC master
- stale/latest frame reuse
- old composition reuse
- automatic device reopen

video output frameのmaster選択は`audio::acceptsVideoMasterSource()`に一本化し、product schedulerが
判定を再実装しない。active audio sourceを登録している間にQPC/wall-clock masterが選ばれた場合は、
その時点でscheduleを止め、`AudioFailure` / `FatalToSession`としてfail-closedにする。退避回数は
`videoMasterQpcFallbackCount`として報告し、audio master projectionの失敗回数
(`audioMasterProjectionFailureCount`) と合算しない。audio sourceを登録していないvideo-only経路
(P5-C) のwall-clockはqualified masterであり、退避ではないのでこのcounterには数えない。

device lostを検出したらerrorをrecordし、transportを停止して`ShuttingDown`へ遷移する。worker joinと
GPU/device resourceのsafe teardown完了後にだけterminal `Error`を公開する。自動reopen/recoveryは将来
workであり、最初のproductization sliceに含めない。

## 17. Bounded telemetry

`PreviewTelemetry`は現在値とbounded counterだけを持つ。

- presented frame count
- aggregate drop countとbounded reason counters
- audio underflow count
- decode failure count
- sourceごとのcurrent queue depth
- GPU retirement current/peak depth
- last error一件
- current adapter
- current audio endpoint
- current state
- current/last-presented position
- latest accepted desired composition token
- last presented composition token

次はproduct telemetryから除外する。

- formal display ledger
- A/V deltaの全履歴
- render timingの全履歴
- marker/transition probe vector
- fixture/catalog provenance
- measurement baseline
- matrix raw/report state
- native device pointer

`CompositorSpikeState`のlifetime-unbounded vectorをproduct stateへ移さない。

## 18. Device informationとcapabilities

`PreviewDeviceInfo`は少なくともadapterのstable description/LUID相当と、audio endpointのbounded
identity/name/formatをvalue snapshotで返す。native pointerを返さない。

現行`WasapiSnapshot`はendpoint formatを持つがendpoint identity/nameを保持していないため、P5-Gで
追加する。これはbackend再検証を必要とする変更ではなく、bounded metadata surfaceの追加である。

`PreviewCapabilities`は少なくとも次を区別する。

- `maxQualifiedActiveVideoSources`
- `maxQualifiedCompositionLayers`
- `maxQualifiedActiveAudioSources`
- supported output frame-rate domain
- supported audio format/source count
- duplicate source layer supportの有無
- device recovery supportの有無

capability reportは将来増加できるが、未検証能力を推測でtrueにしない。

## 19. Ownership

| owner | owned/borrowed resource |
| --- | --- |
| `PreviewEngine` | source registry、source workers、audio worker、WASAPI sink、audio clock、coordinator、runtime state |
| engine render runtime | `SharedD3D11Device` wrapper、`GpuCompositor`、GPU completion/retirement |
| Qt bridge | QQuickRhiItem/renderer、QRhi object、current render target/RTV |
| `SourceDecodeWorker` | FFmpeg decoder、source-local bounded frame buffer |
| `AudioDecodeWorker` | audio decoder/resampler、bounded audio queue |

render targetはcompose callの間だけengine render runtimeへborrowする。source workerはshared device wrapperを
borrowするため、worker join前にdevice referenceをreleaseしない。

## 20. Component disposition

| disposition | component | 方針 |
| --- | --- | --- |
| Reuse | `FFmpegD3D11Decoder` | hardware decodeとgeneration semanticsを維持 |
| Reuse | `SourceFrameBuffer` | source-local bounded bufferを維持 |
| Reuse | `GpuCompositor` | validated shader/issue/lifetime pathを維持 |
| Reuse | GPU completion/retirement | serial-based retirementとfinite drainを維持 |
| Reuse | `AudioMasterClock` | `IAudioClock` master semanticsを維持 |
| Wrap | `SharedD3D11Device` | private render attachとengine ownershipで包む |
| Wrap | `SourceDecodeWorker` | public source mappingとengine lifecycleで包む |
| Wrap | `AudioDecodeWorker` | public source mappingとengine lifecycleで包む |
| Wrap | `WasapiAudioSink` | structured error/device metadata surfaceで包む |
| Refactor | `ExactFramePairer` | A/B固定接続をproduct snapshotへ適合 |
| Refactor | `CompositorCoordinator` | engine-owned accepted snapshotへ適合 |
| Refactor | audio/video scheduler | formal固定算術をconfigurable timebaseへ適合 |
| Test-only | `CompositorRhiItem` | spike regression用に維持 |
| Test-only | `CompositorSpikeState` | product dependencyを禁止 |
| Test-only | Phase 4 catalog/reference/probes | formal verification用に維持 |

ここでいうRefactorはexact-pair、generation validation、composition identity、GPU lifetime semanticsを
維持したまま所有境界を変えることである。ゼロから別実装を作り、既存検査を迂回する意味ではない。

## 21. Contract invariants

Phase 5実装は最低限次を守る。

- public output timeは明示的な有理数frame rateに属する
- accepted snapshot revisionのauthorityはengineだけが持つ
- composition no-opはlatest accepted desiredだけと比較する
- destinationとsourceRectを同じinitial normalized containment domainで検査する
- opacity 0のlayerを構造比較とsource/layer countから除外しない
- empty compositionとinvalid rectangle/opacityをaccept前にrejectし、ID/revisionを消費しない
- active source countとcomposition layer countを別capabilityとして検査する
- capability外をtype shapeだけでsupport済みと扱わない
- render device attachmentはprivate/render-thread-only
- seek completionをrequest acceptanceまたはdecode readyで代用しない
- shutdown completionを`requestShutdown()` returnで代用しない
- `initialize()` failureはtransactionalに`Uninitialized`へrollbackし、dispatcher/resourceを保持しない
- `requestShutdown()`のstate別idempotencyをstate tableどおりに維持する
- terminal `Error`をsafe teardown完了前に公開しない
- `WaitingForRenderDevice`、`ReadyPaused`、`Playing`、`Seeking`、`ShuttingDown`ではengineを破棄しない
- worker join前にrender/device teardownしない
- GPU completion前にframe lifetimeをreleaseしない
- internal generation/epochをpublic APIへ漏らさない
- event/telemetry/historyをunboundedにしない
- fail-closed pathを暗黙fallbackで緩めない
