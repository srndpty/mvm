# P3-6C.1: 数式の高速化（数式 group）を、書き出し設定から選べるようにする

2026-10-14。これまで controller の C++ の関数からしか on にできなかった数式 group の書き出しを、製品の
書き出し設定の dialog から、利用者が選べるようにした。**既定は off** で、選んだ書き出し 1 回だけに効く。

数式 group の合成の式、encoder 直前の画素の authority、MLT の affine、V1 の背景の保護、書き出しの
timeout は変えていない。実行時の失敗のときの自動のやり直しは入れていない。既定を on にする移行も
入れていない。commit・push はしていない。v4 全体（6840 frame）の書き出しは、この phase では行っていない。

証拠は `build/math-group-product-p36c1-20261014/`。

## 方針（この phase で採ったもの）

| 状況 | 動き |
| --- | --- |
| 既定 | off（従来の graph。判定もしない） |
| 利用者が on にした | 書き出しの前に判定する（`MathGroupOptimization::Auto`） |
| 判定が ELIGIBLE | 数式 group の graph |
| 判定が UNKNOWN・INELIGIBLE | 従来の graph |
| 準備する画像が上限（64 MB）を超えた | 従来の graph |
| 数式 group の合成が実行時に失敗した | 書き出しを止める。自動ではやり直さない |

## 判定

| 項目 | 結果 |
| --- | --- |
| 利用者が製品の UI から選べる | 成立。書き出し設定の checkbox「数式の書き出しを高速化（試験的）」 |
| 既定は off | 成立。dialog を開くたびに off。別の Project・起動し直しの後も off |
| 選択が、検証済みの書き出しの道へ届く | 成立。off → `Off`、on → `Auto`。controller が作る要求に observer は付かない |
| fallback と失敗の報告が正確 | 成立。「使った」「対象外で従来の方法」「上限の超過で従来の方法」「高速化の描画の失敗」を分ける |
| 対照 | 成立。製品を 1 か所ずつ壊した 7 通りが、意図した検査で落ちる |
| 画素の一致（代表の負荷） | 成立。UI からの書き出しは、on と off の出力が同じ byte 列（3 種の Project）。製品の入口の統合試験は、encoder 直前の RGBA が 2,520 frame 一致、MP4 が 8 組一致 |
| 全 CTest・lint | **PASS: 1524/1524**（1 回目。実行の前後とも GUI の環境は VALID）。lint 通過 |

**全体: PASS。** 既定は off のまま。次は P3-6C.2（`mvm.exe` を人が操作する検収）。**

確かめていないこと: **`mvm.exe` そのものを人が操作する書き出しは、まだ通していない**（P3-6C.2）。今回の
GUI の試験は、製品の `Main.qml` と実 controller を、試験の実行 file の中で動かしたものである。

## 1. UI（Task A）

書き出しの流れは、これまでと同じである: 「メディアを書き出し」→ 書き出し先の指定 → **書き出し設定** →
進行の dialog。足したのは、書き出し設定の中の 2 つの部品だけである（`apps/mvm/Main.qml`）。

- checkbox「**数式の書き出しを高速化（試験的）**」。既定は off。
- 説明の文: 「この書き出しだけに使います。対象外のプロジェクトは従来の方法で書き出します（速くなり
  ません）。高速化した描画で予期しない失敗が起きたときは、書き出しを止めます（自動ではやり直しません）。」

決めたこと:

- **出力する数式 clip が 1 本も無い Project では、見せない。** 見えていない checkbox は効かない（要求は
  `Off`）。数式を使わない利用者の画面は変わらない。
- 「必ず速くなる」「どの Project にも効く」とは書かない。名前に「試験的」を付け、対象外では速くならない
  ことを説明に書いた。
- 品質の選択など、他の部品は変えていない。dialog の作り直しもしていない。試験が部品を見つけるための
  名前（`objectName`）を 3 つ足した（checkbox・進行の dialog のキャンセルボタン・失敗の dialog）。

実際に描画した画面は `screens/`（設定の既定、on にした状態、高速化の描画が失敗したときの dialog）。

## 2. 選択の寿命（Task B）

**書き出しごと**にした。dialog を開くたびに off に戻り、どこにも保存しない。

