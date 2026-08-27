# P2-D5-2 W3 Canonical Performance — COMPLETED / CLOSED

W2-E が W3 へ保留した canonical performance verdict を、cutover 後の HEAD / binary による
fresh acquisition から生成した段である。

**W3 の目的は PASS を取ることではなく、formal-v2 authority から canonical verdict を
正しく生成することだった。** その目的は達成されている。

```text
W3 acquisition              PASS
W3 protocol/authority       VALID
W3 accounting               VALID
W3 metric construction      COMPLETE
W3 threshold evaluation     COMPLETE
W3 canonical verdict        CANONICAL_PERFORMANCE_FAIL

canonical_effective_fps     29.033  (< 55.0)
canonical_drop_rate         51.611% (> 2.0%)

W3                          COMPLETED / CLOSED
```

performance FAIL であることは W3 を未完了にしない。

## acquisition provenance

cutover 後の checkpoint から取得した。binary 4 点の SHA は cutover checkpoint と同一である
(W2-E は checker-level cutover であり binary を変更していない)。

```text
checkpoint_sha        03528433862647cad0fbd9d1f87a41f1b42027d1
worktree_clean        true
fresh_acquisition     true
acquisition_mode      CanonicalPresentMonLive
run_count             3
warmup_seconds        5
measure_seconds       60
coverage_complete     true
intent_scope_exact    true
cohort                build/p2-d5-2-w3-cohort-20260826
```

acquisition script は取得中の HEAD 変化と binary 変化を fail-close する。dirty worktree からの
取得も拒否する。

## 段の分離

fps / drop の PASS/FAIL をいきなり見ない。1〜3 のいずれかが INVALID なら 4〜6 へ進まず、
結果は authority / protocol INVALID であって performance FAIL へ変換しない (W1 §5.2 の三値)。

```text
1 acquisition / protocol validity     VALID   blockers なし
2 formal-v2 canonical authority       VALID   blockers なし
3 accounting validity                 VALID   blockers なし
4 canonical performance metric        COMPLETE
5 frozen threshold evaluation         COMPLETE
6 canonical verdict                   COMPLETE
```

今回は 1〜3 が VALID だったため 4〜6 へ到達している。したがって
`CANONICAL_PERFORMANCE_FAIL` は authority INVALID の言い換えではなく、
**stage 6 まで到達した本物の performance verdict** である。

## canonical metric

canonical chain の値だけから構成する。legacy presentation authority は使わない。
fps の分母は W2-A physical window であり、legacy の `measurement_elapsed_seconds` は使わない。

```text
canonical_required_intent_count      10800   (3600 x 3 run)
canonical_satisfied_intent_count      5226
canonical_unsatisfied_intent_count    5574
canonical_true_drop_count             5574
canonical_measurement_seconds         180.0  (W2-A window 60.0s x 3)

canonical_effective_fps  = 5226 / 180.0   = 29.033
canonical_drop_rate      = 5574 / 10800   = 0.51611
```

固定している契約:

```text
legacy_presentation_authority_used                = false
thresholds_frozen_unchanged                       = true
frozen_minimum_fps                                = 55.0
frozen_maximum_drop_rate                          = 0.02
layer1a_layer1b_count_difference_is_not_a_verdict = true
```

`required 10800 != physical 10791` は verdict に接続していない。異なる母集団である。

## checker

W3 closure の最後の必須条件として、実 artifact に対して checker を実行した。

```text
P2-D5-2 W3 checker: CANONICAL_PERFORMANCE_FAIL
  fps=29.033333333333335 drop=0.5161111111111111 required=10800 satisfied=5226
exit 0   (21m18s)
```

checker は次を行う。

- 段の順序違反 (1〜3 INVALID なのに 4〜6 を評価) を reject
- authority / protocol INVALID を performance verdict へ変換した artifact を reject
- legacy presentation authority 混入と threshold 変更を reject
- upstream artifact の SHA を再確認し、別 artifact を指す splice を reject
- shared replay で W2-E checker (さらに W2-D / W2-A / B1 / B2 / C1 / C2.1 / C2 / C2.4) を
  再実行したうえで expected artifact を再構築し、artifact 全体を比較

## この FAIL の解釈

cutover が作った失敗ではない。3 つの独立した裏付けがある。

1. fresh-7 (5s x 3) でも **legacy 側の `drop_rate` が 0.5133** を報告していた。formal-v2 の
   0.5133 と完全一致で、両 authority は drop について一致している。
2. 60 秒 x 3 に延ばしても drop 51.6% と比率がほぼ変わらない。計測長に依存する値ではない。
3. legacy gate も drop 0.5133 > 0.02 で FAIL していたはずである。legacy が PASS に見えていたのは
   fps だけで、それは分母 `measurement_elapsed_seconds` (実測窓 5.0s に対し 2.45s) が
   過小だったためである。

W3 は「約半分の intent が physical VBlank domain 内に表示されていない」という実測を、
はじめて正しい分母と exact chain で確定させた。

この段階で threshold、分母、required count を調整して PASS へ寄せることはしない。

## artifact

```text
build/p2-d5-2-w3-cohort-20260826/                      sealed cohort
build/p2-d5-2-w3-cohort-20260826/w3-acquisition-provenance.json
build/p2-d5-2-w3-c1-mapping-20260826-r2.json           C1 (support-domain erratum 適用後)
build/p2-d5-2-w3-c21-required-intent-20260826.json     C2.1
build/p2-d5-2-w3-c24-formal-transport-20260826.json    C2.4
build/p2-d5-2-w3-c2-intent-satisfaction-20260826.json  C2
build/p2-d5-2-w3-d-formal-v2-shadow-20260826.json      W2-D
build/p2-d5-2-w3-e-canonical-authority-20260826.json   W2-E
build/p2-d5-2-w3-canonical-performance-20260826.json   W3
```

## 再現

```powershell
# acquisition は管理者権限が必要 (ETW session)
pwsh scripts/acquire-p2-d5-2-w3-fresh.ps1 `
  -OutputDirectory build/p2-d5-2-w3-cohort-<date> `
  -Runs 3 -WarmupSeconds 5 -MeasureSeconds 60 -TimeoutSeconds 240

pwsh scripts/check-p2-d5-2-w3-canonical-performance.ps1 `
  -Proof build/p2-d5-2-w3-canonical-performance-20260826.json

ctest --test-dir build/ucrt64-release -R 'p2_d5_2_w3_' --output-on-failure --timeout 300
```

## W3 で行っていないこと

```text
threshold / 分母 / required count の調整
5574件の unsatisfied の原因追及  (W4 へ)
instrumentation overhead の切り分け  (W4 の attribution 結果次第)
producer / runtime の変更
```

原因追及は W4 — Unsatisfied Intent Attribution として別段で行う。
