# P3-6B: 製品の書き出しの入口からの全長の検収（と、P3-6A.1 の修正）

2026-10-14。製品の書き出しの入口（`MvmController` → `TimelineExportRequest` → `mvm::app::exportTimeline`
→ MLT）から、数式 group の選択（`Auto`）で v4 の Project 全体（6840 frame、1920x1080、60 fps）を書き出し、
出力を確かめた。

全長の run の前に行った監査（Task A）で、**`Auto` の互換性の欠陥を 1 件見つけた。** 指示どおり全長の
検収を止め、P3-6A.1 として先に直した（1）。全長の run は、その修正を入れた binary で行った。

数式 group の合成の式、下地の alpha の条件、MLT の affine、V1 の背景の保護、書き出しの timeout
（600000 ms、実時間）、画素の authority は変えていない。**既定は off のまま**で、設定の UI は足していない。
commit・push はしていない。P3-4・P3-5A・P3-6A の証拠と判定は変更していない
（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。

証拠は `build/math-group-product-p36b-20261014/`。再現は `pwsh scripts/math-group-product-p36b-verify.ps1`
（`-Stage freeze`・`focused`・`full`）。

## 判定

| # | 項目 | 結果 |
| --- | --- | --- |
| 1 | 製品の入口の選択と、fallback の互換性 | **PASS（P3-6A.1 の修正の後）。** 修正の前は、`Auto` が画像の上限の超過で書き出しを失敗にした（1）。修正の後は 36/36 通りが期待どおり（6） |
| 2 | 全長の完走 | **PASS。** 6840 frame、判定 ELIGIBLE、数式 group の graph、実行時の失敗 0、最終 file を公開 |
| 3 | 書き出しの全体の実時間（基準 600 秒） | **PASS。291.0 秒**（controller の呼び出しの直前から、controller が書き出し中でなくなるまで） |
| 4 | consumer の待ちの timeout（600000 ms） | **PASS。** timeout は起きていない。待ちの区間そのものは測っていない（書き出しの関数の全体が 291.0 秒なので、それ以下である） |
| 5 | MP4 の妥当性と frame の被覆 | **PASS。** H.264 High、6840 frame を復号、error 0、時刻は 1 frame ずつ、尺 114.000 秒 |
| 6 | 画素の一致の証拠と、その限界 | **成立（間接）。** 出力の MP4 の SHA-256 が、P3-4・P3-5A の診断用の全長の出力 4 本と同じ。この run では encoder 直前の RGBA を採っていない（5） |
| 7 | 資源と lifecycle | **PASS。** 成功・失敗・取消・timeout・くり返しのどれでも、transition・callback・準備済みの画像・一時 file の残りは 0。race が無いことの証明ではない |
| 8 | 回帰試験 | **PASS: 全 CTest 1523/1523**（2 回目。前後とも GUI の環境は VALID）。1 回目は 1 件落ちた（7）。lint 通過 |
| 9 | 製品の既定にできるか | **この phase では判断しない。** 既定は off のまま。残っている事項は 8 |

**全体: PASS。** P3-6B の PASS は、数式 group を既定で on にしてよいという意味ではない。

## 1. Auto の fallback の監査と、P3-6A.1 の修正（Task A）

### 実行の順

| 順 | 段 | 場所 |
| --- | --- | --- |
| 1 | 共通の stage（数式・文字・画像を PNG にする） | `mvm::app::exportTimeline` |
| 2 | 数式 group 用の準備済みの画像を作る（数式 clip を 1 本 stage するたびに、同じ画素から） | 同（stage の loop の中） |
| 3 | 準備済みの画像の上限（64 MB）の検査 | 同（画像を 1 枚足すたびに） |
| 4 | 書き出しの前の判定（ELIGIBLE・INELIGIBLE・UNKNOWN） | `mvm_mlt_export_two_track`（MLT の層） |
| 5 | graph の選択（ELIGIBLE でなければ従来の graph） | 同 |

判定（4）は、V1 の clip の配置と、stage した画素の alpha の最小値を使う。どちらも stage の後にしか
揃わないので、2・3 は 4 より前に走る。

### 欠陥（P3-6A のもの）

**`Auto` では、判定が UNKNOWN・INELIGIBLE になる Project でも、準備済みの画像が 64 MB を超えると、
書き出しが失敗した。** 3 の検査が、4 より前に、書き出しそのものを失敗にしていたためである。その Project
は、選択 off なら（そして上限に届かなければ `Auto` でも）従来の graph で書き出せる。最適化のための準備
だけが原因で、書き出せるはずの Project が書き出せなくなる。ELIGIBLE の Project でも同じである。

