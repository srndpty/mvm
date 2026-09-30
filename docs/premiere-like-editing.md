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

`[事実]` (§16.10 で更新) 文字 clip を静止画 layer として合成するため、decode する video source の
上限 (2) と合成する layer の上限を分けた。`configuredMaxCompositionLayers` は 3 になり、
video source は 2 のままである。layer 3 は measured envelope (layer 2) の外である。

`[事実]` (§17 で更新) 上の上限はすべて引き上げた。現在は preview が video source 8 本・合成 layer 16 枚・
audio source 16 本、書き出しは track 数・clip 数とも上限なし、文字はどの映像 track にも置ける。
この節の 2 / 3 という値と export の制約は当時の記録である。

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

## 15. プロジェクトパネル (素材とフォルダ) と schema 4

§1 の schema 3 は、この節の schema 4 で置き換わった。

### 15.1 データと永続化

`[事実]` `Project` に素材 bin を追加した。timeline clip とは独立に存在し、
`mediaPath` で対応づく。シーケンスは単一のまま bin へは出していない。

```cpp
enum class MediaKind { Video, Audio, Image };
struct MediaFolder { std::string id, name, parentId; };      // parentId 空 = root
struct MediaItem {
    std::string id; MediaKind kind; std::filesystem::path mediaPath;
    std::string name, folderId;
    std::int64_t fpsNum, fpsDen, frameCount;  // Video
    int width, height;                        // Video / Image
    int sampleRate; std::int64_t durationSamples;  // Audio
};
```

`[事実]` schema 版の定数は `kProjectSchemaVersion` (project.h) に一本化した。
それまで `validateTimeline` と `project_json.cpp` が別々に `3` を持っていた。

`[事実]` **schema 3 の Project は読めない。** §1 と同じく互換分岐は残していない。
`media_folders` / `media_items` は必須 field で、素材の全 field も種別に関係なく必須である。
種別に該当しない値は 0 で保存し、0 であることを `validateMediaBin` が検査する
(例: 解像度を持つ音声、尺を持つ静止画は拒否)。

`[事実]` `validateMediaBin` は folder / item の id 一意性 (両者で名前空間を共有)、
親 folder の実在、循環、同一ファイルの重複を検査する。
`serializeProjectJson` (= `commitProjectEdit`) と読み込みの両方がこれを通す。
読み込み側は path を project directory へ解決した後に検査する。

### 15.2 編集の規則

`[事実]` 編集は `src/project/media_bin.h` の純粋関数に集約した。
どれも candidate を検証してから置き換え、失敗時は Project を変更しない。

| 操作 | 規則 |
| ---- | ---- |
| 移動 | folder を自分自身・子孫へは入れない |
| 削除 | folder は中身ごと。timeline で使用中の素材を 1 つでも含めば全体を拒否 |
| 追加 | 同じファイル (lexically_normal で比較) の素材は 2 つ持たない |

`[事実]` timeline へ素材を置く経路 (`addVideoClip` / `addAudioClip`、§18.5 以降は `addImageClip` と
`addMediaFileToTimeline` も) は、同じ transaction で bin にも登録する。既に bin にあれば何もしない。
bin 側の操作はすべて `commitProjectEdit` を通るため、1 操作 1 undo になる。

### 15.3 素材種別の判定

§18.1 で置き換えた。以下は MLT の probe だけで判定していた当時の記録である。

`[事実]` 拡張子ではなく MLT の probe で決める (`apps/mvm/media_import.cpp`)。
`mvm_bench probe` で次を観測した。

| 素材 | has_video | frame_count | is_unbounded | 判定 |
| ---- | --------- | ----------- | ------------ | ---- |
| `tests/assets/smoke/png_alpha.png` | true | 2147483647 | true | Image |
| ffmpeg で作った JPEG (testsrc2 1 frame) | true | 1 | false | Image |
| `tests/assets/smoke/wav_48k.wav` | false | 125 | false | Audio |
| ffmpeg で作った AAC の m4a | false | 75 | false | Audio |
| カバーアート (attached_pic) 付き m4a | true (mjpeg) | 180 | false | **Video (誤り)** |

`[事実]` JPEG は image2 demuxer 経由で有限尺 1 frame として返る。
音声を持たない 1 frame の映像には時間方向の長さが無いため静止画とした。

`[未検証]` カバーアート付きの音声素材は動画に分類される。MLT のプロパティに
attached_pic を示す値は無く、手がかりは `meta.media.N.stream.frame_rate = 90000` だけだった。
これで判定すると、本物の Motion JPEG + 音声の動画を誤判定しうるため入れていない。
timeline へ直接追加する既存経路も同じ素材を動画として扱う。

### 15.4 UI

`[事実]` 左上のパネルをタブ化し、「エフェクトコントロール」と「プロジェクト」を
同じ領域で切り替える。境界をドラッグすると左パネルの幅を 240px〜window 幅の 50% で変えられる。

`[事実]` リスト表示の列は 名前 / フレームレート (音声は Hz) / デュレーション / 解像度。
動画の尺は公称 fps (23.976 なら 24) で数える non-drop timecode、音声は `HH:MM:SS.mmm`。

`[事実]` 開閉状態は UI の状態として `MediaBinModel` が持ち、Project には保存しない。
開閉は reset ではなく行の挿入・削除で通知する (reset するとスクロール位置が失われる)。

`[事実]` Delete / F2 はリストに focus があるとき bin 側が受け取る
(`Keys.onShortcutOverride`)。受け取らないと Main.qml の Delete (timeline clip 削除) が発火する。

`[事実]` drop で素材を移動すると model が作り直され、drop 元の delegate が破棄される。
drop ハンドラの途中で破棄されて `ReferenceError: dragProxy is not defined` が出たため、
移動は `Qt.callLater` で drag の後始末の後へ遅らせた。

### 15.5 レビュー指摘への対応

`[事実]` **schema 3 の hard break は維持した。** migration を入れるかは方針として判断し、
AGENTS.md「後方互換のための分岐を残さない」を優先した。手元の開発用 Project は
一度だけ手作業で変換した (元ファイルは `*.schema3.bak`)。

`[事実]` 外部ファイルの drop は `CopyAction` で受理する。以前は
`acceptProposedAction()` で、source が Move を提示するとそのまま Move を受理していた。
mvm は元ファイルを参照するだけなので、Move を返すと source 側が「移動済み」として
元ファイルを後始末しうる。source が Copy を許さない drag は受け付けない。
`test-m7b4-timeline-ui-architecture.ps1` が `acceptProposedAction` を禁止している。

`[未検証]` Explorer からの通常 drag / Shift+drag / Ctrl+drag で、実際に
Explorer 側へ Copy が返ることは実機で確かめていない (合成入力で OLE drag を起こせない)。

`[事実]` 素材の同一性は `src/project/path_identity.h` に一本化した。

| key | 規則 | 使う場所 |
| --- | ---- | -------- |
| `canonicalPathKey` | absolute + lexically_normal + 区切り文字と大文字小文字を揃える。I/O なし | JSON 検証の重複判定、`sameCanonicalPath` |
| `comparePathIdentity` | 両方存在すれば file ID、存在しなければ表記。取れなければ Unknown | 読み込み時の重複判定、timeline 使用中判定 |

JSON 検証を実体に依存させないのは、保存済みの Project が disk 側の変化
(後から hard link が張られた等) だけで開けなくなるのを避けるためである。
hard link 経由の重複登録・使用中判定のすり抜けは、`mediaFileKey` を表記比較へ戻す
mutation で `test_media_bin.cpp` の 4 検査が落ちることを確認した。

`[事実]` 音声の尺は、取り込み時に sample 数が 2^63 未満であることを確かめてから
`llround` する。表示は「秒」と「端数」に分けて換算し、`samples * 1000` を作らない。
動画の公称 fps の切り上げも `num + den - 1` を使わない。

`[事実]` `importMediaFiles` の戻り値は「1 件以上 commit したか」に変えた。
一部が読めなくても読めた分は commit しており、以前はそれでも false を返していた。
false は「Project を変更していない」の意味に保つ。

### 15.6 再レビュー指摘への対応

`[事実]` identity を取れなかった理由を区別する。以前は取得失敗をすべて
「存在しない」と同じ表記比較へ落としており、access denied 等で片方だけ
file ID を取れないと、同じ実体でも別物と判定していた。使用中判定では、
これが「使用中の素材を削除できる」fail-open になる。

| `mvm_file_identity_probe` | 意味 | 比較 |
| ------------------------- | ---- | ---- |
| `OK` → `FileId` | 通常ファイル | file ID で比べる |
| `MISSING` (FILE/PATH_NOT_FOUND のみ) | 何も指していない | 相手と表記が違えば Different |
| `UNAVAILABLE` (それ以外) | 実体が分からない | 表記が同じでなければ Unknown |

`[事実]` `comparePathIdentity` は Same / Different / Unknown を返し、
Unknown の倒し方は呼び出し側が決める。

| 呼び出し側 | Unknown の扱い | 理由 |
| ---------- | -------------- | ---- |
| `mediaItemUsage` (削除の可否) | 使用中か不明として削除を拒否 | 使用中の素材を消さない |
| `findMediaItemByPath` (重複登録) | 一致としない | 重複登録は best effort。拒否すると取り込みが止まる |
| `sameCanonicalPath` → recovery の foreign 判定 | 別 Project として復元しない | 別 Project へ rebase しない |
| Save As の同一先判定 | 同じかもしれないとして外部変更を検査 | 検査なしで canonical を上書きしない |

`[事実]` `sameCanonicalPath` も実体で比べるようにした (以前は表記のみで、
junction・8.3 名・hard link を別物と判定していた)。JSON の検証だけは
`canonicalPathKey` (I/O なし) のまま残す。

`[事実]` Unavailable の再現にはディレクトリを指す素材 path を使う
(存在するが通常ファイルの identity を取れない)。`fileIdentityKey` の
Unavailable を Missing へ戻す mutation で、`test_media_bin.cpp` の
fail-closed 検査 4 件が落ちることを確認した。

`[未検証]` Save As で Unknown になる経路 (開いている Project の identity を取れない)
は controller test で再現していない。

## 16. タイムラインツールパネル

`[事実]` タイムラインパネルの左端に、縦 1 列のツールパネル (`TimelineToolPanel.qml`) を置いた。
ツール名・キー・有効/無効・操作ヒントは `TimelineToolPanel.tools` だけが持ち、
Main.qml のショートカットは `Instantiator` でこの配列から生成する。
キー割り当てを 2 箇所に書かない。

| ツール | キー | 編集関数 (`src/project/timeline_edit`) | 状態 |
| ------ | ---- | -------------------------------------- | ---- |
| 選択 | V | 既存の `moveClips` / `trimTimelineClip` | 実装 |
| 前方 / 後方トラック選択 | A / Shift+A | `clipIdsFromFrame` | 実装 |
| レーザー | C | `splitTimelineClips` (Shift で `clipIdsSpanningFrame` の全 track) | 実装 |
| リップル | B | `rippleTrimTimelineClip` | 実装 |
| ローリング | N | `rollTimelineEdit` | 実装 |
| スリップ | Y | `slipTimelineClip` | 実装 |
| スライド | U | `slideTimelineClip` | 実装 |
| ハンド / ズーム | H / Z | なし (表示だけを変える) | 実装 |
| レート調整 | R | `rateStretchTimelineClip` / `clampRateEdit` | 実装 |
| ペン | P | `previewClipKeyEdit` / `editClipKey` | 実装 |
| 横書き文字 | T | `placeTextClipAt` / `updateTextClip` | 一部実装 |

`[事実]` ペンは Project schema 5、レート調整は schema 6、横書き文字は schema 7 で有効にした。

### 16.1 編集の契約

`[事実]` どの関数も candidate 全体を `validateTimeline` で検証し、失敗時は Project を変更しない。
trim の計算は内部関数 `trimClipBoundary` に一本化し、trim / 分割 / リップル / ローリング /
スライドが同じ換算を使う。リンク相手の展開は `includeLinkedCounterparts` に一本化し、
既存の `rippleDeleteGap` もこれを使うようにした。

- 分割: リンク相手は分割位置を内側に含むときだけ一緒に切る。両方を切ったときだけ
  右半分どうしを新しい link group で結び、片方だけなら右半分は未リンクにする。
  フェードは左半分が in、右半分が out を引き継ぐ
- リップル: 後ろの clip は同じ track の clip とそのリンク相手だけをずらす。
  trim した clip 自身のリンク相手は trim もずらしもしない (trim は clip 固有という link の契約)
- ローリング: 接している clip が無ければ失敗する。素材 fps が timeline と異なり、
  境界を素材 frame に揃えられない場合は隙間を作らずに拒否する
- スリップ: 素材上の長さ (out - in) を保つ。素材の端を超える分は端で止め、
  1 frame もずらせなければ失敗する
- スライド: 隣接関係は移動前の配置で決める。リンク相手も同じ量だけスライドし、
  それぞれの前後 clip を追従させる

`[事実]` `tests/harness/test_timeline_edit.cpp` に各操作の正常系と、拒否時に Project が
変わらないことの検査を足した。29.97fps 素材でローリング境界が揃わない負例もある。
`m7b_4_timeline_ui_architecture` はツールのキー割り当て・当時の R/P/T の無効・文字入力中の
ショートカット無効を検査し、それぞれを壊した負例で検査が落ちることも確かめる。

`[事実]` release の通常 CTest (`-LE "performance|stability"`) は 1384/1384 通過。

`[未検証]` GUI 上での手操作 (各ツールのドラッグ、Alt+クリックの縮小) は確かめていない。
起動して QML の生成と起動時の警告 0 件までは確認した。

### 16.2 リンクされた clip への適用 (Premiere の「リンクされた選択」)

`[事実]` 編集関数は `LinkMode { Linked, Single }` を明示的に受け取る。既定の操作は
Linked で、QML は Alt を押しながらの操作だけ Single を渡す。Alt+クリックの選択も
リンク相手を含めない。Premiere はリンクされた選択が有効なとき video / audio を
一緒に編集し、Alt でその場だけ片方にする (Adobe のヘルプ「Cut clips」「Trim clips」)。

| 操作 | Linked の挙動 |
| ---- | ------------- |
| trim | リンク相手の同じ側の端も同じ量だけ動かす |
| リップル | 相手も同じ量 trim し、両方の track の後ろを詰める。尺の変化量が相手と食い違えば拒否 |
| ローリング | 相手の編集点も動かす。相手が編集点を持たない (L / J カット) なら相手は動かさない |
| スリップ | 相手も同じ量ずらす。全員がずらせる範囲で止め、同期を崩さない |
| スライド / 分割 / 移動 | 従来どおり相手も含める。Single なら操作した clip だけ |

`[事実]` Premiere のリップルは、後ろをずらす track を同期ロックで決める。mvm には
同期ロックが無いので、trim した clip の track とそのリンク相手だけを後ろへ波及させる。

### 16.3 Redo と、分割後の再生の引き継ぎ

`[事実]` Undo で戻した編集を Ctrl+Shift+Z / Ctrl+Y でやり直せる。新しい編集を commit すると
やり直し履歴は捨てる。

