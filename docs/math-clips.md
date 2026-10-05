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
  (`cancelPendingSequences`)。止めた連番は同じ要求で要求し直す。

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