P3-6A の文書は「上限を超えた場合も書き出しは失敗する」と書いており、動きとしては把握していた。
それが UNKNOWN・INELIGIBLE の Project にも及ぶこと（fallback より前に失敗すること）を、互換性の欠陥と
して扱っていなかった。

修正の前の binary での再現（`p36a1-before-fix/math_export_focused.log`。107 検査中 21 件失敗）:

| 場合 | 修正の前 |
| --- | --- |
| 製品の上限（64 MB）、observer なし、V1 が隙間（上限が無ければ ELIGIBLE）、数式 9 本で約 71 MB | 失敗: 「数式 group の準備済みの画像が上限 64 MB を超えました」 |
| 同、V1 に alpha 0 がある（INELIGIBLE） | 同じ失敗 |
| 小さい上限（20000 byte）、V1 が隙間・alpha 0・拡大（UNKNOWN）の 3 通り | 3 通りとも同じ失敗 |

### 修正（P3-6A.1）

`src/app/timeline_export.cpp`。**`Auto` で準備済みの画像が上限を超えたら、準備を取りやめる。**

- 準備済みの画像を全部捨て、以降の clip では準備をしない。
- 判定も数式 group の graph も使わず、選択 off と同じ道（従来の graph）で書き出す。
- 書き出しの結果の記録に、取りやめたこと（`preparationRejected`）、理由
  （`prepared-raster-budget-exceeded`）、上限、超えた時点の byte 数を残す。判定は「していない」と記録する
  （していない判定を UNKNOWN と書かない）。

変えていないこと:

- **上限は 64 MB のままである。** 超えた 1 枚を足した時点で止めるので、準備済みの画像が持つ量は
  「上限 + 1 枚」を超えない（試験で確認: 71,654,784 byte で止まる。上限は 67,108,864 byte）。
- **stage そのものの失敗は、最適化の取りやめにしない。** 描画済みの PNG が無い数式 clip は、`Auto` でも
  書き出しの失敗のままである（試験で確認）。
- **準備済みの画像そのものを作れない場合（参照・大きさが不正）も、失敗のままにした。** 上限の超過と
  違い、入力の不正であり、試験で再現する手段も無いので、fallback の対象に入れていない。
- 診断の指定（`Auto` ではなく、observer だけで数式 group を指定）は、これまでどおり上限の超過を失敗に
  する（P3-3B・P3-3C・P3-3D の対照が前提にしている動き。試験で確認）。
- ELIGIBLE で数式 group の合成が実行時に失敗した場合は、これまでどおり書き出しの失敗である（従来の graph
  でやり直さない。P3-6C で決める）。

回帰試験（CTest の `math_export_focused` に追加。修正の前は失敗し、修正の後は 107 検査中 0 件失敗）:

| 場合 | 確かめること |
| --- | --- |
| 製品の上限（64 MB）、observer なし、V1 が隙間・alpha 0 の 2 通り。1920x1080 に数式 9 本 | 書き出しが成功する。取りやめと理由を記録する。上限は 64 MB で、超えた 1 枚で止まる。**出力は選択 off と同じ byte 列** |
| 小さい上限（20000 byte）、V1 が隙間・alpha 0・拡大の 3 通り | 同上。affine の数が選択 off と同じ。一時 file・準備済みの画像・transition・callback が残らない |
| 上限に余裕があるとき（同じ 3 通り） | 判定が ELIGIBLE・INELIGIBLE・UNKNOWN になる（fixture が意図した状態である）。ELIGIBLE では数式 group が実際に合成する |
| 対照: 診断の指定で上限を超える | 失敗のまま。出力なし |
| 対照: 描画済みの PNG が無い（stage の失敗） | 失敗のまま。取りやめとして記録しない。出力なし |

### 残ること

- 画像の準備は、今も判定より前に走る。**`Auto` では、判定が UNKNOWN・INELIGIBLE になる Project でも、
  使わない画像を準備する時間と memory（上限まで）がかかる。** v4 では 0.7 秒・2.81 MB だった（P3-4 の
  計測）。判定を準備の前へ動かすことは、この phase ではしていない。
- 「V1 に動画がある」Project は試していない。判定できない V1 の対照は、V1 を拡大した静止画である。

## 2. 検収の対象の固定（Task B）

