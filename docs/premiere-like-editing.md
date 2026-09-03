# Premiere / Final Cut 風の編集操作へ寄せた変更

記述の分類は `docs/phase0-findings.md` と同じ印を使う。

| 印         | 意味                                               |
| ---------- | -------------------------------------------------- |
| `[事実]`   | 実際に実行して観測した。再現手順を併記する         |
| `[推測]`   | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない       |
| `[当時]`   | その時点の実装。現行 API とは異なる                |
| `[exit]`   | exit criteria への影響                             |

## この文書の読み方

§1〜§10 は最初の実装、§11 以降はレビュー指摘への対応を**時系列で追記**している。
API の形は後の節が前の節を上書きする。`[当時]` を付けた記述は履歴として残しており、
現行 API ではない。

現時点の output frame rate / capability の contract は **§14.1** が最新である。

## 1. Project schema 3 と `.mvm`

`[事実]` `Project` は video / audio の track 列を持つようになった。
clip は `TrackRef { kind, index }` で track を指す。

```cpp
struct Track { std::string name; bool muted; };
struct TrackRef { TrackKind kind; int index; };
struct Project {
    int schemaVersion = 3;
    std::vector<Track> videoTracks;  // index 0 が最下層 (V1)
    std::vector<Track> audioTracks;
    ...
};
```

video / audio を別 vector にしたのは、片方へ track を足したときに
もう片方の clip の index を振り直さずに済ませるためである。

`[事実]` 保存形式は schema 3 で、`"format": "mvm-project"` marker を必須にした。
拡張子だけを根拠に `.mvm` と判定しない。marker が無いファイルは読み込みを拒否する
(`tests/harness/test_timeline_edit.cpp` の `stranger.mvm` が negative test)。

`[事実]` **schema 2 の Project は読めない。** 互換分岐は残していない
(AGENTS.md「後方互換のための分岐を残さない」)。schema 2 のファイルを開くと
「schema 3 の .mvm を開いてください」で失敗する。
`tests/harness/test_two_track_project.cpp::testSchemaIsFailClosed` が
これを negative test として固定している。

`[事実]` `track_kind` / `track_index` は clip の必須 field である。
欠損時に V1 を仮定しない。旧 schema 2 の `video_track` 欠損は V1 として読んでいたが、
この暗黙の既定値は廃止した。

## 2. timeline frame rate を 60/1 以外にできるようにした

`[事実]` **measured と configurable を別の表に分けた。** 受理できることを
qualification と読み替えないためである。

```text
core::measuredOutputFrameRates()      60/1                      実測済み
core::configurableOutputFrameRates()  24/1, 24000/1001, 25/1,   設定可能
                                      30/1, 30000/1001, 50/1,
                                      60/1, 60000/1001
```

`[事実]` `CheckedOutputTimebase::createQualified` は **60/1 のみ**という元の契約に戻した。
preview engine は新設した `createConfigured` を使う。
`createConfigured` を通ったことは「qualify された」ことを意味しない。

`[事実]` `PreviewCapabilities` は `configuredOutputFrameRate` (initialize で確定した rate)
を公開する。以前は固定値 60/1 を返しており、source の rate 検査が output と別の rate を
基準にしうる形だった。

`[事実]` 「実測済みか」は当初 `outputFrameRateMeasured` という bool field だったが、
現在は `measuredEnvelope` (実測した構成の組) と derived な
`matchesMeasuredEnvelope()` に置き換わっている。経緯は §12.2 / §13.2 / §14.1。

`[事実]` UI は未計測 rate を選んだとき transport 行へ「(未計測)」を出す。

`[未検証]` 60/1 以外は **実測していない**。measured 表へ昇格させるには
canonical preview workload (p1-matrix.ps1 相当) を取得する必要がある。
`tests/core/test_checked_output_timebase.cpp` は
「configurable だが measured でない rate が createQualified を通らないこと」を
negative test として固定しており、実測なしの昇格はここで落ちる。