`[事実]` 再生中の clip 境界で、preview source の同一性を clip ID ではなく
「素材と timeline -> 素材 frame の対応」で判定するようにした (`previewVideoMappingCovers`、
audio は素材と sample offset)。レーザーで分割した直後のように同じ素材が連続していれば、
一時停止と source の作り直しをせずに引き継ぎ、composition だけを差し替える。
以前は境界ごとに pause -> seek -> play を踏み、分割点で再生が一瞬止まっていた。

`[未検証]` 分割点を再生して止まらないことは GUI で確認していない。判定関数の単体テストと、
controller の契約テストまでを確認した。

### 16.4 レビュー指摘への対応

`[事実]` スライドは、操作した clip に接している前後の clip が両方無ければ拒否する。
以前は前後が無くても成功し、単なる移動になっていた (ツールの説明と食い違っていた)。
リンク相手の前後は、ローリングの L / J カットと同じく任意のまま。

`[事実]` clip へのマウス操作をどの編集として確定するかを `apps/mvm/TimelineGestures.js`
へ切り出し、`tests/qml/tst_timeline_gestures.qml` (qmltestrunner) で検査する。
レーザーの分割位置と Alt / Shift の解釈は press 時点で確定する。
qmltestrunner は test 関数 0 件でも成功を返し、Windows では stdout も CTest へ届かないので、
`tests/qml/run-qml-test.ps1` が結果 file を読み、test 関数の通過件数と失敗 0 件で判定する。
分割位置を release 位置へ変えた mutant と、test 関数が 0 件の file の両方で失敗することを確認した。

`[事実]` Undo / Redo 後の recovery が切り替え後の Project を指すことを
`testUndoRedoRewritesRecovery` で固定した。`stepEditHistory` から
`scheduleRecoveryAutosave()` を外すと Redo 側の検査が落ちる
(Undo 側は直前の編集で起動した上限 timer が書き直すため、この mutation では落ちない)。

`[事実]` ローリング / スライドの drag 中、隣接 clip の端も同じ量だけ伸縮して見せる
(`adjacentEditPoints`)。ローリングでリンク相手が編集点を持たない場合は、Project と同じく
相手を動かして見せない。

`[未検証]` 上記 QML の配線を実機のマウス操作で通したことはない。

### 16.5 分割点での再生の引き継ぎ (fps 違い) と、端のドラッグの停止

