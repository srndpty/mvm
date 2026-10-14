# P3-7B: 書き出し設定の「入力映像」の要約を、生成する clip で誤らない

2026-10-14。数式だけの Project で書き出し設定を開くと、次の 2 つが出ていた。

- 「入力映像: 仕様を取得できません」
- 「1件の入力ファイルは仕様を確認できませんでした。」

書き出しは止めないが、読めない素材があるように見える。原因を特定し、書き出し設定の要約
（`MvmController::exportSettingsSummary`）だけを直した。

変えていないもの: 書き出しの graph・Project の file 形式・出力の仕様（解像度・fps）の決め方・
数式 group（合成・alpha の判定・MLT の affine・V1 の下地の保護）・timeout・encoder・画素の基準。
数式 group の既定（off）も変えていない。commit・push はしていない。

証拠は `build/export-summary-p37b-20261014/`。

## 判定

| # | 項目 | 結果 |
| --- | --- | --- |
| 1 | 原因と、影響する clip の種類 | 要約が、音声以外の clip を全部「動画の素材」として probe していた。文字・数式・数式 sequence・グラフ（file が無い）と静止画（動画ではないと拒否される）が、必ず失敗に数えられた |
| 2 | 元の警告の再現 | 製品の controller と Main.qml で再現した（4 の対照の実行に記録） |
| 3 | 入力と出力の仕様の意味 | 下の 3 に定めた。出力は Project の profile のまま |
| 4 | 対照 | 要約だけを修正の前に戻すと、11 の場合で 25 件の検査が落ちる |
| 5 | 実際の `mvm.exe` の GUI | 自動の smoke で A 25/25、B 10/10。B の入力映像は「なし（数式 16件を出力の仕様で描きます）」 |
| 6 | 全 CTest | **PASS: 1525/1525**（2 回目。前後とも GUI の環境は VALID）。1 回目は 1524/1525（関係の無い 1 件。下の 6）。lint 通過 |

## 1. 原因

### 修正の前の実装

`exportSettingsSummary` は、timeline の clip のうち音声以外の全部を「入力映像」に数え、それぞれの
`mediaPath` を `probeMedia`（`probeMediaFile` の結果から、動画の素材の仕様を取り出す）で調べていた。

- 文字・数式・数式 sequence・グラフの clip は `mediaPath` を持たない（`clipKindHasMediaPath` が false。
  Project のデータから描く）。空の path の probe は必ず失敗する。
- 静止画の clip は file を持つが、`probeMedia` は「静止画は動画として追加できません」で失敗にする
  （動画の clip を足すときの検査を、そのまま使っていた）。
- 無効にした clip・非表示の video track の clip も数えていた（書き出しには出ない）。

その結果、動画の素材が 1 つも無い Project では、仕様が 1 つも取れず「入力映像: 仕様を取得できません」、
失敗の数だけ「N件の入力ファイルは仕様を確認できませんでした。」が出た。

### 影響する clip の種類（監査）

| clip の種類 | 映像の出どころ | 修正の前 | 修正の後 |
| --- | --- | --- | --- |
| 動画（Video） | 素材の動画 file | probe（正しい） | probe（同じ） |
| Manim | 生成済みの動画 file（`mediaPath` は Manim の素材の `generatedVideoPath`） | probe | probe（同じ。書き出しはこの file を読む） |
| 静止画（Image） | 素材の画像 file | probe が必ず失敗 | 画像として読めるかだけを確かめる。動画の仕様（fps）は作らない |
| 音声（Audio） | 素材の音声 file | 数えない | 数えない（同じ） |
| 文字（Text） | Project のデータから描く | 空の path を probe して失敗 | probe しない |
| 数式（Math） | 同上 | 同上 | probe しない |
| 数式 sequence（EquationSequence） | 同上（描画の成果物は書き出しの準備で作る） | 同上 | probe しない |
| グラフ（Graph） | 同上 | 同上 | probe しない |

書き出しに出ない clip（無効にした clip、非表示の video track の clip）は、書き出しの区間から外れる
（`timeline_export.cpp` の `dropSilenced`・`enabled`）。要約も同じ判定（`clip.enabled`、
`project::isTrackOutputEnabled`）で数えないようにした。数式の高速化の選択を見せる条件
（`mathClipCount`）も、以前から同じ判定である。

この表示は数式 group の最適化より前からある（P3-6C.1 の記録にも出ている）。文字だけ・静止画だけの
Project でも同じ誤りが出ていた。

## 2. 修正の前の再現

`build/export-summary-p37b-20261014/negative-control/prefix-run.txt`。製品の controller と Main.qml
（書き出し設定は、製品の FileDialog の accepted から開く）で、修正の前の要約は次を返していた。

