# P4-2 検証の機械集計

`scripts/math-p42-report.ps1` が保存済み JSON・TSV・ログから生成する。失敗を含めて保存する。

|証拠|gate|終了コード|結果|
|---|---|---:|---|
|[math-p42-20261009-003435-Real](../build/math-p42-20261009-003435-Real/commands.json)|real-render|1|FAIL|
|[math-p42-20261009-004047-Focused](../build/math-p42-20261009-004047-Focused/commands.json)|build|0|PASS|
|[math-p42-20261009-004047-Focused](../build/math-p42-20261009-004047-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p42-20261009-004344-Mutations](../build/math-p42-20261009-004344-Mutations/commands.json)|mutations|0|PASS|
|[math-p42-20261009-004742-Focused](../build/math-p42-20261009-004742-Focused/commands.json)|build|0|PASS|
|[math-p42-20261009-004742-Focused](../build/math-p42-20261009-004742-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p42-20261009-005115-Focused](../build/math-p42-20261009-005115-Focused/commands.json)|build|0|PASS|
|[math-p42-20261009-005115-Focused](../build/math-p42-20261009-005115-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p42-20261009-005354-Focused](../build/math-p42-20261009-005354-Focused/commands.json)|build|0|PASS|
|[math-p42-20261009-005354-Focused](../build/math-p42-20261009-005354-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p42-20261009-005506-Real](../build/math-p42-20261009-005506-Real/commands.json)|real-render|0|PASS|
|[math-p42-20261009-005837-Mutations](../build/math-p42-20261009-005837-Mutations/commands.json)|mutations|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|regressions|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_graph_numeric|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_graph_domain|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_manim_math_tex|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_manim_equation_sequence|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_math_render|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_math_raster_cache|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|build-mvm_test_process|0|PASS|
|[math-p42-20261009-010134-Regressions](../build/math-p42-20261009-010134-Regressions/commands.json)|graph-manim-artifact-process|0|PASS|
|[math-p42-20261009-010203-Lint](../build/math-p42-20261009-010203-Lint/commands.json)|lint|0|PASS|
|[math-p42-20261009-010327-BuildIndependent](../build/math-p42-20261009-010327-BuildIndependent/commands.json)|independent|0|PASS|
|[math-p42-20261009-011019-Focused](../build/math-p42-20261009-011019-Focused/commands.json)|build|0|PASS|
|[math-p42-20261009-011019-Focused](../build/math-p42-20261009-011019-Focused/commands.json)|presentation-artifact|0|PASS|
|[math-p42-20261009-011100-Real](../build/math-p42-20261009-011100-Real/commands.json)|real-render|0|PASS|
|[math-p42-20261009-011338-Mutations](../build/math-p42-20261009-011338-Mutations/commands.json)|mutations|0|PASS|
|[math-p42-20261009-011640-Release](../build/math-p42-20261009-011640-Release/commands.json)|release|0|PASS|
|[math-p42-20261009-012027-Lint](../build/math-p42-20261009-012027-Lint/commands.json)|lint|0|PASS|

## math-p42-20261009-003435-Real の水平線被覆積分

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

## math-p42-20261009-004344-Mutations の変異

検出 17/17。ビルド終了0・検証終了1だけを検出として数える。

|変異|build|test|検出|
|---|---:|---:|---|
|backend-resamples|0|1|True|
|segment-boundaries-ignored|0|1|True|
|function-order-reversed|0|1|True|
|alpha-forced-255|0|1|True|
|premultiplied-as-straight|0|1|True|
|stroke-scaling-disabled|0|1|True|
|grid-tied-to-axes|0|1|True|
|label-in-plot|0|1|True|
|draw-i-plus-one|0|1|True|
|draw-backend-time-authority|0|1|True|
|ownership-in-static-key|0|1|True|
|draw-n-in-static-key|0|1|True|
|provenance-before-validation|0|1|True|
|missing-draw-frame-ready|0|1|True|
|sha-validation-skipped|0|1|True|
|stale-publishes-ready|0|1|True|
|cancelled-publishes-ready|0|1|True|

## math-p42-20261009-005506-Real の水平線被覆積分

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

## math-p42-20261009-005837-Mutations の変異

検出 17/17。ビルド終了0・検証終了1だけを検出として数える。

|変異|build|test|検出|
|---|---:|---:|---|
|backend-resamples|0|1|True|
|segment-boundaries-ignored|0|1|True|
|function-order-reversed|0|1|True|
|alpha-forced-255|0|1|True|
|premultiplied-as-straight|0|1|True|
|stroke-scaling-disabled|0|1|True|
|grid-tied-to-axes|0|1|True|
|label-in-plot|0|1|True|
|draw-i-plus-one|0|1|True|
|draw-backend-time-authority|0|1|True|
|ownership-in-static-key|0|1|True|
|draw-n-in-static-key|0|1|True|
|provenance-before-validation|0|1|True|
|missing-draw-frame-ready|0|1|True|
|sha-validation-skipped|0|1|True|
|stale-publishes-ready|0|1|True|
|cancelled-publishes-ready|0|1|True|

## math-p42-20261009-010134-Regressions/graph-manim-artifact-process.log

通過 10/10、失敗 0。

## math-p42-20261009-010134-Regressions/regressions.log

通過 2/2、失敗 0。
通過 7/7、失敗 0。

## math-p42-20261009-010327-BuildIndependent/independent.log

通過 1080/1080、失敗 0。

```text
=== テスト種別ごとの結果 ===

Preset         Kind        Total  Ran Passed Failed Exit Note
------         ----        -----  --- ------ ------ ---- ----
ucrt64-release 通常 非依存  1080 1080   1080      0    0 


全テスト通過
```

## math-p42-20261009-011100-Real の水平線被覆積分

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

## math-p42-20261009-011338-Mutations の変異

検出 17/17。ビルド終了0・検証終了1だけを検出として数える。

|変異|build|test|検出|
|---|---:|---:|---|
|backend-resamples|0|1|True|
|segment-boundaries-ignored|0|1|True|
|function-order-reversed|0|1|True|
|alpha-forced-255|0|1|True|
|premultiplied-as-straight|0|1|True|
|stroke-scaling-disabled|0|1|True|
|grid-tied-to-axes|0|1|True|
|label-in-plot|0|1|True|
|draw-i-plus-one|0|1|True|
|draw-backend-time-authority|0|1|True|
|ownership-in-static-key|0|1|True|
|draw-n-in-static-key|0|1|True|
|provenance-before-validation|0|1|True|
|missing-draw-frame-ready|0|1|True|
|sha-validation-skipped|0|1|True|
|stale-publishes-ready|0|1|True|
|cancelled-publishes-ready|0|1|True|

## math-p42-20261009-011640-Release/release.log

通過 1478/1478、失敗 0。

```text
=== テスト種別ごとの結果 ===

Preset         Kind Total  Ran Passed Failed Exit Note
------         ---- -----  --- ------ ------ ---- ----
ucrt64-release 通常  1478 1478   1478      0    0
```
