# P3-7B.1: 見えない desktop の製品 GUI の自動試験を、隔離・後始末・診断の面で固める

2026-10-15。製品 GUI の smoke（`scripts/math-group-gui-smoke.ps1`）は、P3-7B の後に見えない desktop で
動くようにした（利用者の画面の前面を奪わないため）。その実装には次の問題があった。

- 試験用の「Explorer を開かない」動きが、環境変数（`MVM_TEST_REVEAL_WITHOUT_EXPLORER`）だけで有効になった。
  普通の起動でも、その変数が環境にあれば、黙って Explorer の表示を成功として扱ってしまう。
- 時間切れのとき、runner は直接の子（pwsh）だけを止めた。mvm.exe・driver が残りうる。driver が先に
  終わると、session は mvm.exe の終了を限りなく待った。
- 5 回に 1 回、製品の書き出しは 36 秒で終わったのに、driver が 300 秒の間に結果を確かめられなかった
  （原因不明。記録は `build/math-group-gui-smoke/20261015-013512/`）。

書き出しの算法・数式 group の描画・画素の基準・encoder の設定・timeout の意味は変えていない。数式 group の
既定（off）も変えていない。commit・push はしていない。人の操作は使っていない。

## 判定

**PASS。** 見えない desktop の runner は利用者の画面から切り離され、失敗の後も自分の process を job ごと
止め（job の外は止めない）、製品の Shell の失敗を隠さず（試験用の動きは明示の引数と見えない desktop に限る）、
完了を確かめられない失敗の診断を残す。全 CTest は 2 回目で 1526/1526。

完了を確かめられない失敗の仕組みは、製品の側（状態の表示の上書き）まで絞ったが、成功の書き出しの直後に起きた
ところを controller の記録つきで捕まえてはいない（3 と 7）。

## 1. 試験用の Explorer の動きを狭くした（Task A）

| | 以前 | 今 |
| --- | --- | --- |
| 有効にする方法 | 環境変数 `MVM_TEST_REVEAL_WITHOUT_EXPLORER`（子 process へ黙って受け継がれる） | 明示の引数 `--test-reveal-without-explorer` だけ。環境変数は読まない（binary に名前が無いことを試験する） |
| 利用者の画面 | 有効になりえた | **起動を断る**（終了コード 2。window も Project lock も作る前） |
| 見えない desktop | 有効 | 有効。stderr に「試験: Explorerを開かずに…（desktop '…'）」を出し、session が記録する（`reveal_simulated`） |
| 動き | path の正規化と実際の `SHParseDisplayName` まで通し、`SHOpenFolderAndSelectItems` だけを成功として扱う | 同じ |

- **見えない desktop の smoke は、Explorer が実際に開くことを確かめない。** smoke.json の
  `explorer_reveal` にもそう書く。Explorer が開くことは、`-Visible` の実行（利用者が明示したとき）と、
  P3-7A の試験（`reveal_in_explorer_negative`。実際の `SHParseDisplayName`、開く処理は差し替え）の範囲。
- smoke の分類 `reveal-mode-mismatch`: 見えない desktop の run なのに、mvm.exe がこの動きを有効にした記録が
  無ければ成立にしない（実際の Explorer を呼んで GUI thread が止まる状態を、別の失敗に見せない）。
- CTest `mvm_reveal_simulation_gate`（新規）: 利用者の画面で引数を付けた mvm.exe が終了コード 2 で断り、理由を
  出し、Project lock を取らず、binary が以前の環境変数を参照しないこと。

## 2. process の木の所有と後始末（Task B）

`scripts/lib/hidden-desktop-job.ps1`（新規）。`scripts/lib/build-watchdog.ps1` と同じ規則の Job Object。

- 根の process（smoke の内側の pwsh）は一時停止で作り、job に入れてから動かす。session の pwsh・mvm.exe・
  driver・helper は全部この job に入る。
- 時間切れ（`-RunnerTimeoutSeconds`。既定 1200 秒、`-Full` で 3600 秒）は `TerminateJobObject` で job ごと止め、
  止めた process を `runner.json` の `at_timeout` に残す。終了コード 3。
- 根が終わった後に残った process は、`left_after_exit` に記録してから止める。終了コード 4。根の console の
  conhost が少し遅れて終わるので、最大 5 秒は自然に空になるのを待つ。
- job は `KILL_ON_JOB_CLOSE`。runner の process が異常終了しても（外から止められても）、OS が job を閉じて
  中の process が止まる。
