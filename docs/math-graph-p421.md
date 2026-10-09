# P4-2.1 publication 応答性と静止 cache 契約

2026-10-09。P4-2 の後続修正。schema22、sampling、Draw timing、RGBA 算術、renderer の意味は変更しない。
P4-3 は未着手。commit/push は行わない。

## 公開と生存期間

`PublicationAuthority::generate` は初回の世代確認と token 取得だけを mutex 内で行う。
既存 cache の全画素検証、renderer、job の検証、staging 作成、コピー、staging の全画素検証、
manifest の atomic write は mutex 外で行う。コピーごとと検証前に短い authority 確認を挟む。
最後の世代・取消・shutdown 確認と非置換 directory rename だけを同じ mutex 内で行う。
rename が既存の公開線形化点であり、supersede/shutdown と直列化する。
公開後の全画素検証も mutex 外で行い、結果を返す直前に authority を再確認する。
rename 後に無効化された場合は有効な immutable cache を残して typed failure を返す。

`shutdown` は authority を無効化するだけであり、renderer や staging I/O の完了を待たない。
同期呼出しを所有する側は shutdown → 全 generate 呼出しの join → authority 破棄の順序を守る。
同時に destructor を実行することは契約外。request、renderer、cancel、observer の寿命も呼出し完了まで必要。
試験は staging を止めたまま shutdown 完了を確認し、解放後の Cancelled と join を確認してから破棄する。

同じ key の同時要求は別の `.pending-*` で staging し、非置換 rename の勝者だけが公開される。
敗者は勝者の artifact を全検証する。正常 cache を上書きしない。
pending は正常 manifest と全 PNG が揃っていても `validateArtifact` が拒否する。
失敗した pending と job は証拠として残るが、Ready の authority にはならない。

## 静止 cache は B を選択

P4-2/P4-2.1 が保証するのは Draw N に依存しない static identity であり、異なる Draw N 間の物理再利用ではない。
物理配置は次のまま維持する。

```text
<static-key>/static.png + manifest.txt
<draw-key-N>/static.png + frame-*.png + manifest.txt
```

同じ完全 artifact の再要求は全検証後に renderer を起動せず再利用する。
別の Draw N は別の artifact を生成し、static.png も再描画・再保存する。
renderer 呼出し回数と job 内 static.png の実在を確かめる負例付き契約試験で、この区別を固定する。
静止の物理共有には Draw manifest の外部依存、独立検証、共有 artifact の公開・欠損処理を追加する必要がある。
P4-2.1 の mutex 修正に形式の再設計を混ぜず、最適化の既知事項は [roadmap](roadmap.md) に置く。

## 検証

Copy/Validate の observer は処理境界の同期用であり、検証結果を変更しない。
promise/future のバリアで staging を停止し、その間に無効化の完了を要求する。
2秒の deadline は長時間 mutex の負例から試験を解放するための watchdog。
sleep による競争タイミング選択を主要証拠にしない。
新世代は古い staging が停止したまま同じ key を公開でき、古い世代は解放後に Superseded になる。
同じ key の2 job は異なる PNG を staging し、返却される pixel hash が勝者と一致することを検査する。

`scripts/math-p421-mutation.ps1` は staging 全体へ旧長時間 mutex を復元する。
ビルド成功・検証終了1・応答性 assertion の存在だけを検出と数え、source の元 SHA256 を復元検査する。
この負例では重複要求の二者バリアだけを無効にし、応答性バリアと検査は通常と同じものを使う。

【操作可】以下は通常検証と offline Manim 描画。通常 release は display-power lease と既存の背面・入力透過 GUI 試験を使う。

```powershell
pwsh scripts/math-p421-gate.ps1 -Stage Focused
pwsh scripts/math-p421-gate.ps1 -Stage Mutation
pwsh scripts/math-p421-gate.ps1 -Stage Real
pwsh scripts/math-p421-gate.ps1 -Stage Regressions
pwsh scripts/math-p421-gate.ps1 -Stage BuildIndependent
pwsh scripts/math-p421-gate.ps1 -Stage Lint
pwsh scripts/math-p421-gate.ps1 -Stage Release
pwsh scripts/math-p42-report.ps1 -RootFilter 'math-p421-*' -Title P4-2.1 -OutputPath docs/math-graph-p421-results.md
```

各 gate は新規 directory に全 source snapshot、HEAD、source SHA256 の `source-state.json`、
実行引数と終了コードの `commands.json`、全ログを保存する。旧 P4-2 証拠は変更しない。
[事実] 2026-10-09、必要な全 gate が通過したため P4-2.1 は PASS/CLOSED。
staging の Copy/Validate を停止したまま supersede/shutdown の無効化が完了し、
旧世代の公開拒否、新世代の成功、同じ key の勝者保持、取消と pending の非 authority を検証した。
旧長時間 mutex の変異は応答性 assertion で検出し、元 source の hash とビルドを復元した。
P4-3 は未着手のまま維持する。

最終 gate の source provenance は各 `source-state.json` と同じ directory の `sources/` へ固定する。
publication 実装・公開ヘッダ・試験・gate/変異/集計スクリプトの SHA256 は以下の取得すべてで一致している。

|gate|source provenance|
|---|---|
|Focused|[015310-Focused](../build/math-p421-20261009-015310-Focused/source-state.json)|
|Lint|[015335-Lint](../build/math-p421-20261009-015335-Lint/source-state.json)|
|Real|[015358-Real](../build/math-p421-20261009-015358-Real/source-state.json)|
|BuildIndependent|[015429-BuildIndependent](../build/math-p421-20261009-015429-BuildIndependent/source-state.json)|
|回帰|[015749-Regressions](../build/math-p421-20261009-015749-Regressions/source-state.json)|
|長時間 mutex 変異|[015903-Mutation](../build/math-p421-20261009-015903-Mutation/source-state.json)|
|通常 release|[020001-Release](../build/math-p421-20261009-020001-Release/source-state.json)|

初回 `015122-Mutation` は runner の PowerShell 配列記述で実装の変異前に失敗した。
失敗ログと source snapshot を保持し、配列を修正して新規取得した。旧 P4-2 の全証拠も保持する。
実行引数・終了コード・試験件数・測定値は [機械集計](math-graph-p421-results.md) を参照する。