`[事実]` clip の fps は従来どおり timeline fps と一致していなければ preview できない。
今回のスコープは「Project の fps を選べる」であり、混在 rate の frame rate 変換ではない。

`[未検証]` 24 / 25 / 30 / 50 fps および 1001 分母の rate で
実際に preview を回した計測はまだ無い。`p1-matrix.ps1` 相当の計測は 60/1 でしか
取っていないため、非 60fps の presentation 品質を「出る」と書いてはいけない。

## 3. preview の layer 上限

`[事実]` `GpuCompositor::composeLayersToTarget` は N layer を描ける実装だが、
`PreviewCapabilities::configuredMaxCompositionLayers` は 2 である。

`[事実]` この 2 が実測済みなのは **組として**である。
`MeasuredPreviewEnvelope` は
「60/1 × video source 2 × composition layer 2 × audio source 1 × 48kHz stereo」
という tuple であり、「2 layer 単独が qualify されている」ではない
(§12.2 を参照)。

`[事実]` そのため `mapTimelinePreviewFrame` は、同一 frame で 3 本以上の
video track に clip が載っている場合を成功にせず、
`kMaxPreviewVideoLayers` を超えた旨のエラーで返す。
track を 3 本以上作ること自体は編集モデル・保存・UI のいずれでも可能である。

`[事実]` 書き出しも同様に fail-closed である。MLT 側の経路
(`mvm_mlt_export_two_track`) が持つ playlist は 2 本なので、
video track index 2 以上に clip があると `mapTimelineExportPlan` が失敗する。
audio clip を含む timeline の書き出しも現時点では拒否する。

`[exit]` 「track を任意に追加できる」は編集モデルとして満たしているが、
preview 2 layer / export 2 track / export audio 無しが現在の上限である。

## 4. audio

`[事実]` `Project` に audio track と `TimelineClipKind::Audio` を追加した。
audio clip は audio track にしか載らず、逆も検証で拒否する。

`[事実]` audio 素材には映像 fps が無いため、
audio clip の source frame domain には **Project の timeline fps を採用**し、
尺だけを `duration_sec` から求めている (`probeAudioMedia`)。
frame 算術を 1 種類に保つための選択であり、素材側に fps があるという主張ではない。

`[事実]` `PreviewSourceDescriptor` に `audioSampleOffset` を足した。

```text
media sample = (output frame を換算した sample) + audioSampleOffset
```

timeline 上の 0 以外の位置に置いた audio clip を鳴らすために必要である。
engine 側で offset を足し引きするのは次の 2 箇所だけである。

- `seekFrameRequest` の `seekTargetSample` 結果 (output -> media)
- audio master clock から output frame を導く `schedulerOutputFrame` (media -> output)

`[未検証]` **offset を入れた状態の A/V 同期は測っていない。**
`tests/audio_preview` の P3 契約は offset 0 の経路しか通っていない。
「ずれない」と書ける根拠はまだ無い。

`[事実]` engine が受理する active audio source は 1 件のままである。
同じ frame で複数の audio track に clip が載っていると
`mapTimelinePreviewAudio` が失敗を返す。複数 audio の mixing は未実装である。

## 5. audio meter

`[事実]` `WasapiAudioSink` が endpoint へ送る直前の internal PCM
(48kHz / stereo / float32) から channel ごとの peak を取る。
resample 後の device format ではなく、実際に送る PCM を測っている。

`[事実]` 減衰は render block 側で行う (`kMeterPeakDecayPerBlock = 0.90`)。
UI の polling 間隔 (50ms) で peak を取りこぼさないためである。
停止中も block ごとに 0 へ落とす。

`[事実]` `PreviewTelemetry` に linear peak を出し、dBFS への換算は controller が行う。
下限は -60 dBFS で床を打つ。

## 6. UI

`[事実]` レイアウトを 左上=エフェクトコントロール / 右上=preview / 右端=audio meter /
下=timeline に変更した。

`[事実]` 数値フィールド (`DragNumberField.qml`) は左右ドラッグで値が変わる。
ドラッグ中は `commit=false` で preview だけを更新し、離したときに `commit=true` で
Project へ保存する。ドラッグの 1 px ごとに Project を書き直さないための区別である。

