# 数式変形の release flake 調査

## レビューで判明した観測の修正（2026-10-10）

[事実] 旧 observer は backend-start の frame に -1、backend-end に出力 frame 数を
記録していた。集計の組は thread・frame・stage なので、旧集計には backend 区間が無く、
開始だけが incomplete_stages に残っていた。旧 raw log と集計は保存し、描画時間の根拠には
使わない。start/end の識別値を -1 に揃え、Ready では backend の成功区間がちょうど一つ
あることも集計で検査する。識別値の不一致・end の欠落を拒否する負の対照を追加した。

[事実] 固定 cohort は追跡対象の `tests/fixtures/math-preparation-cohort.json` に保存した。
歴史的 artifact の試験名 277 件を保持し、CTest の数値 ID は選択に使わない。
別の集合は `-CohortPath` で明示的に指定できる。schema・空集合・重複・対象試験の欠落を
検査し、実際の CTest 集合と manifest が完全一致しなければ実行を止める。
前回の LastTest が無い初回 build でも開始でき、不在を証拠へ記録する。

[事実] tracked manifest を使った baseline と固定 3 回の 8 並列 cohort は通過し、
各 run の backend 区間が集計された。
[修正後の原ログ・source/runtime と段階集計](../build/math-preparation-20261010-203713-605-Concurrent/preparation-summary.md)。
この source snapshot は初回 LastTest 不在の扱いを追加する前の runner を保存している。

[事実] メニュー項目の WindowShortcut を廃止し、CompactMenu の項目一覧で
ShortcutOverride を受理してからキーイベントを処理するようにした。
Alt を保持したメニューバー操作でも、開いた一覧へ Qt 内の focus を移す。
実 window の GUI 回帰で M/I/O の通常・Alt 保持、File の Alt+E と Edit 見出しの競合、
既存の編集・Esc・非アクティブ化を検査した。File 項目は OS dialog を開かず dispatch を
観測するため、その検査中だけ Action を外し、終了後に戻している。
`build/review76-focus.log` と LastTest は成功、途中の失敗は
`build/review76-targeted.log` と LastTest に保存した。構築時の失敗も上書きしていない。
cohort/backend 段階会計の正負対照は `math_preparation_tools_contract`、
lint の結果は `build/review76-lint-verified.log` に保存した。
[事実] 最終 runner の isolated baseline と準備対照 11 件も通過し、
[修正後の対照ログと段階集計](../build/math-preparation-20261010-204403-006-Focused/preparation-summary.md)
へ保存した。このレビュー対応では GUI・tools・準備の対象試験と固定 cohort を実行した。
以下の通常 release 1507 件の記録は、レビュー修正前の候補で実行した歴史的 gate である。

## 準備契約の修正（2026-10-10）

[事実] 最終候補は準備対照 11 件、記録済み 277 件の cohort を 8 並列で固定 3 回、
通常 release 1507 件（BuildIndependent 1085 件を含む）と lint を通過した。
全件 gate の原ログ・LastTest・metadata・source hash と、cohort に対する runtime hash の
一致は [最終 gate の生成記録](../build/math-preparation-final-20261010-201738-108/validation.md) に保存した。
今回の修正を解決済みの記録へ移し、roadmap の未解決項目を除いた。

[事実] 以下の旧調査を保存したまま、準備と再生の検査を分離した。
旧 `pump` の既定 10000 ms は artifact が正しいかではなく、準備がその時間内に終わるかを
判定していた。準備完了時間について製品の契約は無く、再生の契約へこの上限を持ち込むのが
誤りだった。historical FAIL の個々の decode / I/O / OS scheduling の寄与は依然として未確定。

[事実] 製品の decode・矩形検査・端点検査・frame 保存・SHA-256・provenance の順序は維持した。
`MathRasterCache::setTransformPreparationObserverForTest` と atomic write の観測付き入口は、
試験が明示的に設定したときだけ記録する。通常の入口は observer が空の同じ処理へ委譲する。
製品の描画・cache 方式・flush / rename retry の挙動は変更していない。

[事実] 試験は backend 描画、PNG decode、frame の切り出し・端点検査、frame cache 保存、
flush、rename、hash、manifest 照合、provenance 公開、worker 完了、GUI Ready を観測する。
単調時計・thread ID・thread CPU 時間の取得可否・frame・bytes・段階の成否をメモリに蓄積し、
worker と再生を終えた後にログへ出す。CPU 時間は Windows の thread accounting の粒度を持つ。
保存全体の時間は flush / rename の時間を含むので、親段階と子段階を足し合わせない。

