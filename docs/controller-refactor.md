# MvmController の責務別分割

## 目的と境界

`mvm_controller.cpp` へ集まっていた書き出し・エフェクトとキーフレーム・素材操作・
タイムライン構造編集を、同じクラスの別の翻訳単位へ移した。
字幕と音声自動調整の既存方式に揃え、状態を所有する別クラスへの抽出は行っていない。

- `mvm_controller_export.cpp`: 要求構築・進捗・取消・完了。worker の所有と終了時の join は本体。
- `mvm_controller_effects.cpp`: 値とキーフレームの編集、編集 preview の確定・取消。
- `mvm_controller_media.cpp`: 素材の登録・整理・配置。種別の判定は既存の `probeMediaFile`。
- `mvm_controller_timeline_edit.cpp`: コピー・配置・trim/split・transition・marker・track の編集。
- `mvm_controller_detail.h/.cpp`: 複数の翻訳単位で必要な既存の補助関数だけを共有する。

再生と提示、数式・EquationSequence・Graph の編集と cache、Undo の確定、保存と復旧、
コンストラクターと shutdown は本体に残す。公開 API・保存形式・QML・通知順序は変更しない。
宣言は責務別にまとめ、状態メンバーの宣言順は維持した。

## ビルドと source を読む検査

`cmake/mvm_controller.cmake` がソース一覧と共通リンク依存を定義し、製品・製品 QML の
入力試験・controller 試験がこれを直接使う。export 試験の target property から他の試験の
構成を取り出す方式は廃止した。各 target の定義と resource はその target に残している。

新しい STATIC/OBJECT library は作らず、各 target が同じ一覧をコンパイルする。
分割後の実装も UI 契約試験で読み、速度編集と frame hold の実装が欠ける負例を拒否する。
stale な export 完了の変異対象を移動先へ更新し、準備調査の source snapshot に全分割先を含めた。
歴史的な証拠の内容や参照は変更していない。

## 検証の記録

変更前の作業ツリーは clean。登録件数と対象試験の基準は
`build/controller-refactor-baseline-list.log` と `build/controller-refactor-baseline.log`。
各責務のビルドと試験、最終の通常 release/debug、lint の記録は
`build/controller-refactor-*.log` に保存する。

[事実] 分割前後の controller メンバー関数の本体を文字列で比較し、追加・欠落・変更が無い
ことを確認した。公開の `Q_PROPERTY` と `Q_INVOKABLE` は宣言集合を比較し、private 部分は
文字列で比較した。結果は `build/controller-refactor-function-bodies.json`、最終 source の
hash と行数は `build/controller-refactor-source-state.json` に保存した。

[事実] 書き出し分割後の最初の試験は、復旧ファイルの自動保存を 4 秒以内に確認する assertion
で失敗した。復旧処理の関数本体は変更していない。その試験と次段階のビルドを同時に実行していた。
原ログと LastTest は `build/controller-refactor-export-test.log` と
`build/controller-refactor-export-lasttest.log` に保持する。
[未検証] 同時ビルドの負荷と失敗の因果関係。失敗を一過性とは断定しない。
後続段階ではビルドと試験を重ねず、エフェクト・素材・タイムラインの各変更後に試験を実行した。

状態の所有は変わらないため、ファイルを分割してもクラス全体の結合は残る。
追加の整理に関する判断は `docs/roadmap.md` で扱う。

## 最終の検証結果

以下の件数は最終ゲートの原ログから抽出した
`build/controller-refactor-final-summary.json` に基づく。

| ビルド | 種別 | 対象 | 実行 | 通過 | 失敗 |
| --- | --- | ---: | ---: | ---: | ---: |
| ucrt64-release | 通常 | 1508 | 1508 | 1508 | 0 |
| ucrt64-debug | 通常 非依存 | 1086 | 0 | 0 | 0 |
| ucrt64-debug | 通常 依存 | 422 | 422 | 422 | 0 |

ビルド種別に依存しない試験は release で実行済みなので debug では再実行しない。
performance / stability は通常ゲートから除外した。
原ログは `build/controller-refactor-final-fixed-both.log`、両ビルドの LastTest も同じ prefix で保存した。
最終 lint は `build/controller-refactor-lint-final.log` で通過した。

共有補助ファイルの include 順の更新を release の全 target へ同期した後、書き出し controller・
映像なしの再生・transition preview・製品 QML の直接入力を追加確認し、4/4 件通過した。
記録は `build/controller-refactor-final-sync-build.log` と
`build/controller-refactor-final-sync-test.log`。最後の dry build に controller の再コンパイルは残っていない。

