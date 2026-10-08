# P4-4 の検証結果

基準 HEAD: `39f8995371db625907f1ce9c13ed56bf248a668b`。stableGraph / graphMemo の契約範囲は HEAD と文字完全一致。追跡済みの歴史的 Graph / EquationSequence 文書に差分はない。

P4-4 の判定はこの生結果と設計文書で確認する。P4-5 は開始せず、コミット・push は行わない。

初回の Flow と fixture、native style 警告、同値編集と削除後選択の試験期待の誤り、診断の型誤り、途中で停止した重複 build はすべて保存する。
成立後に追加のイベントを処理して待機条件を再評価する helper の不具合は、queued event の決定的な負例を添えて修正した。
初回の再起動失敗がこの同じ原因だったかは過去 run の診断が不足しており未確定。現在の修正済み待機と native 全画素比較を独立の根拠とする。
Final-Focused と Final-Regressions の `graph_editor_controller` 失敗は、その後の試験修正より前の source である。通常 release は修正後の試験をビルドして通過した。
最初の BuildIndependent は Escape で拒否 draft を現在の確定値へ戻す修正より前の QML である。BuildIndependent-02 は最終 QML で 1084 件を通過した。
Final-Mutations は CP932 コンソールのリダイレクトが UTF-8 の失敗文を崩し、assertion 文字列の照合だけが不成立だった。試験は終了コード 8 で該当拒否が失敗している。Final-Mutations-02 は UTF-8 で捕捉し、6 件すべてを検出して source を復元した。

|取得|終了コード|生結果の集計|証拠|
|---|---:|---|---|
|math-p44-20261009-064451-Focused|8|ctest.log: 3 件中 3 失敗|[生証拠](../build/math-p44-20261009-064451-Focused/)|
|math-p44-20261009-064550-Focused|8|ctest.log: 3 件中 2 失敗|[生証拠](../build/math-p44-20261009-064550-Focused/)|
|math-p44-20261009-064648-Focused|0|ctest.log: 3 件中 0 失敗|[生証拠](../build/math-p44-20261009-064648-Focused/)|
|math-p44-20261009-064921-Real|4|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-064921-Real/)|
|math-p44-20261009-065115-Real|未記録|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-065115-Real/)|
|math-p44-20261009-065209-Real|0|UI 62 検査、失敗 0|[生証拠](../build/math-p44-20261009-065209-Real/)|
|math-p44-20261009-065732-Real|未記録|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-065732-Real/)|
|math-p44-20261009-065842-Real|未記録|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-065842-Real/)|
|math-p44-20261009-065930-Real|1|UI 84 検査、失敗 1 / 原寸全画素比較 5 frame、失敗 0|[生証拠](../build/math-p44-20261009-065930-Real/)|
|math-p44-20261009-070130-Real|0|UI 86 検査、失敗 0 / 原寸全画素比較 5 frame、失敗 0|[生証拠](../build/math-p44-20261009-070130-Real/)|
|math-p44-20261009-Final-BuildIndependent|0|independent.log: 1084 件中 0 失敗|[生証拠](../build/math-p44-20261009-Final-BuildIndependent/)|
|math-p44-20261009-Final-BuildIndependent-02|0|independent.log: 1084 件中 0 失敗|[生証拠](../build/math-p44-20261009-Final-BuildIndependent-02/)|
|math-p44-20261009-Final-Focused|8|ctest.log: 4 件中 1 失敗|[生証拠](../build/math-p44-20261009-Final-Focused/)|
|math-p44-20261009-Final-Lint|0|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-Final-Lint/)|
|math-p44-20261009-Final-Lint-02|0|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-Final-Lint-02/)|
|math-p44-20261009-Final-Mutations|未記録|変異 0/1 検出 / last-function-restored.log: 1 件中 0 失敗 / last-function.log: 1 件中 1 失敗|[生証拠](../build/math-p44-20261009-Final-Mutations/)|
|math-p44-20261009-Final-Mutations-02|0|変異 6/6 検出 / invalid-source-restored.log: 1 件中 0 失敗 / invalid-source.log: 1 件中 1 失敗 / last-function-restored.log: 1 件中 0 失敗 / last-function.log: 1 件中 1 失敗 / narrow-layout-restored.log: 1 件中 0 失敗 / narrow-layout.log: 1 件中 1 失敗 / old-key-restored.log: 1 件中 0 失敗 / old-key.log: 1 件中 1 失敗 / reorder-id-restored.log: 1 件中 0 失敗 / reorder-id.log: 1 件中 1 失敗 / split-draw-restored.log: 1 件中 0 失敗 / split-draw.log: 1 件中 1 失敗|[生証拠](../build/math-p44-20261009-Final-Mutations-02/)|
|math-p44-20261009-Final-Real|0|UI 99 検査、失敗 0 / 原寸全画素比較 5 frame、失敗 0|[生証拠](../build/math-p44-20261009-Final-Real/)|
|math-p44-20261009-Final-Regressions|0,0,0,0,0,0,0,0,0,0,0,0,8|regressions.log: 26 件中 1 失敗|[生証拠](../build/math-p44-20261009-Final-Regressions/)|
|math-p44-20261009-Final-Release|0|release.log: 1489 件中 0 失敗|[生証拠](../build/math-p44-20261009-Final-Release/)|
|math-p44-20261009-Lint-01|0|build・初期化・終了のログを参照|[生証拠](../build/math-p44-20261009-Lint-01/)|
|math-p44-20261009-Mutations-01|未記録|変異 6/6 検出 / invalid-source-restored.log: 1 件中 0 失敗 / invalid-source.log: 1 件中 1 失敗 / last-function-restored.log: 1 件中 0 失敗 / last-function.log: 1 件中 1 失敗 / narrow-layout-restored.log: 1 件中 0 失敗 / narrow-layout.log: 1 件中 1 失敗 / old-key-restored.log: 1 件中 0 失敗 / old-key.log: 1 件中 1 失敗 / reorder-id-restored.log: 1 件中 0 失敗 / reorder-id.log: 1 件中 1 失敗 / split-draw-restored.log: 1 件中 0 失敗 / split-draw.log: 1 件中 1 失敗|[生証拠](../build/math-p44-20261009-Mutations-01/)|

各 gate の `source-state.json` は HEAD とソース SHA256、`sources/` は当該取得の実ソース。変異は original SHA256 の完全一致復元と復元後の正常試験を要求する。
件数は上の生 JSON と CTest log から生成し、性能値を手で転記していない。