| 場合 | 入力映像 | 警告 |
| --- | --- | --- |
| 数式だけ | 仕様を取得できません | 1件の入力ファイルは仕様を確認できませんでした。 |
| 文字だけ | 仕様を取得できません | 同上 |
| 静止画だけ | 仕様を取得できません | 同上 |
| 動画と数式・文字 | 64×48 / 10 fps | （拡大などの警告に加えて）1件の入力ファイルは…… |
| 数式を無効にした Project | 仕様を取得できません | 1件の…（「映像は黒になります」が出ない） |

## 3. 入力と出力の仕様の意味

- **入力映像**: 書き出しが読む**動画 file** の仕様（解像度・fps・SAR）。動画・Manim の clip だけが持つ。
  拡大・fps の上げの警告は、この仕様と出力を比べたもの。
- **出力**: Project の profile（`outputWidth`・`outputHeight`・`timelineFpsNum/Den`）。書き出しの要求と
  同じ値で、clip の種類では変わらない（変えていない）。
- 静止画・文字・数式などは、出力の仕様の画面に描く。入力の codec・fps・解像度を作って見せない。
- 「N件の入力ファイルは仕様を確認できませんでした。」は、**書き出しに出る** 動画・静止画の file が、
  無い・壊れている・種類が違う場合だけ。

表示の文（`exportSettingsSummary` の `inputText`）:

| Project | 入力映像 | 警告 |
| --- | --- | --- |
| 動画の file がある | 「入力映像: W×H / F fps」（複数なら「入力映像（複数仕様）: …」。以前と同じ） | 以前と同じ |
| 動画の file があるが、どれも読めない | 「入力映像: 仕様を取得できません」 | 「N件の入力ファイルは…」 |
| 動画の file が無く、静止画・生成する clip がある | 「入力映像: なし（数式 1件を出力の仕様で描きます）」（種類ごとの件数を「・」で並べる） | 読めない静止画があれば「N件の入力ファイルは…」 |
| 映像に出る clip が無い | 「入力映像: なし」 | 「映像クリップがないため、映像は黒になります。」 |

分類は controller だけが持つ。QML は `exportSettingsSummary` の文を表示するだけで、変えたのは試験の
ための `objectName`（`exportInputSpecLabel`・`exportOutputSpecLabel`・`exportComparisonWarningLabel`）だけ。

## 4. 試験と対照

### 製品の QML の試験（CTest `export_settings_summary_product_ui`、新規）

`mvm_test_text_ui_input --export-summary-ui <smoke の素材>`。実 controller・実 window・実際の素材の
probe。素材は製品の操作（`addMediaFileToTimeline`）で置き、場合ごとの写しを消す・壊す。書き出し
設定は製品の FileDialog の accepted から開き、controller の要約と、dialog に実際に出た文（入力・出力・
警告）が一致することも確かめる。書き出しはしない。16 の場合、170 件の検査。

| 場合 | 入力映像 | 確かめること |
| --- | --- | --- |
| 数式だけ・文字だけ・数式と文字 | なし（…を出力の仕様で描きます） | 仕様を確認できない入力の警告・黒の警告が無い |
| 静止画だけ | なし（静止画 1件を…） | 同上 |
| 動画だけ | 64×48 / 10 fps | 失敗の警告が無い |
| 動画と数式・文字 | 64×48 / 10 fps | 生成する clip を失敗に数えない |
| 動画の file が無い・壊れている | 仕様を取得できません | 「1件の入力ファイルは…」が出る |
| 数式と壊れた動画 | 仕様を取得できません | 「1件」（数式を数えない） |
| 数式と、無い静止画 | なし（静止画 1件・数式 1件を…） | 「1件の入力ファイルは…」が出る |
| 読める動画と壊れた動画 | 64×48 / 10 fps | 「1件の入力ファイルは…」が出る |
| 壊れた動画の clip を無効にした | 64×48 / 10 fps | 失敗の警告が無い（書き出しに出ない） |
| 数式の clip を無効にした | なし | 黒の警告が出る |
| 無い動画の track を非表示にした（数式あり） | なし（数式 1件を…） | 失敗の警告が無い（書き出しに出ない） |
| 空・音声だけ | なし | 黒の警告が出る |

出力の文は、全部の場合で「出力: 640×360 / 60 fps」（Project の profile）。

数式 sequence・グラフ・Manim の clip は、この試験の fixture に含めていない（分類は 1 の表のとおり
code で決まる）。

### 対照

`build/export-summary-p37b-20261014/negative-control/`。試験と QML はそのままで、
`apps/mvm/mvm_controller_export.cpp` だけを HEAD（修正の前）に戻して build し直すと、
**170 件中 25 件が落ちた**（11 の場合: 数式だけ・文字だけ・数式と文字・静止画だけ・動画と数式・
文字・数式と壊れた動画・数式と無い静止画・壊れた動画を無効・数式を無効・非表示の track）。