### 有限の準備待機

- 実際に検証・保存された worker の Ready と、controller の GUI Ready の両方を要求する。
  状態を Ready へ書き換えず、公開後は製品の export 用検査で provenance と必須 frame の
  hash を再照合してから playback assertion を評価する。
- GUI は `QEventLoop` / `QTimer` で event を処理する。sleep で準備を同期しない。
- 15 秒無進捗なら `Stalled`、全体 60 秒なら `SafetyCap` とする。この二つは試験の停止防止で、
  製品の速度要件ではない。各 frame の実作業の開始・終了が進捗になり、長くても進んでいる
  準備は旧 10 秒を超えたことだけでは失敗しない。進捗を無限に送り続けても全体上限を回避できない。
- worker の typed error / cancellation は `Error` / `Cancelled` とし、Ready と混同しない。
- worker 完了後、GUI Ready が 2 秒の診断区間内に届かなければ `NotificationLost`。
  public な準備結果を待つ状態と、worker が戻っていない状態を区別する。
- 故障注入で意図的に gate を閉じた場合だけ、無進捗の観測区間を 500 ms に短縮する。
  注入 worker は cancel を設定してから gate を解放し、shutdown で join する。
  通常条件の安全区間と playback の既存 `pump` / CTest timeout は変えない。

失敗時は typed outcome と最後まで進んだ段階・frame・成否を記録する。
準備の前提が成立しなければ再生結果として成功にしない。

### 対照と保存証拠

[事実] `math_transform_preparation_*` は次を検査する。

| 条件 | 期待する検出 |
| --- | --- |
| backend の開始境界で停止 | Stalled、その後 cancel / join |
| PNG decode 失敗、必須 PNG の欠損・破損 | 対象 frame の decode-end が失敗し worker-error |
| frame cache 保存失敗 | 対象 frame の persist-end が失敗し worker-error |
| provenance 公開停止 | Stalled、その後 cancel / join |
| provenance の atomic 置換失敗 | provenance-end が失敗し worker-error |
| worker 完了後の GUI 通知を試験用に抑止 | worker-ready があるが GUI Ready が無く NotificationLost |
| 誤った source frame | 実 native 再生の観測を一つずらした mutation を既存 timeline oracle が拒否 |
| 公開を条件変数の時刻境界で遅らせる | 旧 10 秒を実際に超え、正しい準備と全 native playback assertion が成功 |
| 進捗が続くが完了しない待機 | SafetyCap。取消も独立して Cancelled を検査 |

故障対照の PASS は期待する状態を明示的に照合した結果であり、任意の非 0 終了を合格にしない。
集計でも対象段階・対象 frame の失敗を照合し、別の I/O 失敗などを mutation の検出と数えない。
既存 `math_raster_cache_focused` の provenance 不完全・世代変更・公開取消・hash / size / 欠損 frame
の拒否も保持する。既存 Math / EquationSequence / Graph と native の seek・mask・layer・frame・
pixel の assertion は削除も緩和もしていない。

最終候補の固定 cohort は isolated baseline を先に確認し、記録済み集合を 8 並列で 3 回実行した。
全 run を保存し、PASS まで retry する選別はしていない。通常の display 電源 lease を取得した。
歴史的な workload の集合と並列数を合わせた再実行であり、過去の OS scheduling の厳密な replay
ではない。

- [最終候補の baseline / cohort と段階集計](../build/math-preparation-20261010-195132-089-Concurrent/preparation-summary.md)
- [最終候補の故障対照と長い正常準備](../build/math-preparation-20261010-195403-329-Focused/preparation-summary.md)
- 各 directory の `source-hashes.json`、`sources/`、`source.patch`、`runtime-hashes.json`、
  CMake cache、CTest metadata、raw log、LastTest が source/runtime provenance を記録する。
- 通常 release gate は `build/math-preparation-release-gate.log`、lint は `build/math-preparation-lint-final.log`。

時間は上記の raw log から `scripts/summarize-math-preparation.ps1` が再計算する。
各成功 run で decode・切り出し・保存・hash・公開後の manifest frame 照合の件数と成否を検査し、
対象 0 件や Ready なのに frame 検査が抜けた記録は集計を失敗にする。
[事実] 集計の対照として、正常ログのコピーから frame 1 の hash-end 観測だけを除いた。
正常コピーは成功し、欠損コピーは Ready の段階会計エラーで失敗した。原ログは変更していない。
保存先は `build/math-preparation-summary-controls/`。