`[事実]` timeline の操作:

| 操作                   | 割り当て                             |
| ---------------------- | ------------------------------------ |
| ホイール               | 横スクロール                         |
| Shift + ホイール       | 高速横スクロール (5 倍)              |
| Alt + ホイール         | ズーム。カーソル下の frame を固定    |
| ルーラー上の drag      | スクラブ                             |
| 再生ヘッドの drag      | スクラブ                             |
| clip の無い場所を右クリック | リップル削除メニュー             |

`[事実]` スクラブは drag の 1 移動ごとに seek せず、40ms 間隔で最新位置だけを処理する。
seek は engine の `Seeking` state を挟むため、coalesce しないと詰まる。
playhead の表示位置だけは即時に動かす。

`[事実]` リップル削除は track ごとである。`gapAt` が返す
「clip が無く、後ろに clip がある」区間を閉じ、その track の後続 clip だけを左へ詰める。
終端より後ろの空白は詰める対象が無いので found=false として拒否する。

`[事実]` ミュートボタンは各トラックヘッダの左端にある。
mute した video track は「黒を合成する」のではなく layer から外す。

`[事実]` UI の形は `tests/gpu_preview/test-m7b4-timeline-ui-architecture.ps1` が
文字列契約として固定している。track 数を固定する旧実装
(`model: ["V2", "V1"]`, `videoTrack === 1`) が復活したら失敗する。

## 7. 既存の失敗 (この変更とは無関係)

`[事実]` 変更前の HEAD を `git stash` して同じ build directory で測ったところ、
次の workstation ラベルの smoke test は **変更前からこの開発機で失敗している**。

```text
preview_engine_p5c_product_smoke      Exit code 0xc0000409 / terminate called without an active exception
preview_engine_p5d_audio_master_smoke 40 秒 timeout (stage=1 state=3 requests=0 decodeReady=0)
preview_engine_p5e_product_smoke      30 秒 timeout
p2_vblank_authority_probe_live        vblank_ring_overflow_count=13568349 / cadence 不一致
p2_present_id_oracle_live             probe=4 checker=1 (correctness verdict が PASS でない)
```

`[事実]` 変更後の full CTest は **1371 件中 1331 件通過・40 件失敗**で、
失敗はすべて `workstation` ラベルの p5c / p5d / p5e / p2 live 群である。
上記 5 件を HEAD で個別に測って同一症状であることを確認した。
残り 35 件は同じ実行ファイルに flag を変えて渡しているだけの派生である。

`[事実]` この群 (`-E "p5c|p5d_|p5e|p2_vblank_authority_probe_live|p2_present_id_oracle_live"`)
を除外して実行すると **1328 件中 1328 件通過・失敗 0** である
(実行時間 1413 秒)。

同じ subset を変更後に走らせても症状は同一で、
`preview_engine_p5b_unit` と `preview_timebase_p5d1_unit` は変更前後とも扱いが変わっている
(下記) 以外は一致した。

`[推測]` `decodeReady=0 / requests=0` のまま `Playing` で止まっているため、
decode worker が最初の frame を出せていない。素材か GPU decode 側の問題に見えるが、
この変更の範囲外なので原因追跡はしていない。

`[未検証]` 別の開発機で同じ失敗になるかは確認していない。

## 8. contract test の更新

`[事実]` 60/1 固定を前提にしていた次の 2 つは、新しい contract へ書き換えた。
以下は現行 API での記述である (途中の版については §12.2 / §13.2 / §14.1 を参照)。

- `tests/core/test_checked_output_timebase.cpp`
  measured / configurable / どちらでもない rate を **literal で列挙**して
  受理・拒否を検査する。実装の `isMeasuredOutputFrameRate` /
  `isConfigurableOutputFrameRate` は呼ばない。呼ぶと表を壊した変更を
  テストが追認してしまう。
