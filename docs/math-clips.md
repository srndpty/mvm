# 数式 clip (Math Clip)

数式を mvm の Project の構造化データとして持ち、timeline 上で直接編集する clip。既存の Manim script clip
(`File > Manim clip`、任意の Python scene) は escape hatch として残し、置き換えない。

記述の印は `docs/phase0-findings.md` と同じ (`[事実]` `[推測]` `[未検証]` `[回避策]`)。

## 設計の要点 (P0)

- **正は Project の `MathClipData`** (`syntax` / `source` / `fontSize` / 色 / 背景)。何で描くか (backend) と
  どこに置くか (位置・拡大・回転) は持たない。配置は画像 clip と同じく既存の `ClipEffects` に任せる。
- **描画結果は派生物。** `<project の directory>/cache/math/<project の file 名>/<key>.png` に置き、
  Project JSON には path も状態も保存しない。
  key は「描画に効く field (`syntax` / `source` / `fontSize`)」と「toolchain fingerprint」の SHA-256。
  色・背景・ClipEffects・尺・clip ID は key に入れない (白一色の mask を mvm 側で着色・配置する)。
- **toolchain fingerprint** は backend id、mvm の描画 template の版、Manim / latex / dvisvgm の版の 1 行目からなる。
  preflight でしか得られないので、P0 の cache は同じ toolchain を持つ機械でしか引けない
  (backend の無い機械では「利用不可」になり、書き出しを拒否する)。
- **確定は描画の成否と無関係に成立する。** 不正な式も Project に残り、描画の失敗で Project を巻き戻さない。
  preview は最後に描けた画像 (last-good) を古い印付きで出し、書き出しは拒否する。
- 描画は worker で非同期に行い、取消すると Manim が起動した latex などの子 process まで止める。

animation の artifact は P1 (Write) で Manim の PNG 連番に決めた (下の「Write (P1)」)。
式から式への変形 (P2) は、隣り合う 2 つの数式 clip の間のトランジションとして持つ
(下の「式から式への変形 (P2)」)。部分式の同一性は P2 では Project に持たない。

## 構成

| 場所 | 責務 |
|---|---|
| `src/media/math/math_render.h` | backend 中立な契約 (`MathRenderSpec`、`MathToolchainFingerprint`、`MathRenderBackend`、cache key)。Manim の型も Project の型も含まない |
| `src/media/math/math_raster_layout.h` | `composeMathRaster`: mask を出力 raster の中央へ置き、着色・背景を合成する。preview と書き出しが共有する |
| `src/media/manim/manim_math_tex.h` | backend `manim-mathtex`。preflight (Manim / latex / dvisvgm の確認と版) と描画 |
| `src/util/mvm_process.h` | 外部 process の起動・timeout・取消。Job Object で孫 process まで止める |
| `src/util/mvm_sha256.h` | SHA-256 (cache key と Manim script の fingerprint) |

- 描画 script は job directory に書き、式は `request.json` で渡す (Python の source へ埋め込まない)。
  失敗の種類は script が `error-kind.txt` に書き、TeX の誤りの説明は TeX の log の `!` 行から取る。
  Manim の console 出力は折り返しが環境で変わるので、判定に使わない。
- latex / dvisvgm は、環境変数 PATH の各 directory だけから探す (Manim へ引き継ぐのと同じ PATH)。

## preview・書き出し・状態 (P0-4)

| 場所 | 責務 |
|---|---|
| `src/app/math_clip_render.h` | `MathClipData` → `MathRenderSpec`、mask への着色と配置 (preview と書き出しが共有) |
| `apps/mvm/math_raster_cache.h` | 描画結果の cache (key 単位、worker 1 本、disk は `cache/math/<project の file 名>`) |
| `MvmController` | 数式 clip の作成・確定・入力中の preview・last-good・書き出しの可否 |

- **cache の disk の形:** `<key>.png` (白い glyph の mask) と `<key>.txt` (provenance: 版・key・大きさ・
  toolchain)。PNG を先に、provenance を後に atomic に書く。provenance・大きさ・toolchain が合わないものは消して描き直す。
  作業 directory は `<cache>/jobs/<session>/` で、backend の確認の前に `<cache>/jobs/` の残り (強制終了など) を消す。
- **権限 (P0-4.1):** cache の変更 (掃除・PNG の書き込み) と外部 renderer の起動は、controller が
  Project lock を取った後でだけ許可する (`MathRasterCache::setAuthority`)。
  - lock を取れない instance は数式の作業を何も始めず、`unavailable` (「他のプロセスが編集中」) を示す。
  - cache は Project の file ごとに分ける。lock は file ごとなので、同じ directory の別の `.mvm` と
    cache を共有すると、相手の作業 directory を消しうるため。
  - 保存先・lock が変わるたびに世代を変え、進行中の確認を捨てて確認をやり直す
    (必ず `Available` / `Unavailable` に着き、`Checking` のまま止まらない)。
- **再試行 (`retryMathRendering`):** backend を確かめ直し、覚えている失敗を忘れて描き直す。
  描けている式は disk の結果を使い続ける。強制の描き直し (artifact の無効化) ではない。
  UI では「再試行」と呼ぶ (MiKTeX の導入後や、一時的な失敗の後に使う)。
  - 帰結: TeX の package 単位の更新 (fingerprint に入らない) で glyph が変わっても、P0 には描き直させる操作が無い。
    [回避策] `cache/math/<project の file 名>` を消す。必要なら artifact を無効化する操作を P0.5 で足す。
- **描画の順:** 入力中の式 → 再生位置に掛かる clip → 残り。要求されなくなった式の描画は process ごと止める。
  Project を変えるたびに、すべての数式 clip の描画を要求する (書き出しの前に揃えておくため)。
- **状態** (`mathClipData` の `state`。Project には保存しない):
  `checking` (backend の確認中) / `rendering` / `stale` (描き直し中で、前の描画を出している) / `ready` /
  `error` (理由は `message`、詳細は `log`) / `unavailable` (backend または Project の権限が無い)。
  `showingPrevious` は、preview に前に描けた画素 (last-good) を出していることを示す。
  `unavailableReason` は `backend` / `authority` / 空、`canRetry` はこの instance で確認を
  やり直せるかを表す。表示文から原因を推測しない。
- **last-good:** clip ごとに最後に描けた mask を session の間だけ持ち、描き直し中・失敗中の preview に出す。
  書き出しには使わない。開き直した後は対応付けない (派生の state を Project に入れないため)。
- **書き出し:** 出力する数式 clip (有効で、出力する track にあるもの) は、現在の式の描画が済んでいなければ
  開始時に拒否する。controller は clip ID → `<key>.png` の表を `TimelineExportRequest::mathArtifacts` で渡し、
  書き出しは Manim を起動しない。
- **保存先の変更 (別名で保存・開く):** cache の場所が変わるので、新しい場所で描き直す (cache は移さない)。
- **終了:** `MvmController::shutdown` が描画中の Manim / LaTeX を process ごと止め、worker を待つ。

