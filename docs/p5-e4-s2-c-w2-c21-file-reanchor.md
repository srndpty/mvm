# P5-E4 S2-c — W2-C2.1 required set producer の再アンカ

## 1. 変更

`tests/gpu_preview/test-p2-d5-2-w2-c21-required-intent-domain-architecture.ps1`
が `requiredIntentOrdinals_.push_back(ordinal)` を検査する対象を、旧所有componentの
`presentation_opportunity_scheduler.cpp` から現在の所有componentである
`required_intent_queue.cpp` へ変更した。

required setをstart時点の`[0,N)`として生成する不変条件、pattern、violation messageは
変更していない。production source、threshold、W3 / P3-C-2 / P4 contractにも
触れていない。

併せて、別scriptのsynthetic data-contract negative群が検出した固有blockerを
実行出力へ残すようにした。さらにsynthetic入力全体のbaseline fingerprintと
mutation後fingerprintを比較し、入力が実際に変化しないnegativeを拒否する。
negativeの合否条件は従来から期待blockerの存在であり、今回その強度は
緩和していない。

## 2. 修正前

```text
architecture Good                      FAIL 1
cause                                  file inventory drift
required set producer invariant        valid
synthetic data-contract false greens   0
```

required set producerはB3-I1で`RequiredIntentQueue`へ抽出済みだったが、architecture
testだけが旧scheduler sourceを検査していた。

## 3. closure evidence

採取条件:

```text
checkpoint  5665f107653f0b9227e877f15450db5af9eac803 + S2-c の未コミット差分
build       build/ucrt64-release
date        2026-08-29
```

architecture test、Good data-contract 3件、negative data-contract 10件を個別実行した。
14/14 PASSであり、negativeはbaseline fingerprintとの差を確認した後に、
次の固有blockerへ到達した。

```text
NegativeMissingRequiredSet
  REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING
NegativeDuplicateRequiredSet
  REQUIRED_SCHEDULER_INTENT_SET_DUPLICATE
NegativeCountSetMismatch
  REQUIRED_INTENT_COUNT_SET_CARDINALITY_MISMATCH
NegativeRequiredSetMissingMembership
  DECISION_REQUIRED_MEMBERSHIP_MISMATCH
NegativeMembershipExtraOutsideRequiredSet
  DECISION_REQUIRED_MEMBERSHIP_MISMATCH
NegativeMissingDecisionQpc
  SCHEDULER_DECISION_QPC_PROVENANCE_MISSING
NegativeBoundaryRelationMutation
  MEASUREMENT_BOUNDARY_RELATION_MISMATCH
NegativeMissingMeasurementStartQpc
  MEASUREMENT_START_QPC_MISSING
NegativeMissingMeasurementEndQpc
  MEASUREMENT_END_QPC_MISSING
NegativeAmbiguousFormalReverseJoin
  FORMAL_PRESENTED_REVERSE_ATTRIBUTION_INVALID
```

各negativeは`authority_exact=false`だけではPASSせず、対応する期待blockerが
`result.blockers`に無ければ失敗する。このため無関係なblockerだけでPASSする経路はない。

CTest登録経路でも確認した。

```powershell
ctest --test-dir build\ucrt64-release `
  -R '^p2_d5_2_w2c21_required_intent_domain_' `
  --output-on-failure --timeout 60
```

```text
14/14 PASS
  architecture Good       1/1
  data-contract Good       3/3
  data-contract negative  10/10
timeout 0
```

PowerShell parserと`git diff --check`もPASSした。

## 4. 判定

```text
S2-c                                  CLOSED
W2-C2.1 ordinary failures             1 -> 0
negative mutation applicability      10/10 verified
negative intended blocker            10/10 verified
observed false-green negatives        0
production semantics changes          0
assertion / threshold relaxation      0
```

S2-cは閉じた。P5-E4 ordinary regression gateは、残るS2-dとS2-eが
未完了のためOPENのままである。