- job の外の process（利用者の mvm.exe など）には触れない。session は以前から、他の mvm.exe が動いていれば
  起動しない（止めない）。

session の段（`scripts/math-group-product-p36c2-accept.ps1`）:

- driver が終わって 30 秒（`-DriverGraceSeconds`）たっても mvm.exe が終わらなければ、mvm.exe を子孫ごと止め、
  `ended_by = driver-exited` と driver の終了コードを残す。
- session の上限（`-SessionLimitSeconds`。smoke は場面 A 900 秒、B 600 秒、C 2400 秒）を超えたら、mvm.exe と
  driver を止め、`ended_by = session-limit` を残す。
- mvm.exe がすぐ終わった（Project lock を取れない・引数を断った）ときも、終了コードと理由を session.json に残す
  （以前は「実行 file を確かめられない」で session の段ごと失敗した）。
- Project lock は `FILE_FLAG_DELETE_ON_CLOSE` の排他の handle なので、process を止めれば OS が外す。持ち主の
  いない `.mvm.lock` の file は、次の起動を妨げない（試験で確かめた）。
- 参照の書き出しの段（reference）: harness が報告を出さずに終わったとき、StrictMode の property の error で
  段ごと落ちていた。harness の stderr の最後を残して、普通の失敗にした。

## 3. 書き出しの完了を確かめられない失敗の調べ（Task C）

### 記録するもの（新しい run から）

| 何を | どこに |
| --- | --- |
| 製品の書き出しの開始・結果と時刻（壁時計も） | session.json の `events`（`received_wall` を足した） |
| controller の状態の表示（statusText）の変化と時刻 | session.json の `status_events`。driver が操作する session では mvm.exe に `--test-log-status`（新規。動きは変えない）を付ける |
| 待つ間の観測: UI Automation の window の要素（RuntimeId・handle）、その process が mvm.exe か、driver と同じ desktop か、GUI thread の応答（`SendMessageTimeout`・`IsHungAppWindow`）、進行の dialog、状態の文、失敗の文、UI Automation の error | drive.json（session.json の `drive`）の `observations[].samples`（状態が変わったときと 10 秒ごと） |
| 時間切れのとき: 見えている文の全部、画面の外を含む全部の文の部品（名前・AutomationId・位置）、別の新しい UI Automation の client（別 process）から見た状態らしい文 | `observations[].last_visible_texts`・`text_inventory`・`fresh_client` |

待つ時間（300 秒）は変えていない。driver は待つ間に何も操作しない。

### 決まった形の負例

`-Fault unobservable-completion`: driver が結果の文を、決して一致しない形で探す。製品の書き出しは成功し、
分類は `completion-not-observed` になること、観測の記録に「GUI は応答し、進行の dialog は消え、状態の文は
書き出しの結果」が残ること、別の client も結果の文を読めることを、失敗の注入の試験で確かめる。

### 観測した事実（原因はまだ確定していない）

新しい記録の付いた run で、注入なしの再現が 2 回あった（失敗の注入の 1 回目の `stale-lock-file`、2 回目の
`normal`）。どちらも場面 B。

- 製品の書き出しは成功していた（結果の行は 21.0 秒・21.9 秒）。
- 結果の行の頃に進行の dialog は消え、その後 300 秒、GUI thread は応答し続けた（hung でない）。UI Automation の
  window の要素は mvm.exe のもので、driver と同じ desktop にあった。
- 状態の表示の位置（menu の下の 1 行目、幅 1420）の文の部品の名前は「素材を追加してください」だった。
  「書き出し」「数式の高速化」で始まる文の部品は、画面の外を含めて 1 つも無かった。
- **別の新しい UI Automation の client（別 process）からも、状態らしい文は見えなかった。** driver の client の
  状態の問題ではない。
- 1 回目の再現では、進行の dialog が消えた瞬間（20.8 秒）に `FindAll` が COM の error を 1 回返した。

製品の code で「素材を追加してください」を書くのは、`MvmController::pollPreviewState`
（`apps/mvm/mvm_controller.cpp`）だけである。preview が使える状態へ変わったとき、作業中でなく、素材の clip が
無ければ（数式だけの Project は当てはまる）、状態の表示をこの文で上書きする。この上書きが書き出しの結果の
後に起きれば、結果の文は消える。