- `tests/preview_engine/test_preview_engine.cpp`
  拒否側の rate を 24/1 から 48/1 へ変えたうえで、configurable な各 rate で
  `initialize` が成功し、`capabilities().configuredOutputFrameRate` が
  **その rate を公開する**ことと、source rate 検査がその rate を基準にすることを
  検査する。あわせて `matchesMeasuredEnvelope()` が 60/1 以外で false になること、
  `measuredEnvelope` が initialize した rate へ追従しないことを固定する。

## 9. GUI の起動確認と、preview が黒いままである件

`[事実]` `smoke.mvm` (V1 に 1080p60 h264、V2 に overlay、A1 に wav) を開いて起動確認した。
track header (mute ボタン付き)・3 本の track・clip の配置・ruler・playhead・
audio meter・エフェクトコントロールはいずれも描画される。

`[事実]` **preview surface は黒のまま**である。status は
「smoke 1080p60 を表示しています」となり seek 自体は受理されている。

`[事実]` これは変更前から同じである。HEAD を stash してビルドした GUI で
schema 2 の同等 Project (同じ 1080p60 h264 clip) を開いても preview は黒だった。

`[事実]` 素材そのものの decode は通る。`gpu_decode_v1080p60_h264` は同じ full CTest 実行で
Passed である。つまり D3D11VA decode ではなく、preview engine の
frame 提示経路の問題であり、§7 の `preview_engine_p5c/p5d/p5e` の失敗と同じ範囲にある。

`[未検証]` 原因は追っていない。この変更の範囲外である。

## 10. Qt Quick Controls の style

`[事実]` native (Windows) style は `background` / `contentItem` の差し替えを
**警告を出して無視する**。track の mute 状態を背景色で出しているため、
native style のままだと mute が視覚的に分からない。

`[事実]` `main.cpp` で `QQuickStyle::setStyle("Basic")` を
`QQmlApplicationEngine::load()` より前に設定した
(AGENTS.md「起動時 configuration の確定順序」)。これで customization 警告は消える。

## 11. レビュー指摘への対応

`[事実]` 静的レビューで挙がった P1 3 件・P2 5 件・P3 1 件へ次のとおり対応した。

| 指摘 | 対応 |
| ---- | ---- |
| 未計測 rate を qualified authority へ昇格していた | measured / configurable を別表に分離 (§2)。`createQualified` は 60/1 のみへ戻し、`createConfigured` を新設 |
| 1001 分母 Project で Manim clip の fps が一致しない | Manim へ渡せる fps は整数のみ。丸めて「対応した」ことにせず、`timelineFpsDen != 1` の Project では生成を fail-closed にした |
| audio source の再利用条件が clip ID だけ | `AudioSourceIdentity` (clipId / sourceInFrame / timelineStartFrame / source fps / timeline fps) を identity にし、どれか 1 つでも変われば remove/add する |
| Project fps 変更で既存 audio の source domain を読み替える | `audioPreviewSampleOffset` が source と timeline を**別 timebase**で sample 化。さらに `setTimelineFrameRate` は clip がある Project では拒否する |
| audio duration の `floor` で末尾が切れる | `audioSourceFrameCount` で `ceil`。1 frame 未満の素材も 1 frame clip として保持する |
| `seekFrameRequest` 失敗時に source を rollback していない | rollback を追加。removeSource が拒否された場合は retirement queue へ回して再試行する |
| track 削除後に preview cache を更新していない | `removeTrack` で pause → save → engine reset → 現在位置で再構築。UI 側も再生中は削除ボタンを無効化 |
| `commit=false` が live Project を書き換えていた | `previewEffectsOverride_` を新設し、drag 中は Project を触らない。`DragNumberField` に `onCanceled` を足し `cancelEffectPreview()` を呼ぶ |
| meter の停止時 decay が render callback 依存 | `pause` / `resetForSeek` / `stop` で peak を確定的に 0 にする |
| canonical fps と ComboBox の equality 不一致 | `validateTimeline` が約分済み pair だけを受理する。Project に 120/2 は保存できない |

`[事実]` 追加した regression test:

