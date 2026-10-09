# P4-3.2 の実行結果

この文書は `scripts/math-p432-report.ps1` が生ログ・JSON・TSV から生成する。過去の FAIL と P4-3.1 の PASS は元の証拠に保持する。

|証拠|gate|終了コード|結果|
|---|---|---:|---|
|[math-p432-20261009-BuildIndependent-01](../build/math-p432-20261009-BuildIndependent-01/commands.json)|independent|0|PASS|
|[math-p432-20261009-BuildIndependent-01](../build/math-p432-20261009-BuildIndependent-01/ctest-raw.log)|CTest 生記録|—|通過 1081、失敗 0|
|[math-p432-20261009-Effects-01](../build/math-p432-20261009-Effects-01/commands.json)|build|0|PASS|
|[math-p432-20261009-Effects-01](../build/math-p432-20261009-Effects-01/commands.json)|effects|1|FAIL|
|[math-p432-20261009-Effects-02](../build/math-p432-20261009-Effects-02/commands.json)|build|0|PASS|
|[math-p432-20261009-Effects-02](../build/math-p432-20261009-Effects-02/commands.json)|effects|0|PASS|
|[math-p432-20261009-Focused-01](../build/math-p432-20261009-Focused-01/commands.json)|build|0|PASS|
|[math-p432-20261009-Focused-01](../build/math-p432-20261009-Focused-01/commands.json)|cache|0|PASS|
|[math-p432-20261009-Focused-01](../build/math-p432-20261009-Focused-01/commands.json)|continuous|0|PASS|
|[math-p432-20261009-Focused-01](../build/math-p432-20261009-Focused-01/commands.json)|build-publication|0|PASS|
|[math-p432-20261009-Focused-01](../build/math-p432-20261009-Focused-01/commands.json)|publication|0|PASS|
|[math-p432-20261009-Lint-01](../build/math-p432-20261009-Lint-01/commands.json)|lint|0|PASS|
|[math-p432-20261009-Real-01](../build/math-p432-20261009-Real-01/commands.json)|build|0|PASS|
|[math-p432-20261009-Real-01](../build/math-p432-20261009-Real-01/commands.json)|video-fixture|0|PASS|
|[math-p432-20261009-Real-01](../build/math-p432-20261009-Real-01/commands.json)|verify-video-fixture|0|PASS|
|[math-p432-20261009-Real-01](../build/math-p432-20261009-Real-01/commands.json)|native|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_graph_native_preview|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_transition_preview|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_graph_preview_cache|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_subtitle_controller|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_math_controller|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_graph_numeric|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_graph_domain|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_graph_render|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_equation_preview_controller|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_timeline_preview_mapping|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_still_layer_compositor|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|build-mvm_test_clip_effects|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/commands.json)|regressions|0|PASS|
|[math-p432-20261009-Regressions-01](../build/math-p432-20261009-Regressions-01/ctest-raw.log)|CTest 生記録|—|通過 22、失敗 0|
|[math-p432-20261009-Release-01](../build/math-p432-20261009-Release-01/commands.json)|release|0|PASS|
|[math-p432-20261009-Release-01](../build/math-p432-20261009-Release-01/ctest-raw.log)|CTest 生記録|—|通過 1485、失敗 0|

## 再利用と毎 frame 新規構築の比較

[math-p432-20261009-Effects-01](../build/math-p432-20261009-Effects-01/native/continuous-effects.tsv): 比較 28 frame、非再利用基準との不一致 0 frame、独立 oracle との不一致 0 frame、case 内の revision 変化 0 群。検査 326、失敗 19、seek request 0。

[math-p432-20261009-Effects-02](../build/math-p432-20261009-Effects-02/native/continuous-effects.tsv): 比較 36 frame、非再利用基準との不一致 0 frame、独立 oracle との不一致 0 frame、case 内の revision 変化 0 群。検査 418、失敗 0、seek request 0。

[math-p432-20261009-Real-01](../build/math-p432-20261009-Real-01/native/continuous-effects.tsv): 比較 40 frame、非再利用基準との不一致 0 frame、独立 oracle との不一致 0 frame、case 内の revision 変化 0 群。検査 464、失敗 0、seek request 0。
Draw の既存連続試験: 比較 24 frame、不一致 0 frame。Manim・動画 alpha・ClipEffects を含む native 全体: 検査 612、失敗 0。

## 負例と復元

[math-p432-20261009-Mutation-01](../build/math-p432-20261009-Mutation-01/mutation.json): graph-frozen-effects、終了 1、検出 True、位置・拡大の不一致 3 frame、復元 hash 一致 True、復元試験の終了 0。

Effects-01 の FAIL は初回 animation record 作成直後で memo がまだ確立していない試験条件による再利用確認の失敗である。生記録を保持し、環境干渉へ分類変更しない。Effects-02 以降は memo 確立後の snapshot を比較する。

P4-3 の BuildIndependent 1079/1080 と Release 1477/1479 は [歴史的結果](math-graph-p43-results.md) に保持する。[P4-3.1 の PASS 証拠](math-graph-p431-results.md) も変更しない。
