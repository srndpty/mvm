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