| 候補 | 採否 | 理由 |
| --- | --- | --- |
| 書き出しごと（開くたびに off） | **採用** | 同じ dialog の品質の選択が、既にこの形である（開くたびに「標準」へ戻す）。試験的な機能を、気付かないうちに使い続けることが無い。実行時の失敗の後、開き直すだけで off の書き出しになる |
| アプリの設定（QML の `Settings`、既定 off） | 不採用 | アプリには `Settings` の仕組みがあり（panel の幅、文字起こしの model など）、足すのは容易である。しかし、一度 on にすると、以降の全部の書き出しが黙って試験的な道を通る |
| Project の file に保存 | 不採用 | 指示のとおり。Project の形式は変えていない |

結果として決まる動き:

- 別の Project を開いた後: off。
- アプリを起動し直した後: off。
- 書き出しの後に設定を開き直したとき: off（直前に on で書き出していても）。
- 設定の file（`QSettings`）に、この選択は書かれない（試験で確認）。

## 3. controller への接続（Task C）

書き出しの実装は足していない。P3-6A・P3-6B で確かめた道（controller が要求を作り、
`mvm::app::exportTimeline` が書き出す）を、そのまま使う。

- QML は `exportTimelineWithOptions(書き出し先, 品質, 高速化)` を呼ぶ（新しい入口）。3 つ目の引数が、
  その書き出しの選択である。
- controller は、worker を始める前に、要求の `mathGroupOptimization` を `Auto` か `Off` に決める。
  要求は値で worker へ渡るので、**書き出しの最中に UI を変えても、その書き出しは変わらない。**
- 選択は controller に残らない。QML から設定できる状態は無い。
- 以前からの内部の選択（`setMathGroupExportOptimization`。検証 harness が使う）は残したが、QML からは
  呼べず、UI からの書き出しには効かない（試験で確認）。
- 診断用の observer、故障の注入、以前の timeout の数え方は、QML から届かない。controller の公開して
  いる method と property の名前を全部調べる検査を足した。
- 進行・取消・結果・最終 file の公開は、変えていない。

## 4. 利用者への報告（Task D）

書き出しの結果の記録（`MathGroupExportReport`）から、1 つの値に分ける（別の出どころは作っていない）。
QML からは、読むだけの `exportMathGroupOutcome` で見える。

| 結果 | 条件（記録から） | 利用者に見える文 |
| --- | --- | --- |
| `off` | 選んでいない | 足さない |
| `used` | 数式 group の graph を選び、成功した | 状態の表示に「数式の高速化を使いました（数式 clip N 本）」 |
| `conventional` | 判定が UNKNOWN・INELIGIBLE で、成功した | 「数式の高速化は使わず、従来の方法で書き出しました（この Project は対象外: 理由）」 |
| `budget` | 画像の準備を取りやめて、成功した | 「数式の高速化は使わず、従来の方法で書き出しました（準備する画像が上限を超えたため）」 |
| `failed` | 数式 group の graph で、実行時の失敗があった | 失敗の dialog に、**元の error をそのまま**出し、続けて「数式の高速化（試験的）の描画で失敗したため、書き出しを止めました。書き出し設定で「数式の書き出しを高速化（試験的）」を外すと、従来の方法で書き出せます。」 |
| `incomplete` | 選んだが、取消・timeout・その他の失敗で終わった | これまでどおりの文だけ（高速化の失敗とは書かない） |

- 従来の graph での書き出しを「高速化を使った」と報告しない（対照で確認）。
- 実行時の失敗の後、自動ではやり直さない（書き出しは 1 回だけ。試験で確認）。利用者が設定を開き直すと
  off になっているので、そのまま「書き出す」で従来の方法になる。
- 成功のときの文は、画面の上の状態の表示（1 行）に出る。専用の完了の dialog は足していない。
- 「対象外」の理由は、判定の理由の名前（`base-clip-transformed` など）をそのまま出している。利用者向けの
  言い換えは、まだしていない。

## 5. 試験（Task E）

### 5.1 製品の UI からの書き出し（CTest `math_group_export_product_ui`、64 検査）

