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
部分式の同一性 (変形・強調のため) は未決定であり、P2 の着手時に比べて決める。

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

## Project の schema (18、Write は 19)

19 で数式 clip の `"math_animation"` を足した (下の「Write (P1)」)。以下は 18 で決めた形で、19 でも変わらない。

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
式から式への変形 (`TransformMatchingTex`・部分式の同一性) は P2。

### Project (schema 19)

- 値は clip の `"math_animation": {"intro": "write", "intro_frames": N}`。中の field はすべて必須で、
  未知・重複・欠落を拒否する。省略は intro 無し (書き出しも intro 無しなら書かない)。
  数式 clip 以外には書けない。18・17・16 の file は 19 として読む (`math_animation` は 19 にだけ現れてよい)。
- `MathClipData` (式の意味と見た目) とは分けて `TimelineClip::mathAnimation` に持つ。静止の
  `MathRenderSpec` と cache key を変えないため (P0 の cache と golden key がそのまま使える)。
  ClipEffects にも入れない (種別に依らない keyframe の仕組みで、Manim の artifact を要らない)。
- `intro_frames` は clip の素材 frame (fade と同じ domain) で、clip の見えている先頭から数える。
  1 から clip の尺と 600 frame (60 fps で 10 秒) の小さい方まで。clip の尺に対する割合にはしない
  (末尾を trim すると書く速さが変わり、artifact の key が尺に依存するため)。
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
  `write/<key>.partial-<ticket>/` へ置いてから rename し、provenance を最後に書く。合わない・
  欠けた・読めない frame の結果は消して描き直す。`.partial-` の残りは確認の前に消す。
- 静止と同じ worker・権限・世代・取消で扱う。preview は連番の全 frame を 1 画素 1 byte で持つので、
  256 MB を超える連番は描けても Failed (理由付き) にする (正しい artifact は消さない)。
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
  ときだけ連番を渡す (静止で代用しない)。

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
- [未検証] 大きな式・長い Write の preview の再生中の負荷 (render thread での patch の着色と送信)。
  1080p 全面の式では 1 frame の patch が約 8 MB になる。
- [未検証] Write を付けた解の公式を人が一通り制作する手順 (P0 と同じく自動試験は契約の確認)。

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
