# Premiere / Final Cut 風の編集操作へ寄せた変更

記述の分類は `docs/phase0-findings.md` と同じ印を使う。

| 印         | 意味                                               |
| ---------- | -------------------------------------------------- |
| `[事実]`   | 実際に実行して観測した。再現手順を併記する         |
| `[推測]`   | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない       |
| `[exit]`   | exit criteria への影響                             |

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

`[事実]` qualified な output frame rate の表は
`core::supportedOutputFrameRates()` に一本化した。
`preview_engine` の output rate も `Project` の timeline fps もこの表だけを読む。

```text
24/1, 24000/1001, 25/1, 30/1, 30000/1001, 50/1, 60/1, 60000/1001
```

`[事実]` `CheckedOutputTimebase::createQualified` は
「60/1 固定」から「上記の表に含まれること」へ緩めた。audio sample rate は
48000 のまま固定である。

`[事実]` `PreviewCapabilities::qualifiedOutputFrameRate` は
`initialize()` で確定した rate を公開するようになった。以前は固定値 60/1 を返しており、
source の rate 検査が output と別の rate を基準にしうる形だった。

`[事実]` clip の fps は従来どおり timeline fps と一致していなければ preview できない。
今回のスコープは「Project の fps を選べる」であり、混在 rate の frame rate 変換ではない。

`[未検証]` 24 / 25 / 30 / 50 fps および 1001 分母の rate で
実際に preview を回した計測はまだ無い。`p1-matrix.ps1` 相当の計測は 60/1 でしか
取っていないため、非 60fps の presentation 品質を「出る」と書いてはいけない。

## 3. preview の layer 上限

`[事実]` `GpuCompositor::composeLayersToTarget` は N layer を描ける実装だが、
`PreviewCapabilities` が qualify しているのは 2 layer までである。

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

- `tests/core/test_checked_output_timebase.cpp`
  qualified / 未 qualified の rate を **literal で列挙**して受理・拒否を検査する。
  実装の `isSupportedOutputFrameRate` は呼ばない。呼ぶと表を壊した変更を
  テストが追認してしまう。
- `tests/preview_engine/test_preview_engine.cpp`
  拒否側の rate を 24/1 から 48/1 へ変えたうえで、qualified な各 rate で
  `initialize` が成功し、`capabilities().qualifiedOutputFrameRate` が
  **その rate を公開する**ことと、source rate 検査がその rate を基準にすることを検査する。

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