P3-6A.1 の修正・lint・全 CTest の後に、`freeze.json` を作った（908 file の SHA-256）。

| 対象 | 内容 |
| --- | --- |
| source | git の HEAD `34a81b4`、`src`・`apps`・`tests`・CMake の全 file、HEAD からの差分（`freeze-git-diff.patch`。SHA-256 `0421AA455D5AD7A1…`） |
| binary | `mvm_math_score_export_parity.exe`（`1B986855EC9C8311…`）。製品の controller と書き出しを link した検証 harness で、全長の書き出しを行う process である |
| runtime | MLT 7.36.1（`libmlt-7.dll` `13D5EFA4536D3225…` と module の全 DLL）、Qt、FFmpeg 8.1.2 の library、`libx264-165.dll`（`ACF3D8F926F42ADA…`）、ffprobe・ffmpeg |
| Project | `quadratic.mvm`（`D6991972807458A7…`）、数式の成果物の cache（stage と画像の準備の入力）、Project が参照する素材 |
| encoder の設定 | 製品の要求が決める。全長の run の記録（要求の写し）と、出力に x264 が書いた設定に残る（3・4） |
| script | `scripts/math-group-product-p36b-verify.ps1`、`scripts/math-group-product-p36a-verify.ps1` |

- `focused` と `full` は、実行の前と後に `freeze.json` と照合する。どちらも前後で一致した。
- freeze の後は build していない。source も書き換えていない。この文書と `docs/roadmap.md` は freeze の
  後に書いた（freeze の対象ではない）。
- `mvm.exe` は freeze に入れていない。`scripts/test.ps1` が実行のたびに link し直すためである（全 CTest の
  前後で変わったのは `mvm.exe` と `mvm_test_text_ui_input.exe` だけ）。**全長の書き出しは `mvm.exe` では
  なく、同じ source を link した検証 harness の process で行った。** UI からの操作は通っていない。

## 3. 全長の書き出し（Task C）

run は 1 回（`20261014-063420-full-authoritative-1/`）。くり返しは足していない（出力が過去の 4 本と同じ
byte 列で、対象も freeze と一致したため）。

harness の `--product-pure` を使った。製品の `MvmController` に書き出しをさせ、**controller が作った要求を、
1 つも書き換えずに** `mvm::app::exportTimeline` へ渡す。P3-6A までの harness の主の書き出しは、observer と
encoder 直前の RGBA の計測を必ず付けていたので、この run には使っていない。

| 項目 | 値 |
| --- | --- |
| 選択 | `MvmController::setMathGroupExportOptimization(true)` → 要求の `mathGroupOptimization = Auto`。controller の既定は off |
| 要求に付いていたもの | 進行と取消の callback だけ（製品のもの）。observer・encoder 直前の検査・stage の観測は付いていない |
| harness が足したもの | なし。MLT の計時の hook 0、FFmpeg の記録の tap なし |
| timeout | 600000 ms（製品の値） |
| worker・encoder | render thread 4、encoder thread 0（自動）、CRF 23、MP4（製品の値） |
| 出力 | 1920x1080、60 fps、6840 frame |
| 判定 | ELIGIBLE（`base-alpha-proven`、V1 の alpha の最小値 255） |
| graph | 数式 group。group 2、まとめた clip 97、従来の経路に残した対象の clip 0、**残った affine 23** |
| 数式 group の callback | 13,730 回、うち合成 13,491 回。**実行時の失敗 0** |
| 準備済みの画像 | 81 枚、2,947,112 byte（上限 67,108,864 byte） |
| 書き出しの全体（実時間） | **291.0 秒**。controller の `exportTimeline` を呼ぶ直前から、controller が書き出し中でなくなるまで（単調な時計） |
| 書き出しの関数の中 | 291.0 秒 |
| process の起動から終了まで | 292.8 秒（Project を開く、数式の成果物の確認、出力の SHA-256 を含む） |
| CPU 時間・memory の peak | 991.9 秒・1.73 GB（working set） |
| watchdog | harness の中 900 秒（取消を送る）、script 1500 秒（process を止める）。どちらも作動していない。製品の timeout とは別で、基準（600 秒）の判定にも使っていない |

- group・clip・affine・callback・合成の回数・画像の byte 数は、P3-4 の診断用の全長の run と同じ値である。
- 合成の回数（13,491）が frame 数（6840）より多いのは、group が 2 つあり、MLT が同じ frame を再び要求する
  ことがあるためである（P3-5A で、どの回も画素が同じであることを確かめている）。この run では frame ごとの
  内訳は採っていない。