controller の状態の記録（`--test-log-status`）を足した後に、場面 B を 8 回、場面 A・B を 5 回動かした
（`build/math-group-gui-smoke-repro/`）。「完了を確かめられない」は再現しなかった。controller の記録から:

- 場面 B の 13 回は、どれも「Previewを初期化しています」→「固定する等号 を表示しています」→「書き出しています…」
  → 書き出しの結果、で終わり、「素材を追加してください」にならなかった。
- 場面 A の 5 回のうち 3 回で「素材を追加してください」が出た。2 回は書き出しの前（4.8 秒・15.4 秒）。**1 回
  （`20261015-041153`）は、書き出しの結果（失敗の文、104.79 秒）の 0.18 秒後（104.97 秒）に、状態の表示を
  「素材を追加してください」で上書きした。** この回は、driver が失敗の dialog で結果を先に確かめていたので
  成立した。

分かったこと: 製品は、書き出しが終わった直後に、書き出しの結果の文を「素材を追加してください」で上書きする
ことがある（controller の記録で確認）。driver が結果の文を読む前（driver は 0.5 秒ごとに見る）にこれが起きれば、
結果の文は見えなくなる。再現した 2 回の観測（状態の表示の位置の文が「素材を追加してください」、GUI は応答、
別の client からも結果の文が見えない）は、この動きと合う。

まだ分かっていないこと: 成功の書き出しの直後にこの上書きが起きたところを、controller の記録つきで捕まえては
いない（再現した 2 回は、記録を足す前だった）。preview が使える状態へ変わる時刻が、なぜ書き出しの終わりと
重なるのか（見えない desktop の描画と関係するか）も調べていない。

これは試験の道具の問題ではなく、製品の状態の表示の欠陥である（利用者の画面でも、書き出しの結果の文が消えうる）。
この phase では直していない（書き出しの経路の外だが、製品の動きを変えるので別の段にする）。smoke は、この場合を
成立にせず `completion-not-observed` として報告する（隠さない）。

## 4. 失敗の注入（Task D）

`scripts/math-group-gui-smoke-faults.ps1`（新規）。全部を見えない desktop で行う（場面 B）。各場合で、分類、
その run の process が残らないこと（job が空・新しい mvm.exe が無い）、Project lock が残らないことを確かめる。

最終の binary での結果（`build/math-group-gui-smoke-faults/20261015-042457/faults.json`）: **12 件全部が期待どおり。**

| 場合 | 期待 | 結果 |
| --- | --- | --- |
| normal（注入なし） | passed | passed |
| driver-exit（driver が記録も後始末もせずに終わる） | driver-failure | driver-failure。30 秒後に session が mvm.exe を止めた |
| product-crash（書き出しの前に 0xC0000005） | product-crash | product-crash |
| exit-during-export（書き出しの最中に終了コード 1） | unexpected-exit-during-export | 同じ |
| missing-element（無い部品・閉じた dialog の古い部品） | driver-step-failed | 同じ。driver は mvm.exe を閉じて終わる |
| unobservable-completion（3 の負例） | completion-not-observed | 同じ。観測に「GUI 応答・dialog なし・状態は結果の文」、別の client も結果の文を読めた |
| session-limit（15 秒） | session-timeout | 同じ |
| runner-timeout（180 秒。mvm.exe が動いている最中） | job ごと停止（終了コード 3） | pwsh・mvm・conhost の 7 個を止めた。job の外の見張りは生きている |
| runner-killed（runner を外から止める） | 子孫が全部止まる | 止める前の子孫 10 個（mvm を含む）が全部終わった |
| stale-lock-file（持ち主の無い .mvm.lock） | passed | passed |
| held-lock（job の外の process が lock を持つ。reference の後に取る） | project-locked | project-locked。持ち主の handle は有効のまま |
| held-lock-released | lock が空く | 空いた |

全部の場合で: 新しい mvm.exe が残らない、job が空、Project lock が空き。

途中の実行の記録も残してある:

- 1 回目（`20261015-024446/`）: 3 件が期待と違った。session-limit は上限 25 秒が長すぎて場面が先に終わった
  （15 秒に直した）。held-lock は lock を最初から持ったので reference が開けず、reference の段が StrictMode の
  error で落ちた（lock を reference の後に取るよう直し、reference の段を null 安全にした）。stale-lock-file は
  3 の「完了を確かめられない」の再現（注入なし）。
- 2 回目（`20261015-031610/`）: normal が 3 の再現。ほかは全部期待どおり。

