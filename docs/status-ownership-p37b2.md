# P3-7B.2: preview の確認が、書き出しの結果の文を上書きしないようにする

2026-10-15。P3-7B.1 の調べで、`MvmController::pollPreviewState()` が書き出しの結果の文を
「素材を追加してください」で上書きすることを、controller の記録で確かめた（書き出しの失敗の文の 0.18 秒後。
`build/math-group-gui-smoke-repro/20261015-041153/`）。それより前の 2 回の「完了を確かめられない」（成功の
書き出しの後、GUI は応答し、進行の dialog は無い）も同じ仕組みと合うが、その 2 回の controller の状態の変化は
記録していない（記録を足す前だった）。この不確かさは残る。

状態の表示の持ち主を決め、利用者の操作によらない定常の文が、書き出しの結果を消せないようにした。書き出しの
描画・数式 group・timeout・preview の描画・画素の基準は変えていない。数式 group の既定（off）と fail-closed の
方針も変えていない。commit・push はしていない。人の操作は使っていない。

証拠は `build/status-ownership-p37b2-20261015/`。

## 判定

**PASS。** preview の確認（と、閉じた後に届く描き直し）は、書き出しの成功・取消・失敗の文を消せない。利用者の
新しい操作（seek・別の書き出し・別の Project・失敗する操作）は、これまでどおり状態の表示を置き換えられ、置き換えた
後は preview の確認の文もまた出る。全 CTest は 1527/1527（1 回目）。

## 1. 状態の表示の書き手（Task A）

`apps/mvm/*.cpp` の全部の書き込み（`setStatus`・`statusText_ =`・`reportExportFailure`・`stopPlaybackWithError`）を、
囲む関数ごとに機械的に数えた: **379 箇所、149 関数**（修正の前。`status-writers-prefix.tsv`）。ほとんどは利用者の
操作（QML から呼ぶ関数）の結果である。

利用者の操作によらずに走り、書き出しの最中・直後に状態を書きうるもの:

| 入口 | 書く文 | 種類 |
| --- | --- | --- |
| `pollPreviewState`（100 ms の timer）: preview が使える状態へ変わった時 | 「素材を追加してください」 | **定常（案内）** |
| 同上: 描画の元が何も無い時の自動の初期 seek（`seekTimelineFrame`） | 「… を表示しています」「timeline gapを表示しています」「Previewがseek可能になるまで待ってください」 | **定常（案内）** |
| 同上: 自動の初期 seek の失敗 | 「Preview error: …」「Previewをseekできません: …」 | error |
| `pollPreviewState`: 待っていた再生・描き直し | 「…Previewを準備できません: …」「Previewを更新できません: …」「Preview error: …」 | error |
| `pollPreviewState`: 生成した Manim の video の取り込み（`installVideoClip`） | 「… を表示しています」・失敗の文 | 利用者が始めた生成の結果 |
| 数式・画像・グラフの描画の完了（cache の signal → `refreshTextPreview`） | 「文字のPreview更新に失敗しました: …」 | error |
| 描画先の大きさの変化（`attachPreview` の singleShot） | 「Previewを再描画できません: …」 | error |
| 自動復旧の保存（timer・worker の完了） | 「自動復旧データを保存できません: …」など | error |
| 文字起こしの完了（`applyTranscription`） | 失敗の文 | 利用者が始めた処理の結果 |
| 書き出しの worker の進み具合・完了（queued） | 「書き出しています…」・結果の文 | 書き出し |

再生（`advanceTimelinePlayback`）・scrub・slip・shuttle の timer は、利用者が始めた操作の間だけ動く。

## 2. 状態の表示の持ち主（Task B）

- **書き出しは、その最中、状態の表示を持つ。**
- **書き出しの結果の文（成功・取消・失敗。始める前の失敗も含む）は、そのまま出ている間、書き出しが持ち続ける。**
- その間、**定常の文**（上の表の「定常」）は出さない（`setRoutineStatus`。出さなかった数と最後の文を残す）。
- 定常の文以外（利用者の操作の結果、全部の error。非同期の error も含む）は、いつでも書ける。書けば別の文に
  なるので、その時点で書き出しは状態の表示を手放す。**時間で待たない。居座らない。**