```text
【操作可】通常の背面 GUI 試験です。PC 操作を続けられます。
```

```powershell
pwsh scripts/build.ps1 -Preset ucrt64-release -Target mvm_test_math_controller
pwsh scripts/verify-math-preparation.ps1 -Mode Focused
pwsh scripts/verify-math-preparation.ps1 -Mode Concurrent
pwsh scripts/test.ps1 -Preset ucrt64-release -Group All -Jobs 8
```

## 旧調査（準備契約の修正前）

## 結論

[事実] `math_transform_native_playback` の初回 FAIL と同じ assertion を、保存ログから復元した
同時実行群の並列実行で再現した。偽 backend の描画終了後、連番の検証と frame cache の
disk 書き込みを行う worker が、GUI 側の既存の disk 準備待ち期限を超えた。
その後は公開まで進み、同じ実行内の再生・mask の途中投入・timeline frame の照合は通った。

[推測] 初回 FAIL もこの準備段階の負荷による待機期限超過と整合する。ただし初回の worker
時刻・stack・OS thread state は記録されておらず、初回の停止位置まで確定したとはしない。
再実行が通ったことを解消の根拠にしない。

[事実] 数式変形の製品処理、assertion、待機期限、CTest の除外条件は変更していない。
試験専用の診断と、公開を意図的に遅らせる対照条件を追加した。

## 保存した証拠と再計算

元の FAIL/PASS は `build/review-evidence-2026-10-10/` に残したまま、
調査時の証拠を `build/math-transform-flake-20261010-184418-447/` に複製した。
`historical-hashes.json` は元ファイルの path と SHA-256、`head.txt` は調査開始時の HEAD、
`source.patch` と `sources/` は再現実験時のソースを示す。
HEAD は初回実行の binary の同一性を証明するものではない。初回 binary と実行時 DLL の
hash は初回ログに無く、厳密な historical runtime provenance は未確定である。

実行時間・各段階の時刻・GUI/worker の thread ID は、次の生成物を参照する。
値はログから再計算し、本文には手で転記しない。

- [実行時間と準備段階の比較](../build/math-transform-flake-20261010-184418-447/comparison.md)
- [各段階の時刻と thread ID](../build/math-transform-flake-20261010-184418-447/comparison.json)
- [各実行の開始時 active 集合と重なった CTest 群](../build/math-transform-flake-20261010-184418-447/workload-comparison.json)
- [固定回数の実験結果](../build/math-transform-flake-20261010-184418-447/results.json)
- [並列実行で再現した FAIL](../build/math-transform-flake-20261010-184418-447/parallel-3.log)
- [公開遅延の対照実験](../build/math-transform-flake-20261010-184418-447/delayed-publish.log)

再計算のみなら PC 操作の制限は無い。

```powershell
pwsh scripts/summarize-math-transform-flake.ps1 -EvidenceDirectory build/math-transform-flake-20261010-184418-447
```

## 初回 FAIL の抽出

[事実] `gate-final-both-all.log` の release 部分の唯一の失敗 assertion は
`FAIL: native 変形: 変形が disk に揃う`。
`nativeTransformPlayback` の `selectTransition("t1")` と、`transformState == "ready"` を待つ
`pump` の合成条件である。`pump` の既定期限はソースで 10000 ms。
CTest 自体の timeout でも、再生 frame の assertion でもない。
同じ実行の最後の検査集計は、この assertion だけが失敗したことを示す。

[事実] 初回実行の開始時には `audio_mix_sum`、`gpu_invalid_media_zero_negative`、
`gpu_seek_out_of_range_negative`、`p1_shutdown_contract_timeout_negative` と、
`p2_d5_2_w4c3_acquisition_runner_negativerunclosureweakened`、
`p2_d5_2_w2c13_formal_population_negativeb2formalupstreaminvalid`、
`p2_d5_2_w2c24_formal_transport_negativeoutsidedispositionswappedtoduplicate` が active だった。
開始・完了行を順に照合した完全な重複集合は生成 JSON に残した。
成功した release full rerun の対象試験区間には他の CTest 試験が重なっていない。
isolated の過去ログは PASS と全体時間だけを示し、準備段階の余裕は記録していない。