- 進み具合は、controller が UI へ出す値を 1 秒ごとに読んだだけである（289 点。記録に残してある）。

## 4. 出力の検査（Task D）

harness の集計を使わず、ffprobe・ffmpeg で確かめた。

| 項目 | 結果 |
| --- | --- |
| 最終 file | あり（2,062,352 byte）。一時 file（`.mvmtmp`）は無い |
| codec・設定 | H.264 High、level 4.2、yuv420p。x264 が stream へ書いた設定（core 165、`crf=23.0` ほか）は、P3-4 の出力のものと 1 字も違わない |
| 大きさ・fps・尺 | 1920x1080、60/1（平均も 60/1）、114.000 秒、開始 0 |
| frame の数 | container 6840、読めた数 6840、**復号 6840**（ffmpeg、`-xerror`、exit 0、error の行 0） |
| 時刻 | 表示の順の pts が 0 から 1 frame ずつ（6840 個）。packet の pts は 6840 個で重複なし。dts は増える一方で、pts を超えない |
| stream | 映像 1 本だけ（Project に音声が無い。過去の出力と同じ） |
| **MP4 の SHA-256** | `ff8eb4d5d80b3021…a8bf6c0`。**P3-4 の全長 3 本、P3-5A の全長 1 本と同じ**（4 本とも今回計算し直した） |
| 数式 group の失敗・frame の欠け | 失敗 0。書き出しが返した frame 数 6840 |
| 後始末 | transition 0、callback 0、準備済みの画像 0 byte |

MP4 全体の SHA-256 が同じであることは、**符号化された出力が同じ byte 列である**ことの確認である。
encoder 直前の RGBA を新しく byte 単位で比べたものではない（5）。

## 5. 画素の authority（Task E）

この run には、encoder 直前の RGBA を採る検査を付けていない（付けると、計測なしの製品の実行ではなく
なる）。画素の authority は、これまでのものをそのまま使う。

| 証拠 | 範囲 | 出どころ |
| --- | --- | --- |
| encoder 直前の RGBA の 64 bit の digest が、製品の経路（数式 group なし）と一致 | 6840/6840 frame。全長の run 4 回 | P3-4・P3-5A |
| 同じ frame の SHA-256 が一致 | 120/120 frame（20 区間 × 6 frame） | P3-5A |
| 同じ frame の RGBA の全 byte が一致 | 40/40 frame | P3-5A |
| 製品の入口（選択 on）と選択 off の、encoder 直前の RGBA が一致 | 2,160 + 360 = 2,520 frame（短い fixture と、v4 の全体 graph の区間 1200・5280 の 60 frame ずつ） | 今回の `focused`（診断の計測つき。全長の run とは別の実行） |
| 今回の全長の出力の MP4 が、上の全長の run 4 回の出力と同じ byte 列 | file 全体 | 今回 |

今回の全長の run と画素の authority をつなぐのは、最後の行である。

限界:

1. **今回の全長の run そのものの RGBA は比べていない。** 過去の run（RGBA を digest で比べたもの）と
   出力の MP4 が同じ、という間接の確認である。H.264 は非可逆なので、原理の上では、違う RGBA から同じ
   MP4 ができることを排除しない。同じ入力から同じ MP4 ができることは、5 本の一致で確かめている。
2. 全長の製品の経路の baseline は、今も 12 区間（570 frame ずつ）に分けて採ったものである。「区間の途中
   から書き出した frame は、別の位置から書き出した同じ frame と同じ」という前提は、P3-5A が 120 frame で
   確かめた範囲に限られる。
3. 全長の比較は 64 bit の digest である。SHA-256 と全 byte の比較は、上の表の frame 数に限られる。
4. **選択 off の全長の書き出しを、1 process で最後まで行った記録は無い。** 新しく 600 秒の書き出しは
   足していない（上の 1〜3 を埋めるものではないため）。

## 6. 失敗のときの安全と fallback（Task F）

どれも製品の `MvmController` が要求を作り、`exportTimeline` が書き出す
（`20261014-055842-focused/`。不成立 0）。