- `tests/harness/test_timeline_preview_mapping.cpp`
  audio offset の source/timeline timebase 分離、clip 移動・左 trim で offset が
  変わること、素材固有 fps と Project fps が違う場合、`ceil` 換算、mute、
  layer 上限、audio 重なり拒否。
- `tests/harness/test_timeline_edit.cpp`
  measured / configurable の分離、canonical fps 必須、
  clip がある Project の fps 変更拒否。
- `tests/core/test_checked_output_timebase.cpp`
  configurable だが measured でない rate が `createQualified` を通らないこと。

## 12. 再レビュー指摘への対応 (P2 2 件)

### 12.1 audio / video preview transaction の非対称性

`[事実]` 変更前は `syncPreviewSourcesAt()` が **先に audio を remove/add してから**
video source と composition と seek を処理していた。そのため後段が失敗すると
「video は旧状態、audio は新 source」という部分 commit が起きえた。

`[事実]` 失敗しうる操作を先に済ませ、後戻りできない audio の差し替えを最後へ寄せた。

```text
video prepare (addSource)
  -> composition submit
  -> audio 差し替え (applyAudioSourceFor)
  -> seekFrameRequest
```

`[事実]` engine は active audio source を 1 件しか受理しないため、audio は
remove -> add の順にしかできず prepare/commit へ素直に割れない。
そこで `AudioSwitchUndo` に切り替え前の source と identity を控え、
後段が失敗したら `revertAudioSource()` で元へ戻す compensation transaction にした。
`rollback()` は video source の retire と audio の revert を **同じ境界で**行う。

`[事実]` 戻せなかった場合は黙って成功にせず、`error` 文字列へ追記して呼び出し側の
status に残す。

`[事実]` 検査は `tests/gpu_preview/test-preview-transaction-contract.ps1` に置いた。
engine と GPU device を要求するため controller を駆動する unit test は書けないので、
次を source 上で固定している。

- audio の差し替えが composition submit より後にあること
- audio 差し替え後の `return false` がすべて `rollback()` を通ること
- `rollback()` が video の retire と `revertAudioSource` の両方を行うこと
- `applyAudioSourceFor` が remove の前に undo を控えていること
- `AudioSourceIdentity` が timing に効く入力を全部持っていること

`[事実]` この検査が効いていることは、audio の差し替えを submit より前へ戻した
mutation で実際に失敗することを確認して確かめた
(AGENTS.md「negative test を必ず添える」)。

### 12.2 capability の qualification provenance

`[当時]` `PreviewCapabilities` には `maxQualified*` / `qualifiedAudio*` という field が
あった。実体は「現在の構成で受理できる上限」であって、それぞれが独立に qualify
されているわけではない。24fps で initialize した capability が
`maxQualifiedCompositionLayers = 2` を返すと、その 2 layer の qualification が
60/1 cohort 由来なのか現在の構成由来なのかを型から判別できなかった。
(これらの field は現在すべて `configured*` へ改名されている)

`[事実]` qualification を **envelope (tuple)** として保持する形へ変えた。

以下は `[当時の実装]` である。`matchesMeasuredEnvelope` はこの後
§13.2 で derived getter へ、§14.1 で `hasConfiguredEnvelope` を見る形へ変わっている。
現行 API は §14.1 の記述を参照すること。

```cpp
struct MeasuredPreviewEnvelope {   // 実測した「構成の組」
    PreviewFrameRate outputFrameRate{60, 1};
    std::uint32_t maxActiveVideoSources = 2;
    std::uint32_t maxCompositionLayers = 2;
    std::uint32_t maxActiveAudioSources = 1;
    std::uint32_t audioSampleRate = 48000;
    std::uint32_t audioChannelCount = 2;
};

struct PreviewCapabilities {
    // 現在の構成で受理できる上限。measured とは限らない。
    std::uint32_t configuredMaxActiveVideoSources;
    std::uint32_t configuredMaxCompositionLayers;
    std::uint32_t configuredMaxActiveAudioSources;
    PreviewFrameRate configuredOutputFrameRate;
    std::uint32_t configuredAudioSampleRate;
    std::uint32_t configuredAudioChannelCount;

    MeasuredPreviewEnvelope measuredEnvelope;
    bool matchesMeasuredEnvelope;   // 現在の構成が envelope と組として一致するか
};
```