`[事実]` `previewVideoMappingCovers` は素材 fps = timeline fps のときだけ引き継ぎを許していた。
再生は fps が違う clip も許すので、60fps timeline 上の 30fps 素材では分割点ごとに
一時停止と seek (「再生開始のためseekしています」) を踏んでいた。
source の写像 `start + ceil((s - in) R)` (R = timeline fps / 素材 fps) が全 frame で一致する
条件「(in' - in) R が整数 k で start' - start = k」で判定するよう一般化した。
30fps 素材を 60fps timeline で分割した右半分はこれを満たす。29.97fps 素材などは満たさず、
境界で組み直す。組み直したときは理由を status へ出す。

`[事実]` trim / リップル / ローリングは、素材の範囲を越える量を渡されると全体を失敗させていた。
QML はドラッグ量を制限しないので、分割前の位置を少しでも越えて引き延ばすと clip が元へ戻っていた。
`clampEdgeEdit` で素材の端・1 frame 以上の尺・timeline 先頭 (通常の trim の left 端だけ) に止め、
確定と drag 中の表示の両方がこれを使う。`testTrimRestoresSplitClip` が
「分割 → 前半削除 → left 端を大きく引き延ばす」で分割前の clip に戻ることを固定している。

`[未検証]` 実機の再生で分割点が止まらなくなったことは確認していない。引き継げない場合の
status 表示を手がかりに確認する。

`[事実]` スライドも `clampSlideEdit` で、前の clip の out・後ろの clip の in を動かせる範囲
(素材の範囲と 1 frame 以上の尺)、timeline 先頭、接していない clip との空白に止める。
`slideTimelineClip` の確定と drag 中の表示 (`clampSlideDrag`) の両方がこれを使い、
trim / リップル / ローリング (`clampEdgeEdit`)、スリップ (`previewSlip`) と揃えた。

### 16.6 ペンツールと Project schema 5

`[事実]` Project schema を 5 に上げた。clip の `effects` に `volume_percent`、
`opacity_keys`、`volume_keys` を保存する。キーの `frame` は clip 先頭からの timeline
frame、`value_percent` は不透明度 0〜100 または音量 0〜200 である。キー間は線形補間し、
両端の外側は端の値を保持する。既存フェードは素材フレーム基準のまま残し、カーブへ乗算する。
schema 4 は読み込まない。

`[事実]` trim、分割、ローリング、スライドで clip の素材範囲が変わるとき、
残るキーを clip 相対位置へ換算し、端には切断位置の補間値を置く。slip と clip 全体の移動では
キーの位置を変えない。ペンのドラッグ表示と確定は Project の `previewClipKeyEdit` を共有する。
リンク相手へペン操作そのものは複製しない。

`[事実]` preview の映像 opacity と通常・シャトル音声の gain に Project の評価値を使う。
書き出しは全出力フレームを評価して MLT の opacity animation と `volume` filter へ渡す。
`m4_timeline_export_focused` は生成した WAV の書き出し後 PCM を測り、0%、100%、200%、
時間変化が実出力へ反映されることを確認する。再現手順は
`ctest --test-dir build/ucrt64-release -R m4_timeline_export_focused --output-on-failure`。

`[事実]` ペンの操作は clip の選択状態を見ない。線の上下 12px 以内をクリックすると
キーを追加し、キーの左右 8px・上下 12px 以内を押すとそのキーをドラッグする。どちらでもなければ
clip を選択する。判定は画面上の距離で決め、キーの左右は frame へ丸める前の x で比べる
(丸めると高倍率で隣の frame のキーまで拾い、Alt+クリックで誤って消す)。以前は値の単位で ±8 に
固定しており、音量 (最大 200%) の clip では線の上下 2px 程度しか受け付けなかった。

`[事実]` 線は clip の上下 4px を空けて描く。値→y (`penY`) と y→値 (`penValueAt`) は
同じ `penGeometry` (TimelineGestures.js) を使い、逆関数になっている。線の描画と線の当たり判定は
同じ頂点 (`penLinePoints`) を使い、判定は押した x で折れ線を補間した y との距離で決める
(`penLineYAt`)。「線を掴んだか」は描画された線の位置、「どの frame へキーを置くか」だけを
frame へ丸めて決める。以前は x を frame へ丸めてから線の値を評価しており、急な傾きの線では
見えている線を押しても clip の選択になった。以前は描画だけが inset を持ち、100% のキーを真横へドラッグすると約 93% に落ちた。
キーを中心から外して掴んだ場合も、そのずれを保ってドラッグする。いずれも
`tst_timeline_gestures.qml` の pen テストで固定している。
線は暗い縁取り付きの黄色 2px、キーは塗りつぶした青い四角で描く。ドラッグ中とポイント中の
キーは白く一回り大きくする。線は `Shape` (CurveRenderer) のベクタ描画である。Canvas は
clip の小数 px 位置で texture が補間され、急な傾きの線がぼやけた。

`[事実]` キーのドラッグ中 (押下・移動・離す時点のいずれか) に Shift を押していると、値を
100% (音量なら 0 dB) へ吸着する。キーを Alt+クリックすると削除する (`deleteClipKey`)。
キー以外の Alt+クリックは何もしない。最後のキーを消すと線は effects の基準値に戻る。
Shift を押しただけでマウスを動かさない間は、表示は吸着しない (離した時点では吸着する)。

`[未検証]` ペンの実機マウス操作と、長い音声 clip で出力フレームごとの key を MLT に渡す場合の
書き出し時間は測っていない。書き出しは clip 尺の全 frame を評価して key にするため、60fps・2 時間の
clip では 1 clip あたり 43.2 万 key になる。preview と同じ `evaluateClipOpacity/Volume` を
frame ごとに使うことで補間・フェード乗算・素材 fps の差を揃えている。キー・フェード境界・
clip 端などの変化点だけへ圧縮する余地はあるが、preview との一致を壊しやすいので、計測してから判断する。

### 16.7 素材 frame の対応を書き出し (MLT) に揃える (レート調整の前段)

レート調整 (R) を入れると、timeline fps と素材 fps の比が整数でない clip が当たり前になる。
その前に、preview と書き出しで「timeline frame がどの素材 frame を出すか」を一致させた。

`[事実]` MLT (7.36.1) の avformat producer は、profile fps の位置 p (素材の 0 frame から数える) に
素材 frame `floor(p × 素材fps / profile fps + 1/2)` を出す。preview は `floor(p / R)` だったので、
fps が違う clip で最大 1 素材 frame 食い違っていた。25fps 素材を 60fps profile で出すと 239 frame 中
118 frame がずれていた。再現手順: `testsrc2` で作った素材を `melt -profile <60fps> src.mp4
-consumer avformat:out.nut vcodec=rawvideo` で出し、素材 fps の profile で出した frame と
`framemd5` で照合する (ffmpeg で直接 decode した md5 とは変換が違うので照合に使えない)。

`[事実]` 同じ理由で、書き出しの cut 開始を `floor(in R)` にしていたため、R < 2 の clip は先頭に
in より前の frame を出していた。50fps 素材を 60fps へ置き in = 4 とすると frame 3 が出た。

`[事実]` ちょうど 1/2 の境界では MLT が double で計算するため、まれに下へ丸まる
(25fps→60fps の p = 138 は 57.5 で frame 57)。mvm は有理数で厳密に上へ丸め、これは再現しない。

変更:

- 対応の定義を `src/core/source_frame_mapping.h` の 1 か所にした。境界は `ceil(s R)`、表示は
  `floor(p / R + 1/2)`。Project の `convertBoundary`、preview (`clipSourceFrameAt`)、
  decode worker (`sourceFrameOutputInterval`)、書き出しの cut (`clipProducerRange`)、
  フェード・音量カーブの評価 (`clipFadeSourceFrameAt`) がすべてこれを使う
- 書き出しの cut は Project の境界と同じ `ceil(in R)` から始める。C 側の floor 換算
  (`mvm_source_boundary_to_producer_boundary`) は削除し、C++ で決めた範囲を渡す
- 素材の末尾で、丸めると存在しない frame を指す位置は cut に含めず、最終 frame を繰り返して埋める。
  埋める数は clip ごとに決まる (以前の「0〜2 frame」の許容は廃止)。埋める clip は tractor 経路で書き出す
- 丸めで trim の外側の frame (sourceOutFrame) を出す位置があるのも MLT と同じにしている
- 位置を素材の絶対位置で数えるので、分割した右半分は素材 fps によらず左半分の preview source を
  引き継げる (§16.5 の「(in' - in) R が整数」の条件は不要になった。29.97fps 素材も引き継ぐ)

`[事実]` `m4_timeline_export_focused` に、48fps 素材を in = 3 から 60fps へ置いた clip を書き出し、
全 8 frame が手計算の対応 3,4,5,6,6,7,8,9 と一致すること、preview も同じ列を返すことを足した。
書き出しは色変換で輝度が変わるので、同じ輝度式の 60fps 素材を 1:1 で後ろに置いた校正表と照合する。
cut 開始を floor に戻した mutant でこの検査が落ちることを確認した。

`[事実]` timewarp producer (レート調整で使う予定) も同じ四捨五入だった。
`timewarp:<speed>:src.mp4` を 60fps profile で出すと、length は素材尺 ÷ speed (端数切り上げ、
0.73 で 329 frame)、frame は `floor(k × speed × 素材fps / profile fps + 1/2)` と一致した
(0.73 で 328/328、1/3 で 716/717、不一致 1 件は 1/2 境界の double 誤差)。
`warp_pitch=0` では 1000Hz の正弦波が 0.5x で 500Hz、2x で 2000Hz になり、
`warp_pitch=1` では 1000Hz のまま保たれた (中央 1 秒のゼロ交差で測定)。

### 16.8 レート調整ツールと Project schema 6

`[事実]` Project schema を 6 に上げた。clip に再生速度 `speed_num` / `speed_den` (約分済みの正の有理数、
10%〜1000%) を必須で保存する。schema 5 は読み込まない。範囲は rbpitch の pitchscale 0.1〜10 に合わせ、
後から音程保持を足しても変えずに済むようにした。

`[事実]` 速度 s の clip は、素材 fps が f s の clip と同じに扱う。実効 fps は `project::clipTimebase`
(`core::multiplyFrameRate`) の 1 か所で決め、trim・分割・リップル・ローリング・スライド・スリップの境界、
clip の尺、preview の frame 対応 (§16.7)、decode worker の区間、書き出しの cut、フェードと音量カーブの
評価がすべてこれを使う。実際の素材 fps は decode の検証と表示だけに使う。

レート調整の操作 (Premiere の Rate Stretch):

- 端をドラッグすると素材範囲 (in/out) を変えずに速度を変える。反対側の端は動かさず、リップルも上書きもしない
- 尺を D にする速度は s = (out - in) R / D。実効 fps が (out - in) timeline fps / D になり、尺はちょうど D になる
  (29.97fps 素材でも端数が出ない)
- 止め方は `clampRateEdit` に一本化し、確定と drag 中の表示 (`clampEdgeDrag` の `rate`) の両方が使う。
  速度の範囲・1 frame 以上の尺・同じ track の隣の clip・timeline 先頭で止め、Linked ならリンク相手の条件も含める。
  判定そのものは candidate に `validateTimeline` を掛けて行い、止める位置は二分探索で求める
  (各条件は尺の上限か下限なので、成り立つ範囲は現在の尺から連続している)
- Linked はリンク相手にも同じ速度を適用する。相手の速度がもともと違う (Alt で片方だけ変えた) 場合は拒否する
- drag 中の表示は確定と同じ計算 (`previewRateStretch`) で作る。clamp して伸縮した candidate から、
  対象 clip ごとの開始 / 終端のずれと速度を返し、QML は `ratePreviewClips` として clip ごとに使う
  (幅、波形の縮尺 `shownSpeed`、ラベルの %)。確定 (`rateStretchTimelineClip`) も同じ candidate を作る。
  以前は QML がリンク相手へ操作した clip と同じ端の移動量を配っており、尺の違うリンク対
  (V 300f / A 240f) で V を +150 すると A を 390f / 61.54% と表示し、離すと 360f / 66.67% へ跳んだ
- 波形は 4 倍ずつ粗くした peak の階層から 1 列あたり高々 8 個程度を走査するだけで、drag 中に縮尺を
  変えても再 decode は起きない (ズームと同じ負荷)
- opacity / volume の key は内容に付いて伸縮する (`rescaleClipKeys`、端は端へ写る)。丸めで同じ frame に
  重なった key は先の 1 つを残す。フェードは素材 frame 基準なので変えない

音声は速度に連動させる (テープ方式。ピッチ保持は未対応):

- preview: `AudioDecodeWorker::setPlaybackSpeed`。swr の rate を (素材 rate × p) : (48000 × q) にし、
  出力を「速度で伸縮した時間軸」の 48kHz sample にする。この時間軸なら「media sample = timeline sample + offset」の
  定数のずらしが保てるので、offset・mix・audio master clock には手を入れていない。rate が int に収まらない
  速度だけ連分数で近似する。シャトルも同じ worker を使う
- 書き出し: 等速でない clip は `timewarp:<speed>:<path>` を loader へ渡して開く (`warp_pitch=0`)。
  `timewarp` は必須 service に加えた

`[事実]` service 名 `"timewarp"` を `mlt_factory_producer` へ直接渡すと、tractor の書き出しで音声が
伸縮されなかった (440Hz・1 秒の WAV を 50% にしても 440Hz・1 秒のまま)。loader へ `timewarp:0.5:<path>` を
渡すと 220Hz・2 秒になった。melt に同じ文字列を渡した単体の書き出しは最初から 2 秒だった。
`[推測]` timewarp は audio の sample rate を変えて速度を表し、loader が付ける resample の正規化で初めて
伸縮した音になる。mix transition はこれを resample しない。

`[事実]` 次を検査に固定した。

- `test_timeline_edit` (尺の違うリンク対): V 300f / A 240f の V を +150 すると preview も確定も
  V 450f・A 360f・2/3 になること、left 端では A が終端を保って開始のずれ -120 になること
- `m7b_4_timeline_ui_architecture`: 端ハンドルが rate のとき `previewRateStretch` を呼び、clip の幅・
  `shownSpeed`・波形の縮尺がその結果を使い、drag の終了で消すこと (1 か所ずつ壊した負例つき)
- `test_timeline_edit`: 50%・75%・1000%・10% の止め方、隣の clip とリンク相手の隣の clip で止まること、
  29.97fps 素材の尺がちょうど 250 になり速度 1001/1250 になること、key の伸縮、速度付き clip の trim と分割、
  速度の違うリンク相手の拒否、拒否時に Project が変わらないこと
- `test_clip_effects`: schema 5・速度の欠落・範囲外・未約分の拒否と、10% / 1000% ちょうどの受理 (対照群つき)
- `test_timeline_preview_mapping`: 50% の clip の frame 対応 (四捨五入)、尺、速度の違う source を使い回さないこと、
  音声の offset
- `m4_timeline_export_focused`: 40% の clip の全 15 frame が手計算の対応と一致すること (校正表で照合)、
  440Hz・1 秒の WAV を 50% で書き出すと 120 frame・約 220Hz になること (上の loader の件で実際に落ちた)
- `audio_playback_speed`: preview の decoder で 44.1kHz・1000Hz・1 秒の WAV を 1 / 1/2 / 2 / 73/100 で読み、
  sample 数が 48000/s (±64)、周波数が 1000 s Hz (±1%)、伸縮した時間軸の seek が通しの decode と一致すること。
  実測は 47983 / 95966 / 23984 / 65730 sample、1000.3 / 500.2 / 2001.3 / 730.3 Hz

`[未検証]` 実機のマウス操作、preview の音声を耳で確かめること、1000% での preview の decode 負荷
(全 frame を decode するので、コマ落ちするかは測っていない)。

### 16.9 横書き文字と Project schema 7

`[事実]` `TimelineClipKind::Text` は素材ファイルを持たず、本文・フォント・画素単位のサイズと位置・
文字色・太字・揃え・縁取り・背景色を `text` に保存する。schema 7 の各項目は必須で、
不正値を保存・読込時に拒否する。既定の文字は Meiryo、64 px、白、左揃え、縁取りと背景は透明。
5 秒相当の frame 数を初期尺とし、分割・トリム・移動・削除・Undo/Redo の Project 編集に乗せる。
素材位置のない文字 clip のスリップと速度変更は拒否する。

`[事実]` T でモニターの表示矩形をクリックすると、そこを左上とする複数行のエディターを表示する。
Ctrl+Enter またはフォーカス移動で確定、Esc で破棄する。空文字は作らない。既存の文字をクリックすると
その場で再編集し、選択ツールでドラッグすると位置を変更する。書式は左パネルで変更する。
文字の描画とフォント検証は `renderTextRaster` に集約し、preview の Qt scene graph と書き出しの
MLT `qimage` producer は同じ出力解像度の透過 PNG を使う。再読込時は文字データから再生成する。

`[事実]` 配置は再生ヘッドから上の V1～V3 を順に調べ、clip の全尺が空いている最初のトラックを使う。
必要なトラックは追加し、V1～V3 が埋まると Project を変更せずに拒否する。書き出しは V1～V3 を
MLT のトラック順で合成する。固定素材の 3 トラック書き出しと文字だけの書き出しは
`text_clip_contract` で画素を確認する。
(§17 で変更: V1～V3 の制限を外し、空きが無ければ上へ track を足して置く。)

`[事実]` (§16.10 で解消) 当初の preview は文字画像を `PreviewSurface` の上の Qt scene graph に
置いていたため、文字トラックより上に映像トラックがある構成で重なり順が書き出しと一致しなかった。

### 16.10 文字を preview engine で track 順に合成する

`[事実]` 映像のある frame では、文字を preview engine の静止画 layer として video layer と同じ
composition に載せ、track index の順 (背面 -> 前面) に合成する。書き出しと同じく track index だけで
前後が決まる。順序を決める箇所は `previewLayerStack` (`timeline_preview_mapping`) の 1 つだけである。

- `PreviewCompositionLayer::stillImage` (RGBA8 straight alpha) を持つ layer が静止画 layer である。
  decode source を持たないので pairing と提示直前の検証の対象外とし、検証を通った後で snapshot の
  z 順どおりに差し込む。同一性は pointer で判定するので、controller は同じ文字に同じ instance を渡す
- GPU 側は `GpuPixelFormat::RGBA8` と専用の pixel shader で描く。texture は初回提示時に作り、
  snapshot が参照しなくなったら SRV cache から外す (`GpuCompositor::retireLayerTexture`)。
  外さないと SRV が texture の参照を持ち続け、文字を編集するたびに漏れる
- 受理条件: 静止画だけの composition、画素数と寸法の不一致、source との併用、effect 付きは拒否する
  (§18.3 で変更: 静止画だけの composition と effect 付きの静止画は受理する。fade 付きは拒否する)
- 上限: video source 2 本、合成 layer 3 枚 (`kMaxPreviewCompositionLayers`)。映像 2 本 + 文字 2 枚
  のように 3 枚を超える frame は preview を失敗にする (fail-closed)

`[事実]` (§18.3 で変更: 映像の無い frame の文字も engine が合成する) 映像の無い frame では従来どおり UI (Qt scene graph) が文字を重ねる。重ねる相手が無いので
順序の問題は起きない。また、ドラッグ中と編集中の文字だけは UI が重ね、engine の合成からは外す
(`setTextOverlayClip`)。二重に描かないためであり、操作中の文字は一時的に最前面に見える。

`[事実]` 次を検査に固定した。

- `still_layer_compositor`: 静止画の z 順・alpha・opacity、V1 映像 / V2 静止画 / V3 映像の 3 layer で
  V3 が覆う範囲だけ文字が隠れること、texture の retire で SRV cache から外れること (対照つき)
- `text_preview_parity`: 同じ Project を GPU compositor (製品の `previewLayerStack` の順) と
  書き出し (MLT) の両方で描き、文字の内側の画素で「白く見えるか」を比べる。
  V2 文字 (V3 映像に隠れる) は 1365/1365、V3 文字 (最前面) は 1364/1365 が一致した。
  対照として文字だけを反対の端へ動かすと 0/1365 と 1/1365 になり、比較が重なり順を判別している。
  文字の縁は 4:2:0 圧縮でにじむため比較から外した (縁を含めると V3 文字で 2773/3045 に落ちた)
- `preview_engine_p5b_unit` (`composition still layers`): 3 layer の受理、同一 instance の再送が no-op で
  あること、静止画を video source として数えないこと、4 枚目・静止画だけ・画素不足・source 併用・
  effect の拒否
- `m7b_2_timeline_preview_mapping_focused`: 文字が `textLayers` に入ること、V2/V3 の入れ替えで
  合成順が変わること、mute した track の文字を外すこと、合成 layer 上限と映像の無い frame の扱い
- `p5e` の layer 上限超過 fault は、video 3 本だと source 上限で先に落ちて layer 上限を検査できない
  ため、video 2 本 + 静止画 2 枚で layer 数だけを超えさせるよう変えた

`[事実]` 直接入力は `text_ui_direct_input` (workstation) で、製品の `Main.qml` を D3D11 の window で
起動して QTest の key / mouse event を送る。T -> クリック -> 入力 -> Ctrl+Enter で作成、
Esc で破棄、V -> 文字のドラッグで移動 (ドラッグ中だけ UI が重ねる)、Ctrl+Z で位置が戻ることを見る。
この検査で製品の不具合を 2 件見つけて直した。

- Esc / Ctrl+Enter で入力を終えた後も非表示の editor が focus を持ち続け、V などの単キー操作が
  別の場所をクリックするまで効かなかった。editor が focus を持つときだけ window へ戻す
- 文字のドラッグで移動量が約半分になっていた。移動量を測る MouseArea 自身が Translate で文字と
  一緒に動くため、自身の座標で測ると移動量が打ち消し合う。動かない `previewHost` の座標で測る。
  修正前の実測は 40 x 20 px のドラッグで 60 x 30 (期待 119 x 60、出力画素)

`[事実]` 文字 inspector と preview 上の操作を次のように変えた。

- 書式欄を詰めた。本文 (枠付き、Ctrl+Enter で確定・Esc で取り消し)、フォント (インストール済み
  書体のプルダウン。各行に書体の見本を出す) とサイズ、太字と揃え (左・中央・右の排他ボタン)、X / Y、
  文字色・縁・背景 (見本を押すと color picker、横に `#AARRGGBB` の直接入力) の順に並べる。
  clip 名は本文の先頭で重複するので、文字 clip では出さない
- preview 上では、選択ツールで文字の描画範囲 (不透明な画素を囲む矩形) のどこを掴んでも動かせる。
  以前は字形の画素の上だけで、字間や字の内側では掴めなかった。選択中の文字には範囲の枠を出す。
  押しただけで動かさなければ選択だけにし、Undo を積まない
- 入力欄の外を押すと focus を window へ戻す (`FocusReleaseFilter`)。数値欄も Enter / Esc で戻す。
  以前は inspector の X / Y などを一度触ると、別の場所を押しても Space の再生や V / T が
  効かないままだった (Qt Quick は timeline や preview を押しても focus を移さない)

- サイズ・X・Y・縁の幅の左右ドラッグ中は、Project を変えずに preview だけを描き直す
  (`previewTextClip`)。離したときに `updateTextClip` で 1 回だけ保存し、値が元に戻っていれば
  取り消す。engine が Seeking の間に来た描き直しは失敗にせず、ReadyPaused になったら最新の値で
  1 回描く
- preview 上で文字を掴んでも再生位置を動かさない。以前は選択に `selectClip` を使っており、
  文字 clip の先頭へ seek していた。ポインタは通常の矢印にした。文字ツール用の MouseArea は
  無効でも I ビームの cursor を出していたので、文字ツール以外では出さない

- preview 上で文字を動かした後に文字が消えていた。離すと位置の保存で engine が seek し、
  その最中に「ドラッグ中の文字を合成から外す」指定を解除して composition を出し直すと
  `composition submissionを受理できないstateです` で拒否され、文字を外した composition が残った。
  解除も `refreshTextPreview` を通し、ReadyPaused 以外では完了を待ってから出し直す
- 揃えのボタンはテロップの定位置ボタンにした (`placeTextClip` / `textPresetPlacement`)。
  押すと文字の矩形の下端を画面の下から 15%、左右は画面幅の 5% の余白で左・中央・右へ置き、
  行揃えも同じ向きにする。状態を持たないので、X / Y やドラッグで動かした後に押し直すと戻る。
  15% / 5% は決め打ちの ad hoc 値で、YouTube の動画を実測して決めたものではない。
  矩形の大きさは描画と同じ組版 (`layoutText`) から求める
- 色の選択は共通部品 (`ModernDialog` / `ModernDialogField` / `ModernDialogButton`) で作った
  `ModernColorPicker` にした。彩度・明度の面、色相、不透明度、テロップ向けの 8 色、変更前後の見本、
  `#AARRGGBB` の入力を持つ

- color picker は押した見本の右横に、背景を暗くせずに (非 modal) 出す。選んでいる途中の色は
  `previewTextClip` で preview へ反映し、OK で保存、キャンセル・Esc・外側の押下で元の色へ戻す
- フォント一覧は Photoshop と同じく、hover した書体を preview へ仮に反映する。選べば保存、
  選ばずに閉じれば元の書体へ戻す。一覧を開くと一覧に focus を渡す (ComboBox 自身は focus を
  取らないので、渡さないと Esc で閉じられなかった)
- 色のドラッグや hover は短い間に何度も来る。最初の値はすぐ描き、その後は動かしている間も
  200ms ごとに最新の値を描く (throttle)。当初は「止まって 40ms 経ったら描く」debounce にしており、
  マウスの move は 40ms より短い間隔で来るので、ドラッグ中は 1 回も描き直されなかった

`text_clip_contract` に定位置の計算 (左・中央・右の座標、描いた画像の下端が下から 15% に来ること、
未知の揃えの拒否) を足した。`text_ui_direct_input` に、文字を動かした後に preview の更新が
失敗しないこと、定位置ボタンで中央下へ置かれ、ずらした後に押し直しても戻ることを足した。
色は、見本の横に非 modal で開くこと、候補を押すと preview が描き直され Project は変わらないこと、
Esc で閉じて元へ戻ること、OK で保存されることを見る。フォントは、一覧の別の書体に hover すると
preview が描き直され Project は変わらないこと、Esc で一覧が閉じて元へ戻ることを見る。

`text_ui_direct_input` は key / mouse event を送るので、検査中に window が非アクティブになると
event が届かない。途中で前面を奪われた run は FAIL の有無にかかわらず `PROTOCOL_INVALID`
(終了コード 4) にする。実際に操作中の desktop で 4 回走らせると 2 回がこれになり、
前面のままの 2 回は通った。

`text_ui_direct_input` に、字形の無い所 (範囲内の透明な画素) で掴めること、本文欄を触った後に
preview を押すと単キー操作が戻ることを足した。修正前の focus 処理では後者が落ちることを確かめた。
さらに、文字を掴んでも再生位置 (40) が動かないこと、X のドラッグ中に preview の文字範囲が動き
Project は離したときだけ変わることを足した。前者は `selectClip` に戻すと落ちることを確かめた。

`[事実]` レビュー指摘への対応。

- (P1) 文字の不透明度 (値・Pen の key・fade) が preview に効いていなかった。書き出しは文字を
  既存の video track の effect 経路へ載せるので、たとえば frame 0 で 100%、frame 15 で 0% の key を
  付けると、書き出しだけがフェードしていた。不透明度は `mapTimelinePreviewFrame` が
  `evaluateClipOpacity` で frame ごとに評価し (`TimelinePreviewTextLayerMapping::opacity`)、
  engine の静止画 layer の opacity と、映像の無い frame で UI が重ねる文字の opacity の両方に使う。
  静止画 layer の effect は引き続き拒否し、最終の不透明度だけを渡す
- (P1 の契約) 文字の effect は不透明度 (値・key・fade) だけにした。位置・拡大・回転・切り抜きと
  音量は、書き出しでは効くが preview の静止画 layer では描けないので、`validateTimeline` が拒否する
- (P2、§17 で撤廃) 文字は V1～V3 だけ、を Project の不変条件にした (`kMaxTextVideoTracks`)。以前は配置・
  書き出し・preview の hit-test だけが V1～V3 で、V4 を足して通常の移動で文字を持っていくと
  Project としては正しいまま、hit-test では掴めず書き出しは失敗した。移動は検証で拒否される
- (P3) 文字画像は出力解像度の全画面 RGBA である (1080p で約 8MiB、4K で約 32MiB)。controller は
  QImage・engine 用の RGBA・UI 用の PNG を持ち、GPU texture も作る。文字の矩形だけの画像にして
  配置を composition へ持たせれば軽くなるが、preview と書き出しが同じ画像を使う今の構成を崩すので
  見送った。`[未検証]` 4K で文字 clip が多いときのメモリと再描画時間は測っていない

検査を足した。`text_preview_parity` に V3 文字の key (frame 0: 100% -> frame 15: 0%) を足し、
frame 0 / 8 / 15 の書き出しと同じ frame の映像で preview を合成して比べる。一致は
1363 / 1365、1365 / 1365、1365 / 1365、文字の平均輝度は 255.0 / 251.5、180.6 / 181.4、
114.9 / 117.5 (preview / 書き出し)。対照として不透明度を無視すると frame 15 は 0 / 1365 になる。
`m7b_2_timeline_preview_mapping_focused` に不透明度の補間 (1 / 0.5 / 0、key 無しは 1) を、
`text_clip_contract` に V4 への移動と V4 の文字の拒否 (対照: V2 への移動は通る)、
不透明度の key / fade の受理、拡大率・位置の拒否を足した。

`[未検証]`

- color picker のドラッグ中の描き直しが実際に何 ms で画面へ出るか。自動検査は、10ms 間隔で
  約 1.2 秒動かし続ける間に 3 回以上描き直すことまでを見る (debounce に戻すと 0 回で落ちることを
  確かめた)。preview が seek 中なら、その描き直しは seek の完了まで遅れる
- 合成 layer 3 枚の性能。measured envelope (layer 2) の外であり、`matchesMeasuredEnvelope` は
  layer の相違でも false になる。計測するまで「layer 3 で 60fps が出る」と書かない
- 再生中に文字 clip が出入りする frame の正確さ。composition は GUI の再生 tick で出し直すので、
  文字の出入りが 1〜2 frame 遅れうる (映像 clip の引き継ぎと同じ経路)。停止中・scrub では
  要求 frame の composition で seek するので一致する
- IME の変換確定。OS の入力方式が要るので自動化していない。手動確認の手順は次のとおり
  1. 文字ツール (T) でモニターをクリックし、日本語入力で「にほんご」と打って変換し、確定する前に
     Esc を押す。変換だけが取り消され、editor は開いたままであること
  2. 続けて変換を確定し、Ctrl+Enter で「日本語」の文字 clip ができること
  3. 変換中 (未確定) のまま timeline をクリックして focus を移したとき、未確定文字列が本文に
     入るか消えるかを記録する (期待値は未決定。Qt の既定動作をまず観測する)
  4. 映像のある frame で 1〜3 を行い、確定後の文字が映像との track 順どおりに表示されること

## 17. track / layer 上限の引き上げ

`[事実]` 比較対象の Premiere は track 数に実質上限が無い。mvm は計測単位ごとに刻んだ上限
(preview は映像 2 本・合成 3 枚、文字は V1～V3、書き出しは V1～V3 と総 clip 64 本) を持っていたので、
使い勝手を優先して**決め打ちの大きめの値へ一度に引き上げた**。軸ごとに性能を測って決めた値ではない。

| 軸 | 以前 | 現在 | 定義 |
| --- | --- | --- | --- |
| preview で同時に decode する映像 | 2 | 8 | `kProductMaxActiveVideoSources` |
| preview で同時に合成する layer (映像 + 文字) | 3 | 16 | `kProductMaxCompositionLayers` |
| preview で同時に mix する音声 | 8 | 16 | `kProductMaxActiveAudioSources` |
| video source の登録 slot | 4 | 17 | 旧 set + 新 set + slip preview 1 本 |
| 文字を置ける track | V1～V3 | 制限なし | 空きが無ければ上へ track を足す |
| 書き出しの映像 track | 3 | 制限なし | playlist を使う最上位 track まで作る |
| 書き出しの総 clip 数 | 64 | 制限なし | 配列を clip 数から確保する |

- preview の上限の数値は `preview_engine/preview_types.h` にだけ書く。engine の capability と
  `mapTimelinePreviewFrame` の fail-closed 判定 (`kMaxPreviewVideoLayers` / `kMaxPreviewCompositionLayers`)
  はどちらもこの定数を参照する。上限を超える frame は引き続き成功に見せず失敗にする
- 書き出しは V2 以上の clip ごとに V1 との間へ affine transition を植える既存の方式のままで、clip は
  track 昇順に並べてから植えるので上の track ほど前面になる。V4 以上の切り抜きが黙って落ちる
  条件 (`track < 3`) があったので外した
- clip 数の上限が無くなったので、tractor 書き出しの cut 配列の要素数 (clip ごとに 2 + 末尾補完 frame 数)
  の合計は、加算前に long long の範囲と確保できる要素数 (`SIZE_MAX / 要素の大きさ`) を検査する。
  signed overflow は未定義動作なので、calloc の失敗では防御にならない。producer 尺 + 末尾補完の
  検算も同じく加算前に止める。`mlt_export_capacity_overflow` で、どれも配列の確保より前に
  「表現範囲を超えています」で止まることを見る。検査を外すと、修正前の経路の
  「cut 配列を確保できません」になって落ちることを確かめた
- `[推測]` 書き出しは video track の番号をそのまま MLT の track 番号に使うので、V1 と V10000 にだけ
  clip がある Project でも空の playlist を 10000 本作る。UI は track を 1 本ずつ足すので通常は
  起きず、実害は測っていない。詰めるなら playlist の番号だけでなく、transition を植える
  track 番号 (`mlt_field_plant_transition(..., 0, video_track)`) も一緒に写し替える
- native present hook の `MVM_NATIVE_PRESENT_HOOK_MAX_SOURCES = 2` は据え置いた。patched Qt と共有する
  固定 ABI で、使うのは診断用の `CompositorRhiItem` だけである。製品の preview は通らない
- `MeasuredPreviewEnvelope` (60/1 × 映像 2 × layer 2 × 音声 1) は変えていない。configured 値と一致しない
  ので、`matchesMeasuredEnvelope` は以前から false のまま (UI は「未計測」)

検査の更新: `m7b_2_timeline_preview_mapping_focused` は映像 8 本の受理と 9 本目の拒否、映像 8 + 文字 8 の
16 枚の受理と 17 枚目の拒否を見る。`text_clip_contract` は V1～V3 が埋まったときに V4 を足して置くこと、
mute した track を飛ばすこと、V4 への移動の受理、V1～V5 映像の上の V6 文字の書き出し (画素)、
1 frame の clip 70 本を V1～V5 に並べた書き出しを見る。`preview_engine_p5e_exceed_source_count` は
17 slot を埋めて 18 本目を、`preview_engine_p5e_exceed_layer_count` は 17 layer を拒否させる。

`[事実]` 上限ちょうどの受理と描画は `preview_engine_p5e_capacity_at_limit` (`mvm_p5e_capacity_smoke`)
で固定した。映像 source 8 本 + 静止画 8 枚の 16 layer と音声 source 16 本で再生し、16 layer の
frame を 30 枚提示する。同じ run で、音声 17 本目の登録、映像 9 本の composition (source 上限)、
映像 8 + 静止画 9 の composition (layer 上限) を拒否することも見る。実行時間は約 1.6 秒。
これは「受理して描画できる」ことの検査であり、fps や VRAM の余裕は下の `[未検証]` のまま。

`[未検証]` 上限いっぱい (映像 8 本 + 文字 8 枚) で再生したときの fps・VRAM。source 1 本ごとに decode
thread 1 本と D3D11 の frame pool (`extra_hw_frames = 16`) を持ち、source-set の切り替え中は旧 set と
新 set が一時的に両方残る。文字 1 枚は 1080p で CPU / GPU それぞれ約 8MiB。4K 素材 8 本では VRAM が
足りない可能性があり、その場合は値を決め打ちで下げる。

## 18. 画像・音声の単体素材

画像・音声のファイルを単体の素材として扱うための変更。段階は次のとおり。

1. 素材種別の判定を FFmpeg の stream の事実で行う (18.1)。静止画 decoder を置く (18.2)
2. decode source の無い composition (画像だけ・音声だけの区間) を engine が提示する
3. Image clip を timeline・preview・書き出しへ通す
4. 配置の経路と UI (拡張子の表を含む)

この節は 4 までの記録である。

### 18.1 判定の順序と許可表

`[事実]` MLT の probe (avformat producer) は画像の判定に使えない。`mvm_bench probe` で次を観測した。
素材は `pwsh scripts/make-testmedia.ps1 -Mode Smoke` が `tests/assets/smoke/_import/` へ作る。

| 素材 | MLT の値 | 当時の判定 |
| ---- | -------- | ---------- |
| 静止画 GIF (1 frame) | 4 frame / 100fps / 有限尺 | Video (誤り) |
| 静止画 WebP | 15000 frame / 600 秒 | Video (誤り) |
| アニメーション APNG (3 frame) | 無限尺 (静止 PNG と同じ) | Image (誤り) |
| アニメーション GIF (3 frame) | 3 frame / 10fps | Video |
| アニメーション WebP | 開けない | 失敗 |
| カバーアート付き mp3 / m4a / flac | has_video (mjpeg) / 60fps / 尺 1.0 秒 | Video (誤り) |
| h264 + カバーアートの mp4 | h264 を選ぶ / 10 frame | Video |

`[事実]` FFmpeg 8.1.2 (`ffprobe`) では次のとおりだった。

- カバーアートの stream は `disposition.attached_pic = 1` で区別できる
- 静止画は image 用の demuxer (`png_pipe` / `jpeg_pipe` / `webp_pipe` / `image2` (tga) / `gif` / `apng`)
  で開かれ、packet は 1 つ。アニメーション GIF / APNG は frame 数だけ packet がある
- **アニメーション WebP は 1 packet として demux され、decode もできない** (`image data not found`)。
  そのため VP8X chunk の animation flag (`0x02`) を header から直接読む

判定は `apps/mvm/media_import.cpp` の `probeMediaFile` だけで行う。拡張子は見ない。

1. `probeMediaStreamFacts` (`src/media/still_image/media_stream_facts.cpp`) が FFmpeg で
   stream の事実を取る。カバーアートを除いた映像 stream 数、カバーアート数、音声 stream 数、
   codec、demuxer、静止画 codec の packet 数 (2 で打ち切る)、WebP の animation flag
2. `routeMedia` がこれだけで振り分ける
   - アニメーション (flag、または静止画 codec で 2 packet 以上) -> 拒否
     「アニメーション画像 (GIF / WebP / APNG) には対応していません。動画 (mp4 など) へ変換してから
     読み込んでください」
   - HDR 画像 (`exr` / `hdr`) -> 拒否。scene-linear で tone map が要るため
   - 静止画 codec -> StillImage。静止画 decoder で実際に decode し、向きを反映した寸法を Image に入れる
   - 映像か音声の stream がある -> TimeBased
3. TimeBased だけ MLT で probe し、`classifyMediaProbe` が Video / Audio を決める
   - カバーアートを除いた映像 stream が無ければ Audio。MLT が映像ありと返しても音声として扱う
   - 映像 stream があるのに MLT が映像を開けなければ失敗 (どちらかを選ばない)
   - 無限尺、または 1 frame の映像は拒否する。以前は Image としていたが、静止画は MLT を通らなくなった

静止画 codec の許可表 `kStillImageCodecs` は `media_stream_facts.cpp` の 1 か所にある。
`mjpeg` などは動画の codec にもなるので、demuxer が画像用 (`*_pipe` / `image2` / `gif` / `apng` /
`jpegxl_anim`) のときだけ静止画とする。Motion JPEG の avi は Video になる。

`[事実]` controller の `probeMedia` / `probeAudioMedia` は独自に MLT の probe を解釈していたため、
bin とタイムラインで判定が食い違っていた。どちらも `probeMediaFile` の結果から用途に合うかを
見るだけにした。`addVideoClip` に JPEG を渡すと「静止画は動画として追加できません」で失敗する。

`[事実]` カバーアート付きの音声の尺は、MLT (profile 60fps) と FFmpeg の両方で 1.0 秒だった。
§15.3 で心配した 90000fps は profile に入らなかったので、尺の照合は入れていない。

### 18.2 静止画 decoder

`src/media/still_image/` (lib `mvm_still_image`) に置いた。FFmpeg (libavformat / libavcodec /
libswscale) だけに依存し、Qt と MLT は使わない。lint が Qt の include を拒否する
(`lint_still_image_isolation_negative` / `_control`)。preview と書き出しの両方がこの decoder の
画素を使う予定であり (段階 2 / 3)、decoder の違いによる見た目の食い違いを起こさない。

- `decodeStillImage`: 先頭の 1 frame を decode し、straight alpha の RGBA8 を返す。
  画素形式の変換は swscale の `sws_scale_frame` (dynamic mode) で、range と行列は frame の属性から決まる
- `applyExifOrientation`: EXIF orientation 1..8 を画素へ反映する。それ以外は拒否する
- `fitStillImageToRaster`: 出力解像度の raster の中央へ縦横比を保って置き、余白を alpha 0 にする。
  文字 clip の全画面 PNG と同じ形にして、crop / 変形の座標系を映像・文字と揃えるため

`[事実]` EXIF の向き。orientation 6 の JPEG を FFmpeg 8.1.2 で decode すると、frame に
`AV_FRAME_DATA_DISPLAYMATRIX` (rotation -90) と `AV_FRAME_DATA_EXIF` の両方が付く。
EXIF 側は 14 byte の空の IFD で、FFmpeg は orientation を display matrix へ移して EXIF からは
取り除いている (`ffprobe -show_frames` で確認)。画素は回転されない。
向きは display matrix を `av_exif_matrix_to_orientation` で換算したものとし、EXIF 側に 0x112 が
残っていれば一致を要求する (食い違えば失敗。どちらかを選ばない)。

`[事実]` 寸法の上限 (既定 16384 px / 64 MiPx) は、画素を確保する前に効かせる。
PNG は `avformat_find_stream_info` が寸法を知るために先頭 frame を decode するので、
codec option の `max_pixels` をそこにも渡す。渡さないと 9000x9000 の PNG を decoder の検査より前に
確保していた (`ffprobe` と `ffprobe -max_pixels 67108864` の比較で確認)。
`still_image_decode_unit` は、上限 16M / 32M 画素で 4200x4200 の PNG の facts の寸法が 0 / 4200 になる
ことでこれを見る。`find_stream_info` へ上限を渡すのをやめると落ちることを確認した。

`[事実]` 縮拡は premultiplied alpha で行う。straight alpha のまま bicubic で拡大すると、
不透明な赤と透明な緑の境目に `G = 0x5a` の緑が出た (ffmpeg の `scale=flags=bicubic` で確認)。
premultiply をやめると検査 (`透明な画素の色が境目へにじみました`) が落ちることを確認した。
なお幅 2 のような小さな画像では swscale の filter が縮退して最近傍と同じ結果になり、境目が
できないため、検査は 16x4 を 4 倍にして半透明の画素があることも確かめている。

`[事実]` `still_image_decode_unit` (75 件) と `media_import_files_focused` (25 件) で次を固定した。

- 判定: 静止画 9 形式 (png / jpg / tga / bmp / qoi / tiff 48bit / webp / gif / pal8 png) が 1 packet の
  静止画、アニメーション GIF / APNG が 2 packet、アニメーション WebP の flag、EXR の HDR、
  カバーアート付き mp3 / m4a / flac の映像 0 本、対照として h264 + カバーアートの mp4 の映像 1 本と
  Motion JPEG の avi
- decode: orientation 1 (対照) と 6 (日本語名の複製を含む) の画素と寸法、full range の灰色 128
  (JPEG と gray16 PNG で ±1)、pal8 の透明、上記の各形式が 64x48 で読めること、画素数と 1 辺の上限
  (それぞれ上限ちょうど・対照つき)
- 向きの 8 通りは 3x2 の画像で、EXIF の定義 (反転と回転) から手で書いた並びと比べる
- 実素材での判定: 上の静止画 (寸法は向きを反映した値) / アニメーションと HDR と壊れた素材の拒否 /
  カバーアート付きを含む音声 5 形式が Audio / Motion JPEG と h264 + カバーアートの mp4 が Video
- `media_bin_model_focused` の分類の検査は、映像の有無を MLT の `has_video` で判断させるか、
  1 frame の映像の拒否を外すと落ちることを確認した

`[事実]` fixture の作り方で 2 つ踏んだ。

- `drawbox` は rgba の alpha を書かず、`-pix_fmt pal8` への単純な変換は alpha を落とす。
  透明を持つ pal8 PNG は `geq` で alpha を明示し、`palettegen=reserve_transparent=1` で作る
- ffmpeg の mjpeg encoder は EXIF を書かない。向き付きの JPEG は SOI の直後へ最小の APP1 Exif を
  byte で差し込んで作る (`Add-JpegExifOrientation`)

`[未検証]` (§18.6 で変更: sRGB 以外の ICC profile を持つ画像は拒否する)
ICC profile は無視している (Display P3 などの素材は彩度が落ちる)。CMYK JPEG、
TIFF の associated alpha、JPEG XL、JPEG 2000、PSD は素材を作っておらず、読めるかも色が正しいかも
測っていない。許可表に入れているが、実際に読めなければ decode の失敗として拒否される。
SVG は許可表に入れていない (描画解像度を決める必要があるため)。
上限 64 MiPx は決め打ちであり、実在する素材の大きさを調べて決めた値ではない。

### 18.3 decode source の無い composition の提示

`[事実]` 以前は映像の無い区間 (画像だけ・音声だけ・何も無い) で次のようになっていた。

- controller の `syncPreviewSourcesAt` が、映像 layer が 0 なら composition も音声も入れずに戻っていた
- 再生は engine を `play()` せず、controller の時計だけを進めていた (`clockOnlyPlayback_`)。
  **このため音声だけの区間は 1x で無音だった**
- `PreviewSurface` は `visible: previewVideoAtPlayhead` で、映像の無い frame では render されなかった
- engine は静止画だけの composition と、effect 付きの静止画 layer を拒否していた

`[事実]` 変更後は次のとおり。

- engine (`src/preview_engine/preview_engine.cpp`)
  - 静止画だけの composition を受理し、decode layer が 0 枚の分岐 (従来の gap) で静止画を
    `composeLayersToTarget` で描く。提示の authority は scheduler の時計 (`docs/preview-engine-contract.md` §7.0)
  - 静止画 layer の effect を受理し、`effectsEnabled` と `rotationDegrees` を compositor へ渡す。
    compositor は RGBA8 を既に effect 経路で描けた (`drawInternal` の `rgbaPs_`)。fade は source frame
    番号で評価されるので、静止画では 0 を要求する
  - **gap の分岐の seek 完了が、再開する音声 transport を控えていなかった。** そのため音声だけの区間で
    seek して再生を再開すると、音声が戻らず「audio master clockが停止しています」で fatal になった。
    控える処理を decode layer の有る分岐と共用の `captureSeekAudioResume` にした
- controller (`apps/mvm/mvm_controller.cpp`)
  - `syncPreviewSourcesAt` の早期 return と `clockOnlyPlayback_` を撤去した。映像の無い区間でも
    composition を出し、音声を差し替え、engine を再生する。gap から映像へ入るときは
    `handOffPlaybackSources` が layer 数の違いで失敗し、既存の組み直しの経路へ落ちる
  - 文字の preview 更新 (`refreshTextPreview`) も映像の有無で分けない
- `mapTimelinePreviewFrame` の合成 layer 上限を、映像の無い frame にも掛ける (文字も engine が合成するため)
- `Main.qml` の `PreviewSurface` を常に表示し、UI が文字を重ねるのはドラッグ・編集中の文字だけにした

`[事実]` 次の検査で固定した。

- `preview_engine_p5b_unit` (`composition still layers`): 静止画 2 枚だけの composition と effect 付きの
  静止画を受理する。fade in / fade out 付きの静止画を拒否し、対照として同じ fade の video layer は受理する
- `still_layer_compositor` の 6: video layer の無い composition で、左 1/4 が白の静止画を 180 度回すと
  白が右端へ移る。対照として effect 無しでは左端に残る
- `preview_engine_sourceless_playback` (`mvm_sourceless_playback_smoke`、workstation):
  静止画 2 枚 (1 枚は回転付き) を音声なしで再生して 2 layer の frame を 30 枚、空の composition + 音声で
  30 枚 (meter 0.5)、再生中に seek して seek 先より後で 30 枚を提示する。gap の分岐の
  `captureSeekAudioResume()` を外すと、seek 後に「audio master clockが停止しています」で落ちることを確認した
- `sourceless_timeline_playback` (workstation): 実際の preview surface を付けた controller で A1 の音声だけの
  timeline を再生し、playhead が 0 -> 61 と進み meter の最大が -6.0 dB になる。
  `syncPreviewSourcesAt` の早期 return を戻すと、playhead 0 -> 0、meter -60.0 dB で落ちることを確認した
- `m7b_2_timeline_preview_mapping_focused`: 映像の無い frame の文字 16 枚を受理し、17 枚目で拒否する

`[未検証]` 1x 再生中に gap から映像の区間へ入るとき、組み直しで一瞬止まる時間。
静止画だけの区間で opacity key を再生したときの遅れ (再生中は GUI の tick で composition を出し直すので、
文字と同じく 1〜2 frame 遅れうる)。`PreviewSurface` を常に表示したときの負荷の変化。

### 18.4 Image clip と Project schema 8

`[事実]` `TimelineClipKind::Image` (保存名 `"image"`) を足し、`kProjectSchemaVersion` を 8 にした。
§1 と同じく互換分岐は持たず、schema 7 のファイルは読まない (`clip_effects_focused` の `schema7.mvm`)。

- 文字と画像は「素材の時間軸を持たない clip」として同じ規則に従う (`isStillClipKind`)。
  素材 frame domain は in = 0・out = 尺 の合成値で、速度 1/1・リンク無し・スリップとレート調整は不可。
  fps は置いたときの timeline の値で、timeline の fps を変えても振り直さない (尺は clip の fps で換算され、
  trim で現在の fps へ揃う)。これを `validateTimeline` で検査する
- 画像は mediaPath が必須。effect は video と同じく位置・拡大・回転・crop・不透明度 (key・fade) を持ち、
  音量は持たない。置いたときの既定の尺は 5 秒 (`defaultStillClipFrames`、文字と共有)
- 置き場所は文字と同じ `placeStillClipAt` (旧 `placeTextClipAt`)

`[事実]` **文字 clip の分割が常に失敗していた。** 文字の trim は両半分とも in = 0 に振り直すので、
`splitTimelineClips` の「左の out と右の in が一致する」検査に必ず当たり、「分割位置を素材 frame へ
一意に換算できません」になった (小さなプログラムで分割を呼んで確認)。文字・画像では境界の一致を
timeline 上で見るようにした。

`[事実]` preview は、画像を `mapTimelinePreviewFrame` の静止画 layer (`stillLayers`、旧 `textLayers`) に入れる。
画素は静止画 decoder で decode し、出力解像度の raster へ配置したもの (§18.2) で、素材 path と
出力解像度を key に controller が保持する。effect → layer の変換は `applyPreviewLayerEffects`
(`timeline_preview_mapping`) に出し、video と画像で共有する。書き出しは同じ decoder の raster を PNG に
stage し、文字と同じ qimage producer で開く (`MvmExportClip::is_still_image`、旧 `is_text`)。

`[事実]` preview と書き出しの画素比較 (`image_preview_parity`) で、画像に限らない既存の不具合を 2 つ見つけて直した。

1. **preview の回転が正規化座標で行われていた。** effect の vertex shader が 0..1 の座標のまま回していたため、
   正方形でない出力では回転が shear になっていた。回転を画素空間で行うようにした
   (`nv12_converter.cpp`、`geometry.zw` に出力の寸法)
2. **書き出しの V1 の回転が効いていなかった。** V1 の affine filter は `transition.fix_rotate_z` を使っていたが、
   MLT 7.36.1 では 2D 回転は keyed=0 の `fix_rotate_x` が担う (`docs/m7b-p0-findings.md`)。上位 track の
   transition はこれに従っていたが、V1 だけ残っていた。既存の `m4_timeline_export_focused` は回転 25 度を
   掛けて「外接矩形が 230 x 210 未満」だけを見ており、回転が無くても通っていた

両方を直した後、V1 の画像 (位置 +12% / -8%、拡大 70%、回転 15 度、右 crop 10%) の preview と書き出しの
画素の分類は 51080 / 51080 で一致した。対照として preview だけ位置を 25% ずらすと 32817 / 51080 に落ちる。
書き出し側を `fix_rotate_z` に戻すと 46516 / 52358 で落ちることを確認した。
`m4_timeline_export_focused` は外接矩形を手計算の期待値 (±5 px) と比べるようにし、回転を crop と分けた。
回転 25 度の実測 x 82..314 / y 0..205 は期待 x 81..316 / y 0..207 と一致し、`fix_rotate_z` に戻すと
x 112..285 / y 30..173 (回転なし) で落ちる。

`[事実]` **crop と回転を同時に使うと、MLT の書き出しは shear になる。** 320x240 で crop (左 10 / 上 10 /
右 20 / 下 5%) と回転 25 度を掛けると平行四辺形になり、crop を外すと正しい矩形になった
(書き出した frame を目視で確認)。preview は crop を sourceUv で行うので shear にならず、この組み合わせだけ
preview と書き出しが食い違う。V1 の回転が効いていなかった以前は、回転そのものが書き出されていなかった。

`[推測]` crop filter が frame の寸法を変え、affine が正方形でない座標系で回転しているため。
上位 track (transition 経路) も同じ crop filter を使うので同様と考えているが、測っていない。

`[未検証]` (§18.6 で解消) crop + 回転の書き出しを直す方法 (crop を寸法を変えない方法で掛けるなど)。上位 track での同じ組み合わせ。

`[事実]` `image_clip_contract` で次を固定した。

- schema 8 の往復 (path は同じファイルを指すこと、他の field は完全一致)
- 検証の負例 (path が空、速度 50%、in ≠ 0、out ≠ 尺、音量、audio track、文字データ、リンク付き。
  リンク付きは対照としてリンクを外すと受理する)。位置・拡大・回転・crop・不透明度 key は受理する
- 既定の尺 (60fps で 300、29.97fps で 150、24fps で 120)、trim で素材の尺を超えて伸ばせること、
  スリップとレート調整の拒否、画像と文字の分割 (0-100 / 100-300)、V1 の画像の上 (V2) への配置、
  preview で静止画 layer に入ること
- 書き出しの画素: 64x32 の画像が 320x240 で上下 40 px の余白を持って中央に出ること、
  EXIF orientation 6 の画像が縦長になり赤が右上に来ること
- in / out の検査を外すか、分割の境界の検査を素材 frame に戻すと落ちることを確認した

`[事実]` 途中で、MLT の shutdown (`mvm_mlt_runtime_shutdown`) の後に静止画 decoder を呼ぶと avformat の中で
落ちた (gdb の backtrace で `openMediaInput` の中)。製品は終了時にだけ shutdown するので、検査側で順序を直した。
`[推測]` MLT の avformat module が登録した log callback が、module の unload 後も残るため。

`[未検証]` (§18.6 で解消: decode は worker、差し替えは同一性と再検証で見つける)
大きな画像を最初に表示するときの GUI thread の停止時間 (decode と配置を同期で行う)。
素材ファイルを外部で書き換えたときの preview の画素の更新 (path と出力解像度が同じなら保持したものを使う)。

### 18.5 配置の経路と UI

`[事実]` 素材を timeline へ置く経路を、内容の判定 (`probeMediaFile`) から振り分ける形にした。拡張子は見ない。

- `addMediaFileToTimeline(url)`: 判定した種別で Video / Audio / Image のどれかとして置く。
  メニューの「メディアを追加」(旧「動画を追加」「音声を追加」を統合) と、timeline への drop がここを通る。
  以前の drop は常に `addVideoClip` を呼び、音声・画像の drop は失敗していた
- `addImageClip(url)` / bin の「タイムラインに追加」: 画像を再生ヘッドの位置に、最上位の clip より上の
  空いた映像 track (`placeStillClipAt`) へ 5 秒で置く。判定の結果を bin への登録にも渡し、decode を繰り返さない
- `addAudioClip(url)`: 以前は A1 の末尾へ足し、audio track が無ければ失敗していた。再生ヘッドの位置に、
  A1 から順に空いた非 mute の audio track へ置き、無ければ audio track を足す (`placeAudioClipAt`)
- 置いた後の選択 (preview の seek) が失敗しても、置いた編集は commit 済みである (既存の `addVideoClip` と同じ)

拡張子の表は `apps/mvm/media_file_filters.h` の 1 か所に置き、`MvmController::mediaFileNameFilters` として
メニューのダイアログと bin の「読み込み…」の両方へ出す。表は選びやすさのためだけにあり、「すべて」も選べる。
判定はあくまで内容で行う。

`[事実]` `m7b_4_controller_export_lifecycle` に次を足した。

- bin の画像を「タイムラインに追加」できる (以前は拒否を検査していた)
- 再生ヘッド 30 で、画像が V1 の映像の上 (V2) に 30 から 300 frame で置かれる
- audio track の無い Project で音声が A1 を足して置かれ、同じ位置へもう 1 つ置くと A2 を足して置かれる
- アニメーション GIF は「アニメーション」を含む理由で拒否され、Project は変わらない。画像は `addVideoClip` で
  動画として置けない
- 音声の配置を A1 の末尾への追加に戻すと「追加先の track が存在しません」で落ちることを確認した
- UI 構造の検査 (`m7b_4_timeline_ui_architecture`) は、drop が `addMediaFileToTimeline(url)` を呼ぶことを要求する

`[事実]` この検査で MLT を 1 プロセス内で 2 回初期化したところ、2 回目の初期化の途中で
`GGML_ASSERT(prev != ggml_uncaught_exception)` により落ちた (gdb で ggml-base.dll の読み込み中の例外を確認)。
ggml は UCRT64 の FFmpeg の whisper filter の依存である。製品は MLT を 1 回だけ初期化するので、
検査側で初期化を 1 回にまとめた。

`[未検証]` (§18.6 で CPU 側に budget を入れた) 画像を多数 (数十枚) 置いた timeline での preview の memory。
画像 1 枚は出力解像度の RGBA8 (1080p で約 8MiB、4K で約 32MiB) を CPU と GPU にそれぞれ持つ。

### 18.6 レビュー指摘への対応 (P1 1 件 / P2 3 件 / P3 1 件)

#### crop + 回転の書き出しの shear (P1)

`[事実]` 書き出しの crop を、寸法を変える MLT の `crop` filter から、範囲外を透明で塗って寸法を保つ
`qtcrop` filter に替えた (`attach_export_crop`)。affine へ渡す矩形は crop 範囲ではなく crop 前の frame 全体を
置く位置にし (`mapExportEffects`)、MLT は矩形の中心で回すので、preview と同じく crop 範囲の中心で回るよう
中心の差 d について全体を d - Rd ずらす。`qtcrop` は必須 service に加えた。

`[事実]` 以前の shear は動画素材でだけ起きていた。画像 (qimage producer) は以前の crop filter でも
非対称 crop + 回転 25 度で preview と一致した (`image_preview_parity` を変更前の書き出しで実行して確認)。
`[推測]` producer によって crop filter の後の frame の扱いが違うため。そこで動画の検査は
`m4_timeline_export_focused` に置いた: 320x240 の動画に crop (10/10/20/5%) と回転 25 度を掛け、外接矩形が
手計算の x 96..269 / y 23..191 (±5 px) になること。変更前の書き出しでは x 58..305 / y 41..171 で落ちる。

`[事実]` 途中で、**V1 が空の区間では V2 以上の clip の effect が書き出されていなかった**ことが分かった。
V1 の gap は blank (test card) で、affine transition は下の frame が test card だと上の frame をそのまま返す。
画像を V2 に置いて V1 を空けると、位置・拡大・回転・crop・不透明度が掛からずに全画面で出ていた。
V1 の gap を黒の `color` producer で埋めるようにした (`append_gap`)。gap の数 (`playlist_blank_count`) は
以前と同じく数える。

`[事実]` `image_preview_parity` を 6 case に広げた: V1 の右 crop + 回転 15 度、V1 の非対称 crop + 回転 25 度、
V2 の回転だけ、V2 の非対称 crop だけ、V2 の非対称 crop + 回転 25 度 (V1 に灰色の背景)、同じく V1 が空。
画素の分類 (赤・青・黒) が境界を除いてすべて一致し、対照 (preview だけ位置を 25% ずらす) では 34〜64% に落ちる。
書き出しを変更前に戻すと V1 が空の case が 12931 / 37390 で落ちる。

#### 画像 cache が素材の差し替えと旧解像度を扱えない (P2) / 大きな画像の decode が GUI を止める (P3)

`[事実]` preview 用の raster を `ImageRasterCache` (`apps/mvm/image_raster_cache.*`) に移した。
`WaveformCache` と同じ規則で、素材の同一性の判定 (`media_source_identity.h`) は両者で共有する。

- decode と配置は worker thread。完成までは request が Loading を返し、controller はその画像を合成に
  入れず、完成 (`entryChanged`) で preview を組み直す
- key は素材の実体 (volume + file ID) と出力解像度。request のたびに size と更新時刻 (100ns) を照合し、
  差し替えられていれば作り直す。size と更新時刻が同じまま中身だけ変わったものは、アプリが前面へ戻ったときの
  `revalidateMedia` → `revalidateAll` の内容 fingerprint で見つける (`main.cpp` から波形と同じ契機で呼ぶ)
- `refreshTimelineModel` で、現在の画像 clip と現在の出力解像度の組だけを残す (`retainOnly`)。
  出力解像度を変えると旧解像度の raster は捨てる
- 512MiB の byte budget を超えたら、engine の composition も controller も参照していないものから古い順に捨てる
  (§18.7 で、参照が外れた時点でも再評価するようにした)
- 書き出しは以前から毎回 decode するので、差し替え後も preview と書き出しは同じ画素になる

`[事実]` `image_raster_cache_focused` で、初回が Loading であること、同じ素材・解像度の再利用、解像度の変更と
`retainOnly`、差し替え (BMP → 左上が赤の JPEG) での作り直し、size と更新時刻を戻した中身の変更を `revalidateAll`
で見つけること、読めない画像の Failed、budget で参照中のものを残し参照されなくなったものを捨てることを見る。
request が size と更新時刻を無視する、`retainOnly` が何も捨てない、のどちらでも落ちることを確認した。
更新時刻は Win32 で読み書きする (std::filesystem は秒精度のことがあり戻せない)。

#### fps 変更後の静止画の fade (P2)

`[事実]` 文字・画像の素材 frame domain は置いたときの fps のままなので、preview の不透明度も書き出しと同じく
`clipFadeSourceFrameAt` で素材 frame へ換算してから評価するようにした。以前は clip 内の timeline 位置を
そのまま渡していたため、60fps で作った画像を 30fps の Project に置くと、fade in 1 秒の 0.5 秒地点が 0.508 では
なく 0.254、fade out の最終 frame が 0.017 ではなく 1.0 になっていた。
`m7b_2_timeline_preview_mapping_focused` で 60 → 30fps と 30 → 60fps、文字と画像の 0.5 秒・1 秒・最終 frame を
手計算の値で固定した。換算をやめると落ちることを確認した。

#### ICC profile を持つ画像を黙って違う色で描く (P2)

`[事実]` decoder は色の変換をしないので、sRGB 以外の ICC profile (`AV_FRAME_DATA_ICC_PROFILE`) を持つ画像は
decode で拒否する (「sRGB 以外の ICC profile を持つ画像には対応していません (Display P3)。色が変わるため
読み込みません」)。header の色空間が RGB で、説明 ('desc' tag。v2 の desc 型と v4 の mluc 型を読む) が "sRGB" を
含むものだけを sRGB とみなす。名乗りと中身が違う profile は見分けられない。
(§18.7 で、説明ではなく中身で判定するように変えた)
fixture は iCCP chunk を入れた PNG を byte で作る (Display P3 と、対照の sRGB)。FFmpeg 8.1.2 の png decoder は
iCCP を ICC side data として出す (`ffprobe -show_frames` で確認)。`still_image_decode_unit` (85 件) と
`media_import_files_focused` (27 件) で拒否と受理を見る。拒否をやめると落ちることを確認した。

`[未検証]` ICC を sRGB へ変換して受理すること (LittleCMS などが要る)。JPEG の APP2 に入った ICC
(同じ side data になるはずだが、fixture を作っていない)。

#### テストの音量

`[事実]` 検証アプリの音量 `kVerificationSessionVolume` を 0.15 から 0.1 にし、これを使っていなかった
`preview_engine_sourceless_playback`、`sourceless_timeline_playback`、p5e の preview smoke と capacity smoke
(音声 16 本を同時に鳴らす) にも適用した。Windows の session volume なので PCM と meter は変わらず、
検査の値には影響しない。

### 18.7 再レビュー指摘への対応 (P2 2 件 / P3 1 件)

#### 取り込み後に差し替えた素材が、種別の判定を通らずに画像として読まれる (P2)

`[事実]` 静止画かどうかの判定 (`routeMedia`) を `apps/mvm/media_import` から `src/media/still_image/static_image`
へ移し、「判定して静止画だけを decode する」`loadStaticImage` を置いた。画像の画素が要る 3 経路、
取り込み (`probeMediaFile`)、preview の raster cache (`ImageRasterCache`)、書き出し (`exportTimeline`) は
すべてこれを通す。以前は cache と書き出しが `decodeStillImage` を直接呼んでいたため、取り込んだ静止画を
同じ path のまま animated GIF や mp4 に差し替えると、先頭 frame を静止画として受理していた。
`decodeStillImage` は判定をしない (header にそう書いた) ので、apps と src/app からは直接呼ばない。

`[事実]` 回帰テスト:

- `image_raster_cache_focused`: 静止画 PNG を Ready にした後、同じ path を animated GIF / APNG / animated WebP /
  mp4 / EXR に差し替えると Failed になり、理由 (アニメーション / 静止画ではありません / HDR) が付くこと
- `image_clip_contract`: 同じ 5 種を指す画像 clip の書き出しが、同じ理由で失敗すること。
  正しい静止画の書き出し 2 件が対照
- `still_image_decode_unit`: `loadStaticImage` が上の 5 種と Motion JPEG の avi を拒否し、静止画 PNG は読むこと。
  対照として、`decodeStillImage` 単体は animated GIF の先頭 frame を読めてしまうこと

cache と書き出しを `decodeStillImage` に戻すと、上の 2 本が 5 種すべてで落ちることを確認した
(animated WebP は decoder 単体でも失敗するが、理由が違うので落ちる)。

`[推測]` 判定と decode はファイルを 2 回開く。その間に差し替えられた場合は decode 側の結果になるが、
次の request (size と更新時刻の照合) で作り直される。

#### ICC の判定が説明の文字列だけを見ている (P2)

`[事実]` `iccProfileIsSrgb` は説明 ('desc') を判定に使わず (エラー文言に添えるだけ)、色を決める中身を調べる。
次をすべて満たすものだけを sRGB とみなす。

- header: profile size が buffer に収まる、class が `mntr`、色空間 `RGB `、PCS `XYZ `
- LUT 型の tag (A2B0..2 / B2A0..2 / D2B0..3 / B2D0..3) が無い。色管理は LUT を matrix/TRC より優先するが、
  中身を検証しないので拒否する
- 原色 rXYZ / gXYZ / bXYZ が sRGB の D50 値 (Windows 同梱の sRGB profile と同じ値) と ±0.003 で一致。
  Display P3 の赤は X が 0.079 違う
- 白色点 wtpt が D50 か D65 (v2 の sRGB profile は D65 を書く) と ±0.003 で一致
- トーンカーブ rTRC / gTRC / bTRC が、`curv` (2 点以上の表) なら各点、`para` (関数型 0..4) なら 1024 点で、
  sRGB の式と ±0.002 で一致。gamma 2.2 は sRGB と最大 0.0085 違うので通らない。1 点の `curv` (単純な gamma)
  と 0 点 (恒等) は通さない

`[事実]` `still_image_decode_unit` (109 件) で、v2 / v4 それぞれの sRGB (説明の無いものを含む) の受理、
Display P3 と「説明は sRGB、原色は Display P3」の拒否、sRGB の表の受理、gamma 2.2 (表と 1 点) の拒否、
白色点 D65 の受理と A 光源の拒否、LUT 型・bXYZ 欠け・`scnr` class・短い buffer・外を指す tag の拒否を見る。
実物として Windows 同梱の `sRGB Color Space Profile.icm` (HP の v2、`curv` 1024 点) を sRGB と判定することも見る。
fixture は原色・白色点・トーンカーブを持つ matrix/TRC の v2 profile を組み直し、説明だけ sRGB を名乗る
`png_icc_fake_srgb.png` を足した (`media_import_files_focused` でも拒否を見る)。
説明の文字列で判定する実装に戻すと、上の拒否がすべて落ちることを確認した。

`[未検証]` 実際の写真に付く sRGB profile のうち、LUT 型のもの (ICC の `sRGB_v4_ICC_preference.icc` など) は
拒否される。カメラ・スマートフォンの出力で問題になるかは確かめていない。

#### byte budget が次の decode 完了時にしか再評価されない (P3)

`[事実]` request は cache の raster を直接渡さず、参照が外れたら cache に `trimToBudget` を queued で頼む handle で
包んで渡す。同じ record からの handle は同じ address を指すので、engine の同一性の判定 (address で raster を
見分ける) は変わらない。engine の render thread で最後の handle が破棄されても、cache の破棄と mutex で排他し、
破棄後に届いた queued call は Qt が捨てる。まだ一度も渡していない raster は捨てない
(完成直後に controller が受け取る前に消えないように)。

これで「参照されていない raster が budget を超えて残る」ことは無くなった。参照中の raster は捨てられないので、
参照中のものだけで 512MiB を超える間は超えたままになる (engine が使っている画素を消せないため)。

`[事実]` `image_raster_cache_focused` で、budget 1 byte の cache に A (保持) と B (受け取ってすぐ破棄) を読むと
B だけが捨てられ、A の保持をやめると新しい decode 無しに 0 件になることを見る。対照として既定の budget では
捨てない。通知をやめると落ちることを確認した。

## 19. 編集ショートカットとトランジション

### 19.1 Project schema 13 (clip の有効/無効とトランジション)

`[事実]` `kProjectSchemaVersion` を 13 にした。clip に `enabled` (必須)、Project に
`timeline_transitions` (必須、空配列可) を足した。互換分岐は持たず、schema 12 のファイルは読まない
(`m5_timeline_edit_focused` と `m7a_1_clip_effects_focused` が出力の版を 12 に書き換えて拒否を確かめる)。
2 つを同じ版に入れたのは、手元のファイルの移行を 1 回で済ませるため。

- トランジションは同じ track で接している 2 clip の編集点に置き、`framesBeforeCut` / `framesAfterCut`
  (timeline frame) で区間 `[cut - before, cut + after)` を持つ。cut より前は incoming の頭の余白、
  後は outgoing の尻の余白を使う。種類 (ディゾルブ / クロスフェード) は track 種別で決まるので保存しない
- `validateTimeline` の検査: ID の重複、clip の存在・同じ track・接していること、フレーム保持でないこと、
  長さ (負でなく 1 frame 以上)、素材の余白、clip の尺、同じ clip 端に 1 つだけ、同じ clip 端のフェードとの
  併用禁止、1 clip の前後のトランジションが内側で重ならないこと。規則ごとに負例がある
- 編集の確定は `finalizeTimelineCandidate` (= `reconcileTimelineTransitions` + `validateTimeline`) を通す。
  離れた・clip が消えたトランジションは消し、余白や尺が減れば縮める。分割は outgoing 側を右半分へ
  付け替え、fps 変更は長さを秒位置で換算する。JSON の読み込みは reconcile せず fail-closed にする
- effect の変更 (`setClipEffectValues`) は reconcile を通さない。トランジションのある端へフェードを
  付けようとするとエラーになる

`[回避策]` 描画 (preview・音声・シャトル/スクラブ・書き出し) はまだトランジションと無効 clip を扱えない
(無効 clip は §19.2 で対応した)。
黙って無視すると見えるものと保存内容が食い違うので、`unsupportedTimelineRenderFeature` で
mapping を失敗させている。UI からはまだどちらも作れないので、手で編集したファイルでだけ起きる。

`[事実]` 手元の `build/ucrt64-debug/m6a-gui/project.mvm` は使い捨てのスクリプトで一度だけテキスト変換した
(clip 8 件に `"enabled": true`、`"timeline_transitions": []` を追加。元ファイルは `*.schema12.bak`)。
変換後のファイルが現在の loader で読めることを確認した (schema 13、clip 8、トランジション 0)。

### 19.2 clip の有効/無効 (Shift+E)

`[事実]` `toggleClipsEnabled` は対象 (選択、無ければ current clip) とリンク相手のうち 1 つでも有効なら
全部を無効に、全部が無効なら全部を有効にする。1 回が 1 undo。右クリックメニューからは押した clip だけを
切り換える。

- 無効にした clip は timeline に残し、暗く表示する (model の role は `clipEnabled`。`enabled` にすると
  delegate の `Item.enabled` を隠して MouseArea まで止まり、選び直せなくなる)
- preview は layer / audio から外す (上の track の clip を無効にすると下の track が見える)。シャトル・スクラブの
  音声、再生ヘッドの映像判定 (`previewVideoAtPlayhead` / `clipVisibleAtPlayhead`)、inspector と再生開始の
  clip (`topVideoClipAt`) も無効 clip を選ばない。編集 (`activeClipAt`) は無効 clip も対象にする
- 書き出しは無効 clip を外し、尺は timeline 全体のまま保つ。穴は tractor が黒・無音で埋めるので、
  1 つでも外したら tractor にする。全部が無効なら「書き出す有効なclipがありません」で失敗する

`[事実]` 除外の検査を外すと `m7b_2_timeline_preview_mapping_focused` が落ちることを確認した。

`[未検証]` track の mute は書き出しで無視されている (preview では外す)。無効 clip とは別の既存の食い違いで、
今回は変えていない。

### 19.3 既定のトランジション: clip 選択時のフェード (Shift+D)

`[事実]` clip を選んで Shift+D を押すと、選択 clip とリンク相手の先頭と末尾に 1 秒 (timeline frame で
`round(fps)`、`defaultTransitionFrames`) のフェードを付ける。黒・無音からの片側のトランジションは clip 内の
減衰そのものなので、新しい表現を作らず既存の `fadeInFrames` / `fadeOutFrames` を使う (preview・シャトル・
書き出しが既に同じ規則で扱っている)。

- フェードは素材 frame で持つので、clip ごとに timeline の境界から 1 秒内側の位置を素材 frame へ戻して
  換算する (2 倍速なら素材 120 frame)。リンク相手も自分の素材 frame で換算する
- 1 秒より短い clip は前後で分け合う (先頭が半分を切り上げで取る。1 frame の clip は 1 / 0)
- トランジションのある端は変えない (同じ端でフェードとトランジションを併用しない、§19.1)
- 何も変わらなければ「フェードは既に付いています」で失敗し、undo を積まない

`[事実]` 1 秒より短い clip で末尾側の内側の位置が clip の先頭を越えて負になり、換算が失敗していた
(`m5_timeline_edit_focused` の 50 frame / 1 frame の clip で検出)。内側の位置を clip の中に収めて直した。

`[事実]` 音声のフェードは直線 (等パワーではない)。無音への片側のフェードなので許容し、等パワーは 2 clip の
クロスフェード (トランジション) だけで使う。

### 19.4 トランジションの描画区間 (書き出し・シャトル・スクラブ)

`[事実]` 描画は `project::timelineRenderSegments` (src/project/timeline_render) が作る区間に一本化した。
トランジションの無い clip は clip そのものが 1 区間。トランジションがあると clip を余白の分だけ延ばし
(outgoing は cut の後ろへ、incoming は cut の前へ)、同じ track の 2 clip を重ねる。延ばした clip は
`clipWithEdgeAt` (trim・分割と同じ `trimClipBoundary`) で作り、端がちょうどその位置に来なければ失敗する。

- 映像: incoming の頭の区間 `[cut - before, cut + after)` を lane 1 に切り出して outgoing の上へ重ね、
  不透明度を `p = (f - start + 0.5) / frames` で上げる。outgoing は lane 0 で不透明のまま残す。不透明な
  clip どうしなら `A(1 - p) + B p` になる。両方を `1 - p` / `p` にすると中央で暗くなる
  (`0.25 A + 0.5 B`)。incoming が透過・変形していると下の outgoing が減らず、区間の終わりの 1 frame で
  突然消えるので、映像のトランジションは画面全体を覆う不透明な clip どうしに限る (§19.10)
- 音声: clip ごとに 1 区間で、incoming に `sin(p π/2)`、outgoing に `cos(p π/2)` を掛けて加算する
  (等パワー、二乗和が 1)
- 延ばした区間の effect (key・フェード) は clip の端の値のまま評価する
- 無効な clip を含むトランジションは描かない (両側とも cut で切り替わる)

`[事実]` 書き出しは MLT の luma / mix の dissolve を使わない。track ごとに lane 0 と、トランジションの
ある track は lane 1 を MLT の layer として積み (`video_track` は layer 番号)、lane 1 は既存の V2 以上と同じ
affine の overlay (per-frame の不透明度) で合成する。音声は既存どおり clip ごとの playlist を `mix sum=1`
で加算する。トランジションがあれば tractor を使う。

`[事実]` 確認したこと:
- `m7b_3_timeline_export_mapping_focused`: 3 区間の配置 (素材範囲・layer・不透明度 0.025 / 0.525 / 0.975)、
  上の track の layer のずれ、2 倍速の延長 (timeline 10 frame = 素材 20 frame)、クロスフェードの gain
  (cos(0.525 π/2) = 0.678801 と二乗和 1)
- `m6b_shuttle_audio_mix`: 重なった 2 clip を区間中央 (p = 0.5) で 1/√2 ずつ加算する
- `m4_timeline_export_focused` (実 MLT、label extended): 青→赤のディゾルブの中央 (frame 60) が半々に混ざり、
  前後で青・赤になる
- 不透明度・gain の掛け算を外すと 3 つとも落ちる

preview は §19.5 で対応した。

### 19.5 preview でのトランジション

`[事実]` preview の mapping も `timelineRenderSegments` の区間から作る。layer には (track, lane) を下から
数えた `slot` を持たせ、合成順 (`previewLayerStack`) と preview source の置き場所 (`trackSources_` の key) を
slot で決める (書き出しの MLT layer と同じ数え方。トランジションが無ければ track の index と同じ)。

- decode source は延ばした区間の clip (`renderClip`) で作る。source は in より前の素材を写せないので、
  incoming の頭の区間は延ばした in から始める。同じ素材を分割してディゾルブすると、同じファイルの
  source が 2 つ同時に要る
- incoming の不透明度は、effect が既定値でも進み具合を掛けて渡す。延ばした区間の effect は clip の端の値
- 音声は同じ track の 2 clip を開始の早い順に重ね、gain は書き出しと同じ `renderSegmentGain`。
  クロスフェードの区間は audio source の identity に含める (区間が変われば作り直す)
- 暫定の描画ガード (`unsupportedTimelineRenderFeature`) は外した

`[事実]` `transition_preview` (workstation、実 D3D11 surface): 同じ動画を 2 つに分けた clip の cut に前後
10 frame のディゾルブを置き、区間の中 (frame 125) で 2 layer を合成して不透明度が 1.0 / 0.775 になること、
区間の後は 1 layer になること、区間の前 (frame 90) から再生して区間を通り抜けても再生が続くこと
(frame 151 まで) を確認した。音声も同じ WAV を 2 つに分けてクロスフェードさせている。incoming の不透明度の
掛け算を外すとこの試験が落ちる。

`[未検証]` 区間に出入りするたびに layer 数が変わるので、再生中は source を組み直す (clip 境界と同じ経路)。
試験では再生が止まらないことだけを見ており、組み直しの間に frame が落ちるか (表示の滑らかさ) は測っていない。

### 19.6 編集点の選択と、編集点での既定のトランジション (Shift+D)

`[事実]` 選択・リップル・ローリングのツールで clip の端を動かさずに離すと編集点を選ぶ
(`TimelineGestures.edgeRelease` が `selectEdit` を返す。レート調整では何もしない)。接している clip の無い端は
clip の選択になる。編集点・トランジションの選択は clip の選択と排他で、`setTimelineSelection` が外す。
編集・undo の後に無くなった編集点・トランジションの選択は `refreshTimelineModel` で外す。

- 編集点 (またはトランジション) を選んで Shift+D を押すと `applyDefaultEditTransition` が 1 秒のトランジションを
  置き、置いたトランジションを選ぶ。cut を中央にし、片側の余白が足りなければもう片側へ寄せる
  (尻の余白 10 frame なら 50 / 10)。両側とも余白が無ければ「素材の余白が足りないため
  トランジションを作れません」で失敗する。既存のトランジションは置き換え、その端のフェードは消す
- 速度や fps の違いで延ばした端が素材 frame に乗らない長さは、描画区間を作れるまで長い側から 1 frame ずつ縮める
  (30fps 素材を 60fps timeline に置くと 61 frame は 60 frame になる)
- リンク相手どうしも同じ cut で接していれば、同じ長さで一緒に置く (映像のディゾルブと音声のクロスフェード)。
  ずらして置いた音声は別の編集点なので置かない
- timeline は cut の前後の区間にトランジションを描き、押すと選ぶ。Delete は選んだトランジションを消し、
  そうでなければ従来どおり clip を消す。編集点を選んだ Delete は何も消さない

`[事実]` Repeater の model (`timelineTransitions`) は `stateChanged` ではなく専用の
`timelineTransitionsChanged` で、中身が変わったときだけ通知する。`stateChanged` で通知すると、再生中や
clip の選択のたびに delegate が作り直される (`text_ui_direct_input` で、押そうとした item が選択の変更で
作り直されて消えることを見つけた)。選択の表示は model に入れず、delegate が `selectedTransitionId` と比べる。

`[事実]` 確認したこと:
- `m5_timeline_edit_focused`: 中央揃え 30 / 30、置き換え、片側の余白不足での寄せ、余白なしの拒否、
  61 → 60 frame への縮め (縮めを外すと落ちる)、リンク相手への作成と Single、削除
- `m7b_4_controller_export_lifecycle`: 分割した映像・音声の編集点で 2 つ作成・選択、1 undo、redo、
  選んだトランジションだけを Delete、clip の選択で編集点の選択が外れる
- `text_ui_direct_input` (workstation): 実 window で clip の端を押して編集点を選び、Shift+D で置き、
  描いたトランジションを押して選び、Delete で消す (clip は消えない)
- `tst_timeline_gestures`: 動かさずに離したときの `selectEdit` と、レート調整の `none`

### 19.7 上書き移動と、移動時の吸着

`[事実]` clip の移動は上書きで置く (Premiere の上書き)。`moveClips` に `newId` を渡すと、動かした clip の下になる
他の clip を、丸ごと覆えば消し (リンク相手は未リンク)、端が掛かれば削り、中に置けば 2 つに分ける (右側は新しい
ID で未リンク、outgoing 側のトランジションは右側へ付け替え)。`newId` を渡さなければ従来どおり重なりを拒否する。
controller の移動 (`moveTimelineClip`) は上書きで呼ぶ。複製 (Alt ドラッグ) は変えていない。

`[事実]` 移動のドラッグ中、動かす clip 群の両端が他の clip の両端・再生ヘッド・timeline 先頭から 8 px 以内に来たら、
端がちょうど重なる量へ寄せ、吸着した位置に縦線を出す (`Gestures.dragSnapFrames` / `snapDragOffsetX`)。
Ctrl を押している間は吸着しない (プレビューの枠と同じ)。一緒に動く clip は `timelineDragBounds` の `clipIds` で
候補から外す。

`[事実]` 確認したこと:
- `m5_timeline_edit_focused`: 中へ置いたときの分割 (0-100 / 100-160 / 160-300、右側の素材 in = 160)、末尾・先頭の
  削り、丸ごと覆った clip の削除とリンク相手の未リンク、`newId` 無しの拒否
- `m7b_4_controller_export_lifecycle`: 重なる位置への移動で下の clip の末尾を削る、1 undo、`clipIds`
- `tst_timeline_gestures`: 閾値の内外、最も近い吸着先の選択、自分の端に吸着しないこと
- `text_ui_direct_input` (workstation): 実 window で V2 の clip を V1 の終端の 4 frame (6 px) 手前で離すと、
  ちょうど終端 (120) から始まる

### 19.8 JavaScript の `.pragma library`

`[事実]` `TimelineGestures.js` と `PreviewTransform.js` の `.pragma library` を外した。QML 専用の指示なので、
VS Code の JavaScript 検査 (TypeScript) が "Unexpected keyword or identifier" の構文エラーにしていた
(`scripts/lint.ps1` の検査ではない)。どちらも状態を持たない関数だけなので、import した QML ごとに別の instance に
なっても振る舞いは変わらない (`tst_timeline_gestures` / `tst_preview_transform` が通る)。

`[事実]` `.pragma library` を外したため、configure のたびに Qt が「各 QML 文書で評価し直す」と警告していた
(`qt_add_qml_module` の 2 target × 2 file)。どちらの JS も QML からは相対パスで import しているので、
`QT_QML_SKIP_QMLDIR_ENTRY TRUE` で module の qmldir に載せないようにして警告を消した。あわせて、意図して
使っている GuiPrivate の警告も `QT_NO_PRIVATE_MODULE_WARNING` で止めた (版は `mvm_guard_qt` が固定する)。

### 19.9 トランジション・clip 境界での再生の一瞬の停止

`[事実]` 再生中に表示する decode source が変わる (トランジションの区間に入る、別ファイルの clip へ切り替わる) と、
controller は preview を一時停止して source を組み直す ("clip境界でPreviewを組み直しています")。engine の
`addSource` / `removeSource` が `ReadyPaused` でしか受理しないため。

`[事実]` engine を変えずに、再生前に先の source を登録しておく案は成立しない:
- `seekFrameRequest` は accepted composition の source 集合と一致する要求しか受理しない
  ("source frame requestがaccepted compositionのsource集合と一致しません")。合成に使っていない source を seek できない
- seek していない `SourceDecodeWorker` は素材の先頭から decode し、timeline 対応の in より前の frame は
  出力区間へ換算できず fatal になる (`sourceFrameOutputInterval` が無効を返す)
- 不透明度 0 で合成に入れておくと、その source の frame が無い間は exact pairing が frame 全体を落とす

`[事実]` `feature/playing-source-handoff` で engine が `Playing` 中の source 追加・削除を受理するようにした。
映像は mapping の最初の素材 frame へ seek し、output anchor を設定してから worker を再生する。
controller は `timelineRenderSegments` に基づく次の区間開始を 2 秒前から準備し、境界では
composition を切り替える。旧 source は新 composition の提示後に削除する。音声入力は callback と
排他して差し替え、主音声の交代と無音区間でも timeline sample の時計を進める。

`[事実]` release の `transition_preview` を実行し、トランジション、別ファイルへの cut、
無音から音声、音声から無音の 4 条件すべてで境界前後の提示が続き、境界 ±1 frame の
source pairing 欠落は 0、controller の組み直しは 0 回だった。続けて同じ試験を 3 回実行し、
3/3 通過した。代表 run の区間全体の engine drop は順に 0 / 0 / 18 / 19 frame で、
scheduler や OS 負荷による drop を含む。
source 準備の最大観測時間は順に 28.3 / 14.2 / 21.6 / 11.5 ms で、いずれも 1 秒未満だったため
先読み幅 2 秒を採用した。再現手順は
`pwsh scripts/build.ps1 -Target mvm_test_transition_preview`、
`ctest --test-dir build/ucrt64-release -R '^transition_preview$' -V --timeout 120`。
登録上限を 1 本にした負例では、理由に登録上限を含む組み直しが 1 回発生し、再生を再開した。

`[未検証]` 他の GPU / 音声 endpoint、長尺素材、操作中の高負荷環境での境界欠落率は未測定。

### 19.10 レビュー指摘への対応 (P1 1 件 / P2 4 件)

`[事実]` P1: クロスディゾルブは incoming を不透明度 p で outgoing の上に重ねて作るので、incoming が透過・縮小・
移動・切り抜きされていると、覆わない所で outgoing が 100% 見え続け、区間の終わりの 1 frame で突然背景へ
切り替わる。各 clip を別々に描いてから混ぜる合成 (compositor の dissolve) を持つまでは、映像のトランジションを
「画面全体を覆う不透明な映像 clip どうし」に限る (`validateTimeline` の規則)。
- 文字・画像・フレーム保持は不可。位置・拡大・回転・切り抜きは既定値、区間の中の不透明度 (値・key・fade) は 1
- 作成 (Shift+D) は余白不足と取り違えずにこの理由で断る。既にトランジションのある clip へ効果を付けると
  効果の確定 (`setClipEffectValues`) が同じ理由で失敗する (先にトランジションを消す)
- 音声のクロスフェードは制限しない
- alpha を持つ動画素材は取り込みで断る (§19.11)

`[事実]` P2-2: リンクした映像・音声の編集点へ置くとき、両方の編集点の余白から cut の前後それぞれの上限を
先に求め、同じ長さ・同じ前後で置く。以前は主の編集点の長さをリンク相手へ要求し、相手が縮めても主は
元の長さのままだった (音声の尻の余白 20 frame で、映像 30 / 30・音声 30 / 20)。今は両方 40 / 20。

`[事実]` P2-3: シャトル音声の gain を求める frame への換算が失敗したら、clip 先頭の gain で鳴らさずに
失敗させる (fail-closed に戻した)。`[未検証]` 有効な timebase ではこの換算は int64 を超えないと失敗しない
ので、公開 API から失敗させる負例は作れていない。

`[事実]` P2-4: preview の frame 問い合わせは、Project が変わったときに 1 度だけ作る `TimelinePreviewPlan`
(描画区間と、その timeline 上の範囲・slot の起点) を引く。controller は `refreshTimelineModel` (project_ を
変える全経路が通る) で捨て、次の問い合わせで作り直す。区間の作り方は `timelineRenderSegments` に一本化した
まま。`timeline_preview_plan_bench` (label performance、release) で 2000 clip・各 cut にディゾルブの
timeline を 2000 frame 問い合わせると、1 frame あたり 36000 µs (毎回作り直し) → 1.7 µs (使い回し)、
作るのは 1 回 23 ms だった。作り直しを外すと `m7b_4_controller_export_lifecycle` が落ちる。

`[事実]` P2-5 (`[` / `]` の向き): 変えない。利用者の判断で、キーボードで上にある `[` を音量を上げる操作に
している (Premiere とは逆)。

### 19.11 透過 (alpha) のある動画は未対応とする

`[事実]` preview と書き出しで alpha の扱いが食い違うので、透過のある動画は取り込みで断る
(「透過 (アルファ) のある動画には対応していません」)。scratchpad で作った半透明の赤 (alpha 50%) で実測した:

| 形式 | preview の decode (`mvm_test_gpu_decode`) | MLT (`mvm_bench probe`) |
| --- | --- | --- |
| ProRes 4444 (yuva444p12le) | D3D11VA が無く開けない | alpha 50% を残す |
| PNG in MOV (rgba) / QuickTime RLE (argb) | 同上 | alpha 50% を残す |
| VP9 alpha (webm、yuv420p + alpha_mode=1) | NV12 で開き alpha を捨てる (不透明) | libvpx で alpha 50% を残す |

preview は D3D11VA の hardware decode だけで NV12 / P010 (alpha 無し) を出し、書き出しの MLT は software decode で
alpha を残す。VP9 alpha は preview で不透明、書き出しで半透明になり、クロスディゾルブの条件 (§19.10) もすり抜けていた。
HEVC alpha は手元の x265 が alpha を encode できず試していない。

- 判定は 2 つ: stream の事実 (`MediaStreamFacts::videoAlphaCapable`。画素形式の `AV_PIX_FMT_FLAG_ALPHA`、または
  VP8 / VP9 の webm の `alpha_mode=1`) と、MLT が decode した frame の実測 (`MvmMltProbeResult::has_alpha`)。
  どちらかが真なら断る
- 取り込み済みの素材は検査しない (Project の読み込みでは判定しない)
- 対応するには preview に alpha を持てる decode 経路 (software decode から GPU への upload) と、clip を別々に
  描いてから混ぜる合成が要る (規模が大きいので当面行わない)

`[事実]` 確認したこと: `still_image_decode_unit` が `vp9_alpha.webm` / `prores4444_alpha.mov` を透過ありと、
`vp9_opaque.webm` / H.264 / Motion JPEG を透過なしと判定する (素材は `make-testmedia.ps1 -Mode Smoke` が生成する)。
`media_bin_model` の分類が、どちらの判定でも断る。

### 19.12 再レビュー指摘への対応 (P2 2 件 / P3 1 件)

`[事実]` P2-1: 編集点へ置くときの不透明度の検査を、実際に置く区間だけで行う。以前は求めた長さ (60) を cut の前後
それぞれで丸ごと検査し、最後の 30 frame だけが不透明な outgoing への 30 / 30 のディゾルブを断っていた。
長さに依らない条件 (種別・位置・拡大・回転・切り抜き、区間の端の frame の不透明度) は先に理由を付けて断る。

`[事実]` P2-2: 置ける長さは cut の前 (incoming を左へ延ばせる・outgoing の最後の frame 群が不透明) と後 (outgoing を
右へ延ばせる・incoming を区間の終わりで分けられる・incoming の最初の frame 群が不透明) で独立に決まる。
それぞれで置ける長さを求め、合計が最大で cut に最も近い中央の組を選ぶ。以前は描画区間を作れないと長い側を
1 frame ずつ削り、outgoing が 30fps・incoming が 60fps の 61 frame を 30 / 30 (60) まで縮めていた。今は 31 / 30。
選んだ組で描画区間を作れなければ、黙って縮めずに理由を返す。

`[事実]` P3: トランジション ID は置く数だけ作る (長さを探す間に使い捨てない)。

`[事実]` 不透明度の検査を求めた長さで行うと、また中央の組だけを試すと、`m5_timeline_edit_focused` の新しい試験が
落ちることを確認した。

### 19.13 再生中のディゾルブで outgoing が区間の終わりまで残る

`[事実]` 再生中にディゾルブを通ると、区間の間ずっと outgoing だけが表示され、区間の終わりで incoming へ
突然切り替わっていた (Premiere のように A が薄れながら B が現れない)。描画の式 (§19.4) は正しく、止めて
区間の中へ seek すれば混ざっていた。

- 原因: controller は再生の tick (16 ms) ごとに、その tick の frame の不透明度で composition を組み、
  `activationOutputFrame` をその frame にして出し直していた。engine は activation の frame に届くまで前に
  提示した composition を描き、保留は最新の 1 つしか持たない。engine の提示は controller の frame より
  遅れているので、incoming の不透明度が毎 tick 変わる区間では、保留が有効になる前に次の tick で上書き
  され続け、区間に入る前の 1 layer の composition を描き続けた。区間が終わって composition が変わらなく
  なった所で初めて追いつく
- 修正: 重ねる source・静止画が同じ順のまま値 (不透明度など) だけ変わる出し直しは、重ね方が変わった
  frame の `activationOutputFrame` を引き継ぐ (`handOffPlaybackSources`)。source が変わる境界では従来どおり
  その frame から有効にし、新しい source を早く要求しない
- clip の fade や不透明度のキーも毎 tick 変わるので、再生中は同じ理由で止まっていたはず (`[未検証]`)。
  この修正で同じく反映される

`[事実]` `transition_preview` (workstation、release) に、再生中に提示した区間の frame が 2 layer で、
incoming の不透明度が進み具合 `p = (f - 110 + 0.5) / 20` から 0.15 (3 frame) 以内であることの検査を足した。
engine の診断に、提示した frame ごとの layer 数と最前面の不透明度を持たせて見る。修正前は区間の 20 frame
すべてが 1 layer で落ち、修正後は 3 回続けて通って最大の差は 0.05 (1 frame 分) だった。

`[未検証]` 不透明度は controller の tick の frame で求めるので、提示とは engine との時計差の分 (実測 1 frame)
ずれる。engine が提示する frame で評価するには、区間の不透明度の変化を composition に持たせる必要がある。

### 19.14 余白のある素材のディゾルブで outgoing が区間の終わりまで残る

`[事実]` 縦長の素材 (横長の Project に置くと左右に余白が出る) へディゾルブすると、余白の所で outgoing が
不透明度 1 のまま見え続け、区間の終わりで突然黒へ切り替わっていた (preview・書き出しとも)。§19.13 の
再生中の反映とは別の原因で、止めて seek しても同じだった。
- 原因: incoming は余白が透明のまま不透明度 p で重なるので、余白の所では outgoing が減らない。
  §19.10 の「画面全体を覆う不透明な映像どうし」の検査は位置・拡大などの effect だけを見ており、
  素材の縦横比による余白は判定できない (Project は素材の寸法を持たない)
- 修正: incoming (トランジションの区間で lane 1 に重ねる側) を、余白を不透明な黒で埋めた出力全体の
  1 枚として p で重ねる。余白の所は `(1 - p) A + p 黒`、素材の所は `(1 - p) A + p B` になり、V1 では
  下が黒なので Premiere のクロスディゾルブ (A がフェードアウトしながら B がフェードイン) と一致する
- preview: composition の layer に `opaqueBackdrop` を足し、compositor は出力全体に描いて配置矩形の外を
  黒にする shader (`ps_backdrop`) を使う。回転した layer には使えない (受理しない)
- 書き出し: lane 1 の clip に黒背景の affine filter を付けて余白を埋める (`attach_export_backdrop`)。
  矩形は素材を probe した寸法 (画素の縦横比を含む) から縦横比を保って求め、`distort=1` で置く。
  `distort=0` で MLT に任せると、上の affine transition が frame に設定する distort が伝わって素材が
  出力全体へ引き伸ばされ、`distort=0` を両方に付けると余白が透明に戻った (いずれも実測)

`[事実]` 確認したこと:
- `still_layer_compositor` (実 D3D11): 中央の枠に 2:1 の素材を置いた incoming を p = 0.5 で重ね、余白が
  `0.5 赤`、素材の所が赤と青の半々になる。effect 経路と effect 無しの両方。対照として backdrop 無しでは
  余白が赤のまま残る。回転した backdrop layer は拒否する
- `m4_timeline_export_focused` (実 MLT): 青 → 縦長 (120x240) の赤のディゾルブで、余白の画素が frame 60
  (p = 0.525) で `0,1,121`、frame 67 (p = 0.875) で `0,0,32`、区間の後は黒。修正前は区間の中で
  `0,0,255` (青のまま) だった。同じ縦横比どうしの既存の検査も通る
- `m7b_2` / `m7b_3`: incoming の頭の区間だけに flag が立つ

`[未検証]` V2 以上のトランジションで incoming に余白があると、余白の所では下の track が黒へ向けて
薄れ、区間の後に下の track へ戻る (Premiere では下の track が見えたまま)。V2 以上で余白のある素材どうしを
正しく混ぜるには、各 clip を下の track ごと別々に描いてから混ぜる合成が要る。

### 19.15 素材 fps と timeline fps が違うディゾルブで、区間の最後の 1 frame に outgoing が全面に出る

`[事実]` 23.976fps の素材を 60fps の timeline に置いてディゾルブし、フレーム送りすると、区間の最後の
frame だけ outgoing が不透明度 1 で出て、次の frame から incoming になっていた。
- 原因: timeline frame は最も近い素材 frame を表示するので、incoming の頭の区間 (lane 1) の最後の timeline
  frame は、区間の素材範囲のすぐ外 (続きの区間の最初の素材 frame) を表示する (mapping の実測: 区間
  [749, 809) の frame 808 が素材 411 を指し、頭の区間の素材範囲は [387, 411))。compositor は表示する素材
  frame から clip 内の位置を求めて `clipFadeFactor` へ渡しており、範囲外は 0 を返すので、その 1 frame
  だけ incoming が不透明度 0 で描かれていた
- 修正: `resolveLayerOpacity` で、範囲外の素材 frame は端の frame の値で評価する (`clipFadeFactor` の
  範囲外 = 0 の契約は変えない)。effect 付きの clip 一般で、最後の 1 frame が消えていたのも同じ原因で直る
  (`[未検証]` ディゾルブ以外では確かめていない)
- 書き出しは timeline の local frame で不透明度を決めるので影響しない

`[事実]` `transition_preview` に、23.976fps の横長 → 縦長の別ファイル (音声とリンク、Shift+D と同じ
`applyDefaultEditTransition` の 60 frame) の区間の始まりと終わりを 1 frame ずつ送る検査を足した。各 frame で
engine が提示した layer 数と最背面の素材 frame が mapping と一致し、送りの間に別の frame を提示しないこと、
画面 (`grabWindow`) の左端 (縦長の incoming では余白) が区間の中で `255 (1 - p)` 以下、区間の後で黒で
あることを見る。修正前は区間の最後の frame で左端が 254 になって落ち、修正後は 2 (p = 0.99) で 3 回続けて
通った。engine の提示の記録だけでは検出できなかった (layer 数も素材 frame も正しく、不透明度だけが違う)。
`test_gpu_pure` に範囲外の frame の評価を足した。