| 場合 | 結果 |
| --- | --- |
| `Auto` + ELIGIBLE（observer なし、要求を書き換えない） | 数式 group の graph、合成 145 回。MP4 は選択 off と同じ |
| `Auto` + UNKNOWN（V1 を拡大。同上） | 従来の graph（`base-clip-transformed`）。MP4 は選択 off と同じ |
| `Auto` + INELIGIBLE（V1 に alpha 0。同上） | 従来の graph（`base-alpha-zero`）。MP4 は選択 off と同じ |
| alpha の証拠が無い・範囲外（observer あり） | UNKNOWN → 従来の graph。全 frame 一致 |
| alpha 0 の V1 に「不透明」という誤った証拠（observer あり） | 判定は ELIGIBLE になるが、合成器が実行時に拒否し、**書き出しが失敗**。出力なし |
| **画像の上限の超過**（100000 byte。上限が無ければ ELIGIBLE・UNKNOWN・INELIGIBLE の 3 通り。observer あり） | 準備を取りやめ、従来の graph で完走。全 120 frame 一致。**MP4 は選択 off と同じ** |
| 実行時の下地の拒否（故障の注入） | **書き出しが失敗**（従来の graph でやり直さない）。最終 file・一時 file なし |
| 取消 | 取消で終わる。最終 file・一時 file なし |
| consumer の待ちの timeout（300 ms） | timeout で終わる。最終 file・一時 file なし |
| 同じ process で 3 回続けて | 全 frame 一致。残り 0 |

- P3-6A の場合（candidate 22、MP4 2 組、主の書き出し 5）を、今回の binary でそのまま回し直した。全部が
  期待どおりで、一致した frame は 2,160。
- 新しい場合は、上限の超過 3、要求を書き換えない書き出し 6（`Auto` と選択 off を 3 組）、MP4 の比較 6 組
  （選択 off 対 `Auto`、選択 off 対 上限を超えた `Auto`）。
- どの場合も、書き出しの後の transition・callback・準備済みの画像は 0。
- 「observer あり」は、製品の選択に診断用の observer（証拠の差し替え・故障の注入・byte 単位の上限）を
  組み合わせた形で、製品の要求そのものではない。製品の上限（64 MB）そのものの超過は、observer なしで
  CTest の `math_export_focused` が確かめる（1）。
- 取消と timeout の場合は、harness の主の書き出し（observer と RGBA の計測つき）である。
- UNKNOWN の fixture（V1 を 110% に拡大）の MP4 は、ELIGIBLE の fixture の MP4 と同じ SHA-256 だった。
  この背景の画像では、拡大しても出力が変わらないためである（判定は配置で決まり、UNKNOWN になる）。

**実行時の失敗の扱いは P3-6A のままである。** ELIGIBLE と判定した後に数式 group の合成が失敗すると、
書き出しは失敗する。従来の graph での自動のやり直しは入れていない（P3-6C）。

## 7. 回帰（Task G）

| 試験 | 結果 |
| --- | --- |
| 製品の入口の統合（`math-group-product-p36b-verify.ps1 -Stage focused`） | 不成立 0（6） |
| 数式の書き出し（`math_export_focused`、P3-6A.1 の回帰を含む） | 107 検査中 0 件失敗 |
| 画像・MLT・timeout・判定の試験 | 全 CTest に含まれる（下） |
| **release の全 CTest**（`pwsh scripts/test.ps1 -Preset ucrt64-release`） | **1523/1523 通過**（2 回目。1,136 秒。`ctest-run-2/`）。GUI の環境は前後とも VALID |
| lint | 通過 |

全 CTest の経過（前の結果は上書きせず、全部残してある）:

| 回 | 結果 | 落ちた試験 | 調べたこと |
| --- | --- | --- | --- |
| 1（`ctest-run-1/`） | 1522/1523。環境は前後とも VALID | `math_controller_focused`（40.3 秒。通常は 16〜17 秒） | 「収まらない: disk の変形は ready」ほか 3 件（468 検査中 4 件）。数式の変形の準備が ready になるのを 10 秒待つ箇所で、時間切れになった。残りの 3 件はその続きである。**単独では 20 回中 20 回通過**（`ctest-run-1-followup/`） |
| 2（`ctest-run-2/`） | **1523/1523**。環境は前後とも VALID | なし | — |

- 1 回目に落ちた経路（数式の変形の成果物の準備と preview の配置）は、今回の変更（書き出しの画像の準備、
  試験の harness）が触れていない。**原因は切り分けていない（UNKNOWN）。** 全 CTest の並列の負荷の下で
  待ちが 10 秒を超えた、というのが観測であり、それ以上は分かっていない。検査は弱めていない。sleep も
  伸ばしていない。
- 全 CTest は 1 回ずつ実行した（重ねていない）。実行の前に `ctest.exe` が動いていないことを確かめている。
- debug の全 CTest は回していない。
- 検出器（TSan・ASan）つきの harness は実行し直していない。今回は transition・背景・合成の module を
  変えていない。

