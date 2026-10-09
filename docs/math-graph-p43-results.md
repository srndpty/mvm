# P4-3 検証の機械集計

`scripts/math-p43-report.ps1` が保存済み JSON・TSV から生成する。過去の失敗も保持する。

|証拠|gate|終了コード|結果|
|---|---|---:|---|
|[math-p43-20261009-023851-Real](../build/math-p43-20261009-023851-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-023851-Real](../build/math-p43-20261009-023851-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-023851-Real](../build/math-p43-20261009-023851-Real/commands.json)|native|-1073741819|FAIL|
|[math-p43-20261009-024127-Real](../build/math-p43-20261009-024127-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-024127-Real](../build/math-p43-20261009-024127-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-024127-Real](../build/math-p43-20261009-024127-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-024306-Real](../build/math-p43-20261009-024306-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-024306-Real](../build/math-p43-20261009-024306-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-024306-Real](../build/math-p43-20261009-024306-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-024422-Focused](../build/math-p43-20261009-024422-Focused/commands.json)|build|0|PASS|
|[math-p43-20261009-024422-Focused](../build/math-p43-20261009-024422-Focused/commands.json)|cache|0|PASS|
|[math-p43-20261009-024512-Real](../build/math-p43-20261009-024512-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-024512-Real](../build/math-p43-20261009-024512-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-024512-Real](../build/math-p43-20261009-024512-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-024853-Real](../build/math-p43-20261009-024853-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-024853-Real](../build/math-p43-20261009-024853-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-024853-Real](../build/math-p43-20261009-024853-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-025059-Real](../build/math-p43-20261009-025059-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-025059-Real](../build/math-p43-20261009-025059-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-025059-Real](../build/math-p43-20261009-025059-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-025640-Real](../build/math-p43-20261009-025640-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-025640-Real](../build/math-p43-20261009-025640-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-025640-Real](../build/math-p43-20261009-025640-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-025729-Real](../build/math-p43-20261009-025729-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-025729-Real](../build/math-p43-20261009-025729-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-025729-Real](../build/math-p43-20261009-025729-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-025823-Real](../build/math-p43-20261009-025823-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-025823-Real](../build/math-p43-20261009-025823-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-025823-Real](../build/math-p43-20261009-025823-Real/commands.json)|native|1|FAIL|
|[math-p43-20261009-025939-Real](../build/math-p43-20261009-025939-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-025939-Real](../build/math-p43-20261009-025939-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-025939-Real](../build/math-p43-20261009-025939-Real/commands.json)|verify-video-fixture|0|PASS|
|[math-p43-20261009-025939-Real](../build/math-p43-20261009-025939-Real/commands.json)|native|0|PASS|
|[math-p43-20261009-030137-Focused](../build/math-p43-20261009-030137-Focused/commands.json)|build|1|FAIL|
|[math-p43-20261009-030231-Focused](../build/math-p43-20261009-030231-Focused/commands.json)|build|0|PASS|
|[math-p43-20261009-030231-Focused](../build/math-p43-20261009-030231-Focused/commands.json)|cache|0|PASS|
|[math-p43-20261009-032358-Focused](../build/math-p43-20261009-032358-Focused/commands.json)|build|0|PASS|
|[math-p43-20261009-032358-Focused](../build/math-p43-20261009-032358-Focused/commands.json)|cache|1|FAIL|
|[math-p43-20261009-033408-Focused](../build/math-p43-20261009-033408-Focused/commands.json)|build|0|PASS|
|[math-p43-20261009-033408-Focused](../build/math-p43-20261009-033408-Focused/commands.json)|cache|0|PASS|
|[math-p43-20261009-033429-Focused](../build/math-p43-20261009-033429-Focused/commands.json)|build|0|PASS|
|[math-p43-20261009-033429-Focused](../build/math-p43-20261009-033429-Focused/commands.json)|cache|0|PASS|
|[math-p43-20261009-033458-Real](../build/math-p43-20261009-033458-Real/commands.json)|build|0|PASS|
|[math-p43-20261009-033458-Real](../build/math-p43-20261009-033458-Real/commands.json)|video-fixture|0|PASS|
|[math-p43-20261009-033458-Real](../build/math-p43-20261009-033458-Real/commands.json)|verify-video-fixture|0|PASS|
|[math-p43-20261009-033458-Real](../build/math-p43-20261009-033458-Real/commands.json)|native|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_graph_preview_cache|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_math_controller|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_graph_numeric|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_graph_domain|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_graph_render|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_equation_preview_controller|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_timeline_preview_mapping|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_still_layer_compositor|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|build-mvm_test_clip_effects|0|PASS|
|[math-p43-20261009-033537-Regressions](../build/math-p43-20261009-033537-Regressions/commands.json)|regressions|0|PASS|
|[math-p43-20261009-033806-Lint](../build/math-p43-20261009-033806-Lint/commands.json)|lint|0|PASS|
|[math-p43-20261009-033857-BuildIndependent](../build/math-p43-20261009-033857-BuildIndependent/commands.json)|independent|1|FAIL|
|[math-p43-20261009-034831-Release](../build/math-p43-20261009-034831-Release/commands.json)|release|1|FAIL|

