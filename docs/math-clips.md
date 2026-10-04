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

部分式の同一性 (変形・強調のため) と animation の artifact の方式は未決定であり、P1 / P2 の着手時に比べて決める。

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
  `error` (理由は `message`、詳細は `log`) / `unavailable` (backend が無い)。
  `showingPrevious` は、preview に前に描けた画素 (last-good) を出していることを示す。
- **last-good:** clip ごとに最後に描けた mask を session の間だけ持ち、描き直し中・失敗中の preview に出す。
  書き出しには使わない。開き直した後は対応付けない (派生の state を Project に入れないため)。
- **書き出し:** 出力する数式 clip (有効で、出力する track にあるもの) は、現在の式の描画が済んでいなければ
  開始時に拒否する。controller は clip ID → `<key>.png` の表を `TimelineExportRequest::mathArtifacts` で渡し、
  書き出しは Manim を起動しない。
- **保存先の変更 (別名で保存・開く):** cache の場所が変わるので、新しい場所で描き直す (cache は移さない)。
- **終了:** `MvmController::shutdown` が描画中の Manim / LaTeX を process ごと止め、worker を待つ。

未実装: 文字サイズを変えている間は、描き直しが済むまで前の大きさの描画を出す
(計画の「前の描画を拡大縮小して即座に見せる」はまだ作っていない)。

## Project の schema (18)

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

```powershell
pwsh scripts/build.ps1 -Target mvm_math_manim_smoke
.\build\ucrt64-release\bin\mvm_math_manim_smoke.exe "$env:USERPROFILE\.local\bin\manim.exe" <作業 directory>
```

- [事実] 2026-10-05: 44 検査中 0 件失敗。fingerprint は
  `manim=Manim Community v0.21.0` / `latex=MiKTeX-pdfTeX 4.27 (MiKTeX 26.5)` / `dvisvgm=dvisvgm 3.6`。
  受け入れ scenario の 5 式、`\boxed` を含む式、引用符と改行を含む式がそれぞれ 1540〜1748 ms で描けた。
  PNG は mvm の静止画 decoder で読め、四隅は透明、不透明な画素はすべて白だった。
  `\fracc{a}{b}` は `InvalidSource` で、message は `Undefined control sequence.`。

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
