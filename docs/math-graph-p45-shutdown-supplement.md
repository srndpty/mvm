# P4-5 閉鎖補遺: Graph の encoding 中の shutdown

状態: **P4-5 PASS/CLOSED**、**P4 PASS/CLOSED** は変更しない。
[閉鎖記録](math-graph-p45-closure.md) と、その全証拠 directory は履歴として保存する。
これは新しい Graph 機能の段階ではなく、shutdown の負例を補う検査である。

## 補った弱点

閉鎖時の shutdown 検査 (`stopMode 2`) は、`書き出しています…` の観測、`exporting()` の解除、
最終出力の不在で取消を判定していた。これらは、export が encoder の frame 処理へ到達する前に
止まっても成立する。worker が実際に frame を処理している最中の取消は証明していなかった。

## 検査の構成

実 `Main.qml`・`MvmController`・書き出しダイアログ・実 Manim の artifact を使う既存の
`mvm_test_text_ui_input --graph-export-ui` に `stopMode 3` を足した。既存の取消
(`stopMode 1`) と shutdown (`stopMode 2`) はそのまま残し、その後に保存済み Project を
再起動して開き直してから実行する。

1. 既存の `encoderFrameValidator` (最終 MLT 合成後、YUV 変換前の境界) を barrier にする。
   最初の呼出 (frame 0) を独立 oracle で全画素比較してから lock を外し、encoder を保持する。
   frame 0 に到達し oracle が一致したことで、preflight・compile・artifact 検証を通ったことを示す。
2. GUI thread (`controller->thread()` と同一) から製品の `shutdown()` を呼ぶ。保持中であること
   (barrier 未解放・runner 未返却・`exporting()`) を shutdown の直前に確かめる。
3. barrier の解放条件は、製品の取消 flag を encoder の完了待ち loop が `progress` で受け取ったこと
   だけである。最終公開境界での取消では解放されない。待機時間を合否に使わない。期限 10 秒は、
   変異で取消が来ない場合に試験を終わらせるためのもので、consumer の timeout 30 秒より短い。
4. shutdown が戻った時点で、barrier の解放、runner の返却 (`cancelled`、`GraphExportFailure::Cancelled`)、
   `exporting()`・`busy()` の解除を要求する。join が worker の寿命を解決したことの証明である。
5. worker は返却後に完了の queued 呼出を投函している。shutdown 後に投函した marker が届くまで
   event を配送し、その間に status 変化と `exportFailed` が一件も無いことを要求する。
   完了呼出が配送済みで、かつ握り潰されたことを示す。
6. 最終出力と `.mvmtmp` が無いことを要求する。

Graph の式の意味、schema22、renderer、artifact、MLT の alpha、書き出しの frame mapping、
製品のコードは変更していない。変更は試験 (`tests/harness/test_equation_sequence_ui.cpp`) と
変異の script (`scripts/math-p45-active-shutdown-mutations.ps1`) だけである。

## 失敗の記録

| 証拠 directory | 因果と修正 |
| --- | --- |
| `math-p45-20261010-043828-Real` | 再起動した session の oracle の背景が既定の黒で、共存 Project の既知 RGB 実動画 `{20,40,60}` を引き継いでいなかった (frame 0 pixel 0 で実値 20、期待 0)。frame 0 の比較が空振りしていないことの実証でもある。背景を引き継いだ。再起動後の UI の Ready 待ちも、FFV1 の preview 失敗で成立しなかった。既存の再起動経路と同じく待たず、書き出しが disk の artifact を検証する |

`math-p45-20261010-044315-Real` は通過したが、その後 barrier の解放条件を
「取消 flag」から「完了待ち loop が取消を受け取ったこと」へ強めたので、閉鎖の判定には使わない。

## 結果

[事実] 2026-10-10、同一 source `76FC9C6ECA478D0F1B5060A3506848F668BC6BE3E91B4B22452FA518DB46CD64` で取得した。

- `math-p45-20261010-044640-Real`: 216 件、失敗 0。`graph-active-shutdown.mp4.result.json` の
  `activeShutdown` は barrier frame 0、oracle 一致、保持中の取消観測、runner の取消
  (`書き出しをキャンセルしました`、C backend の完了待ち loop の文言)、寿命の解決、shutdown 後の
  status 変化 0 件・失敗通知 0 件を記録した。最終出力と一時出力は無い。
- `math-p45-20261010-044828-ActiveShutdownMutations`: 検出 2/2、byte 復元 2/2、復元後の再実行は
  いずれも終了コード 0。
  - `shutdown-cancel-bypass`: `shutdown()` の取消 flag の設定を外す。保持中に取消が届かず、
    目的の assertion で検出した。
  - `active-stale-completion`: 完了呼出の `shutdownStarted_` 判定を外す。shutdown 後に
    `書き出しをキャンセルしました` が通知され、目的の assertion で検出した。既存の
    `stopMode 2` はこの変異で失敗しなかった。
- `math-p45-20261010-045732-Focused`: 明示取消・公開境界取消を含む focused 試験は全段通過。
- `math-p45-20261010-045742-Regressions`: 17/17 通過。controller の shutdown lifecycle
  (`m7b_4_controller_export_lifecycle`) を含む。
- `math-p45-20261010-050353-Lint`: 通過。

製品のコードと QML を変更していないため、BuildIndependent と通常 release 全体は再取得していない。
