# P2-D5-2 W4-C1 Causal Compatibility Replay — CLOSED

W4-C1 は新規 capture と product instrumentationを行わず、W3のsealed cohortに残る
`formal_opportunity_ledger.pre_render_authority.refresh_count` と
`formal_opportunity_origin_refresh_count` から、schedulerのcompleted refresh ordinalを再生した。

```text
W4-C1 CLOSED
verdict                         = EXACT_CAUSAL_COMPATIBILITY_PARTIAL_COVERAGE
root_cause_determined           = false
branch_execution_exact          = false
new_capture_performed           = false
producer_instrumentation_changed = false
c2_instrumentation_required     = true
```

契約は [p2-d5-2-w4-c-causal-attribution-contract.md](p2-d5-2-w4-c-causal-attribution-contract.md)
にfreezeしてある。

## exact joinと再生式

producer decisionとscheduler ledgerは、両方に記録済みの`render_begin_qpc`完全一致だけで結んだ。
nearest QPC、tolerance、source frame、missing state補間は使用していない。

anchored decisionについて次を再生した。

```text
completed = pre_render_authority.refresh_count - origin_refresh_count
predicted intent ordinal = completed + 1
```

全runで次が成立した。

```text
primary decision                    1742 / run
scheduler ledger exact QPC join     1741 / run
completed input witness             1740 / run
COMPLETED_PLUS_ONE_MISMATCH             0
```

最初のdecision ordinal 0はunanchored pathであり、originはその後の最初のswapで確定する。
final originを最初のpre-render sampleへ逆適用していない。最後のdecision ordinal 3598は
`past_source_domain=true`でrender/commit前に終了するためscheduler ledgerに入らない。

## Cause A compatibility

W4-B transition 5,223件を全件partitionした。

```text
EXACT_CAUSAL_COMPATIBILITY  5217
NOT_OBSERVABLE                 6
INCOMPATIBLE                   0
合計                         5223
```

compatible 5,217件では全件、次がexactに成立した。

```text
ΔintentOrdinal = ΔcompletedRefreshOrdinal

delta +2: 4869 / 4869
delta +3:  348 /  348
```

NOT_OBSERVABLEは各runで次の2境界だけである。

```text
0    -> 2     unanchored先頭にcompleted inputが無い
3596 -> 3598  terminal decisionがscheduler ledgerへcommitされない
```

したがってCause Aは、観測可能な全transitionについて

> +2/+3 advancementはtarget frameやlast finalizedから作られたものではなく、schedulerが
> consumeしたcompleted-refresh authorityの同じ+2/+3 advancementとexactに整合する。

まで閉じた。ただし6境界の入力とbranch discriminatorが無いため、Cause A全体の
`BRANCH_EXECUTION_EXACT`には昇格しない。

## Cause B candidate partition

source-frame domainとrequired-intent domainは別fieldのまま評価した。全3 runで最後のprimaryは
次だった。

```text
intent_ordinal                 3598
required_intent_membership     true
past_source_domain             true
formal_transport_disposition  TRANSPORT
```

これは「required intent membership内だがsource-frame domain外」であり、二つのdomainを
同一視できないことの実測上の反例でもある。

候補ごとの結果:

```text
DOMAIN_TERMINAL
  EXACT_CAUSAL_COMPATIBILITY
  branch_execution_exact = false

PLANNED_WINDOW_END
  INCOMPATIBLE
  last decision QPC < measurement_end_qpc_exclusive をexact比較
  gap = 309,424,506 .. 309,424,983 ticks（約30.94秒）
  tolerance / legacy elapsed heuristic不使用

EXPLICIT_STOP
  NOT_OBSERVABLE
  measurement_stop_captured=trueはあるが、request source / request QPCが無い
```

`DOMAIN_TERMINAL`は既存recordとstatic control-flowに整合するが、post-invocationのbranch/gate
witnessが無い。`EXPLICIT_STOP`も代替原因から除外できない。したがってCause BとA→Bは未確定である。

## proofとchecker

```text
artifact:
  build/p2-d5-2-w4-c1-causal-compatibility-20260827.json
SHA-256:
  be18a4d615930790a46c53fdaeaee9f57448f4f537c46064530a00528db31207

checker:
  P2-D5-2 W4-C1 checker: PASS compatible=5217 not_observable=6
```

checkerはW4-B checkerを再実行し、その中でW4-A→W3→W2-E以下も再生する。W4-B proof SHAと
各sealed `traced-app.json` SHAをbindし、artifact全体を独立再構築して比較する。

## negative

```text
NegativeCompletedOrdinalMutation
NegativeIntentDeltaMutation
NegativeNearestQpcBinding
NegativeSourceRequiredDomainConflation
NegativeCandidateForced
NegativePlannedEndHeuristic
NegativeMissingStateInterpolation
NegativeRootCauseDeclared
```

## 次

C1だけではterminal decision後にcapture gateが閉じた実行事実とexplicit stopとの順序を識別できない。
契約どおりW4-C2 scheduler invocation ledgerへ進む。instrumented captureはdiagnostic onlyであり、
W3 canonical performance verdictを書き換えない。