本当の素材の失敗の場合（動画が無い・壊れている、読める動画と壊れた動画）は、修正の前も後も通る。
修正は本当の失敗の警告を消していない。

修正に戻し、build し直してから通ることを確かめた（`fixed-run-after-restore.txt`）。

## 5. 実際の `mvm.exe` の GUI（自動）

`scripts/math-group-gui-smoke.ps1`（既定の短い場面 A・B。P3-6C.2 の UI Automation の driver。人の
mouse・keyboard の操作なし）を、直した binary で実行した（`build/math-group-gui-smoke/20261014-210137/`）。

| 場面 | session | verify | driver の段 |
| --- | --- | --- | --- |
| A（対象外の Project） | 0 | 0（全項目成立、この run の verify.json） | 25/25 |
| B（数式 16 本だけの Project） | 0 | 0（同） | 10/10 |

B1 の段は、以前は「入力映像: 仕様を取得できません」が**出ていること**を確かめていた（修正の前の
表示を記録するため）。P3-7B で、入力映像が「なし（数式 …件を出力の仕様で描きます）」で、「仕様を
取得できません」と「…件の入力ファイルは仕様を確認できませんでした。」が**無い**ことを確かめるように
変えた。実際の表示は「入力映像: なし（数式 16件を出力の仕様で描きます）」。

書き出しの経路は変えていないので、v4 全体（6840 frame）の場面 C は実行していない。

### smoke の判定を固くした（Task E）

- 場面ごとの成立には、session と verify の両方が 0 で終わること、verify の開始より後に書かれた
  verify.json があること、その全項目が成立すること、driver の段が 1 つ以上あって全部成功することを
  求める（`Get-ScenarioVerdict`）。以前は verify の終了コードだけで判定していた。
- session の directory も、その場面の開始より後に作られたものだけを使う。
- 毎回新しい出力先（`build/math-group-gui-smoke/<日時>/`）に書く。前の結果は消さない。
- 検収の道具の自己試験（`scripts/math-group-product-p36c2-selftest.ps1`）に、この判定の 9 件を足した
  （session の失敗、前の run の verify.json だけがある、古い verify.json、verify の失敗、verify.json が
  無い、不成立の項目、driver の段の失敗、driver の段が無い、と成立の場合）。自己試験は 68/68
  （`selftest/20261014-210525/`）。
- 1 回目の自己試験（`selftest/20261014-205956/`、32 件不成立）は無効である。実行中に、固定の対象である
  手順書を私が書き換えたため、freeze との照合が失敗した。記録は残してある。

## 6. 回帰

| 試験 | 結果 |
| --- | --- |
| `export_settings_summary_product_ui`（新規）・`m7b_4_controller_export_lifecycle`・`math_group_export_product_ui`・`reveal_in_explorer_negative` | 通過 |
| 製品 GUI の smoke（A・B） | 通過 |
| 検収の道具の自己試験 | 68/68 |
| lint | 通過（clang-format を当てた後） |
| **release の全 CTest（2 回目）** | **1525/1525 通過**（1,195 秒。`ctest-run-2/`）。GUI の環境は前後とも VALID。登録数は 1 増えた（新しい試験） |

全 CTest の 1 回目（`ctest-run-1/`、1,206 秒、前後とも VALID）は **1524/1525** で、
`math_transform_authoring_product_ui` が 1 件落ちた（「前の Write に重なる長さを、Write の手前 (20) で
止めません」。数式の変形の長さを timeline で drag する検査）。この修正が触れていない機能である。
その試験だけを 10 回くり返すと 10/10 通った（`authoring-repeat/repeat-10.log`）。2 回目の全体の実行では
通った。P3-6A・P3-6B で見た「全体の実行の負荷で 1 件落ちる」と同じ形と考えるが、原因は調べていない。

実行の前後で `mvm.exe` と `mvm_test_text_ui_input.exe` の SHA-256 が変わるのは、`scripts/test.ps1` が
毎回この 2 つを link し直すためである（source の状態 `git status` は前後で同じ）。

## 7. 残っていること（関係の無いもの）

- `math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま。
- 全 CTest は、負荷や利用者の操作で 1 件落ちることがある（P3-6A・P3-6B。今回は
  `math_transform_authoring_product_ui`）。
- 数式 sequence・グラフ・Manim の clip を含む Project の要約は、分類の code で決まるが、製品の QML の
  試験の fixture には含めていない。
- 製品 GUI の smoke を走らせると、書き出しのたびに Explorer の window が開く（製品の動き）。
