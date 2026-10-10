# 数式変形の release flake 調査

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