`[事実]` `matchesMeasuredEnvelope` は rate だけでなく layer 上限・source 上限・
audio domain まで**組として**比較する。軸ごとに独立に合成できると仮定しない。

`[事実]` `measuredEnvelope` は initialize した rate へ追従しない。
`tests/preview_engine/test_preview_engine.cpp` が
「非 60/1 で initialize しても envelope が 60/1 のままであること」を検査している。
追従させる変更を入れるとここで落ちる。

`[事実]` UI の「(未計測)」表示は engine の `matchesMeasuredEnvelope` を authority に
した。Project 側の rate 表だけで判断すると、rate 以外の軸を見落とす。

## 13. 再々レビュー指摘への対応 (P2 1 件 / P3 2 件)

### 13.1 audio rollback が旧 source の exact state を復元していなかった

`[事実]` `AudioSwitchUndo` は旧 source の id と identity だけを控えており、
`revertAudioSource()` は descriptor を **現在の `project_` から作り直して**いた。

`[事実]` そのため move / ripple / left-trim で `timelineStartFrame` や
`sourceInFrame` が変わった後に seek が失敗すると、
「engine 上の source は新しい offset、controller の identity は旧値」という
食い違いが残った。compensation transaction の目的を満たしていない。

`[事実]` `AudioPreviewSource` に `PreviewSourceDescriptor descriptor` を持たせ、
`addSource` へ実際に渡した descriptor をそのまま控えるようにした。
`revertAudioSource()` は `undo.previous->descriptor` をそのまま `addSource` へ渡し、
現在の Project からは作り直さない。identity と descriptor の両方を控えた値へ戻す。

`[事実]` `tests/gpu_preview/test-preview-transaction-contract.ps1` に次を追加した。

- `revertAudioSource` が `addSource(undo.previous->descriptor)` を使うこと
- `revertAudioSource` が `audioDescriptorFor(` を**呼ばない**こと
- `applyAudioSourceFor` が渡した descriptor をそのまま控えること
- `AudioPreviewSource` が descriptor を保持していること

`[事実]` 効いていることは、`revertAudioSource` を旧実装 (現在の Project から
再計算) へ戻す mutation で実際に失敗することを確認して確かめた。

### 13.2 `matchesMeasuredEnvelope` が保存値だった

`[事実]` initialize 時に一度計算して保存していたため、
`PreviewRenderPort::setVideoSourceLimitForTest()` のように configured field を
後から書き換える経路では stale な `true` が残りえた。

`[事実]` `PreviewCapabilities::matchesMeasuredEnvelope()` を **derived getter** にし、
保存値を無くした。派生値が元と食い違う状態そのものを作らない。

`[事実]` `tests/preview_engine/test_preview_engine.cpp::measuredEnvelopeIsDerived` が、
6 つの軸を 1 つずつ崩して一致しなくなること、戻せば一致へ復帰すること
(保存値なら stale のまま) を検査する。

### 13.3 旧 `qualified` 用語の残り

`[事実]` 型は直っていたが文言が旧契約のままだった箇所を落とした。

| 場所 | 変更 |
| ---- | ---- |
| engine の error 文字列 | 「qualified layer countを超えています」→「設定されたcomposition layer countを超えています」など、`configured*` を見ている実態に合わせた |
| `kMaxPreviewVideoLayers` のコメント | 2 は **現在の configured limit** であり、実測済みなのは `MeasuredPreviewEnvelope` の**組**としてである旨を明記 |
| docs §3 | 「`PreviewCapabilities` が qualify しているのは 2 layer まで」→ configured limit と measured envelope の区別を明示 |
| test の変数名 | 非 60fps 群の `qualifiedRates` → `configurableOnlyRates`。実体は `matchesMeasuredEnvelope() == false` の検査である |