source の行数は空行を含めて本体 10108 行から 6563 行。
初回の全体ビルドで、共通化時に独立した素材モデル・shuttle・scrub 試験のソースを除いた
誤りを検出し、その定義を復元した。修正前のビルド失敗は
`build/controller-refactor-final-both.log` に保持し、最終ゲートの結果とは区別する。

## 保存・復旧・ロックの追加分離

保存・読み込み・復旧・ファイルロックの 31 メンバー実装と、復旧キューの内部型を
`mvm_controller_project_io.cpp` へ移した。単独で使う `atomicSaveProject` も移動先の
匿名 namespace に置いた。共有済みの path 変換と clip 検索は detail の実装を使う。
Project の確定 (`adoptProject`)、Undo、動画設定変更、コンストラクター、shutdown は本体に
残す。復旧の worker・キュー・タイマーを含む状態所有と終了時の完了待機は変更していない。

[事実] 追加分離の基準 HEAD は `8c3740ed617291be4b3e4a1c5f3eda9786cc97f9`、作業ツリーは clean。
本体は空行込みで 6563 行から 5834 行、移動先は 751 行。
本体にあった 198 メンバー関数の本文を移動前後で比較し、追加・欠落・変更は無かった。
復旧の内部型と保存 helper も文字列で一致した。公開ヘッダーは変更していない。

既存の保存・復旧・ロック試験 19 ケースを `test_mvm_controller_project_io.cpp` へ移し、
`controller_project_io` として登録した。Project fixture・判定・event 待機は
`test_mvm_controller_fixture.h` を共有する。書き出し・編集などの既存ケースは元の試験に残した。
試験本体 59 関数と main からの 61 呼び出しは移動前後で維持した。
復旧の待機上限は今回の基準 source にあった 20 秒のままであり、過去の 4 秒の失敗を
解決済みとする変更は今回行っていない。

共通 CMake 一覧に移動先を追加し、製品と全 controller 利用試験が同じ定義を使う。
既存の keyframe/subtitle 検証 target には、移した試験をビルドできるよう新 target を追加した。
UI 契約試験は移動先を直接読み、外部変更検査の実装・ロックの削除フラグ・source 全体の
欠落を拒否する負例も確認する。変異対象はすべて確認し、今回動かした関数を対象にする
既存の変異スクリプトは無かった。現行の調査 snapshot には新しい実装・試験・fixture と
試験 binary を追加し、過去の snapshot と結果は保持した。

[事実] 変更前 2/2 件、実装移動後 2/2 件、試験分離後 3/3 件の関連 CTest が通過した。
通常試験の登録は 1508 件から 1509 件になった。比較結果・元 source・各段階のログは
`build/controller-project-io/` に保存した。比較記録は `comparison.json` と
`test-comparison.json`、各段階の試験結果は `baseline-test.log`、`move-test.log`、
`split-test.log`。最終 lint は `lint-final.log` で通過した。

最終ゲートは `pwsh scripts/test.ps1 -Preset both` で実行した。
以下は `final-both.log` の種別別集計から生成した結果。

| ビルド | 種別 | 登録 | 実行 | 通過 | 失敗 |
| --- | --- | ---: | ---: | ---: | ---: |
| ucrt64-release | 通常 | 1509 | 1509 | 1509 | 0 |
| ucrt64-debug | 通常 非依存 | 1086 | 0 | 0 | 0 |
| ucrt64-debug | 通常 依存 | 423 | 423 | 423 | 0 |

ビルド種別に依存しない試験は release で実行済みなので debug では省略した。
performance / stability は除外した。両ビルドの LastTest は
`final-release-lasttest.log` と `final-debug-lasttest.log`、集計は `final-summary.json` に保持した。
最終ゲート中の実装・ビルド定義・検査 source の hash は `source-state.json` と全件一致し、
release / debug の dry build に未コンパイル・未リンクの差分は無かった。
コミットは行っていない。

## 分割後の変異互換検査

P4-5 の active-shutdown 2 ケースと通常 stale-completion 1 ケースを最終分割 source で
実行した。全ケースでビルド成功・狙った assertion の失敗・元 byte 列と SHA-256 の復元・
復元後試験の通過を確認した。初回 Real 基準の未特定の FAIL も保持している。
[実行結果と source の来歴](controller-mutation-compatibility.md) を参照。