製品の `Main.qml`、実 controller、実 window、製品の書き出しの関数を使う。数式の描画だけ偽の backend で
ある。書き出し先の指定・checkbox・「書き出す」・「キャンセル」は、製品の QML の部品を操作する。
checkbox の状態ではなく、**controller が作った要求、選ばれた graph、結果の記録、出力の byte 列**を見る。

| 場合 | 確かめたこと |
| --- | --- |
| 既定 | checkbox は off。説明がある。要求は `Off`（判定しない、observer なし、timeout 600000 ms） |
| 明示の on、ELIGIBLE | 要求は `Auto`。判定 ELIGIBLE、数式 group の graph で実際に合成、affine 0。「使いました」と報告。**出力は off と同じ byte 列** |
| 設定を開き直す | off に戻っている |
| くり返し | もう一度 on で書き出しても同じ出力。画像の残り 0 |
| 書き出しの最中に checkbox を変える | off で始めた書き出しは `Off` のまま、on で始めた書き出しは `Auto` のまま |
| 内部の選択が on | UI で選ばなければ `Off` |
| 画像の上限の超過 | 準備を取りやめ、従来の graph で成功。「上限を超えた」と報告。出力は off と同じ |
| 実行時の合成器の失敗 | 書き出しは 1 回で止まる。失敗の dialog に元の error と案内。最終 file・一時 file なし。開き直すと off で、そのまま書き出せる（出力は同じ） |
| 取消（製品のキャンセルボタン） | 取消で終わる。file なし |
| consumer の待ちの timeout | timeout として報告（高速化の失敗にしない）。file なし |
| 別の Project（V1 に alpha 0 = INELIGIBLE、V1 を拡大 = UNKNOWN） | 開いた直後は off。on にすると判定のとおり従来の graph。「従来の方法で書き出しました」と報告。出力は off と同じ |
| 数式の無い Project | 選択を見せない。要求は `Off` |
| 起動し直し（controller と QML を作り直す） | off。前の選択も結果も残っていない。設定の file に書かれていない |

上限の超過・実行時の失敗・timeout は、試験の runner が、controller の作った要求の写しに診断用の入力を
足して起こしている（QML からは届かない入力）。それ以外の書き出しは、要求をそのまま渡している。

### 5.2 対照（製品を壊して、意図した検査が落ちるか）

`mutations/summary.json`。7 通りとも、build は通り、試験は意図した検査で落ちた。終わった後、source が
元と 1 byte も違わないことを確かめ、build し直した。

| 壊し方 | 落ちた検査 |
| --- | --- |
| UI が checkbox を無視して常に off を渡す | 「B: on の要求は Auto」ほか |
| 設定を開いたときに off へ戻さない | 「C: 開き直すと off に戻っている」ほか |
| 見えていない checkbox も効く | 「J: 数式の無い Project の要求は Off」 |
| controller が内部の選択も使う | 「E: 内部の選択が on でも、UI で選ばなければ Off」 |
| 従来の graph を「使った」と報告する | 「J: 高速化を使ったと報告しない」（2 件） |
| 上限の超過を分けて報告しない | 「F: 高速化を使ったと報告しない」 |
| 失敗の文から元の error を落とす | 「G: 元の error を残し…」ほか |

### 5.3 controller の単体試験（CTest `m7b_4_controller_export_lifecycle` に追加）

偽の書き出しの関数で、結果の分け方の全部（`off`・`used`・`conventional`・`budget`・`failed`・
`incomplete` の取消と timeout）、選択が次の書き出しへ残らないこと、QML から届く名前に診断用の入力が
無いことを確かめる。

### 5.4 代表の負荷での画素の一致

最終の binary で、P3-6B の製品の入口の統合試験をそのまま回し直した
（`pwsh scripts/math-group-product-p36b-verify.ps1 -Stage focused`、`20261014-073840-focused/`。不成立 0）。
検証 harness は、同じ controller の source を link している。

- P3-6A の場合: candidate 22/22、MP4 2/2 組、主の書き出し 5/5 が期待どおり。encoder 直前の RGBA が
  従来の経路と一致した frame は 2,160（v4 の全体 graph の区間 1200・5280 を含む）。
- 画像の上限の超過 3 通り: 全 120 frame が一致（計 360 frame）。
- 要求を書き換えない書き出し 6 通り（`Auto` と選択 off を、ELIGIBLE・UNKNOWN・INELIGIBLE で）: 期待どおり。
- MP4 の SHA-256: 選択 off 対 `Auto`、選択 off 対 上限を超えた `Auto` の 6 組が同じ。