## 14. 4 巡目レビュー指摘への対応 (P2 1 件 / P3 1 件)

### 14.1 未初期化 engine が measured 一致になりえた

`[事実]` 一致してしまうのは **`PreviewEngine` が initialize 前に持つ product
capability** である。`PreviewCapabilities` struct 自体の既定値は envelope と
一致しない。主語を取り違えると、この修正が不要に見える。

```text
PreviewCapabilities{} 既定       : 60/1, video 1, layer 1, audio 0, 0Hz, 0ch
PreviewEngine の initialize 前   : 60/1, video 2, layer 2, audio 1, 48kHz, stereo
measuredEnvelope{} 既定          : 60/1, video 2, layer 2, audio 1, 48kHz, stereo
                                   ^^^^ engine 側と同値になる
```

`[事実]` `PreviewEngine::Impl` は construct 時に product の上限
(2 / 2 / 1 / 48000 / 2) を capability へ書き込む。
`configuredOutputFrameRate` は struct 既定の 60/1 のままなので、
initialize を通していなくても envelope と同じ組になる。

`[事実]` そのため tuple equality だけの derived getter では、
`initialize()` を通していない engine が `matchesMeasuredEnvelope() == true` を返した。
controller の `frameRateMeasured()` は engine pointer の存在だけで engine authority へ
切り替えていたため、`initialize()` が失敗した engine でも
「測定済み」と答えうる **実測 authority の false positive** になっていた。

`[事実]` `PreviewCapabilities::hasConfiguredEnvelope` を足した。
これは derived value の cache ではなく「現在構成が確定した」という**一次 state**
であり、§13.2 で避けた stale-derived-value の問題とは別物である。

- `initialize()` の全検証を通った後にだけ `true`
- dispatcher post 失敗の rollback 経路で `false` へ戻す
- `matchesMeasuredEnvelope()` は先頭でこれを見る

`[事実]` `frameRateMeasured()` は Project の rate 表への fallback を廃止した。
fallback があると、engine の初期化に失敗していても
「60fps だから測定済み」と答えてしまう。

`[事実]` regression は次の 4 状態を検査する
(`tests/preview_engine/test_preview_engine.cpp`)。

| 状態 | 期待 |
| ---- | ---- |
| fresh engine (initialize 前) | `false` |
| initialize 60/1 成功 | `true` |
| initialize 24/1 等 成功 | `false` |
| initialize 失敗 / rollback | `false` |

加えて capability 値そのものに対して、
「値が envelope と一致していても `hasConfiguredEnvelope == false` なら measured にしない」
という第 0 状態を検査する。

`[事実]` 効いていることは、`matchesMeasuredEnvelope()` から
`hasConfiguredEnvelope &&` を外す mutation で実際に失敗することを確認して確かめた。

### 14.2 test / smoke に残っていた旧 `qualified` 語彙

`[事実]` 次を `configured` / `measured` へ揃えた。

| 場所 | 変更 |
| ---- | ---- |
| `configurableOnlyRates` 直前のコメント | 「60/1 以外の qualified rate」→「configurable rate (= 未計測)。受理は qualification ではない」 |
| equivalent-rate 部の「qualified audio sourceは1件」 | 「configured audio sourceは1件」 |
| `unqualifiedEngine` / `validButUnqualified` | `unconfigurableEngine` / `validButUnconfigurable` |
| `qualifiedConfig()` | `measuredConfig()` (60/1 は measured 表の唯一の要素) |
| `p5dAudioDomainAndCapabilities` の assertion message | `configured audio channel数` など、見ている field に合わせた |
| `p5d_preview_smoke` の comment / failure message | 「configured capability」。個々の軸を qualified と呼ばない |
| `p5e_preview_smoke` の wall-clock comment | 「正規の master」 |

`[事実]` `validateQualifiedAudioDomain` と `kQualifiedAudioSampleRate` は
48kHz / stereo / float32 という **measured envelope の一部**を指しており、
個別軸を qualified と呼んでいる例ではないため名前を変えていない。
