# P4-3: Graph の非同期描画と native preview

2026-10-09。P4-3 の作業記録。schema22 と P4-2.1 の artifact/publication 契約 B は維持する。
P4-4 の authoring UI と P4-5 の export は対象外。コミット・push は行わない。

## 現在の接続

`GraphPreviewCache` が Graph 専用の型で renderer・disk 検証・decoded RGBA の所有を扱う。
Manim preflight、`validateArtifact`、`PublicationAuthority::generate` は renderer worker、PNG decode は別の worker で行う。
GUI/render thread で PNG を読まず、Project に artifact path を保存しない。
cache は `cache/graph/<Project file 名>/<正準 key>/`。公開は Project lock の所有者だけに許可する。

request の immutable spec と key、controller の Project generation/revision、publication generation は別の値である。
同じ key の consumer は一つの request を共有する。Project の現在 key の集合から外れた request を取り消し、
ticket と session generation が一致しない完了を受理しない。renderer は同時二件まで、次は最新の要求を優先する。
Graph の cache 通知は、Project に Graph clip があるときだけ preview を組み直す。
Graph の無い Project で初期 seek の後にもう一度提示しない。
shutdown は publication authority の無効化 → cancellation → worker join → authority 破棄の順。
join の間に authority mutex を保持しない。通常の編集・seek は join しない。

`intro=None` は静止 package、Draw は Draw package 内の端点を使う。
Draw の source i<N はその package の frame-i.png、i>=N は static.png。
package 全体を検証してから一枚だけ decode し、decode 時にも検証済み pixel hash と照合する。
破損を検出した package の他の resident frame も現在の authority から外す。

Graph の decoded RGBA は専用の64 MiB上限で扱う。P3 の A8 層と eviction 規則を変更しないため、
P3 の共有 budget には入れない。decode 前に実 RGBA byte を予約し、最後の shared owner の解放で返す。
GPU snapshot が参照する allocation は eviction しない。読み込みは同時・待機を合わせ二件まで。
先読み horizon は1。現在の exact frame が resident になってから一枚先を要求する。先読みは退避を起こさず、余裕がなければ行わない。完全な Draw を RAM に一括展開しない。
OverBudget は disk 破損を意味しない。

`PreviewStillAnimation` は immutable RGBA の所有者を保持する。P4-1 の evaluator が output frame から
source frame を引き、保持している exact frame と一致する場合だけ画素を渡す。他の frame は透明。
静止端点を Draw の代用にせず、以前の key や以前の提示 frame を現在として渡さない。
pixel は straight RGBA のまま既存 D3D11/QRhi 合成境界へ渡す。外側 ClipEffects は通常 layer に一回だけ掛ける。
同じ clip/frame/allocation の animation instance を使い回す。

disabled clip と hidden video track は既存 mapping から除外する。Graph は音声を持たない。
外側 TimelineTransition を持つ Graph は typed UnsupportedTransition と透明 contribution にする。
通常動画の transition は変更しない。未準備・破損・上限超過の Graph も下の通常動画を停止させない。

`graphPreviewStatus` は request を発行せず、現在の spec/frame と最後に独立検証した readiness を照会する。
現在 key、Project revision/session generation、backend、artifact、residency、typed reason を分けて返す。
`frameAvailable` は RAM の証拠であり、physical display の証拠ではない。
`validatedSnapshot` は検証時点の記録で、照会時点の disk が変更されていないことを保証しない。
session 内の確認済み toolchain identity は backend の一時停止でも保持する。
未確認の新 session では旧 artifact を受理しない。

## 検証と現在の判定

集中試験は独立に区別できる PNG の frame 番号・色・alpha と promise の同期バリアを使う。
N=1/3/10 の非単調 seek、同じ key の共有、状態照会、RAM 上限、退避中の所有、旧 key/取消、
B が A より先に完了する順序、package の欠損、読み取り専用、新 session の identity 不在を検査する。