## 5. 道具の自己試験

`scripts/math-group-product-p36c2-selftest.ps1` に足した:

- 分類の 12 件（作りものの session.json: 成立、completion-not-observed、driver-failure、session-timeout、
  project-locked、unexpected-exit-during-export、product-crash、reveal-mode-mismatch、`-Visible` では求めない、
  driver-no-result、注入した run は不成立、session.json が無い）。
- runner の 5 件（見えない desktop の pwsh と孫で: 時間切れで job ごと止める、根の後に残った孫を記録して止めて
  終了コードを返す、何も残さなければ 0、runner の異常終了でも孫が止まる、job の外の見張りは止めない）。

## 6. 結果

証拠は `build/gui-smoke-hidden-desktop-p37b1-20261015/`（CTest・対照）、`build/math-group-gui-smoke/`（smoke）、
`build/math-group-gui-smoke-faults/`（失敗の注入）、`build/math-group-gui-smoke-repro/`（繰り返し）。

| 試験 | 結果 |
| --- | --- |
| 製品 GUI の smoke（見えない desktop、場面 A・B） | 成立（`20261015-024011`。A 26/26、B 11/11、`reveal_simulated`、job は空） |
| 繰り返し（controller の記録つき） | 場面 B 8 回、場面 A・B 5 回、全部成立 |
| 失敗の注入 | 12/12 が期待どおり（4 の表） |
| 道具の自己試験 | 87/87 |
| `mvm_reveal_simulation_gate`（新規）・`reveal_in_explorer_negative`・`m7b_4_timeline_ui_architecture` | 通過 |
| 対照: 利用者の画面で断る処理を外す | `mvm_reveal_simulation_gate` が落ちる（mvm.exe が起動して 30 秒で終わらない。`negative-control/`）。元に戻して通過 |
| lint | 通過 |
| **release の全 CTest（2 回目）** | **1526/1526 通過**（1,200 秒。`ctest-run-2/`）。GUI の環境は前後とも VALID |

- 全 CTest の 1 回目（`ctest-run-1/`、前後とも VALID）は 1525/1526。`transition_preview`（再生中のディゾルブの
  不透明度。preview の試験で、この phase は触れていない）が 1 件落ちた。単独で 5 回くり返して 5/5 通過
  （`transition-preview-repeat/`）。2 回目の全体では通過。
- 同一性（2 回目）: HEAD `598c14b`、作業 tree の差分（`git diff HEAD -- src apps tests scripts CMakeLists.txt cmake`）
  の SHA-256 `1DF4AA9B…A985A`、実行の後の `mvm.exe` `651A135A…891571`、MSYS2 の `Qt6Core.dll` `C7661844…16868B`。
  実行の前後で `mvm.exe` と `mvm_test_text_ui_input.exe` が変わるのは、`scripts/test.ps1` が毎回 link し直す
  ため。前後の `git status` の違いは `docs/roadmap.md` だけ（実行中に私が文書を直した。source・試験は同じ）。
- 失敗した途中の実行も残してある: smoke `20261015-023726`（実行中に私が freeze の対象の script を直したので、
  reference の段の freeze の照合が不一致で止まった。runner の job は空で終わった）、失敗の注入の 1・2 回目（4）。
- 歴史の 1/5 の失敗（`build/math-group-gui-smoke/20261015-013512/`）と、Explorer で止まった run
  （`20261015-012505/`）はそのまま。

## 7. 残っていること

- **製品の欠陥（未修正）**: 書き出しの直後に、`pollPreviewState` が状態の表示（書き出しの結果の文）を「素材を
  追加してください」で上書きすることがある（素材の clip の無い Project）。利用者の画面でも、書き出しの結果が
  消えて見える。smoke は `completion-not-observed` と報告する。直すのは別の段。
- **まだ分かっていないこと**: 成功の書き出しの直後の上書きを、controller の記録つきで捕まえていない。preview が
  使える状態へ変わる時刻が書き出しの終わりと重なる理由（見えない desktop の描画との関係を含む）。
- 見えない desktop の smoke は、Explorer が実際に開くことを確かめない（`-Visible` は今回実行していない）。
- 見えない desktop では画面の保存が取れない。
- 対照の実行で、mvm.exe が利用者の画面に 30 秒ほど window を出した（断る処理を外したため。試験が止めた）。
- `math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま。全 CTest は、負荷で 1 件落ちることがある
  （今回は `transition_preview`）。