静かな環境が要る試験（これまでに全 CTest の中で 1 回ずつ落ち、単独では通ったもの）:

| 試験 | 要るもの | 観測 |
| --- | --- | --- |
| window の描画が要る試験（preview・GUI） | monitor が点灯している、lock されていない（`scripts/gui-environment-preflight.ps1` が確かめる） | P3-5A.2 |
| `math_equation_sequence_product_ui` | 利用者が PC を操作していない（試験が自分で「操作と干渉した」と判定して失敗にする） | P3-6A の 2 回目 |
| `graph_render_artifact`、`math_equation_sequence_preview_controller`、`math_raster_cache_focused`、`math_controller_focused` | 外からの負荷が小さい（時間で待つ箇所がある） | P3-5A.2、P3-6A の 1 回目、今回の 1 回目 |

全長の書き出しも、他の重い処理と同時に実行していない（時間の基準があるため）。

## 8. 製品の既定にできるか

この phase では判断しない。既定は off のままである。判断の材料として、分かっていることを並べる。

- 製品の入口から `Auto` で v4 全体を書き出すと、291.0 秒で終わり、出力は過去の診断用の出力と同じである。
- 診断用の全長の run（P3-4・P3-5A）は 272〜294 秒だった。製品の入口でも同じ範囲である。
- **選択 off（今の既定）では、この Project は製品の timeout の中で書き出せない可能性が高い。** P3-6A で
  誤って実行した選択 off の全長は、611.7 秒で timeout した。この run は手順として不成立の扱いのままで、
  検収の証拠ではないが、P3-4 が baseline を 12 区間に分けた理由（1 process では 600 秒で終わらない）と
  合っている。

残っている事項:

1. 実行時の失敗のときに従来の graph でやり直すかどうか（P3-6C）。今は書き出しの失敗である。
2. `Auto` は、使わない画像の準備の時間と memory（上限まで）を、UNKNOWN・INELIGIBLE の Project にも
   払わせる（1）。
3. 準備済みの画像そのものを作れない場合は、`Auto` でも失敗のままである（試験で再現していない）。
4. V1 に動画がある Project は試していない。
5. 全長の run は v4 の 1 つの Project、1 台の PC、1 回である（診断用を含めると 5 回）。
6. `mvm_mlt_export.c` そのものは、race の検出器の下で動かしていない。残りが 0 であることは、race が
   無いことの証明ではない。
7. `mvm.exe` の UI からの書き出しは通していない。on にする入口は、controller の C++ の関数だけである。
8. 全 CTest は、負荷や利用者の操作で 1 件落ちることがある（7）。`math_equation_sequence_product_ui` の
   SegFault は UNKNOWN のまま。

## 9. これまでの証拠の扱い

- P3-4・P3-5A・P3-6A の証拠の directory と文書の判定は変えていない。
- **P3-6A で誤って実行した全長の run**（`build/math-group-product-p36a-20261014/20261014-022305-verify-run-1-script-error/`）
  は、そのまま残してある。扱いも変えていない: script の誤りによる、指示に反した実行で、検収ではない
  （全 frame の比較をしていない。observer は付いていなかったが、harness が encoder 直前の RGBA の検査を
  付けていたので、計測なしの製品の実行でもない）。記録されている値（選択 off 611.7 秒で timeout、選択 on 305.4 秒・297.3 秒、MP4 の SHA-256
  `ff8eb4d5d80b3021…`）は、今回の正式の run の結果と矛盾しない。
- P3-6A の文書の「上限を超えた場合も書き出しは失敗する」には、P3-6A.1 で変えたことを追記した（元の文は
  残してある）。
- 今回の途中の記録も残してある: `p36a1-before-fix/`（修正の前の失敗）、`trial/`・`trial-script/`
  （harness と script の試走）、`ctest-run-1/`（1 件落ちた全 CTest）。

## 再現

```powershell
pwsh scripts/math-group-product-p36b-verify.ps1 -Stage freeze   # 対象を固定する (1 回だけ)
pwsh scripts/math-group-product-p36b-verify.ps1 -Stage focused  # 製品の入口の選択と fallback
pwsh scripts/math-group-product-p36b-verify.ps1 -Stage full     # 全長の書き出しと出力の検査
```

`focused` は約 35 分、`full` は約 6 分かかる。どちらも build をせず、source を書き換えない。