## math-p43-20261009-024306-Real の native 検査

検査 13、失敗 6。artifact Ready まで 7525 ms。
画素比較の行が無いので native 合成の成功証拠にはしない。

## math-p43-20261009-024512-Real の native 検査

検査 13、失敗 6。artifact Ready まで 7895 ms。
画素比較の行が無いので native 合成の成功証拠にはしない。

## math-p43-20261009-024853-Real の native 検査

検査 8、失敗 1。artifact Ready まで 7400 ms。
画素比較の行が無いので native 合成の成功証拠にはしない。

## math-p43-20261009-025059-Real の native 検査

検査 37、失敗 6。artifact Ready まで 7615 ms。

|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|
|---:|---:|---:|---:|---:|
|2|147|460800|460800|57600|
|0|75|460800|460800|57254|
|1|134|691200|691200|57252|
|3|133|921600|921600|57248|
|4|83|921600|921600|57248|
|9|104|921600|921600|57248|

所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。

## math-p43-20261009-025640-Real の native 検査

検査 37、失敗 6。artifact Ready まで 7974 ms。

|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|
|---:|---:|---:|---:|---:|
|2|135|460800|460800|57600|
|0|74|460800|460800|57254|
|1|150|691200|691200|57252|
|3|117|921600|921600|57248|
|4|100|921600|921600|57248|
|9|99|921600|921600|57248|

所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。

## math-p43-20261009-025729-Real の native 検査

検査 8、失敗 1。artifact Ready まで 7534 ms。
画素比較の行が無いので native 合成の成功証拠にはしない。

## math-p43-20261009-025823-Real の native 検査

検査 37、失敗 6。artifact Ready まで 7416 ms。

|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|
|---:|---:|---:|---:|---:|
|2|139|460800|460800|57600|
|0|81|460800|460800|57254|
|1|116|691200|691200|57252|
|3|133|921600|921600|57248|
|4|83|921600|921600|57248|
|9|100|921600|921600|57248|

所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。

## math-p43-20261009-025939-Real の native 検査

検査 37、失敗 0。artifact Ready まで 7642 ms。

|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|
|---:|---:|---:|---:|---:|
|2|189|691200|691200|0|
|0|156|921600|921600|0|
|1|99|921600|921600|0|
|3|83|921600|921600|0|
|4|89|921600|921600|0|
|9|111|921600|921600|0|

所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。

## math-p43-20261009-033458-Real の native 検査

検査 42、失敗 0。artifact Ready まで 7550 ms。

|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|
|---:|---:|---:|---:|---:|
|2|191|691200|691200|0|
|0|164|921600|921600|0|
|1|116|921600|921600|0|
|3|100|921600|921600|0|
|4|84|921600|921600|0|
|9|83|921600|921600|0|

所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。
