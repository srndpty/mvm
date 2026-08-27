# P2-D5-2 W4-A Unsatisfied Intent Attribution

W3 は canonical performance verdict を FAIL で確定した (drop 51.611%)。W4-A は
その 5574 件の unsatisfied が chain のどこで失われたかを exact に分類する段である。

**原因判定はしない。partition を作るだけである。** instrumentation A/B も行わない。

新規 capture は取得していない。W3 で保存済みの同一 sealed cohort を offline 再評価している。

## 母集団と partition 契約

母集団は各 run の **exact required current intent set** だけである。1 intent identity につき
必ずちょうど 1 bucket へ入る。

```text
REQUIRED CURRENT INTENT
├─ A. NO_PRIMARY_SCHEDULER_DECISION
└─ primary decision exists
     ├─ C. NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY
     ├─ D. NO_NATIVE_PRESENT
     ├─ E. NO_EXACT_FORMAL_PRESENTED
     ├─ F. FORMAL_PRESENTED_OUTSIDE_DOMAIN
     └─ G. SATISFIED_IN_DOMAIN
```

### primary scheduler decision の exact な定義

「ordinal X の decision record が存在する」では弱い。次を満たすものだけを primary とする。

```text
intent_scope                      == CURRENT_MEASUREMENT
required_current_membership_exact == true
required_current_membership       == true
duplicate_callback                == false
intent_ordinal                    ∈ required set
```

### B は bucket にしない

同一 current intent に primary decision が複数存在するのは producer semantic corruption
である。performance loss へ混ぜず、authority / provenance INVALID として fail-close する。

```text
REQUIRED_INTENT_PRIMARY_DECISION_DUPLICATE
```

### duplicate callback / outside-required は母集団外

`duplicate_callback=true` は、同じ required intent に primary decision が既に存在する場合の
付帯 record である。したがって

```text
required ordinal X
  primary decision   → satisfied
  duplicate callback → suppressed
```

を `SATISFIED + SUPPRESSED` の 2 intent として数えない。X は **G の 1 件だけ**である。
duplicate suppression と outside-required decision は別の diagnostic counter に置く。

## 結果 (W3 cohort、3 run 合計)

```text
A_NO_PRIMARY_SCHEDULER_DECISION            5574
C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY        0
D_NO_NATIVE_PRESENT                           0
E_NO_EXACT_FORMAL_PRESENTED                   0
F_FORMAL_PRESENTED_OUTSIDE_DOMAIN             0
G_SATISFIED_IN_DOMAIN                      5226

bucket_sum        10800  = required_intent_count 10800
satisfied          5226  = canonical_satisfied_intent_count
unsatisfied        5574  = canonical_unsatisfied_intent_count
downstream_loss       0  (C + D + E + F)
```

母集団外 diagnostic:

```text
duplicate_callback_suppressed_count   3   (1 / run)
outside_required_decision_count       0
multiple_primary_decision_count       0
```

[事実] **5574 件の unsatisfied はすべて primary scheduler decision の生成以前に集中している。**
decision が出た intent は 1 件残らず satisfied まで到達しており、
decision → native Present → exact PresentEvent → FinalState → DisplayedQPC → physical ordinal
の損失はゼロである。

## missing ordinal の構造

missing set は集合差そのものから作る。

```text
M = required_set - primary_decision_ordinal_set
```

run 1 (missing 1858 / required 3600):

```text
first missing ordinal        1
last missing ordinal      3599
min present ordinal          0
max present ordinal       3598

Δmissing 分布                {1: 116, 2: 1741}
連続 missing run-length 分布 {1: 1626, 2: 116}     1626x1 + 116x2 = 1858
missing ordinal mod 2        {0: 929, 1: 929}
missing ordinal mod 3        {0: 620, 1: 620, 2: 618}
missing ordinal mod 4        {0: 465, 1: 465, 2: 464, 3: 464}
```