- 新しい書き出しは前の結果を手放して「書き出しています…」を持つ。別の Project を開くと、前の Project の結果は
  持たない。

実装（`apps/mvm/mvm_controller.cpp`・`mvm_controller_export.cpp`）:

- `exportOwnsStatus()` = 書き出しの最中、または、結果の文（`exportResultStatus_`）が今の文と同じ。
- `setRoutineStatus()`: 持ち主が書き出しなら出さない。`pollPreviewState` の「素材を追加してください」と、
  自動の初期 seek の間（`automaticPreviewSeek_`）の `seekTimelineFrame` の 3 つの案内の文が使う。利用者の
  seek（ruler・コマ送り・marker など）の文は、これまでどおり書く。
- `setExportResultStatus()`: 書き出しの取消・成功・失敗（`reportExportFailure`）の文。
- 閉じた後（`shutdown`）に届いた、描画先の大きさの変化の描き直しは行わない（`queuePreviewResizeRefresh`）。
  全 CTest の中でこの経路が「Previewを再描画できません: Previewが使える状態ではありません」で結果の文を上書き
  したので、見つかった（下の 4）。

## 3. 修正の前の負例と、決まった順序の試験（Task C・D）

CTest `export_status_ownership_product_ui`（新規。`mvm_test_text_ui_input --status-ownership-ui`）。製品の
Main.qml と実 controller、製品の書き出し（`mvm::app::exportTimeline`）、数式の描画だけ偽の backend。素材の clip の
無い数式だけの Project。preview が使える状態へ変わった時の処理は、試験用の入口で決まった順序で起こす
（`markPreviewNotReadyForTest` → `pollPreviewStateForTest`。この後に timer の poll も届ける）。controller の文と、
Main.qml の状態の表示（`statusTextLabel`）の両方を見る。

| # | 場合 | 確かめること |
| --- | --- | --- |
| 前提 | 書き出しの無い状態 | preview の確認は定常の文を出す（以前と同じ） |
| 最中 | 書き出しの最中 | 「書き出しています…」のまま |
| 1 | 従来の方法の成功 | 結果の文が残る。定常の文（「素材を追加してください」）を出さなかった記録がある |
| 2 | 数式の高速化の成功 | 同じ |
| 3 | 取消（製品のキャンセルボタン） | 「書き出しをキャンセルしました」が残る |
| 4 | timeout | 失敗の文と通知。文が残る |
| 5 | 最終の名前へ置けない失敗（書き出し先を他が掴む） | 失敗の文が残る |
| 6 | preview の engine の作り直し（自動の初期 seek の経路） | 結果の文が残る。「… を表示しています」を出さなかった記録 |
| 7 | 利用者の seek | 結果の文を置き換える。その後の失敗する操作の文の後は、preview の確認の文がまた出る（居座らない） |
| 8 | 2 回目の書き出し | 前の結果を「書き出しています…」が置き換え、新しい結果が残る |
| 9 | 結果の後の失敗する操作（無い素材） | 理由が出る |
| 10 | 別の Project を開く | 前の結果を持ち越さない |
| 11 | 閉じる直前に preview の確認と描き直しを積み、閉じる | 閉じた後の poll と、残った event は文を書き換えず、落ちない |

**修正の前の負例**（`prefix/prefix-run-3.txt`。試験の入口だけを足した、修正の前の code）: **61 件中 14 件が
落ちた。** 1〜5 と 8 で、preview の確認が結果の文を「素材を追加してください」で上書きした（controller と Main.qml の
両方）。6 で、自動の初期 seek が「m2 を表示しています」で上書きした。残りの 1 件（7 の後半）は、私の試験の
書き方の誤り（同じ文が書かれ、区別できない）で、直した。

**修正の後**: **69/69**（`fixed/`）。

**対照**（`negative-control/`）:

- 最終の試験で、`setRoutineStatus` の抑止だけを外す（修正の前と同じ動き）と、**69 件中 20 件が落ちる**（1〜6・8 の
  上書きと、定常の文を出さなかった記録が無いこと）。
- 閉じた後の描き直しの抑止だけを外すと、11 が「Previewを再描画できません: Previewが使える状態ではありません」で
  落ちる。この経路は、修正の後の最初の focused の CTest で、window の大きさが変わったために偶然起きて見つかった。
  window の大きさの変更では決まって起こせなかった（3 回とも起きない）ので、同じ処理を積む試験用の入口
  （`queuePreviewResizeRefreshForTest`）を足した。
