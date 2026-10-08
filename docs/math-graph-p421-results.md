# P4-2.1 検証の機械集計

`scripts/math-p42-report.ps1` が保存済み JSON・TSV・ログから生成する。失敗を含めて保存する。

|証拠|gate|終了コード|結果|
|---|---|---:|---|
|[math-p421-20261009-015050-Focused](../build/math-p421-20261009-015050-Focused/commands.json)|build|0|PASS|
|[math-p421-20261009-015050-Focused](../build/math-p421-20261009-015050-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p421-20261009-015122-Mutation](../build/math-p421-20261009-015122-Mutation/commands.json)|publication-mutation|1|FAIL|
|[math-p421-20261009-015146-Mutation](../build/math-p421-20261009-015146-Mutation/commands.json)|publication-mutation|0|PASS|
|[math-p421-20261009-015310-Focused](../build/math-p421-20261009-015310-Focused/commands.json)|build|0|PASS|
|[math-p421-20261009-015310-Focused](../build/math-p421-20261009-015310-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p421-20261009-015335-Lint](../build/math-p421-20261009-015335-Lint/commands.json)|lint|0|PASS|
|[math-p421-20261009-015358-Real](../build/math-p421-20261009-015358-Real/commands.json)|real-render|0|PASS|
|[math-p421-20261009-015429-BuildIndependent](../build/math-p421-20261009-015429-BuildIndependent/commands.json)|independent|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|regressions|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_graph_numeric|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_graph_domain|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_manim_math_tex|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_manim_equation_sequence|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_math_render|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_math_raster_cache|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|build-mvm_test_process|0|PASS|
|[math-p421-20261009-015749-Regressions](../build/math-p421-20261009-015749-Regressions/commands.json)|graph-manim-artifact-process|0|PASS|
|[math-p421-20261009-015903-Mutation](../build/math-p421-20261009-015903-Mutation/commands.json)|publication-mutation|0|PASS|
|[math-p421-20261009-020001-Release](../build/math-p421-20261009-020001-Release/commands.json)|release|0|PASS|

## math-p421-20261009-015146-Mutation の応答性変異

build=0、test=1、応答性 assertion による検出=True。

## math-p421-20261009-015358-Real の水平線被覆積分

|canvas|指定 pixel 幅|被覆積分 pixel 幅|絶対差|
|---|---:|---:|---:|
|1920×1080|0.1|0.133333|0.033333|
|1920×1080|3|3|0|
|1920×1080|64|64|0|
|1280×720|0.0666667|0.133333|0.0666663|
|1280×720|2|2|0|
|1280×720|42.6667|42.6667|0|
|854×480|0.0444444|0|0.0444444|
|854×480|1.33333|1.33333|0|
|854×480|28.4444|28.4|0.0444000000000031|

これは Cairo の固定した水平 fixture の被覆量子化を含む。preview/export の全画素同値に許容差を導入しない。

## math-p421-20261009-015429-BuildIndependent/independent.log

通過 1080/1080、失敗 0。

```text
=== テスト種別ごとの結果 ===

Preset         Kind        Total  Ran Passed Failed Exit Note
------         ----        -----  --- ------ ------ ---- ----
ucrt64-release 通常 非依存  1080 1080   1080      0    0 


全テスト通過
```

## math-p421-20261009-015749-Regressions/graph-manim-artifact-process.log

通過 10/10、失敗 0。

## math-p421-20261009-015749-Regressions/regressions.log

通過 2/2、失敗 0。
通過 7/7、失敗 0。

## math-p421-20261009-015903-Mutation の応答性変異

build=0、test=1、応答性 assertion による検出=True。

## math-p421-20261009-020001-Release/release.log

通過 1478/1478、失敗 0。

```text
=== テスト種別ごとの結果 ===

Preset         Kind Total  Ran Passed Failed Exit Note
------         ---- -----  --- ------ ------ ---- ----
ucrt64-release 通常  1478 1478   1478      0    0
```