UI からの書き出し（5.1）の一致は MP4 の byte 列で、小さい fixture（640x360、60 frame、数式 2〜3 本）で
ある。encoder 直前の RGBA を比べたのは、この統合試験の側である。統合試験は、UI ではなく、controller の
内部の選択（`setMathGroupExportOptimization`）から on にしている。UI の選択が同じ要求になることは、5.1 が
確かめる。

## 6. 回帰（Task F）

| 試験 | 結果 |
| --- | --- |
| 製品の UI からの書き出し（5.1） | 64 検査中 0 件失敗 |
| 対照（5.2） | 7/7 が意図した検査で検出 |
| 製品の入口の統合（5.4） | 不成立 0（36 通りが期待どおり、MP4 8 組が一致） |
| **release の全 CTest**（`pwsh scripts/test.ps1 -Preset ucrt64-release`） | **1524/1524 通過**（1,177 秒。`ctest-run-1/`）。GUI の環境は前後とも VALID。登録数は、新しい試験 1 件（`math_group_export_product_ui`）が加わって 1524 |
| lint | 通過 |

経過（残してあるもの）:

- 全 CTest は 1 回目で通った。落ちた試験は無い。
- 統合試験の 1 回目は、**始まる前に止まった**（freeze との照合が不一致）。変わった file は無く、原因は
  script にあった: HEAD からの差分の hash を、`git diff` の出力を PowerShell が文字列として読んでから
  計算していたので、起動の仕方（console の文字 code）で値が変わった。git に file へ書かせて byte 列を
  hash するように直し（`scripts/math-group-product-p36b-verify.ps1`）、freeze を取り直した。使えなかった
  freeze は `freeze-invalid-console-encoding.json` として残してある。製品の code と binary は変えていない
  （この修正の後に lint をやり直し、通過）。
- 新しい GUI の試験は、window の描画と、利用者の操作から切り離されていることが要る（他の製品 UI の
  試験と同じ。切り離せなければ PROTOCOL_INVALID で失敗にする）。

- 全 CTest は 1 回ずつ実行した（重ねていない）。
- debug の全 CTest は回していない。検出器（TSan・ASan）つきの harness も実行し直していない（MLT の
  層と合成の module を変えていない）。

対象の記録: `freeze.json`（909 file の SHA-256。git の HEAD `3bde2f8`、HEAD からの差分 `DD76EFD9FA9F89EC…`）。検証 harness `0EB4DC5844F5AB2E…`、`libmlt-7.dll` `13D5EFA4536D3225…`、`libx264-165.dll` `ACF3D8F926F42ADA…`、`Main.qml` `D2824933DB65FC6B…`、`mvm_controller_export.cpp` `AE600395F182A680…`。統合試験の前後で一致した。`mvm.exe` と `mvm_test_text_ui_input.exe` は、`scripts/test.ps1` が実行のたびに link し直すので含めていない（全 CTest の前後の値は `ctest-run-1/identity-*.json`）。

## 7. 残っていること

1. **`mvm.exe` を人が操作する検収（P3-6C.2）。** 画面の読みやすさ、実際の Project での体感、書き出しの
   後の状態の表示が目に入るか、を含む。
2. 成功のときの報告は、状態の表示の 1 行だけである。長い path の後ろに付くので、見落とすおそれがある。
3. 「対象外」の理由は、内部の名前のままである。
4. 実行時の失敗は、書き出しの失敗のままである（方針どおり。自動のやり直しは入れていない）。
5. on にすると、対象外の Project でも、使わない画像の準備の時間と memory（上限まで）がかかる（P3-6B）。
6. V1 に動画がある Project は試していない（P3-6B から変わらない）。
7. 既定は off のまま。既定を on にする判断は、この phase の対象ではない。
8. 気付いたこと（今回の変更ではない）: 数式 clip だけの Project の書き出し設定に「入力映像: 仕様を取得
   できません」「1件の入力ファイルは仕様を確認できませんでした。」と出る（`screens/`）。数式 clip を
   素材の file として調べているためと見えるが、調べていない。