- どちらも元に戻し、build し直して 69/69。

## 4. 見えない desktop の smoke（Task E）

driver（`scripts/math-group-product-p36c2-drive.ps1`）は、書き出しの結果の文を見た後、**5 秒の間**（0.5 秒ごと）
同じ文が出続けることを確かめる（段「書き出しの結果の文が 5 秒そのまま残る」。A5・A7・A8・B3）。300 秒の待ちは
変えていない。

smoke（`scripts/math-group-gui-smoke.ps1`）の分類に 2 つ足した:

- `result-overwritten`: controller の記録（`--test-log-status`）で、書き出しの結果の文の直後（10 秒以内）に定常の
  文が来た。
- `result-not-retained`: driver が見た結果の文が、5 秒の間に別の文に変わった。

実際の `mvm.exe`（見えない desktop。`build/math-group-gui-smoke/20261015-074348/`）: **場面 A 29/29、場面 B 12/12、
分類 passed。** controller の記録では、どの結果の文（成功・取消・失敗）の後にも、定常の文は来ていない。

- Explorer は、見えない desktop では模擬（`--test-reveal-without-explorer`。path の解釈までは実際の Shell）。
  実際の Shell での確かめ（`-Visible`）は今回行っていない。分類は分けたまま。
- 失敗の注入（`build/math-group-gui-smoke-faults/20261015-074825/`）: 12/12 が期待どおり（driver の変更の後）。

## 5. 結果

| 試験 | 結果 |
| --- | --- |
| `export_status_ownership_product_ui`（新規） | 修正の前 14/61 落ち → 修正の後 69/69。対照 2 つ（20 件落ち・1 件落ち） |
| focused（`m7b_4_controller_export_lifecycle`・`m7b_4_selection_identity`・`math_group_export_product_ui`・`export_settings_summary_product_ui`・`mvm_reveal_simulation_gate`・`reveal_in_explorer_negative`・`transition_preview`・`m7b_4_timeline_ui_architecture`・`math_controller_focused`・`timeline_export*`・`math_transform_authoring_product_ui`） | 11/11 |
| 見えない desktop の smoke | A 29/29、B 12/12、passed |
| 失敗の注入 | 12/12 |
| 道具の自己試験 | 90/90 |
| lint | 通過 |
| **release の全 CTest** | **1527/1527 通過（1 回目）**（1,296 秒。`ctest-run-1/`）。GUI の環境は前後とも VALID。前後の `git status` は同じ |

- 同一性: HEAD `a6c5ad7`、作業 tree の差分（`git diff HEAD -- src apps tests scripts CMakeLists.txt cmake`）の
  SHA-256 `3FFC8ED5…080B88`、実行の後の `mvm.exe` `F34B1FBA…86B8EC31`。前後で `mvm.exe` と
  `mvm_test_text_ui_input.exe` が変わるのは `scripts/test.ps1` の link のし直し。
- 書き出しの経路は変えていないので、v4 全体（6840 frame）は実行していない。

## 6. 残っていること

- P3-7B.1 で再現した 2 回の「完了を確かめられない」（成功の書き出しの後）が、この上書きだったことは、controller の
  記録が無いので確定していない（仕組みが合うことと、失敗の後の上書きを記録で確かめたことまで）。
- 定常の文は、書き出しの結果が出ている間だけ出さない。書き出し以外の文（例: 失敗する操作の理由）は、これまで
  どおり preview の確認の定常の文で置き換わりうる（この phase の範囲外。以前からの動き）。
- 書き出しの最中に、関係の無い非同期の error（自動復旧の保存の失敗など）が出ると、「書き出しています…」を置き換える
  （error を隠さない、という決め。書き出しが終われば結果の文が出る）。
- 見えない desktop の smoke は、Explorer が実際に開くことを確かめない（`-Visible` は今回行っていない）。
- 関係の無い GUI の不安定さ: `math_equation_sequence_product_ui` の SegFault（UNKNOWN）、全 CTest で負荷により 1 件
  落ちることがある（P3-7B の `math_transform_authoring_product_ui`、P3-7B.1 の `transition_preview`。今回は 0 件）。
