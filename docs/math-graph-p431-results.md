# P4-3.1 検証の機械集計

`scripts/math-p431-report.ps1` が新規証拠の command JSON と native TSV から生成する。初回失敗も保持する。

|証拠|gate|終了コード|結果|
|---|---|---:|---|
|[math-p431-20261009-BuildIndependent-01](../build/math-p431-20261009-BuildIndependent-01/commands.json)|independent|0|PASS|
|[math-p431-20261009-BuildIndependent-01](../build/math-p431-20261009-BuildIndependent-01/ctest-raw.log)|CTest 生記録|—|通過 1081、失敗 0|
|[math-p431-20261009-Focused-01](../build/math-p431-20261009-Focused-01/commands.json)|build|0|PASS|
|[math-p431-20261009-Focused-01](../build/math-p431-20261009-Focused-01/commands.json)|cache|0|PASS|
|[math-p431-20261009-Focused-01](../build/math-p431-20261009-Focused-01/commands.json)|continuous|0|PASS|
|[math-p431-20261009-Focused-01](../build/math-p431-20261009-Focused-01/commands.json)|build-publication|1|FAIL|
|[math-p431-20261009-Focused-02](../build/math-p431-20261009-Focused-02/commands.json)|build|0|PASS|
|[math-p431-20261009-Focused-02](../build/math-p431-20261009-Focused-02/commands.json)|cache|1|FAIL|
|[math-p431-20261009-Focused-03](../build/math-p431-20261009-Focused-03/commands.json)|build|0|PASS|
|[math-p431-20261009-Focused-03](../build/math-p431-20261009-Focused-03/commands.json)|cache|0|PASS|
|[math-p431-20261009-Focused-03](../build/math-p431-20261009-Focused-03/commands.json)|continuous|0|PASS|
|[math-p431-20261009-Focused-03](../build/math-p431-20261009-Focused-03/commands.json)|build-publication|0|PASS|
|[math-p431-20261009-Focused-03](../build/math-p431-20261009-Focused-03/commands.json)|publication|0|PASS|
|[math-p431-20261009-Focused-04](../build/math-p431-20261009-Focused-04/commands.json)|build|0|PASS|
|[math-p431-20261009-Focused-04](../build/math-p431-20261009-Focused-04/commands.json)|cache|0|PASS|
|[math-p431-20261009-Focused-04](../build/math-p431-20261009-Focused-04/commands.json)|continuous|0|PASS|
|[math-p431-20261009-Focused-04](../build/math-p431-20261009-Focused-04/commands.json)|build-publication|0|PASS|
|[math-p431-20261009-Focused-04](../build/math-p431-20261009-Focused-04/commands.json)|publication|0|PASS|
|[math-p431-20261009-Lint-01](../build/math-p431-20261009-Lint-01/commands.json)|lint|0|PASS|
|[math-p431-20261009-Lint-02](../build/math-p431-20261009-Lint-02/commands.json)|lint|0|PASS|
|[math-p431-20261009-Lint-03](../build/math-p431-20261009-Lint-03/commands.json)|lint|0|PASS|
|[math-p431-20261009-Real-01](../build/math-p431-20261009-Real-01/commands.json)|build|0|PASS|
|[math-p431-20261009-Real-01](../build/math-p431-20261009-Real-01/commands.json)|video-fixture|0|PASS|
|[math-p431-20261009-Real-01](../build/math-p431-20261009-Real-01/commands.json)|verify-video-fixture|0|PASS|
|[math-p431-20261009-Real-01](../build/math-p431-20261009-Real-01/commands.json)|native|0|PASS|
|[math-p431-20261009-Real-02](../build/math-p431-20261009-Real-02/commands.json)|build|0|PASS|
|[math-p431-20261009-Real-02](../build/math-p431-20261009-Real-02/commands.json)|video-fixture|0|PASS|
|[math-p431-20261009-Real-02](../build/math-p431-20261009-Real-02/commands.json)|verify-video-fixture|0|PASS|
|[math-p431-20261009-Real-02](../build/math-p431-20261009-Real-02/commands.json)|native|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_graph_preview_cache|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_subtitle_controller|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_math_controller|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_graph_numeric|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_graph_domain|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_graph_render|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_equation_preview_controller|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_timeline_preview_mapping|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_still_layer_compositor|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|build-mvm_test_clip_effects|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/commands.json)|regressions|0|PASS|
|[math-p431-20261009-Regressions-01](../build/math-p431-20261009-Regressions-01/ctest-raw.log)|CTest 生記録|—|通過 20、失敗 0|
|[math-p431-20261009-Release-01](../build/math-p431-20261009-Release-01/commands.json)|release|0|PASS|
|[math-p431-20261009-Release-01](../build/math-p431-20261009-Release-01/ctest-raw.log)|CTest 生記録|—|通過 1484、失敗 0|

## 固定 composition の native clock

[math-p431-20261009-Real-01](../build/math-p431-20261009-Real-01/native/continuous-native.tsv): 比較 20 frame、不一致 0 frame、case 内で revision が変わった群 0。
native 全体: 検査 128、失敗 0。

[math-p431-20261009-Real-02](../build/math-p431-20261009-Real-02/native/continuous-native.tsv): 比較 24 frame、不一致 0 frame、case 内で revision が変わった群 0。
native 全体: 検査 148、失敗 0。
clock 試験の seek request 0、提示 24 frame、最終 composition revision 6。

## 変異と A/B
[math-p431-20261009-Mutations-01](../build/math-p431-20261009-Mutations-01/mutations.json): single-frame-capture、負例の終了 1、検出 True。
[math-p431-20261009-Mutations-01](../build/math-p431-20261009-Mutations-01/mutations.json): mixer-visible-cycle、負例の終了 8、検出 True。
[math-p431-20261009-Mutations-01](../build/math-p431-20261009-Mutations-01/mutations.json): unconditional-graph-cache-notification、負例の終了 8、検出 True。
[math-p431-mixer-ab-20261009-044257](../build/math-p431-mixer-ab-20261009-044257/comparison.json): baseline、終了 0、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-044257](../build/math-p431-mixer-ab-20261009-044257/comparison.json): current、終了 0、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-044426](../build/math-p431-mixer-ab-20261009-044426/comparison.json): baseline、終了 1、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-044426](../build/math-p431-mixer-ab-20261009-044426/comparison.json): current、終了 1、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-044506](../build/math-p431-mixer-ab-20261009-044506/comparison.json): baseline、終了 1、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-044506](../build/math-p431-mixer-ab-20261009-044506/comparison.json): current、終了 0、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-045109](../build/math-p431-mixer-ab-20261009-045109/comparison.json): baseline、終了 0、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。
[math-p431-mixer-ab-20261009-045109](../build/math-p431-mixer-ab-20261009-045109/comparison.json): current、終了 0、fixture SHA256 79111302D87198FC6BACAB19F9AFDBADCF503A392CA82915931AAB9EC58F52AD。

P4-3 の BuildIndependent 1079/1080、Release 1477/1479 は [元の結果](math-graph-p43-results.md) と元 directory を保持する。