[事実] Δ=2 が 1741 件で支配的であり、大半が「1 つおきの missing」である。一方 mod 2 は
929/929 で均等に割れている。これは 116 箇所の Δ=1 (2 連続 missing) で位相が切り替わるためで、
run-length 分布 (isolated 1626 / pair 116) と整合する。

言えるのはここまでである。

```text
scheduler は required intent のおおむね隔回にしか primary decision を生成しておらず、
加えて run あたり 116 箇所で 2 連続の欠落が起きて位相がずれる
```

**「30fps source だから 2 個に 1 個」という仮説から判定を作っていない。** mod 2 が偏っていない
以上、固定位相の隔回でもない。source frame と intent identity は別物であるという freeze を
維持している。

## artifact が保存する値

aggregate だけでなく、W4-B が exact に追えるよう ordinal set 自体を保存する。

```text
run ごと:
  required_intent_ordinals
  primary_decision_ordinals
  missing_primary_decision_ordinals
  satisfied_intent_ordinals
  buckets / bucket_sum
  missing_delta_distribution
  consecutive_missing_run_length_distribution
  missing_ordinal_modulo_distribution (mod 2..8)
```

## checker

checker は artifact の aggregate を信用しない。

- W3 canonical performance checker を再実行する (その中で W2-E / W2-D / 各 upstream も再実行される)
- upstream artifact (W3 / C1 / C2.1) の SHA を再確認し、別 artifact を指す splice を reject
- C2.1 の `source_c1_proof_sha256` が C1 と一致することを確認 (required exact set の provenance)
- sealed cohort の producer record と C1 mapping record から partition を再構築し、
  artifact 全体を比較する
- bucket の網羅性 / 排他性、canonical 値との一致を artifact 上でも先に検査する
- `root_cause_determined` / `instrumentation_ab_performed` が true の artifact を reject

## negative

```text
NegativeRequiredIntentMissingFromAllBuckets   bucket countを減らす (完全性)
NegativeRequiredIntentInTwoBuckets            bucket countを増やす (排他性)
NegativeMultiplePrimaryDecision               performance bucketではなくauthority INVALID
NegativeDuplicateCallbackCountedAsDrop        duplicate callbackをdropへ混ぜない
NegativeOutsideRequiredCountedAsDrop          outside-required decisionを母集団へ入れない
NegativeMissingSetMutation                    missing setの改変
NegativeSatisfiedSetMutation                  satisfied setの改変
NegativeAggregateOnlyForgery                  aggregateだけの偽造
```

positive として `GoodDownstreamLossBuckets` を置き、C/D/E/F を 1 件ずつ作って partition が
排他的に効くことを固定している (今回の実測では 4 つとも 0 だが、bucket 自体が空振りで
ないことを示す)。

## 再現

```powershell
pwsh scripts/build-p2-d5-2-w4-a-intent-attribution.ps1 `
  -W3Proof build/p2-d5-2-w3-canonical-performance-20260826.json `
  -C1Proof build/p2-d5-2-w3-c1-mapping-20260826-r2.json `
  -C21Proof build/p2-d5-2-w3-c21-required-intent-20260826.json `
  -Output build/p2-d5-2-w4-a-attribution-<new-name>.json

pwsh scripts/check-p2-d5-2-w4-a-intent-attribution.ps1 `
  -Proof build/p2-d5-2-w4-a-attribution-20260826.json

ctest --test-dir build/ucrt64-release -R 'p2_d5_2_w4a_' --output-on-failure --timeout 300
```

`-SkipW3Replay` は W3 checker の再実行 (約 22 分) を省く開発用の逃げ道であり、
closure の証拠には使わない。

## W4-A で行っていないこと

```text
原因判定 (なぜ selectForRender() が約半分でdecisionを生成しないのか)
instrumentation overhead の A/B 切り分け
producer / runtime の変更
新規 capture
```

次は W4-B — Producer Semantics Attribution。保存済み同一 W3 cohort から、
decision 側の producer field を ordinal 順に並べ、isolated missing と double-missing
boundary の 2 群比較で位相反転を作るイベントを特定する。