実 Manim 試験は schema22 を保存・controller を閉じて再読込し、通常動画の上に
x^2 / sin(x) / 1/x、grid・axes・labels、Draw N=3 を製品 controller と QRhi preview で提示する。
参照は検証済み disk PNG と CPU の独立 source-over。GPU 出力から期待値を作らず、RGB 許容差を置かない。
白い H.264 fixture は共通合成段階の下地を制御するために使い、一般の H.264 decode の byte 同値は主張しない。

【操作可】gate は背面・非フォーカス・入力透過の window と既存 display-power lease を使う。

```powershell
pwsh scripts/math-p43-gate.ps1 -Stage Focused
pwsh scripts/math-p43-gate.ps1 -Stage Real
pwsh scripts/math-p43-gate.ps1 -Stage Regressions
pwsh scripts/math-p43-gate.ps1 -Stage BuildIndependent
pwsh scripts/math-p43-gate.ps1 -Stage Lint
pwsh scripts/math-p43-gate.ps1 -Stage Release
pwsh scripts/math-p43-report.ps1
```

全 source snapshot（apps も含む）、SHA256、HEAD、正確な引数・終了コードは新規 gate directory に保存する。
生 JSON/TSV の集計は [結果文書](math-graph-p43-results.md) を正とする。
初期 sandbox 内の directory 作成失敗ログは `build/math-p43-*-failure-*/` と各 gate に保持する。
初期の native fixture の不正 track、試験 helper の二重 seek の失敗も削除していない。

[事実] `build/math-p43-20261009-032358-Focused` の cache 試験は終了コード 1。
出力は「完全な Draw の authority」と「artifact を公開できません」。
`graph-preview-DeQSEy/residency/` に、frame-0/1/2.png・static.png・manifest.txt を含む
`.pending-a692c144…-32603898305800-3` が残った。公開先 key は無く、同じ directory は後から
`move` できた。短い path の同一試験は通過している。file の atomic 置換と同じく、
索引やウイルス対策が共有ロックしている間の directory rename 失敗とみなす。
公開 rename は既存 artifact を置換せず、`ERROR_ACCESS_DENIED` / `ERROR_SHARING_VIOLATION` /
`ERROR_LOCK_VIOLATION` のときだけ mutex の外で最大約 2 秒待つ。待ちの間も authority を再確認する。
共有ロックを 300ms 保持する回帰は `mvm_test_graph_render` の artifact 試験に入れた。

[事実] 修正後の gate。失敗した directory は残している。

- `build/math-p43-20261009-033429-Focused`: build 0、cache 0
- `build/math-p43-20261009-033458-Real`: build 0、video fixture 0、native 0。
  `native/results.json` は checks 42、failures 0、artifact_ready_ms 7550
- `build/math-p43-20261009-033537-Regressions`: 13/13 passed
- `build/math-p43-20261009-033806-Lint`: lint 通過
- `build/math-p43-20261009-033857-BuildIndependent`: 1080 中 1079 通過、1 失敗。
  失敗は `audio_mixer_controls_qml` の `test_mixerImplicitHeightAndResize`。
  Qt Windows の `ScrollView.qml` / `DefaultScrollBar.qml` が `visible` の binding loop を出し、
  試験の `failOnWarning` がそれを失敗にした。Graph の変更ファイルに mixer QML は含まれない。
  単独再実行でも同じ警告で失敗した。P4-3 の描画・cache・preview の失敗ではない。
- `build/math-p43-20261009-034831-Release`: 1479 中 1477 通過、2 失敗。
  `audio_mixer_controls_qml` は上と同じ Qt Windows の binding loop。
  `subtitle_native_preview` は「字幕だけの初期seekを繰り返さない」。
  Graph が無い Project でも cache の authority 設定が preview の再組立てを予約し、
  初期提示の後にもう一度 seek していた。Graph clip があるときだけ組み直すようにした。
  修正後の `subtitle_native_preview` 単独実行は通過した。この Release directory の失敗は残す。

P4-3 は検証中であり PASS/CLOSED ではない。BuildIndependent と Release が上記の失敗を含む。
全閉鎖条件と通常 gate の確認が終わるまで P4-4 へ進まない。
