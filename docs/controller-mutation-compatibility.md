# Controller 分割後の P4-5 変異互換検査

## 対象と source の来歴

`MvmController` の意味論とビルド定義は変更せず、最終分割先で既存の P4-5 変異を実行した。
`math-p45-mutations.ps1` には取得対象を選ぶ `-CaseName` と表示電源 lease を追加した。
省略時は既存の全ケースを選ぶ。今回は `-CaseName stale-completion` を指定した。
active-shutdown スクリプトの変異内容・判定・復元処理は変更していない。

基準 revision: `d11358c1313bd7da4ec68e4a86819c2a6229de21`。
Real / Focused に保存した 1023 ファイルの source 集合 SHA-256:
`D871D6BDE0BF8D44BC20CA2D536DE372F17DB1D485E69C2DE83BAB055E1E8614`。この集合には今回のスクリプト修正も含む。
変異終了後に全ファイルの hash を再照合した。製品・試験本体・CMake の差分はない。

両スクリプトの 12 定義について、期待する変異前の文字列が各指定ファイルにちょうど
一箇所あることを AST から読み取って検査した。移動した stale 完了 guard は
`mvm_controller_export.cpp`、shutdown の取消 flag は本体にある。
位置と hash は `build/controller-mutation-review-20261011-022656/target-spans.json` に保存した。
動的に実行したのは下表の 3 ケースで、他の 9 定義の動的検出は今回の対象にしていない。

## 実行結果

新規 Focused: `build/math-p45-20261011-022936-Focused`。全記録コマンドが終了コード 0。
新規 Real: `build/math-p45-20261011-023017-Real`。216 検査、失敗 0。
Real と変異は固定window・音量scale 0.25・背面／入力透過を使い、
`Start-MvmTestDisplayLease` / `AssertValid` / `Dispose` で表示電源の前提を維持した。
通常の PC 操作は可能。ビルドと試験は重ねず、公式 build script を sandbox 外で実行した。

| 変異 | build exit | 変異試験 exit | 復元後試験 exit | 元byte列とSHA-256 |
| --- | ---: | ---: | ---: | --- |
| `shutdown-cancel-bypass` | 0 | 1 | 0 | 一致 |
| `active-stale-completion` | 0 | 1 | 0 | 一致 |
| `stale-completion` | 0 | 8 | 0 | 一致 |

active の終了コード 1 は狙った assertion の FAIL、通常の終了コード 8 は CTest の
assertion 失敗を示す。timeout・crash・PROTOCOL_INVALID による検出ではない。
active の両ケースで、保持した frame は 0、独立 oracle は一致、worker は所有thread外、
保持中の shutdown と join 後の寿命解決が成立した。
取消変異では保持中に取消を観測せず、stale 通知変異では取消と join が成立した後に
queued 完了が配送され、shutdown 後の status 通知を実際に観測した。

取消変異では停止・出力不在の判定にも 2 本、active stale 変異では停止判定にも 1 本の
FAIL が出た。通常 stale 変異は同じ目的の assertion が 2 本出た。
すべての FAIL 行と shutdown の観測フィールドは最終照合 JSON に保持している。

各変異の finally で元の byte 列を復元し、その場でビルドと対応試験を再実行した。
active の復元後 Real は両方通過し、通常 stale の復元後は
`m7b_4_controller_export_lifecycle` が 1/1 件通過した。最終 lint も通過した。
製品 source / CMake に恒久的な変更がないため、通過済みの全 release suite は再実行していない。

active の FAIL / PASS: `build/math-p45-20261011-023147-ActiveShutdownMutations`。
通常 stale の FAIL / PASS: `build/math-p45-20261011-023824-Mutations`。
最終照合: `build/controller-mutation-review-20261011-022656/final-audit.json`。

## 初回 Real の失敗と制限

初回 `build/math-p45-20261011-022701-Real` は 216 検査中 2 件失敗した。
worker を保持した frame が期待値 0 ではなく 1 で、最初の encoder frame を要求する
前提 assertion が失敗した。取消・worker 保持・join の観測自体は成立していた。
停止の総合判定も失敗し、status には FFV1 に D3D11VA hardware decoder がないという
preview エラーが記録された。これらの因果と発生原因は未特定。

source を変更せず、追加の基準取得は一回だけと事前に記録して実行した。
2 回目は 216/216 件通過したが、初回の FAIL を解決済みにしていない。
変異互換の結論は実際に通過した基準、狙った変異検出、復元後の通過に基づく。
Real 受入の反復安定性まで保証するものではない。初回 FAIL と後続 PASS は別 directory
に保持し、過去の P4-5 の証拠・結果文書も変更していない。コミット・push は行っていない。
