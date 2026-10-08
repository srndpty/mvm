# P4-1 初期検証の記録

2026-10-08。作業中の未コミット source。以下の実行は実装途中の確認であり、閉鎖 gate ではない。
tool 実行の stdout/stderr は会話の実行記録にも残る。既存 P4-0/P3 証拠は変更していない。

|コマンド|観測|分類|
|---|---|---|
|`pwsh scripts/build.ps1 -Target mvm_test_graph_numeric` (sandbox 内)|CMake configure 成功後、Ninja 停止。PID 33408/12916、CPU ほぼ不変、child compiler なし、`.ninja_log` 不変|環境制約、製品 FAIL ではない|
|`pwsh scripts/build-diagnostics.ps1 -Preset ucrt64-release`|sandbox 内では command line の取得権限なし。外側の CIM で上記 PID の target と親子関係を確認|読み取り診断|
|`Stop-Process -Id 12916,33408; pwsh scripts/build.ps1 -Target mvm_test_graph_numeric` (sandbox 外)|対象 PID だけを終了後、公式ビルド成功|PASS|
|`ctest --test-dir build/ucrt64-release -R '^graph_numeric$' --output-on-failure --timeout 120`|数値モジュール focused test 成功|PASS、途中版|
|`pwsh scripts/build.ps1 -Target mvm_test_graph_domain` (sandbox 外)|test が存在しない `LinkMode::ClipOnly` を参照して compile 失敗|FAIL、test source を既存の `Single` へ修正|
|`pwsh scripts/build.ps1 -Target mvm_test_graph_domain -ReuseConfigure`|修正後ビルド成功|PASS|
|`ctest --test-dir build/ucrt64-release -R '^graph_domain$' -V --timeout 120`|domain/schema focused test 成功|PASS、途中版|
|`pwsh scripts/build.ps1 -Target mvm_test_math_controller`|controller focused target のビルド成功|PASS|
|`ctest --test-dir build/ucrt64-release -R '^graph_controller_history$' --output-on-failure --timeout 120` (sandbox 内)|初期 Project 保存、Graph 作成、作成 Undo の対照が失敗。初期保存後の検査へ進めない|FAIL、sandbox 保存経路|
|同じ controller test (sandbox 外)|保存と Graph 履歴の検査が成功|PASS、環境の切り分け|

同条件の retry で PASS を選別していない。compiler source の修正または sandbox の有無を明示した別条件である。
最終 source snapshot、正式 focused/negative/mutation/regression/gate は専用 runner の新規証拠 directory に保存する。

`build/math-p41-20261008-185942-Mutations/` は最初の変異 gate の失敗を保存する。
前半の変異は assertion で検出したが、`jump-connected/test.log` は変異したのに PASS だった。
原因は jump の診断数だけを見て、出力 segment が空かを検査していなかったこと。
負例に NoFiniteSamples と空 geometry の照合を加えた。製品の閾値は変更していない。
新規 `build/math-p41-20261008-190115-Mutations/` では全15変異を assertion failure で検出し、
各 source の SHA256 を復元照合した。古い run と変異 source/test/build log は保持する。

専用スクリプトの事前静的検査では、手動コマンドが誤った Analyzer version 1.24.0 を指定して
module が見つからなかった。repo の `PSScriptAnalyzer.version` は1.25.0であり、
その値を読み込んだ専用三スクリプトの検査は通過した。依存を導入・変更していない。