文字サイズを変えている間は、描き直しが済むまで前の大きさの描画を出す。
計画との差と今後の改善は [ロードマップ](roadmap.md#数式-clip) に記録する。

## Project の schema (18、Write は 19、変形は 20)

19 で数式 clip の `"math_animation"` を足した (下の「Write (P1)」)。
20 で transition の `"kind"` を足した (下の「式から式への変形 (P2)」)。以下は 18 で決めた形で、19・20 でも変わらない。

- `TimelineClipKind::Math` (JSON の kind は `"math"`)。値は clip の `"math"` object に保存する:
  `syntax` / `source` / `font_size` / `color` / `background_color`。未知の field (例えば `renderer`)・
  重複・欠落は拒否する。位置・拡大・回転は他の clip と同じ `"effects"` に保存する。
- 数式 clip は `media_path` を持たない (`clipKindHasMediaPath` が文字と数式を除く。検証・JSON の読み書き・
  path の解決はすべてこの判定を使う)。素材 (`media_item_id`) も参照しない。
- 尺だけを持つ静止 clip (`isStillClipKind`) であり、速度・リンク・フリーズを持たない。
- `validateMathClipData` は形だけを見る (空でない式、記法 `latex`、文字サイズ 1〜出力の高さ、
  色 `#AARRGGBB`)。**描けるかどうかは見ない。** 描けない式も Project の正として保存できる。
- 読める版:
  - 18 はそのまま読む。
  - 17 と 16 は field の追加だけなので、読み込み後に 18 として扱う。
  - それ以外は拒否する。
  - 数式 clip は 18 の file にだけ現れてよい。
  - 自動音量調整の field の欠落は 16 の file だけに許す。以前は「現行版だけ」を検査していたため、
    schema を 18 へ上げた時点で 17 の file の欠落を見逃すところだった。版で判定するよう直した。

## 計画からの変更

- `MathRenderSpec` (描画に効く field) は `src/media/math` に置いた。`MathClipData` からの抽出は app 層で行う。
  `src/media` の層は Project に依存しないため。
- 色の形式の判定 (`parseArgbColor`) は `src/project` に一本化した。文字 clip の検証もこれを使う。
  `composeMathRaster` は数値 (0xAARRGGBB) を受け取る。
- 実 Manim の smoke は、新しい CTest label (`external`) を作らなかった。
  既存の `mvm_manim_smoke` と同じく、登録しない手動の executable にした。
  通常の CTest の除外の規約 (`-LE 'performance|stability'`) を変えずに済むため。

## 実 Manim での smoke

通常の CTest には入れない (Manim と MiKTeX を build・試験の必須条件にしない)。MiKTeX を導入した後に、
**新しく開いた shell** (PATH に MiKTeX がある) で実行する。
作業 directory は存在しない新しい path を指定する。既存の結果は削除・上書きせず終了コード 2 で拒否する。
この試験は画面を表示せず音声 endpoint も使わないので、実行中も通常の PC 操作をしてよい。

```powershell
pwsh scripts/build.ps1 -Target mvm_math_manim_smoke
.\build\ucrt64-release\bin\mvm_math_manim_smoke.exe "$env:USERPROFILE\.local\bin\manim.exe" <作業 directory>
```

- [事実] 2026-10-05: 44 検査中 0 件失敗。fingerprint は
  `manim=Manim Community v0.21.0` / `latex=MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)` / `dvisvgm=dvisvgm 3.6`。
  受け入れ scenario の 5 式、`\boxed` を含む式、引用符と改行を含む式がそれぞれ 1540〜1748 ms で描けた。
  PNG は mvm の静止画 decoder で読め、四隅は透明、不透明な画素はすべて白だった。
  `\fracc{a}{b}` は `InvalidSource` で、message は `Undefined control sequence.`。

## 数式の編集 UI (P0-5)

- ファイルメニューの「数式 clip を追加」で、再生ヘッド位置の既存 clip より上の空き track に既定 5 秒の
  数式を置く。追加後はエフェクトコントロールの式入力欄を選択する。
  画像・文字と同じ配置規則なので、V1 が空なら V1、V1 に clip があれば V2 から置く。
- 式は 600ms 入力が止まると確定前の preview を要求する。Ctrl+Enter、入力欄からの移動、
  clip の選択変更で確定し、Esc で編集前へ戻す。描画通知は未確定の入力を上書きしない。
- 文字サイズ・文字色・背景色は既存の数値欄と色選択部品で編集する。位置・拡大・回転は
  clip 共通のエフェクトを使う。式中の `\color` は反映されない。
- 準備中・描画中・完了・古い表示・エラー・利用不可を表示し、ログは展開して読む。
  「再試行」は描画環境を確認し直す操作であり、成功した disk cache を強制再生成しない。
- `math_inspector_qml` は確定・取消・選択変更と、通常幅 / 狭幅・低いパネル / 利用不可の
  実描画・最下部へのスクロールを検査する。外部の Manim / MiKTeX は使わない。

### 確定拒否と利用不可の区別 (P0-5.1)

- 空の式など、構造が不正な入力の確定が失敗したら、選択変更後も旧 clip の入力を欄に保持する。
  保持中の案内を表示し、描画通知・フォーカス移動では置き換えない。修正して Ctrl+Enter で
  旧 clip へ確定するか、Esc で明示的に取り消すと、選択中の clip の入力欄へ移る。
  保持中は書式の操作を無効にし、旧入力と新しい選択の書式を取り違えない。
- backend の不在では導入案内と再試行を表示する。Project lock を取れない instance では
  lock の理由だけを表示し、Manim / MiKTeX の導入案内と再試行を出さない。
  直接 `retryMathRendering` を呼んでも、権限が無ければ外部 process の起動や cache の掃除をしない。
- QML の負例は空欄・空白だけの入力、権限不足、原因区分の欠落。
  controller の負例は、確定拒否による Project / Undo / artifact の不変と、実際の二重 lock。
  `math_inspector_product_ui` は実 `Main.qml` と controller を結び、同じ拒否と案内を検査する。

## 受け入れと完了確認 (P0-6)

P0-6 は静止数式 clip の既存経路を検証する段階とし、animation・新しい編集操作・cache の
可搬性は追加しない。通常試験は偽の backend を使い、実 toolchain の確認は既存の
`mvm_math_manim_smoke` を別に実行する（CTest に登録しない既存の方針を維持する）。

| 確認対象 | 検証の入口 |
|---|---|
| schema 18 の往復、旧版の読み込み、未知・欠落・重複 field と空の式の拒否 | `math_project_json_focused` |
| 独立な golden key、着色・中央配置・alpha・出力を超える mask の拒否 | `math_render_key_and_layout` |
| 取消・timeout と孫 process の終了、式を JSON で渡す契約 | `process_runner_focused` / `manim_math_tex_focused` |
| disk hit、壊れた PNG / provenance、取消・世代の変更・権限・描画中の破棄 | `math_raster_cache_focused` |
| 作成・Undo / Redo・コピー・カット・貼り付け・複製・保存と開き直し | `math_controller_focused` |
| 不正式の保存・last-good・未完了 / backend 不在時の書き出し拒否 | `math_controller_focused` |
| 入力の保持・依存不足 / 権限不足の案内・狭幅と低いパネルの実描画 | `math_inspector_qml` / `math_inspector_product_ui` |
| ClipEffects と位置 keyframe を付けた数式の実 GPU preview / MP4 の画素分類 | `math_export_focused` |
| 二次方程式の 5 式・boxed・引用符と改行・TeX の誤りの分類 | `mvm_math_manim_smoke` (実 Manim + MiKTeX) |
| 実数式の連続配置、文字 title、fade・不透明度と変形 keyframe、A1、保存・再読込・MP4 復号 | `mvm_math_manim_smoke` の受け入れ検査 |
| 既存の作業 directory を拒否し、保存済みの結果を変更しない | `math_smoke_existing_workspace_rejected` (実 toolchain 不要) |

書き出しの試験は、独立に決めた矩形の内外を比較する。GPU preview との比較は圧縮による
色の差を避けて内部画素の色分類を使い、preview の位置をずらした負の対照が不一致になることも
要求する。対象画素・frame 数が 0 件なら成功にしない。

再現する通常の gate は `pwsh scripts/test.ps1 -Preset ucrt64-release -Group All` と
`pwsh scripts/lint.ps1`。前者は build と通常 CTest を実行し、`performance|stability` を除外する。
GUI 試験は入力を透過する背面 window へ合成 event を送り、利用者の操作を止めない。
実 Manim の手順は上の「実 Manim での smoke」を使う。

### 確認の範囲 (2026-10-05)

- [事実] `scripts/test.ps1 -Preset ucrt64-release -Group All` は通常 1449 / 1449 件が通過。
  `-Group BuildIndependent` の選定は 1078 / 1078 件が通過（通常全体に含まれる同じ試験）。
  `performance|stability` は除外した。`scripts/lint.ps1` と整形差分検査も通過した。
- [事実] Inspector の QML は 11 ケースが通過した（初期化・終了処理を除く）。
  実 `Main.qml` の試験も、旧入力の保持・修正確定・二重 lock の案内を検査した。
- [事実] ClipEffects と位置 keyframe を付けた数式は、独立な期待矩形の内部 5824 画素で
  GPU preview と MP4 の色分類が一致した。位置をずらした対照は不一致を検出した。
- [事実] 実 Manim 0.21.0 / MiKTeX 26.5 / dvisvgm 3.6 の smoke は終了コード 0。
  二次方程式の 5 式・boxed・引用符と改行、および不正な TeX の分類を検査した。
- [未検証] ナレーション付きの二次方程式動画を、手操作で一通り制作する scenario は実施していない。
  上の自動試験は各契約の確認であり、その制作手順の実施記録ではない。

### P0-6 の統合受け入れ (2026-10-05)

以下は P0-6.1 の帰属前の実施記録。失敗 run の結果と当時の HOLD 判定を保持する。

既存の実 backend smoke を拡張し、描画した 5 式と boxed の 6 本を V2 に並べ、V1 の文字 title、
fade、最後の式の位置・拡大・不透明度 keyframe、A1 の試験音を含む Project を作る。
ナレーションの収録はせず、3 秒の試験音で音声経路を検査する。実 renderer の試験は従来どおり
CTest の外で行い、新しい依存や製品機能を足さない。

- [事実] 保存と再読込で Project が一致し、6 区間すべてで preview の mapping に数式がある。
  最後の式の artifact だけを欠いた負例は書き出しを拒否する。
- [事実] 映像だけの独立な対照は 180 frame の MP4 を出力し、復号した frame 数と 6 区間の
  glyph を検査できた。文字 title だけで成功にならないよう、中央の数式領域を比較する。
- [事実] 音声付きは AAC encoder が `Input contains (near) NaN/+-Inf` を報告し、
  `tractor出力を検証できません` で失敗した。mono / stereo の fixture と、文字を V3 に移して
  V1 を空にした対照でも同じ失敗を観測した。原因はまだ特定していない。
- [事実] 数式間のクロスディゾルブは既存の Project 契約が拒否する。すべての ClipEffects を
  既定値に戻した負例でも拒否した。計画の dissolve を含む scenario は実装済みとは扱わない。
- [exit] P0-6 の受け入れ試験を実装したが、**P0 完了の判定は保留**。音声付き出力の不成立と
  計画の dissolve の扱いが未解決である。映像対照の成功で音声付きの失敗を置き換えず、
  smoke は非 0 終了を維持する。
- [事実] 拡張した実 smoke は 67 検査中 1 件失敗（音声付き出力）。音声復号・sample 比較は
  出力が不成立なので未実施。通常の focused CTest は 10 / 10 件、lint は通過した。

確認 artifact: `build/math-p06-acceptance-20261004-202534/` と同名の `.log`。
映像対照は `acceptance-video-only.mp4`、音声を含む Project は `acceptance.mvm`。
過去の失敗 run は保持する。未解決事項は [roadmap](roadmap.md#数式-clip) に集約する。

### P0-6.1 の帰属と完了範囲 (2026-10-05)

新しい run で同じ WAV を共有し、Math の 6 本を通常 Text、通常画像にそれぞれ置き換える。
clip の配置・尺・fade・keyframe、文字 title、A1、Project FPS は保持する。
export request をコピーし、artifact map を空にする以外は、出力ファイル名だけを各ケースで変える。
すべて新しい同一作業 directory 内の MP4 とし、通常の製品 export を呼ぶ。

| 新しい対照 | Math の有無 | 観測結果 |
|---|---|---|
| `audio-text.mvm` | 無し | AAC NaN/Inf、frame 2〜4 の encode error、出力検証失敗 |
| `audio-image.mvm` | 無し | 同じ AAC NaN/Inf と encode error、出力検証失敗 |
| `acceptance.mvm` | 有り | 同じ AAC NaN/Inf と encode error、出力検証失敗 |
| `acceptance-video-only.mvm` | 有り・音声無し | 180 frame の MP4 と 6 区間の glyph 比較が通過 |

- [事実] Math を含まない両対照でも失敗を再現した。音声出力の不具合は Math Clip の必要条件では
  なく、[一般の音声・書き出し](roadmap.md#一般の音声書き出し) の残件へ移す。根本原因の確定や修正は
  この帰属試験では行っていない。過去の failed run を成功へ読み替えない。
- [exit] Math Clip P0 gate は実 Manim の映像のみの出力、既存の focused 契約検査、fade と
  ClipEffects とする。独立な一般の音声 export の成功は Math P0 gate から外す。
  新しい同条件対照で Math だけが失敗した場合は、独立とは分類せず HOLD にする。
- [exit] Math-to-Math dissolve は P1+ の計画とする。generic still-layer の既存契約は
  dissolve を保証せず、`dissolveClipShapeEligible` も Video / Manim だけを許可する。
  静止数式 P0 に必要なのは検査済みの既存 fade / effects であり、新機能は追加しない。
- [事実] 実 smoke は 70 検査中 0 件失敗、終了コード 0。これは新しい Math P0 の gate の結果であり、
  音声付き MP4 の成功を意味しない。音声の復号・sample 比較は出力が不成立なので未実施。
- [事実] 帰属の負例を追加した最終版は 76 検査中 0 件失敗、終了コード 0。
  Math だけが失敗する対照、片方だけが失敗する対照、異なる失敗、空の失敗理由は
  独立と分類しない。実対照の Math 0 本 / A1 1 本も検査した。
  最終ログは `build/math-p061-final-20261004-204218.log`、同名 directory に各 Project と
  実 Manim PNG、180 frame の `acceptance-video-only.mp4` を保持している。

新しい証拠は `build/math-p061-attribution-20261004-203505.log` と同名 directory。
Project は 60 fps、1920x1080、音声は同一 48 kHz stereo PCM WAV の 144000 sample。
出力は 180 frame、MP4 / AAC、CRF 23、render threads 4、encoder threads 0 を共通に使った。

- [事実] 入力 WAV を UCRT64 FFmpeg で直接 AAC に変換した対照は終了コード 0。
  `input-aac-control.m4a` は ffprobe で AAC / 48000 Hz / 2 channel / 3 秒。
  したがって入力 WAV や AAC encoder の不在を Math の不成立の根拠にはしない。
  共通 WAV の SHA-256 は `706a1d48e27e58c6af9a969a6566a16f9816a21a2c5e78e10fe6556985728b5b`。

### 最終判定: P0 PASS/CLOSED (2026-10-05)

静止 Math Clip P0 は、上の P0-6.1 の範囲で完了とする。製品機能の追加は無い。
一般の音声 export の失敗は未解決のまま残し、数式間 dissolve は P1+ へ延期する。

| gate | 最終結果 | 証拠 |
|---|---|---|
| 通常 release (`scripts/test.ps1 -Preset ucrt64-release -Group All`) | 1450 / 1450 通過 | `build/math-p061-release-gate.log` |
| focused Math / process / backend (通常試験にも含まれる) | 10 / 10 通過 | `build/math-p061-focused.log` |
| `scripts/lint.ps1` | 通過 | 整形・層の隔離・静的検査すべて通過 |
| 最終の実 Manim smoke | 76 / 76 通過、終了コード 0 | `build/math-p061-final-20261004-204218.log` |

通常 gate と focused は `performance|stability` を除外し、timeout を付けた。
実 smoke の成功は Math の無い対照でも音声失敗が再現した帰属と、実数式の映像のみの出力を
確認した結果である。一般の音声付き export を修正した、または成功したという判定ではない。
P0-6 の失敗記録・artifact と P0-4.1 の authority/cache 設計は保持した。

## Write (P1)

clip の先頭で式を Manim の `Write` で書いていく、最初の時間に沿った数式固有の animation。
fade・不透明度・位置・拡大・回転は既存の ClipEffects で足りるので、数式専用には作らない。
式から式への変形は P2 (下の「式から式への変形 (P2)」)。

### Project (schema 19)

- 値は clip の `"math_animation": {"intro": "write", "intro_frames": N}`。中の field はすべて必須で、
  未知・重複・欠落を拒否する。省略は intro 無し (書き出しも intro 無しなら書かない)。
  数式 clip 以外には書けない。18・17・16 の file は 19 として読む (`math_animation` は 19 にだけ現れてよい)。
- `MathClipData` (式の意味と見た目) とは分けて `TimelineClip::mathAnimation` に持つ。静止の
  `MathRenderSpec` と cache key を変えないため (P0 の cache と golden key がそのまま使える)。
  ClipEffects にも入れない (種別に依らない keyframe の仕組みで、Manim の artifact を要らない)。
- `intro_frames` は clip の素材 frame (fade と同じ domain) で、clip の見えている先頭から数える。
  1 から clip の尺まで (時間の意味だけで検証する)。clip の尺に対する割合にはしない
  (末尾を trim すると書く速さが変わり、artifact の key が尺に依存するため)。
- 描画の方式による上限 (連番の枚数・memory) は Project の値に持ち込まない (P1.1)。backend は
  描ける最大の枚数を `MathRenderBackend::maximumSequenceFrames` で示し (Manim は 4 桁の連番名に
  収まる 9999)、超える Write は Project としては正しいまま、描画が理由付きの error
  (`writeState`) になり、書き出しを拒否する。
- 編集の規則は fade in に揃える: 分割・上書き・時間編集で分けた右側は Write を持たない。
  trim は縮めた尺に収める (左 trim でも先頭から書き直す)。置いたときと違う fps の timeline で
  trim すると、素材 frame を timeline の fps へ揃えるのと一緒に同じ秒数へ換算する。
- Write の後 (local frame N 以降) は P0 の静止の描画をそのまま見せる。frame i (< N) は
  進み具合 i/N の連番の frame。

### artifact・cache

- backend は 1 秒の Write を `config.frame_rate = N` で描き、ちょうど N 枚の PNG にする (P1-0)。
  artifact は N だけに依存し、timeline の fps に依存しない。script は静止と同じ共通部分
  (式の読み込みと frame の大きさ合わせ) に scene を足したもの。
- key は静止と別の名前空間 `mvm-math-sequence/1` (`mathSequenceKey`)。静止の field に animation・
  frame 数・連番の script の識別 (`manim-write/1`) を足す。script の識別は toolchain fingerprint に
  入れない (連番の描き方を変えても静止の cache を無効にしない)。
- disk は `<cache>/write/<key>/00000.png …` と provenance `<cache>/write/<key>.txt`
  (`mvm-math-sequence-artifact/1`、大きさ・枚数・各 frame の byte 数・script・toolchain)。
  古い provenance を消してから PNG を `<key>/` へ写し、provenance を最後に書く (provenance が
  確定の印)。合わない・欠けた・読めない frame の結果は消して描き直す。
  - [事実] P1.2: 以前は `write/<key>.partial-<ticket>/` に置いてから directory を rename していたが、
    `math_raster_cache_focused` が ctest で 15 回に 1 回ほど `Permission denied` で落ちた
    (書いた直後の file を他の process が開いていると、Windows は directory の rename を拒む)。
    rename をやめた後は 20 回連続で通過した。
- 静止と同じ worker・権限・世代・取消で扱う。連番の Ready は disk に揃っている (書き出しに使える)
  ことだけを表し、frame を decode しない。
- **preview 用の mask の memory (P1.1):** preview が合成で要求した連番だけを、別の worker
  (`residentPool_`) で 1 画素 1 byte の mask に読む (`residentSequence`)。全 clip の合計に上限
  (既定 256 MB、`setResidentMemoryBudget`) があり、読む前に予約する (`MathResidencyBudget`)。
  予約は mask が破棄されるときに返るので、cache が手放しても preview engine が持っている分は
  上限に数え続ける (memory に実際にある mask の量を超えない)。
  - この上限が数えるのは、memory に置いた A8 の被覆 (1 画素 1 byte x 幅 x 高さ x 枚数) の
    中身だけである。process 全体・GPU・decode の memory は数えない。数えないものの例:
    PNG の decode 中の一時的な RGBA、静止の描画、preview engine の texture (静止画の出力全面の
    RGBA と、書き換える patch の作業領域)、合成の結果、video の decode。
  - 足りなければ、cache だけが持つ mask を最も長く使っていないものから外す (LRU)。合成中の
    animation や engine が使っている mask は外さない (外しても memory は空かず、同じ frame の
    clip どうしで追い出し合う)。
  - それでも足りなければ OverBudget: preview は書き終えた式 (静止) を見せ、inspector に理由を
    出す (`writePreview` = `memory`)。書き出しは disk の連番を使うので影響しない。使用中の mask が
    手放されると (予約が返ると) 収まらなかった連番の `entryChanged` を出し、もう一度試させる。
  - controller は、その frame に見える数式 clip の animation だけを持つ (見えない clip の
    animation が古い mask を memory に残さない)。
  - disk の大きさは合うが読めない frame は、読むときに見つけて artifact を消し、連番を Failed に
    する (再試行で描き直す)。
- 入力中の式の静止がまだ描けていなければ、描きかけ・待ちの連番を止めて先に描かせる
  (P2-5.1 で変形も止める `cancelPendingAnimations` にした)。止めた連番は同じ要求で要求し直す。

### preview

- preview engine に `PreviewStillAnimation` を足した (Math の型を持たない汎用の部品)。静止画 layer の
  一部の矩形だけを、出力 frame の state が変わったときに `fillPatch` で作り、その矩形だけを
  GPU へ送る (`UpdateSubresource`)。下地は P0 の静止の画素で、位置・拡大・不透明度は従来どおり
  `PreviewMotion` が掛ける。持たない layer の経路は変えていない。
- controller は連番の mask (A8) と色・背景で patch を作る。frame の選び方 (`mathIntroFrameAt`) と
  画素の式 (`composeMathPatch`) は書き出しと共有する。
- 入力中の clip、連番の描画中・失敗中、静止の描画が現在の式のもので無い間は Write を付けず、
  書き終えた式 (静止) を見せる。別の式の古い連番は見せない。

### 書き出し

- Write のある数式 clip は、先頭の Write の区間と、その後の静止の区間の 2 つの mapping にする。
  ClipEffects は各区間の local frame の位置から評価するので、keyframe は境で途切れない。
- Write の区間は timeline の frame ごとに全画面の透過 PNG を stage し、`qimage` の連番
  (`ttl=1`、`MvmExportClip::is_image_sequence`) で開く。書き出しは Manim を起動しない。
- 連番が渡されていない・足りない数式 clip は書き出さない。controller は Write の状態が ready の
  ときだけ連番を渡す (静止で代用しない)。preview の memory に置けたかどうかとは無関係。
- [推測] 費用: Write の区間の timeline frame ごとに出力全面の PNG を合成・encode・disk へ書く
  (1080p で 1 frame の RGBA は約 8 MB、PNG は透過の余白が多いので小さい)。Write の尺と出力の
  解像度に比例する。P1 はこのまま残し、mask の矩形だけを stage して MLT 側で配置する方式は
  P2 の最適化とする ([roadmap](roadmap.md#数式-clip))。

### 確認 (2026-10-05)

- [事実] focused: `math_project_json_focused` 106 件、`math_render_key_and_layout` 38 件、
  `math_raster_cache_focused` 91 件、`math_controller_focused` 137 件、`math_export_focused` 58 件が
  通過。`manim_math_tex_focused`・`preview_engine_p5b_unit`・`preview_engine_p5c_native_attach`・
  `math_inspector_qml` (15 ケース)・`math_inspector_product_ui` も通過。
- [事実] 書き出しの試験は、Write の frame i が左から 4i 列を覆う mask で、出力 frame ごとの
  独立な期待矩形 (横位置の keyframe を含む) と比べた。3 frame ずらした期待値、別の frame の
  preview は不一致になる。静止の区間の keyframe の起点を Write の尺だけずらさない変異では、
  frame 20・25・29 の比較が落ちることを確かめた。
- [事実] preview engine の実 D3D11 試験で、矩形の中だけが state の画素になり、外は静止画のまま、
  state が変わらない frame では `fillPatch` を呼ばず、Write の後は静止画へ戻ることを確かめた。
- [事実] 実 Manim の smoke (`mvm_math_manim_smoke`) は 95 検査中 0 件失敗、終了コード 0。
  解の公式の Write (90 枚、854x230) は 1919 ms。被覆は静止比で frame 0 が 0、45 が 0.507、
  89 が 0.999。5 秒の clip (Write 1.5 秒、fade in 6 frame) を保存・再読込し、映像だけの MP4
  (300 frame) で glyph の画素が frame 10 / 45 / 89 / 150 / 299 で 35 / 222 / 440 / 440 / 442。
  ログは `build/math-p1-write-20261005-065908.log` と同名の directory。
- [事実] `scripts/test.ps1 -Preset ucrt64-release -Group All` は通常 1450 / 1450 件が通過
  (`build/math-p1-release-gate.log`)。`-Group BuildIndependent` は 1078 / 1078 件
  (`build/math-p1-build-independent.log`)。`performance|stability` は除外した。`scripts/lint.ps1` も通過。
- [事実] P1.1 (Project から描画の上限を外し、preview の mask に全体の上限を設けた) の後:
  通常 1450 / 1450 (`build/math-p11-release-gate.log`)、BuildIndependent 1078 / 1078
  (`build/math-p11-build-independent.log`)、lint 通過、`math_raster_cache_focused` 130 件、
  `math_controller_focused` 151 件。実 Manim の smoke は 95 / 95、終了コード 0
  (`build/math-p11-write-20261005-081951.log`)。予約の上限検査を無効にする変異では、
  6 本の連番の memory が最大 24576 byte (上限 10000) になり、cache と controller の試験が落ちた。
### 再生中に mask が届くとき (P1.2、2026-10-05)

- 再生中は tick ごとに `handOffPlaybackSources` が合成を組み、前と違えば engine へ出し直す。
  mask が届いた次の tick の合成には Write の animation が付き、出し直す。静止画の pointer は
  変わらないので activation は前の合成の frame を引き継ぎ (過去の frame)、engine はすぐに使う。
  Write の frame は engine が出力 frame から `stateAt` で決めるので、届いた時刻の frame から見せ、
  0 からやり直さない。再生を止めたり seek したりはしない。prefetch は P2 のまま。
- 試験: `math_write_native_playback` (`mvm_test_math_controller --native-write`)。実 D3D11 の
  preview で再生し、engine の render thread が評価した (出力 frame, Write の frame) を記録する。
  mask が届く時刻は cache の試験用の保留 (`holdResidentLoadsForTest`) で決める。
  - [事実] 1. 再生前に mask が memory にある: 再生の先頭 (frame 0) から 61〜62 件を記録し、すべて
    Write の frame = 出力 frame。
  - [事実] 2. 再生前は memory に無く、frame 30 で届ける: 届く前の記録は 0 件 (静止を見せる)。
    届いた後は frame 32〜33 から 38〜39 件を記録し、すべて Write の frame = 出力 frame
    (frame 0 を見せない)。再生の組み直しの回数は変わらない。5 回連続で通過した。
  - [事実] 合成の出し直しを「重ねる source が変わったときだけ」に変える変異では、2 の記録が
    0 件になり、試験が落ちた。
- [事実] P1.2 の後: 通常 1451 / 1451 (`build/math-p12-release-gate-rerun.log`)、BuildIndependent
  1078 / 1078 (`build/math-p12-build-independent.log`)、lint 通過、実 Manim の smoke 95 / 95
  (`build/math-p12-write-20261005-085813.log`)。最初の通常の実行 (`build/math-p12-release-gate.log`) は
  GUI・提示の 5 件が落ちたが、利用者の無操作 20.5 分 (画面の消灯は 15 分) の間に実行していた。
  うち 2 件は P1.2 で変えた file を link しておらず、画面を点けた再実行では全件通過した。
- [未検証] 大きな式・長い Write の preview の再生中の負荷 (render thread での patch の着色と送信)。
  1080p 全面の式では 1 frame の patch が約 8 MB になる。
- [未検証] Write を付けた解の公式を人が一通り制作する手順 (P0 と同じく自動試験は契約の確認)。

## 式から式への変形 (P2)

隣り合う 2 つの数式 clip の間で、前の式を後ろの式へ時間 T で変形する。導出の 1 段を示すための
最小の縦切りで、完全な導出の editor や任意の部分式の操作は後の段階とする。

### 設計の結論 (P2-0 の後)

- **所有:** 既存の `TimelineTransition` に種類 `TransitionKind::MathTransform` を足して持つ。
  - 端点の式は両 clip を ID で参照するだけで、写さない。A・B を編集すると、変形の端点もそれに追従する。
    「今の A から今の B への変形」以外の値を持たないので、端点が食い違う状態を作れない。
  - 隣接・reconcile・fps の換算・分割・削除は、既存のトランジションの規則をそのまま使う。
  - 数式専用の関係の store は作らない。
  - 比べた他の案 (前の clip の出の animation、後ろの clip の入りの animation、複数の式を持つ
    Math Sequence clip) は、相手の参照の追跡や clip の中の timeline が新しく要るので採らない。
- **Project に持たないもの:** 照合・分け方・Manim の値 (`MathTex`・`TransformMatchingTex`・
  `{{ }}`・`substrings_to_isolate`) はすべて renderer の側に置く。
- **静止の renderer (P0) は変えない。** P2-0 で、分けた式も glyph の配置が 1 文字列と同じだったため。
- **自動の照合の意味は mvm が持つ。**
  - mvm が式を分けた部分の文字列で対応させ、同じ文字列が重複するときは
    「source の n 番目の出現 → target の n 番目の出現」とする。
  - 対応は派生の値で Project に保存しないが、algorithm の版を変形の cache key に入れる。
- **Manim の backend (後の段階):**
  - `MathTex(*segments)` で部分を作る。
  - mvm の対応を `ReplacementTransform`・`FadeOut`・`FadeIn` で組む。
    `TransformMatchingTex` は意味の正としない。
  - 部分の数と各部分の文字列を検査する。Manim が式全体の group で黙って代用した結果を受け付けない。
- **端点の画素:**
  - 半画素の位相の補正は必須とする (P2-0 で、補正なしは 2603 画素ずれた)。
  - 変形の canvas と artifact の矩形は、全 frame の alpha の bbox を実測して決める。
  - alpha が canvas の縁に触れたら失敗とする (「両端の大きさの最大が全 frame を含む」は前提にしない)。

### Project と timeline (P2-1、schema 20)

- 各 transition に `"kind": "blend" | "math_transform"` を**必ず**書く。
  - 19・18・17・16 の file は kind を持たず、読み込み後は 20 の `blend` になる。
  - kind は 20 の file にだけ現れてよい (19 以前の file の kind は、`math_transform` に限らず拒否する)。
  - 未知・重複した kind、20 の file での欠落は拒否する。
- 変形の条件 (`validateTimelineTransitions`)。Project の意味だけで決め、描けるかどうかは見ない。
  - 既存の条件 (同じ track で接している・フレーム保持でない・その端のフェードが 0・尺) はそのまま。
  - 両端が数式 clip である。
  - 後ろの clip に Write が無い (変形が後ろの clip の先頭を使うため)。
  - 前の clip の Write と区間が重ならない。Write の表示が終わる frame は `mathIntroFrameAt` と同じ
    `clipFadeSourceFrameAt` (素材 frame の四捨五入) で数える。
    - 例: 30 fps で置いた Write 135 frame は、60 fps の timeline では 269 frame 目で表示を終える。
  - 両端の背景の alpha が 0 (背景の矩形は式の大きさで変わり、端で段差になる)。
  - 評価した ClipEffects の位置・拡大・回転・切り抜き・不透明度が、次の範囲で一定かつ等しい
    (区間の外の key・fade は自由)。
    - 区間 [cut − before, cut + after)
    - cut の両側の frame (前の clip の最後と後ろの clip の最初)
- 作成は `applyMathTransformTransition`。
  - cut から始め (前 0・後 T)、後ろの clip の尺や見た目の条件で置けない分は縮める。
  - 同じ編集点の既存のトランジションを置き換え、その端のフェードを消す (Blend と同じ)。
  - 長さの変更・削除は既存の `setTimelineTransitionSpan` / `deleteTimelineTransition` を使う。
    上限と吸着は、前の clip の Write と見た目の条件を含む。
  - 数式 clip の編集点のクロスディゾルブ (Blend) は従来どおり拒否する。
- 編集の振る舞い (既存のトランジションの規則による):

  | 操作 | 変形 |
  |---|---|
  | 式・文字サイズ・文字色の変更 | 残る (端点は clip を参照する) |
  | 後ろの clip への Write、両端への背景、区間の見た目を変える ClipEffects | 確定を拒否する (Project・Undo は変わらない) |
  | cut 以外の端の trim | 尺が足りなければ縮め、合計 0 で消える |
  | rolling edit | 接したまま縮める。前の clip の Write が先頭側を使う分は変形に充てない |
  | ripple trim | 接したままなら残る (式は変わらない) |
  | 離す・削除・上書きで間に clip が入る | 消える (Undo で戻る) |
  | 前の clip の分割 | 右側 (同じ式) が前の clip になる。区間の中なら縮める |
  | 後ろの clip の分割 | 左側 (元の ID) が後ろの clip のまま。区間の中なら縮める |
  | fps の変更 | 既存の換算で秒を保つ |
  | 片方の clip を無効にする | Project には残り、描画区間には出さない (Blend と同じ) |
  | コピー・貼り付け・複製 | clip だけを写し、変形は写さない (Blend と同じ) |

- 区間分け (`timelineRenderSegments`) では変形を cut として扱う (延ばさない・重ねない)。
  - 変形の描画・preview はまだ無いので、preview は両 clip を cut で切り替える。
  - 書き出しは、出力する変形があれば `mapTimelineExportPlan` が拒否する。
    変形を cut で黙って置き換えないためで、片方の clip が無効、または track が出力されない変形は対象外。
- 試験:
  - `math_transform_timeline_focused` (新規、83 検査)
  - `math_controller_focused` の `testMathTransformEditing`
    (確定の拒否・trim / split / 削除の Undo / Redo・コピー・複製・保存と開き直し・書き出しの拒否)
  - `m5_timeline_edit_focused` の schema 19 → 20 の移行と kind の厳格な読み込み
  - `math_project_json_focused` の読める版

### Write の時間の正の一本化 (P2-1.1)

- Write が見えるかどうかと境界の丸めは、`timeline_edit.h` の 2 つの関数だけが決める。
  - `mathIntroSourceFrameAt`: timeline local frame で見せる Write の frame。
    Write の外は -1。素材 frame への換算は `clipFadeSourceFrameAt` の四捨五入。
  - `mathIntroTimelineFrames`: Write が見える timeline frame の数。
- 次の 2 つはどちらもこれを通し、丸めを別々に持たない。動作は変えていない。
  - preview・書き出しの `mathIntroFrameAt`
  - 変形の条件・上限・吸着・reconcile
- 試験: `math_transform_timeline_focused` の `testSharedWriteBoundary`。
  - preview の frame の選び方を 1 frame ずつ走査した境界と、変形の境界・cut の前の上限・検証を比べる。
    いずれも手で数えた値と一致する。
  - 対象: 60/60 fps の 90、60 fps の timeline の 30 fps の 135 (269) と 1 (1)、
    30 fps の timeline の 60 fps の 45 (23)、clip 全体。
  - [事実] 境界を 1 frame ずらす変異では 113 検査中 23 件が落ちた。
- [事実] 変更後、次の focused 試験は 8 / 8 件が通過し、lint も通過した。
  - `math_transform_timeline_focused` (113 検査)
  - `math_controller_focused`、`math_export_focused`、`math_project_json_focused`
  - `m5_timeline_edit_focused`、`m7a_1_clip_effects_focused`、`subtitles_contract`、
    `math_raster_cache_focused`

### 変形の中立な契約 (P2-2)

renderer・cache・preview・書き出し・UI はまだ無い。Project の schema と P2-1 の意味は変えていない。

- 分け方 `segmentMathTex` (`src/media/math/math_tex_segments.h`、版 `mvm-tex-segments/1`)
  - P2-0 の spike の規則 (深さ 0 の関係子と、項の後の二項演算子で分ける) を基にした。
  - spike との違い:
    - 空白を捨てず、演算子の前後の空白を演算子の部分に含める。部分を連結すると入力と byte 単位で一致する。
    - `^`・`_`・`
ot` の直後 (`x^-1`・`
ot=`)、`%` の comment の中では分けない。
    - `
e` を関係子に足した。
  - 照合の値 (key) は部分の前後の空白を除いた文字列。
- 照合 `matchMathTexSegments` (版 `mvm-tex-match/1`)
  - key が等しい部分を「source の n 番目の出現 → target の n 番目の出現」で対応させる。
  - 余った source の部分は消え、余った target の部分は現れる。対応は Project に保存しない。
- 描画要求 `MathTransformSpec` (`math_transform.h`): 両端の `MathRenderSpec` と frame 数だけを持つ。
  - frame i は進み具合 i/N。frame 0 は source の静止と同じで、進み具合 1 (target の静止) は含めない (Write と同じ数え方)。
  - app 層の `mathTransformSpecFor` が、トランジションと両 clip から作る (frame 数は前 + 後)。
- cache key `mathTransformKey` (名前空間 `mvm-math-transform/1`)
  - 材料: 分け方と照合の版、frame 数、両端の式・記法・文字サイズ、backend、toolchain、変形の script の識別。
  - 色・背景・ClipEffects・clip とトランジションの ID・timeline の位置と fps は含めない。
- 端点の配置 `mathEndpointPlacement` / `mathTransformPlacement`
  - 整数の位置は静止の `mathRasterPlacement` と同じ (中央、余りは左上寄せ)。
  - 補正 shift は `left + W/2 − W_C/2` で、0 または −0.5 px になる。
    backend は式を canvas の中心に置き、shift だけ動かして描く。
  - shiftX・shiftY は raster の座標 (+X は右、+Y は下)。Manim は +Y が上なので、
    backend は縦に −shiftY だけ動かす。
  - spike の位置 `ceil(W_C/2) − ceil(W/2)` とは、canvas の幅が奇数のときに 1 px 違う。
    補正をその位置から計算するので、どちらでも端点は静止と画素で一致する。
  - canvas の大きさ (全 frame の alpha の実測) はまだ決めない。
- 文字色 `mathTransformColorAt`: straight ARGB の各成分を i/N で線形に補間し、0.5 は切り上げる。
  frame 0 は A、frame N (参照の終状態) は B にちょうど一致する。
- 試験 (いずれも期待値は手で数えた値、または `printf | sha256sum` の値)
  - `math_transform_contract` (新規、290 検査): 分け方の golden (E1・E2・E3・重複項と規則の境界)、
    可逆性、照合、決定性、key の golden と変異、偶奇 16 通りの配置、色。
  - `math_transform_timeline_focused` の `testTransformSpec` (16 検査を追加): 描画要求の内容。
    色・ClipEffects・ID・fps・timeline の位置で key が変わらないこと、
    対照として式・文字サイズ・frame 数で変わること。
  - [事実] 実装に 13 種の変異を 1 つずつ入れると、`math_transform_contract` はすべてで失敗した (1〜22 件)。

### Manim の変形の backend (P2-3)

cache・controller・preview・書き出し・UI はまだつないでいない (P2-4 以降)。
`MathRenderBackend` と preflight も変えていない。

- `renderManimMathTransform` (`src/media/manim/manim_math_tex.h`、script の識別 `manim-transform/1`)
  - 両端を `segmentMathTex` で分け、`matchMathTexSegments` で対応を決める (`planManimMathTransform`)。
  - Manim では `MathTex(*segments)` で両端を作る。
    対応は `ReplacementTransform`、消える部分は `FadeOut`、現れる部分は `FadeIn` で組む。
    `TransformMatchingTex` は使わない。
  - script は部分の型・文字列・glyph の数と、Manim の代用の log (`Could not find SVG group`) を
    `structure.txt` に事実として書く。合否は mvm が決める (`checkManimTransformStructure`)。
    - 部分の数・型 (`MathTexPart`)・文字列のどれかが違う、または代用の log があれば失敗にする。
    - 代用された group で script が後から落ちた場合も、構造の誤りとして知らせる。
  - frames 枚に、終状態の照合の 1 枚を足して描く。PNG がちょうど frames + 1 枚でなければ失敗にする。
  - canvas は端点の大きい方の静止の矩形に、各辺 200 px を足した大きさ。
    - 端点は P2-2 の配置に置く。Manim へ渡す縦の shift は −shiftY (+Y が上)。
- 全 frame の alpha を、呼び出し側が渡す loader (`MathCoverageLoader`) で読んで検査する。
  - `src/media/manim` は decoder に依存しない。製品の loader は app 層の `loadMathCoverage` (静止画 decoder)。
  - 一時的な canvas の縁 (最外周の 1 画素) に alpha が触れたら失敗にする。
  - frame 0 は source、照合の 1 枚は target の静止 (呼び出し側が渡す正の mask) と、全画素で一致しなければ失敗にする。
    mask の外の alpha も 0 と比べる。
  - artifact の矩形は、全 frame の alpha の外接矩形と、両端の静止の矩形の和 (canvas の座標)。
- 結果 (`MathTransformRenderResult`)
  - frames 枚の PNG (canvas の大きさ、白の glyph で alpha が被覆率、色は含まない)
  - canvas の大きさ・artifact の矩形・端点の配置
  - 切り出しと保存は P2-4 で行う。
- 中立な検査の関数 (`math_transform.h`): `mathCoverageBounds`・`mathRectUnion`・`mathRectTouchesEdge`・`mathEndpointDifference`。
- 試験
  - `manim_math_tex_focused` (偽の Manim): request.json の golden (重複項・縦の符号・escape)、構造の照合、
    代用・部分の数と文字列の拒否、取消・timeout、読めない frame、枚数の過不足、
    canvas の縁、端点の不一致、前の実行の `structure.txt` を読まないこと。
  - `math_transform_contract`: 中立な検査の関数。
  - [事実] 16 種の変異を 1 つずつ入れると、すべてで上の試験のどちらかが失敗した。
- [事実] 実 Manim の smoke (`mvm_math_transform_smoke`、CTest に登録しない) は 71 / 71 検査、終了コード 0
  (`build/math-p23-real-20261005-233515.log`)。環境は P2-0 と同じ。
  - 対象: E1→E2・E2→E3・重複項・文字サイズ違い (96→64)・`\frac`・`\left … \right`・小さい式 3 件。
  - 全ケースで、frame 0 と終状態は静止と全画素で一致した。frame 0 を縦に 1 画素ずらすと一致しない。
  - 半画素の補正は横 4・縦 5 の端点で使われた (`small-y` は両方)。
  - artifact は全ケースで両端の静止の矩形の和に等しかった (途中の frame は外に出なかった)。
  - 変形 1 件の所要時間: N = 30 で 2.6〜3.3 秒、N = 12 で 2.2 秒 (端点の静止の描画は別)。
  - `{{a}} + b = c` は、Manim が `{{ }}` で部分を分け直すので、部分の文字列の違いとして拒否した。
- [事実] 縦の符号の換算を外す変異 (shiftY をそのまま渡す) を実 Manim で走らせると、
  縦の補正が 0 でない 5 ケースだけが、端点の不一致 (266〜8546 画素) で失敗した
  (`build/math-p23-real-ysign-mutant-*.log`)。
- [未検証] 実際に Manim が代用 (`Could not find SVG group`) する式。検査は偽の Manim と構造の照合で確かめた。

### 変形の artifact・cache・memory (P2-4)

controller・preview・書き出し・UI はまだ変形を使わない。preview は A→B を cut で切り替え、
出力する変形のある書き出しは拒否したまま (P2-5・P2-6)。Project の schema は変えていない。

- backend の境界 (`src/media/math/math_backend.h`、新規)
  - `MathRenderBackend` と preflight の結果を `math_render.h` から移した。
    変形の描画関数の型が `math_transform.h` の型を使うため。
  - 足した field: `transformTemplate` (`manim-transform/1`)、`renderTransform`、`maximumTransformFrames` (9998)。
    静止と Write の field は変えていない。
  - `renderTransform` は `MathCoverageLoader` を引数で受け取る。
    `src/media/manim` は decoder に依存しないまま。cache が `loadMathCoverage` (app 層、静止画 decoder) を渡す。
  - backend の上限を超える枚数は Project の誤りにしない。cache が描かずに、この描画環境の未対応として失敗させる。
- key は P2-2 の `mathTransformKey` そのまま。
  - 材料: 両端の `MathRenderSpec`・frame 数・分け方と照合の版・backend・toolchain・変形の script の識別。
  - 色・ClipEffects・トランジションと clip の ID・timeline の位置と fps は含めない。
- 端点の静止への依存
  - 変形を描く前に、両端の今の静止 (同じ spec の静止の key) が Ready であることを待つ。
    その mask (`Entry::mask`、静止の artifact を decode したもの) をそのまま backend へ渡す。
  - 前に描けた別の式の静止 (last-good) では描かない。片方が Pending の間は始めない。
  - どちらかの静止が Failed / Unavailable なら、変形も描かずに同じ状態にする (理由に「変形前 / 変形後」を付ける)。
  - 静止の結果が出たら、それを待つ変形を進める (`advanceTransformsWaitingOn`)。
  - 端点を書き換えると key が変わる。古い描画の結果は古い key にしか入らない。
- disk の形
  - `<cache>/transform/<key>/00000.a8 …` と provenance `<cache>/transform/<key>.txt`。
  - 各 frame は、backend の一時的な canvas (余白 200 px) から `result.artifact` の矩形だけを切り出した被覆。
    1 画素 1 byte の生の byte 列で、全 frame が同じ大きさ (artifact の幅 x 高さ)。
  - PNG にしなかった理由
    - mvm には PNG の encoder が無い (画素の decode は静止画 decoder だけを通す)。
    - 被覆を byte 単位でそのまま残せる。
    - 代わりに各 frame の中身を SHA-256 で照合する。
  - 終状態の照合の 1 枚 (P2-3 の frames + 1 枚目) は保存しない。
  - 端点の位置は切り出した座標へ移す (`endpointArtifact = endpointCanvas − artifact.xy`)。
- 公開の手順 (P1.1 の Write と同じ)
  1. 古い provenance を消す。
  2. frame の directory を空にして frame を書く。
  3. provenance を最後に atomic に書く。
  - directory の rename は使わない。provenance があり、正確に合うことが確定の印。
  - 取消 (権限・世代の変更、`retainOnly`) と provenance の書き込みは同じ mutex (`publishGate_`) で排他にする。
    worker は lock の中で取消を見てから書くので、取り消した後に古い世代の結果は確定しない。
  - 権限を失った後は、書きかけの frame も消さない (cache directory を変えない)。
    provenance が無いので使われない。
- provenance `mvm-math-transform-artifact/1`
  - 中身: key・枚数・切り出した大きさ・canvas の大きさ・artifact の矩形 (canvas の座標)・両端の位置 (切り出した座標)・
    両端の静止の大きさと key・各 frame の名前・byte 数・SHA-256 (順に)・変形の script・分け方と照合の版・toolchain。
  - 読むとき (fail-closed、どれかが合わなければ消して描き直す)
    - 読んだ数値と、期待する identity (key・静止の key・script・版・toolchain) から正準形を組み直し、file と byte 単位で比べる。
      余分な行・`068` のような正準形でない数値も拒否する。
    - 枚数が要求と同じか。
    - 両端の静止の大きさが今の静止と同じか。
    - 端点の位置が、静止と canvas の大きさから P2-2 の配置で決まる値と同じか。
    - 各 frame の大きさと SHA-256。
    - frame 0 が今の変形前の静止と全画素一致するか。
  - 描いたとき (backend の結果を検査する、fail-closed)
    - 枚数・矩形が canvas に収まるか、端点の配置が P2-2 の契約と同じか。
    - 各 frame の alpha が artifact の矩形の外に無いか (切り出しで画素を失わない)。
    - 切り出した frame 0 が変形前の静止と全画素一致するか。
- 状態の意味
  - 変形の Ready は「検証済みの artifact が disk にある (preview・書き出しが後で使える)」だけを表す。
    mask が memory にあることは意味しない。
  - 書き出し (P2-6) は `readyTransform` と `loadMathTransformFrame` で disk から読める。preview の mask を追い出した後でもよい。
- preview 用の memory (`residentTransform`)
  - Write と同じ全体の上限・予約 (`MathResidencyBudget`)・LRU・`resident_` の表を共有する。別の 256 MB は持たない。
  - 数えるのは、Write と変形の A8 の mask の合計 (使用中で cache が手放したものを含む)。
  - 追い出すのは使用中でない mask だけ。予約は mask が実際に破棄されるときに返る (P1.1 のまま)。
  - 1 本で上限を超える mask は、他の mask を追い出さずに OverBudget (理由付き) にする。disk の変形は Ready のまま。
    この短絡は Write にも効く (以前は、収まらないと分かっていても先に LRU を空にしていた)。
  - 追い出した変形は、Manim を起動せず disk から読み直す。
  - memory に読むときに中身の壊れた frame を見つけたら、artifact を消して変形を Failed にする (再試行で描き直す)。
- 公開した API (controller からはまだ呼ばない)
  - `transformKeyFor`・`requestTransform`・`readyTransform`・`residentTransform`・`transformResidencyOf`
- 試験 (偽の backend。期待値は偽の backend の定義から手で数えた値)
  - `math_raster_cache_focused` (241 検査、以前の静止・Write の検査は変えていない)。変形について確かめること:
    - 両端の今の静止を待つ。渡す mask は今の静止そのもの。last-good で代用しない。
    - 描画中に端点を書き換える (古い結果は古い key だけに入る。古い A と新しい B を混ぜない)。
    - 取り下げ・権限の喪失・世代の変更の後に確定しない。
      provenance を書く直前に権限を失う場合も含める。このとき frame は 4 枚揃い、provenance はまだ無い。
    - 権限の無い instance は描かず、他の instance の file を消さない。
    - 開き直しで描かない。
    - provenance の欠落・変更 (端点の位置・切れ・余分な行・正準形でない数値・枚数) を拒否する。
    - frame の中身の破損・大きさ・欠落・順の入れ替えを拒否する。
    - 切り出し: 68x11、端点 (34,6)・(4,3)、frame 0 が静止と全画素一致、frame 1 以降の位置。
    - 矩形の外の画素・frame 0 のずれ・backend の上限を拒否する。
    - Write と変形の合計の上限・LRU・使用中の mask。1 本で上限を超える変形は disk で Ready のまま。
    - memory での破損の検出。
  - [事実] `math_raster_cache_focused` を 10 回続けて回し、10 / 10 が通過した。
  - `manim_math_tex_focused`: preflight が変形の関数・script の識別・上限 (9998) を束ねることを確かめる。
    束ねた関数が渡した loader で全 7 枚を読むことも確かめる。
  - [事実] cache の実装に変異を 1 つずつ入れた結果
    - 失敗した (検出できた) 10 種
      - provenance を書くときに取消を見ない
      - 描いたときの frame 0 の照合を外す
      - 矩形の外の画素の検査を外す
      - 読むときの SHA-256 を外す
      - 枚数の照合を外す
      - 片方の静止だけで描き始める
      - 変形を上限に数えない
      - 使用中の mask を追い出す
      - `retainOnly` が変形を残す
      - 配置の照合 4 つをすべて外す
    - 通過した (検出できなかった) 2 種。どちらも他の検査が同じ誤りを先に止める
      - 静止の大きさの照合を外す: 配置の照合と frame 0 の照合が同じ不一致を止める
      - 書く前に古い provenance を消すのをやめる: 描き直すのは読み込みで artifact が合わなかったときだけで、
        そのとき読み込みが provenance を消している
- [事実] 2026-10-06 の通常の release gate (`ctest -LE "performance|stability"`、`build/math-p24-release-gate.log`) は
  1453 / 1453 件が通過した (1975 秒)。`scripts/lint.ps1` も通過した。
- [未検証] 実 Manim では回し直していない。P2-4 は P2-3 の renderer の描画を変えず、preflight で束ねただけである。
  P2-3 の smoke (71 / 71) の結果がそのまま当てはまると考えている。

### 変形の preview と inspector (P2-5)

書き出しはまだ変形を使わない (出力する変形のある書き出しは拒否したまま、P2-6)。
P2-4 の disk・cache・memory の意味 (`MathRasterCache`) は変えていない。Project の schema も変えていない。

- 時間の正は timeline。変形の区間は `[cut - 前, cut + 後)` で、区間の i 番目の timeline frame は変形の frame i
  (`mathTransformWindowFor`・`mathTransformFrameAt`、`src/app/math_clip_render.h`)。区間の外は両端の静止。
- 描かれるかどうかは `mathTransformIsRendered` (両 clip が有効で、track が出力される) だけが決める。
  書き出しの拒否 (`mapTimelineExportPlan`) も同じ関数を通すようにした (動作は変えていない)。
- artifact の位置 (`mathTransformRasterPlacement`・`mathTransformArtifactOriginAt`、`src/media/math/math_transform.h`)
  - 端点の位置: `source = mathRasterPlacement(A の静止) - artifact の中の source の位置`、target も同じ。
    出力の中央に置く静止の配置そのものから決めるので、frame 0 は普通の A の静止と画素で一致する。
  - 両端の静止の大きさの偶奇が違い、canvas (大きい方の端点 + 余白) と出力の偶奇が違う軸では、
    2 つの位置は 1 画素違う。途中の frame は線形に動かし、最も近い整数に丸め、ちょうど半分は
    target の側へ丸める。したがって 2 枚以上なら最後の frame は target の位置で、次の B の静止へ段差なく続く。
  - 「0.5 は切り上げ」(文字色と同じ丸め) にしなかった理由: target が 1 画素上・左にあり 2 枚のとき、
    最後の frame (進み具合 1/2) が source の位置に残り、B の静止で 1 画素跳ぶ。許容誤差で隠さず、規則で防ぐ。
  - artifact がどちらかの端の位置で出力からはみ出すなら使わない (cut で見せ、黙って切らない)。
    disk の変形は ready のままで、preview で使えない理由として示す (P2-5.1)。
- preview の animation (`MathClipPreviewAnimation`、`mvm_controller.cpp`)
  - 既存の `PreviewStillAnimation` (静止画 layer の一部の矩形を出力 frame ごとに変える) にそのまま収まった。
    engine は変えていない。
  - 1 本の数式 clip に 1 つの instance で、Write の部分と、その clip が前・後ろの端の変形の部分を持つ。
    - engine は instance ごとに静止画の texture を持ち、合成の切り替えは tick 単位で提示より遅れて届く。
      区間の境ごとに instance を替えると、その間の frame が前の instance で提示される。
    - state は出力 frame だけから決まる。前の clip の layer が cut の後まで残っても、同じ変形の frame を見せる。
  - 書き換える矩形は、artifact を両端の位置に置いた矩形の和 (と Write の矩形)。
    - 変形の frame は矩形を透明で埋めてから artifact を置く。1 画素動いた位置で静止の glyph を残さない。
    - 背景は透明 (P2-1 で両端の背景は透明に限る)。
    - 文字色は `mathTransformColorAt(A の色, B の色, i, N)`。
  - ClipEffects は今までどおり layer (`PreviewMotion` / 合成の値) が 1 回だけ掛ける。artifact には焼き込まない。
- 変形を付ける条件 (`mathTransformPreviewInputs`)。満たさない間は今までの cut で見せる。
  - 両 clip が描かれ、どちらも入力中でない。
  - 両端の今の式の静止が Ready。前に描けた別の式の静止 (last-good) では置かない。
  - 変形の disk の artifact が Ready。
  - preview 用の mask が memory にある (Resident)。Loading・OverBudget の間は cut。
  - 古い変形・前に見せた変形は使わない。
    - key は端点の式と長さで変わる。
    - animation は毎回の合成で作り直す (同じ値なら同じ instance)。
- 再生中に mask が届いたとき: P1.2 と同じく、次の tick の合成に変形が付き、出し直す。
  変形の frame は engine が出力 frame から決めるので、届いた時刻の frame から見せ、0 からやり直さない。
- 描画の要求 (`requestMathRenders`)
  - Project にある変形をすべて disk に要求する (Write と同じく、書き出しの前に揃えておくため)。
    - 再生位置がどちらかの clip に掛かる変形は、両端の静止と一緒に先に要求する。
  - `retainOnly` に今の変形の key を入れる。
    - 今の変形 (disk と memory) は取り下げない。
    - 端点・長さを変えた前の変形は取り下げる。描きかけなら process ごと止め、結果を確定しない。
  - memory への読み込みは、前・後ろの clip が見える frame の合成だけが要求する (先読みは足していない)。
- inspector (`selectedTransition` に足した値。変形のときだけ)
  - `kind`
  - `transformState`: checking / rendering / ready / error / unavailable。disk の変形の状態で、書き出しが使う。
    - `transformMessage`・`transformLog`
  - `transformPreview` ("" / loading / ready / memory / placement) と `transformPreviewMessage`
    - preview で変形を使えるか。memory の上限に収まらない (memory)・artifact が出力に収まらない
      (placement) 間は cut で見せる。どちらでも `transformState` は ready のまま。
  - `transformUnavailableReason` (backend / authority)・`transformCanRetry`・`transformToolchain`
  - エフェクトコントロール (`TransitionInspector.qml`) は、変形のとき題を「数式の変形」にし、次を出す。
    - 状態
    - 描画の理由と、memory の理由 (別の行)
    - 導入の案内 (`MathDependencyGuidance.qml`、数式 clip の inspector と共有)
    - 再試行・ログ
  - 作成・長さの操作は足していない。
  - 合成が読み込みを始めた・上限に収まらなかったことは、合成の後に状態だけを出し直して示す
    (`refreshSelectedMathTransformStatus`)。
- [未検証] 大きな式の変形の preview の再生中の負荷。区間では state が frame ごとに変わり、render thread が
  矩形を着色して送る (Write と同じ方式)。
- 試験 (期待値は手で数えた値、または偽の backend の定義から手で数えた値。実装の配置関数で作らない)
  - `math_transform_contract` (836 検査)
    - 端点の位置と途中の frame の手計算の値 (偶奇の違う 7x2→4x5 で横 +1・縦 -1)。
    - 2 枚で縦に小さくなる向きでも、最後の frame が target の位置になること。
    - 範囲外の拒否。
    - 偶奇 16 通り x 出力の偶奇 4 通りで、端点の静止が静止の配置に重なり、差が 1 画素以内で、
      2・3・9 枚の最後の frame が target の位置になること。
    - `composeMathPatchAt` が `composeMathPatch` と同じ画素を書くこと。
  - `math_controller_focused` (374 検査)。偽の backend で、区間は 290..319 (30 枚)。
    - 偶奇の組 5 つ (7x2→4x5、4x5→7x2、6x4→4x2、5x3→9x7、8x3→5x6)。
      - cut の前後の timeline frame と変形の frame の対応。
      - frame 0 が普通の A の静止の preview と全画素で一致する。
      - frame 0・1・14・15・16・29 が手で数えた画素と一致する (前・後ろの layer の両方)。
      - 最後の frame と次の B の静止の違いが、偽の変形が足した 1 画素だけ (glyph は跳ばない)。
    - 色 (#FF102030 → #FF5021F0): frame 14・15 (0.5 の切り上げ)・29 と、被覆 200 の画素。
    - ClipEffects (位置 X 10%・不透明度 50%)
      - 変形の layer の値が普通の静止の layer と同じ。
      - 合成の layer は 1 枚。
      - patch の glyph は被覆 255 のまま (artifact に焼き込まない)。
    - fallback (いずれも hard cut の画素)
      - disk: Pending・Failed (`BADT`)・Unavailable (`GONET`)
      - memory: Loading (試験用の保留)・OverBudget。OverBudget では disk は ready のまま、memory の理由を描画の error と分けて示す。
    - memory に置き直すと disk から読み、描き直さない。
    - 色の変更で `retainOnly` が今の変形 (disk と memory) を残す。
    - 式の変更・描画中の再度の変更・長さの変更で古い key を捨てる (描きかけの結果を確定しない)。
    - Write
      - 区間に掛かる A の Write と、B の Write の確定を拒否する (P2-1 の不変条件)。
      - 区間の前で終わる Write と変形は 1 つの animation で見せる。
  - `math_transform_native_playback` (新規、`workstation`、26 検査)。実 D3D11 の preview で、区間は 240..419 (180 枚)。
    1. 再生前に memory にある: 区間の先頭から、cut の前は A の layer、後は B の layer で、変形の frame = 出力 frame - 240。
    2. memory に無い。frame 270 (cut の前) で届ける。
       - [事実] 届いた後の frame 273 から見せ、frame 0 を出さない。
       - cut を越えて B の layer で続く。
       - 再生の組み直しは無い。
    3. memory の上限に収まらない区間へ seek する: cut の静止を提示し、選び直さなくても inspector が memory の理由を示す。
  - `math_transform_inspector_product_ui` (新規): 製品の `Main.qml` と実 controller で確かめる。
    - backend の不在: 「利用不可」・導入の案内・再試行を出す。
    - OverBudget: 「完了」と memory の理由を分けて出し、理由はパネルの幅で折り返す。
  - [事実] 実装に変異を 1 つずつ入れると、すべて試験のどれかが失敗した (8 種)。
    | 変異 | 失敗した試験 |
    |---|---|
    | 位置を「0.5 は切り上げ」で丸める | contract 18 件・focused 6 件 |
    | 変形の frame の前に矩形を透明で埋めない | focused 29 件 |
    | 位置を source のまま動かさない | focused 18 件 |
    | `retainOnly` に変形の key を入れない | focused 1 件 |
    | 色を補間しない | focused 4 件 |
    | 変形の frame を 1 frame ずらす | focused 42 件・native 2 件 |
    | 使えなくなった変形の前の animation を使い続ける | focused 2 件 |
    | 最初に見せた frame を 0 とする (途中から 0 で始める) | focused 2 件・native 3 件 |
    - 「透明で埋めない」は最初は試験を通過した。
      - 試験の合成が patch を毎回 0 で初期化しており、engine の作業領域の使い回しを再現していなかった。
      - patch を無関係な値で埋めてから `fillPatch` を呼ぶように直すと、検出できるようになった。
- 実 Manim の受け入れ (`mvm_test_math_controller --real-manim-transform <manim.exe> <作業 directory>`、CTest に登録しない)
  - 経路: Manim の確認 → 両端の静止 → 変形の cache (.a8) → memory → 実 D3D11 の preview の再生。
    製品の controller を通す。
  - [事実] 2026-10-06: 36 / 36 検査、終了コード 0 (`build/math-p25-real-20261006-021951.log`)。
    - 環境は P2-3 と同じ (Manim Community v0.21.0、MiKTeX 26.5、dvisvgm 3.6)。
    - 所要時間: 確認 7.4 秒、静止と変形 5 件の描画 40.6 秒。
  - 解の公式の連鎖 E1 (416x147) → E2 (976x182) → E3 (636x182)
    - 30 枚の .a8 と、frame 0 が A の静止と全画素一致することを確かめた。
    - 再生中の engine の評価 58 件が「変形の frame = 出力 frame - 区間の先頭」で、両方の変形を cut の前後の layer で見せた。
  - 偶奇: `small-y` (`y` 48 px の 38x47 → `y^2 = 1` 72 px の 209x94)
    - 幅が偶数 → 奇数で、大きい方が奇数なので、端点の左上が横に 1 画素ずれる (856 → 855)。
    - 最後の frame の位置で target の静止が B の静止に重なることを確かめた。
    - 最後の frame と B の静止は全画素で一致した。
  - [事実] 他のケースの最後の frame (進み具合 29/30) と B の静止の違う画素数 (参考)。
    - e1-e2 11733、e2-e3 8546、small-a 247、small-k 232。
    - 途中の frame なので一致は期待しない。
    - 位置の正しさは frame 0 と、終状態の照合 (P2-4) と位置の規則で確かめている。
- [事実] 2026-10-06 の通常の release gate (`scripts/test.ps1 -Preset ucrt64-release -Group All`、
  `build/math-p25-release-gate.log`) は 1455 / 1455 件が通過した (741 秒)。
  - `-Group BuildIndependent` は 1078 / 1078 件 (`build/math-p25-build-independent.log`)。
  - `performance|stability` は除外した。`scripts/lint.ps1` も通過した。
  - GUI・提示の試験は、画面を消灯させない設定 (`SetThreadExecutionState(ES_DISPLAY_REQUIRED)`) の下で回した。

### 入力中の式の優先と、preview で使えない変形の状態 (P2-5.1)

P2-5 の描画・timeline・色・ClipEffects の振る舞いは変えていない。書き出しはまだ (P2-6)。

- 入力中の式の静止を、裏で disk に揃えている変形より先に描く。
  - P2-5 では Project にある変形をすべて要求するので、worker (1 本) が変形を描いている間、
    入力中の式の静止が数秒待たされることがあった。止めていたのは Write の連番だけだった。
  - `cancelPendingAnimations` (`cancelPendingSequences` を置き換え): 入力中の式の静止が Pending なら、
    描き終えていない連番と変形を取り消して忘れる。
    - 描画中なら process ごと止める。待ち行列の仕事は始めずに捨てる。
    - 変形の取消は provenance の書き込み (確定) と同じ mutex で排他にする (retainOnly と同じ)。
    - disk の Ready の artifact と memory の mask は消さない。
    - 止めた連番と変形は、同じ `requestMathRenders` の中で要求し直す。入力中の静止が先に
      要求済みなので、worker は静止を先に描く。
- artifact が出力 raster に収まらない変形
  - P2-5 では Ready の変形の `transformState` を error にしていた。disk の状態と preview の状態を混ぜていた。
  - `transformState` は ready のまま、`transformPreview` = placement と理由
    (`transformPreviewMessage`) で示す。preview は cut で見せ、memory には読まない。
  - 書き出し (P2-6) は同じ中立な検査 (`mathTransformRasterPlacement`) を自分で行い、収まらなければ
    拒否すること (preview の判定を流用しない)。
- エフェクトコントロールは memory と placement の理由を同じ行 (`mathTransformPreviewReason`) に出す。
- 試験 (期待値は手で数えた値、または偽の backend の定義から手で数えた値)
  - `math_controller_focused` (396 検査)
    - 入力中の式の優先
      - 無関係な変形の描画を、取消を見る偽の renderer で止めておく。
      - 静止の描けていない式を入力すると、変形が取消を受け取り、入力中の静止が先に描き終わる。
      - 偽の backend が描き終えた順の記録で確かめる。
      - その後、変形が要求し直されて Ready になり、preview に付く。
    - 出力に収まらない変形 (A 1916x2、artifact 1920 幅、出力の左に 2 画素はみ出す)
      - disk は ready で、`transformMessage` は空。
      - `transformPreview` は placement で、理由を示す。memory には読まない。
      - preview は手で数えた hard cut の画素。
  - `math_raster_cache_focused` (248 検査): `cancelPendingAnimations` は描画中の変形だけを止めて
    結果を確定させない。Ready の変形の provenance・frame・memory の mask は残す。
    取り消した変形は要求し直すと Ready になる。
  - [事実] 変異を 1 つずつ入れると、すべて検出された。
    | 変異 | 失敗した試験 |
    |---|---|
    | 変形を取り消さない | focused 3 件・cache 3 件 |
    | Ready の変形も取り消す | cache 2 件 |
    | 収まらない変形を disk の error にする (P2-5 の振る舞い) | focused 3 件 |
- [事実] 2026-10-06 の通常の release gate は 1455 / 1455 件が通過した (`build/math-p251-release-gate.log`、729 秒)。
  - BuildIndependent は 1078 / 1078 件 (`build/math-p251-build-independent.log`)。
  - lint も通過した。
  - GUI・提示の試験は、画面を消灯させない設定の下で回した。

### MathTransform の書き出し (P2-6)

- 書き出しは preview の常駐 mask や `transformPreview` を参照しない。controller は現在の Project から
  spec を作り、`MathRasterCache::readyTransformForExport` で現在の両端の静止と変形の disk を検査する。
  静止の provenance・key・画素、変形の provenance・端点・枚数・各 frame のサイズと SHA-256 は
  既存の cache の検査を通す。検査の失敗では cache を消したり、描き直したり、last-good へ戻したりしない。
- `mapTimelineExportPlan` は `mathTransformIsRendered` が真の変形すべてについて、要求の spec が現在の
  Project と一致し、枚数が `mathTransformWindowFor` の区間と一致することを確認する。
  `mathTransformRasterPlacement` で現在の出力サイズへの配置を検査し、出力開始前に全 A8 frame を読む。
  disk が Ready でも、配置が収まらない・frame が欠けた・内容が変わった場合は書き出しを拒否する。
- 各 clip を Write・変形・静止の区間に分ける。変形の timeline frame は `mathTransformFrameAt` で選び、
  検証済み disk artifact の `.a8` を 1 枚ずつ `loadMathTransformFrame` で読む。
  原点は `mathTransformArtifactOriginAt`、文字色は `mathTransformColorAt`、透過の画素は
  `composeMathPatchAt` を使う。出力全面の透過 PNG を連番として stage し、通常の export の合成へ渡す。
  ClipEffects は通常の mapping で一度だけ掛け、A8 には焼き込まない。
  V1 に描画対象の変形がある場合は黒の下地を一層残し、変形と静止を既存の overlay 合成へ通す。
  最下層を直接映像へ変換して A8 の alpha を捨てないためで、変形のない Project の経路は変えない。
- preview の NotReady・OverBudget・全 mask の追い出しは書き出しを妨げない。preview の共有予算へ
  変形全体を読み込む要求は出さない。両端の片方が無効、または track が非出力の場合は、
  `mathTransformIsRendered` に従ってその変形を要求しない。必要な変形を hard cut で代用する経路はない。
- 集中試験は `math_transform_export_focused`。cut 前後の disk frame の番号、静止 A と変形 frame 0 の
  全画素一致、±1 px の原点差、最後の原点と次の静止 B、異なる文字色、共通エフェクト、
  先行 Write、常駐状態からの独立、配置不成立、欠損・破損・古い成果物の拒否を検査する。
- 実 Manim の受け入れは既存の `mvm_test_math_controller --real-manim-transform` を拡張した。
  E1 → E2 → E3 の映像のみの Project と MP4 を保存し、frame 0、各変形の先頭、cut の前後、
  最後と直後の frame を復号して、製品 preview の合成画素と比較する。cut 前後では hard cut の対照とも
  比較する。通常の PC 操作は可能。一般の音声/AAC NaN の問題はこの受け入れに含めない。

#### P2-6 の検証 (2026-10-06)

- [事実] `math_transform_export_focused` は 144 検査中 0 件失敗。通常 release gate にも含めた。
- [事実] release の Math / export 集中試験は 19 件中 19 件通過。
  実行: `ctest --test-dir build/ucrt64-release -V -R '(math|timeline_export)' -LE 'performance|stability' --timeout 120`。
  `-N` で 19 件と確認してから実行した。証拠は `build/math-p26-focused-final-20261006.log`。
- [事実] 実 Manim の映像のみ受け入れは 80 検査中 0 件失敗。E1 → E2 → E3 の 360 frame を
  書き出し、11 frame の全 RGB 画素を製品の preview 合成と比較した。4:2:0 の色差の許容を含む。
  通常静止 A と最初の変形 frame 0、通常静止 B と次の変形 frame 0 は復号後も全画素一致。
  cut 前後の 4 frame は hard cut の対照より変形の期待画素に近いことも確認した。
  実行: `mvm_test_math_controller --real-manim-transform <manim.exe> <新規 directory>`。
  証拠は `build/math-p26-real-final-20261006.log` と `build/math-p26-real-final-20261006/` の
  `quadratic-video-only.mvm` / `quadratic-video-only.mp4`。
- [事実] 最終 lint は通過 (`build/math-p26-lint-final-20261006.log`)。
  通常 release gate は `pwsh scripts/test.ps1 -Preset ucrt64-release` で 1456 件中 1455 件通過、
  `ownership_soak_100` の音声 consumer timeout が 1 件失敗。全通過とはしない。
  証拠は `build/math-p26-release-alpha-20261006.log`。未解決の調査は `docs/roadmap.md` に置く。
  performance / stability は実行していない。過去の P0・P1・P2 の記録は上書きしていない。

### P2-1 の gate (2026-10-05)

- [事実] 通常の release gate (`build/math-p21-release-gate.log`) は 1452 件中 1444 件が通過し、
  8 件が失敗した。いずれも実際の提示・window の描画を待つ試験である。
  - `transition_preview`・`text_ui_direct_input`・`math_write_native_playback`・`p4_c_contract_smoke`
    (と `p4_c_contract_smoke_check` の未実行)・`audio_mixer_product_ui`・
    `fixed_test_window_independent_of_screen`・`preview_engine_p5c_engine_lifetime_detach`
  - 実行の終わりで利用者の無操作は 15.6 分 (画面の消灯は 15 分)。
    同じ条件の再実行も同じ 8 件が落ちた。
- [事実] 画面を点け、試験の間 `SetThreadExecutionState(ES_DISPLAY_REQUIRED)` で消灯を止めて
  P2-1.1 の build で再実行すると、8 / 8 件が通過した (`build/math-p211-presentation-rerun.log`)。
  - したがってこの 8 件の失敗は画面の消灯によるもので、P2-1 の変更によるものではない。

## P2-0: 式から式への変形の検証 (2026-10-05)

P2 (式 A → 式 B の変形) の着手前に、Manim の分け方・照合・端点の画素・途中の frame の bbox を
実 Manim で測った。製品の code は変えていない。renderer と layout の最終の契約は、この結果の review の後に決める。
環境は P1-0 と同じ (Manim Community v0.21.0、MiKTeX 26.5、dvisvgm 3.6)。

### 再現

- 静止の参照は、製品の script (`kSceneCommon` + `kStaticScene`) を写した `p20_static.py` を `-s` で描いた。
- 変形は `p20_transform.py` で描いた。
  - 両式を静止と同じ `font_size = em x 12/17` で作る。
  - 静止の最大の大きさに各辺 200 px を足した canvas に置き、`frame_rate = N` で 1 秒の `play` の後に `wait(1/N)` を描く。
  - A・B は、静止の `mathRasterPlacement` (出力が偶数) と同じ整数の offset `ceil(W_C/2) − ceil(W/2)` に置く。
  - 静止と画素の位相を揃えるため、0 または ±0.5 px ずらす (半画素の補正)。
- driver は `p20_driver.py` (segmenter と n 番目の出現の照合を含む)。
- 結果: `build/math-p20-20261005-124047/` と同名の `.log` (27 ケース)、および
  `build/math-p20-rerun-20261005-124328/` (10 ケースと確認用の画像 `sheet-*.png`)。各 directory の
  `spike-scripts/` に script を置いた (置いたのは修正後の版。修正の内容は下の「方式の比較」)。
- 対象の式 (文字サイズ 64、`e12-args-mvm-fs96to64` だけは A を 96)
  - E1 `x^2 + \frac{b}{a}x = -\frac{c}{a}`
  - E2 `x^2 + \frac{b}{a}x + \left(\frac{b}{2a}\right)^2 = -\frac{c}{a} + \left(\frac{b}{2a}\right)^2`
  - E3 `\left(x + \frac{b}{2a}\right)^2 = \frac{b^2 - 4ac}{4a^2}`
  - 重複項の対照 `x + x = 2x` → `x + x + x = 3x`
  - `\boxed{x^2 + y^2 = r^2}` → `\boxed{x^2 = r^2 - y^2}`

### 分け方と glyph の配置

- [事実] Manim 0.21 の `MathTex` は、分けた式を **1 回だけ** TeX で処理する。各部分の前後に
  `\special{dvisvgm:raw <g id=…>}` を挟み、SVG の group で部分を見分ける (`tex_mobject.py`)。
- [事実] 次の 4 方式で、glyph の点の座標は 1 文字列の `MathTex` と完全に一致した
  (最大の差 0 px、bbox も一致。E1・E2・E3・重複項・boxed のすべて)。
  - `single`: 分けない
  - `args`: `MathTex(*segments)`
  - `braces`: backend の中で `{{ … }}` に書き換えた 1 文字列
  - `isolate`: 1 文字列 + `substrings_to_isolate`
  - [推測] 分けても静止の描画を分けた描画へ変える必要は無い。
- [事実] `args` は `\frac{` / `b` / `}{a}x` のように `\frac` の中で分けても描けた
  (`\frac{` は glyph 0 個の部分になる)。`\left(` / `x` / … / `\right)^2` も描けた。
  - [推測] 式全体で中括弧が釣り合っていれば足りる。
- [事実] `isolate` は、`TransformMatchingTex` が使う部分 (`submobjects`) を作らない。
  - 部分は式全体の 1 つだけで、`single` と同じ変形になった (画素の変化の量も同じ)。
  - 分ける文字列 `a` が `\frac` の中の `a` にも一致して命令を壊し、latex が失敗した (`probe-frac-inside-isolate`)。
- [事実] `braces` は部分の間に空白だけの部分 (glyph 0 個) を挟み、部分の文字列にも前後の空白が付く。
  - 部分の番号で照合すると食い違う。最初の run の `braces/mvm` 3 ケースは、終状態が B と一致しなかった
    (差のある画素 2438〜5535)。これは spike の script の誤りで、空白だけの部分を数えずに番号を付けると一致した (rerun)。
  - 空白でない部分だけを数えれば、画素は `args` と同じになった。
  - `{{` は文字列の先頭か空白の直後でだけ group として扱われる。
- [事実] 部分の SVG group が見つからないとき、Manim は error を log に出して式全体の group で代用する
  (`Could not find SVG group … Using fallback`)。今回のケースでは起きなかった。
  - 製品では、部分の数と各部分の文字列を検査しないと、この代用を見逃す。

### 端点の画素

- [事実] 半画素の補正をした描画では、失敗した 1 ケースを除く全ケースで、alpha が完全に一致した
  (差のある画素 0)。canvas の中で静止の矩形の外に alpha は無かった。
  - 最初の frame と静止の A
  - 最後の frame (`wait` の 1 枚) と静止の B
  - 文字サイズが違う A (96) → B (64) でも同じだった。
- [事実] 補正を外した対照 (E1 の縦の補正は −0.5 px) では、frame 0 と静止 A の差が 2603 画素、alpha の差は最大 134。
  [exit] 半画素の補正は必要。
- [事実] PNG は N + 1 枚 (N = 30 で 31 枚、60 で 61 枚)。空の frame は無かった。
  - frame i (< N) は進み具合 i/N、最後の 1 枚は終状態。
- [事実] 半透明の画素では RGB が 255 未満になる (alpha の付いた画素の RGB を数えた)。
  製品は alpha だけを被覆率に使うので影響しない。

### 途中の frame の bbox

- [事実] 全ケースの全 frame で、alpha の bbox は端点の glyph の bbox の和に収まった (はみ出し 0 px)。
  端点の静止の矩形 (余白 8 px を含む) の和にも収まった。
  - 対象には `TransformMatchingTex` の移動付きの fade (`target_position`) と、mvm の対応による変形を含む。
- [推測] 直線の経路の補間と、移動しない fade なら、点は両端の点の凸結合なので和に収まる。
  - `TransformMatchingTex` の fade は部分を相手の group の中心へ動かす。
  - 円弧の経路 (`path_arc`) はこの議論の外である。
  - したがって「max(A, B) が全 frame を含む」は、今回のケースでの観測にとどまる。
- [推測] 契約の候補 (review で決める):
  - backend は余白の広い canvas に描く。
  - mvm は全 frame の alpha の bbox を実測し、artifact の矩形を「端点の矩形の和 ∪ 実測の bbox」とする。
  - alpha が canvas の縁に触れたら、切れたとみなして失敗にする。
  - 端点の offset は、この矩形からの整数の位置として provenance に書く。

### 照合

- [事実] `TransformMatchingTex` は、同じ文字列の部分を 1 つの group にまとめて変形する。
  - 重複項の対照では、A の `x` 2 個の group が B の `x` 3 個の group へ変形する。
  - どの `x` がどこへ行くかは Manim の部分の数合わせで決まる (`sheet-dup-args-manim.png`)。
- [事実] mvm が決めた対応 (source の n 番目の出現 → target の n 番目の出現) を、
  `ReplacementTransform` (対応する部分)・`FadeOut` (A だけの部分)・`FadeIn` (B だけの部分) で組めた。
  - 端点は完全に一致した。
  - 重複項では `x₁→x₁`・`x₂→x₂`・`+→+`・`=→=` が動き、3 つ目の `+ x` と `3x` が現れ、`2x` が消えた
    (`sheet-dup-args-mvm.png`)。
  - 曖昧な照合を Manim に任せずに済む。
- [事実] E2→E3 は、自動の分け方では `=` だけが対応する
  (E3 の左辺 `\left(x + \frac{b}{2a}\right)^2` は 1 つの部分)。
  分けて (`\left(` / `x` / `+` / `\frac{b}{2a}` / `\right)^2`) も描けるので、
  将来の手動の照合で部分を細かくする余地はある。
- 照合の algorithm の版は変形の cache key に入れる (派生の値で Project には保存しない)。

### 所要時間

- [事実] 静止 1446〜2162 ms (最初の 1 回が 2162 ms)。
- [事実] 変形 (N = 30) 2168〜3913 ms、N = 60 で 3875 ms。
  - spike は配置の比較用に 1 文字列の `MathTex` も 2 つ作るので、製品より LaTeX の処理が 2 回多い。

### 未検証

- [未検証] mvm から `CREATE_NO_WINDOW` で起動した場合。
- [未検証] 複数行 (`align` の `\\`) の式。
- [未検証] 式が出力に近い幅の場合。
- [未検証] N が数百を超える場合の時間。
- [未検証] SVG group の代用が実際に起きる式。

## P1-0: Write の artifact と書き出しの検証 (2026-10-05)

P1 (数式の `Write` animation) の着手前に、artifact の方式 (Manim の PNG 連番) と書き出しの経路
(MLT `qimage` の連番) が成り立つかを確かめた。製品の code は変えていない。
環境は P0-0 と同じ (Manim Community v0.21.0、MiKTeX 26.5、dvisvgm 3.6)。

再現: P0 の scene と同じ module level の `MathTex` で frame を式に合わせ、`config.frame_rate = N` とし、
`self.play(Write(TEX), run_time=1)` を `manim render --format png --transparent --progress_bar none`
(`-s` なし) で描く。

- [事実] PNG はちょうど N 枚 (`<Scene>0000.png` から 4 桁の連番)。N = 30 / 60 / 90 / 180 / 600 で
  すべて一致した。mp4 などの動画 file は出さない。
- [事実] 全 frame が静止の mask (`-s`) と同じ大きさで、不透明な画素はすべて白。外周 1 px に alpha は無い
  (二次方程式の 96 px、同じ式の 400 px、`\boxed{x^2 + y^2 = r^2}` の 24 px)。
  Write の線は静止の glyph の外へ出る (96 px で最大 1989 画素) が、P0 の余白 8 px に収まった。
- [事実] frame 0 は空 (alpha の合計 0)。frame i は進み具合 i/N で、最後の frame は (N-1)/N。
  96 px の二次方程式 (N = 90) では、frame 89 と静止の mask の差は 741 画素、alpha の差は最大 50。
  frame N から静止の mask へ切り替えたときの段差は、Write の 1 frame 分にあたる。
- [事実] alpha の合計は単調に増えるとは限らない。二次方程式 (96 px) は 2% を超える減少が 0 frame だったが、
  `\boxed` (24 px) は輪郭を描いてから塗るため、途中で静止の 1.88 倍まで増えてから減った。
  したがって受け入れでは単調性を一般には要求せず、frame 0 が空であることと、最後の frame が
  静止に近いことを確かめる。
- [事実] 所要時間 (二次方程式 96 px): N = 30 で 1653 ms、60 で 1741 ms、90 で 1853 ms、180 で 2207 ms、
  600 で 4501 ms。静止の描画は 1539 ms。PNG の合計は N = 90 で約 1.0 MB。
- [事実] MLT 7 (UCRT64) の `qimage` producer は `%05d` の連番を `ttl=1` で 1 frame ずつ開く。
  位置で番号を符号化した 60 枚に `in=5 out=14` を指定すると、出力の 10 frame は元の 5〜14 と
  1 対 1 で一致した。書き出しは連番の PNG を stage して `qimage` で開けばよく、可逆の中間動画は要らない。
- [推測] preview に持つ mask を A8 (1 画素 1 byte) にすれば、96 px の二次方程式 (854x230) は 1 frame
  約 196 KB、N = 90 で約 18 MB。出力全面の RGBA (1920x1080) を frame ごとに持つ方式の約 1/40。

## P0-0: Manim MathTex の検証 (2026-10-05)

環境: Manim Community v0.21.0 (`~/.local/bin/manim.exe`)、MiKTeX 26.5 (ユーザー単位の導入、
`%LOCALAPPDATA%\Programs\MiKTeX`)、MiKTeX-pdfTeX 4.27、dvisvgm 3.6。

### MiKTeX の導入

- [事実] `winget install -e --id MiKTeX.MiKTeX` (25.12) は本体を導入したが、最後の初期設定 (`initexmf`) が
  失敗し、bin を PATH に追加せず、ファイル名の索引も作らなかった。原因はユーザーの PATH にあった
  **ファイルを指す項目** (`C:\Program Files\WinGet\Links\wget2.exe`) で、`initexmf` は PATH の各項目をディレクトリとして
  調べて `Windows API error 267: ディレクトリ名が無効です` で止まる (`%LOCALAPPDATA%\MiKTeX\miktex\log\initexmf.log`)。
- [回避策] その項目をユーザー PATH から外し (`C:\Program Files\WinGet\Links` はシステム PATH にあるので wget2 は引き続き
  解決される)、`initexmf --modify-path`、`initexmf --update-fndb`、`initexmf --set-config-value=[MPM]AutoInstall=1`、
  `miktex packages update` を実行した。`initexmf --report` は `PathOkay: yes`。
- [事実] 更新前の `latex --version` は 1 行目より前に `latex: major issue: So far, you have not checked for MiKTeX updates.`
  を出した。**版の取得 (fingerprint) は stdout の 1 行目だけを読み、stderr を混ぜないこと。** 更新後は出ない。
- [事実] 足りない package (`preview`) は、初回の描画中に確認なしで自動導入された (`latex.log`:
  `installing package preview triggered by tex/latex/preview\preview.sty`)。

### 描画

再現: `request.json` (`{"source": ..., "font_size": ...}`) を読む scene を
`manim render -s --format png --transparent --progress_bar none --media_dir <dir> <script>.py MvmMathTex` で描く。

- [事実] `MathTex(source).set_color(WHITE)` は、透過背景に白の glyph の PNG を出す。
- [事実] 所要時間 (式 `x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}`、frame 1920x1080):
  - 導入直後の初回 19.4 秒 (latex の format の生成と package の自動導入を含む。1 回だけ)
  - 新しい式、または新しい `--media_dir` で約 1.5 秒 (1470 / 1505 / 1546 ms)
  - 同じ式・同じ `--media_dir` の再描画 944 ms
  - [推測] 1.5 秒のうち大半は Python と Manim の起動である。P0 では非同期化と last-good で足り、
    latex + dvisvgm を直接呼ぶ backend (P0.5) の前倒しは要らない。
- [事実] `\frac`・`\sqrt`・`\pm`・`\boxed` は既定の template で描ける (`\boxed` は amsmath)。
- [事実] **frame を式に合わせられる。** script の module level で `MathTex` を作り、その大きさから
  `config.pixel_width/pixel_height/frame_width/frame_height` を設定すると、CLI の `--resolution` より優先される
  (`--resolution` の有無で同じ結果)。1080p 相当の 135 px/unit、余白 8 px で、出力は 1204x320、
  alpha の bbox は (8,8)-(1195,311) であり、glyph が余白を除いて画像にちょうど収まる。
- [事実] **文字サイズの換算:** 135 px/unit で `\rule{1em}{1em}` の一辺は font_size 48 で 68 px、96 で 136 px。
  すなわち 1 em = font_size x 17/12 px。mvm の `fontSize` を文字 clip と同じ「em の px」とするなら、
  Manim へ渡す値は `fontSize x 12/17` である。

### 描画の失敗

- [事実] 不正な式 (`\fracc{a}{b}`) は exit 1。stderr に `ValueError: latex error converting to dvi` と
  TeX の log の path が出る。log には `! Undefined control sequence.` の行がある。
  - 分類: この文言を `InvalidSource` の根拠にする。
  - 利用者への message: log の `!` で始まる行とその直後の行から作る。
- [未検証] 実行中に latex / dvisvgm が見つからない場合の stderr の文言。preflight で事前に検出するので、
  render 中の `BackendUnavailable` の判定はそれに頼らない。
- [未検証] mvm から `CREATE_NO_WINDOW` で起動した場合も、package の自動導入が確認なしで進むこと
  (上の検証は通常の console から起動した。設定 `AutoInstall=1` は起動方法に依らない見込み)。