[未検証] 初回 assertion 発生の絶対時刻、OS scheduler の thread state、thread stack、
他アプリの CPU/disk 負荷は元ログから復元できない。ログにない値を 0 として扱わない。

## 原因の切り分け

| 候補 | 根拠と判定 |
| --- | --- |
| 表示・preview scheduling | FAIL は disk ready の事前条件。再生時の frame 対応と両 layer は同じ run で通る。display drop を原因とする証拠はない。 |
| 同期・通知 | 再現時の GUI は `rendering` のまま期限で終了し、worker はそれより後に公開直前へ進む。完了した結果の通知だけが失われる説明は、この再現には該当しない。 |
| 偽 backend | 再現時の描画開始回数は重複せず、描画は期限前に終了。偽 renderer の永久待ちではない。 |
| 試験 fixture の準備 | disk ready が実時間の期限内に揃うことを playback 試験の前提としている。並列負荷で worker の連番検証・disk 書き込み区間が期限を超える現象を再現。 |
| 製品不具合 | artifact 不一致、再生の停止、frame 0 への巻き戻り、cut 後の mask 消失は再現していない。製品にこの準備完了時間の契約はないため、速度だけから製品変更を導かない。 |

[事実] `math_raster_cache.cpp` の worker は backend が生成した PNG を decode し、
frame ごとの切り出し・端点検査・atomic 保存・hash 計算を行い、最後に provenance を公開する。
今回の観測点は backend 描画終了と provenance 公開直前の既存 hook。
この間の decode・保存・OS scheduling の寄与は分離していない。
atomic 保存を外す、検証を省くなどの製品変更の根拠にはならない。

[事実] 固定した並列実験の FAIL は保存し、PASS が出るまで retry する選別は行っていない。
同じ重複集合・並列数での再実行であり、初回の start 順・CTest cost 履歴・OS scheduling を
厳密に再現したものではない。負荷の類似と初回 runtime の同一性は区別する。

[事実] `MVM_TEST_TRANSFORM_DELAY_PUBLISH` の対照条件は、既存 hook で公開だけを遅らせる。
通常の待機期限は延ばさず、同じ assertion だけが失敗し、後続の再生検査は通ることを確認した。

再現用 runner は毎回新しい証拠 directory を作る。通常条件の FAIL は終了失敗として返す。

```text
【操作可】通常の背面 GUI 試験です。PC 操作を続けられます。
```

```powershell
pwsh scripts/build.ps1 -Preset ucrt64-release -Target mvm_test_math_controller
pwsh scripts/build.ps1 -Preset ucrt64-release -Target mvm_test_controller_export
pwsh scripts/investigate-math-transform-flake.ps1 -Run
```

## 別件: 前の clip の削除と選択 ID

[事実] 既存の最後の clip 削除試験は範囲外 index の通知を検査するが、範囲内の index が
別 ID を指す場合を検査していなかった。追加した `m7b_4_selection_identity` は、
中央の clip を選択し、その clip の上書き移動で前の clip を削除する。
後ろにも clip を置き、旧 index が依然として範囲内になる対照を作った。
同期的な model 通知中と操作終了後、および Undo/Redo の通知中に、対象 ID を照合する。

[事実] 修正前は削除通知中の identity 検査で失敗した。
`selection-overwrite-before-fix.log` を保存してから製品を変更した。
`commitProjectEdit` は旧 Project から取得した ID を新 Project の index に通知前に解決し、
Undo/Redo も履歴の ID を model 通知より前に復元する。index が変わらない通常の確定では
選択を付け直さない。数式変形の処理とは別の、回帰試験で実証した不具合の修正である。

[事実] 修正後の個別試験と既存 `m7b_4_controller_export_lifecycle` は release / debug の
両方で通った。ログは `selection-after-fix.log` と `selection-debug-after-fix.log`。
lint は `lint.log`。新しい identity 検査は修正前に失敗し、修正後に通るので、空振りではない。
共有コードを使う数式・Graph・Equation controller と Write / Transform native preview の
release 回帰試験も通った (`math-after-selection-fix.log`)。
最終ソース・runtime の hash と検証集計は `final-source-hashes.json`、
`final-runtime-hashes.json`、`final-validation.json` に保存した。

[事実] 最初に作った track 削除 fixture は、空でない track の削除を製品が拒否するため無効だった。
その `selection.log` も消さず残している。この FAIL を製品不具合の根拠にはせず、
許可された上書き削除の fixture の FAIL だけを根拠にした。
