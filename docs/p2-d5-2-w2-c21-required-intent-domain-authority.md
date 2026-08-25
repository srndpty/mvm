# P2-D5-2 W2-C2.1 Required Intent Domain Authority Attribution

## 目的

`required_measurement_frame_count`からchecker側で`[0, count)`を生成せず、scheduler sole
producerが記録したrequired current intent identity setを確定する。historical C2と
performance accountingは変更しない。

入力はC1 checkpoint `5034bfcd41dd9f5c860827a9594b604be5db7446`と、
fresh-2 sealed artifactである。Presentedはrequired setの導出には使わず、
producer decisionへのreverse attributionにだけ使う。

## fresh-2で利用できるproducer record

[事実] `native_present_hook.intent_scope_provenance.records`は、
`formalDecision.opportunityOrdinal`を生成した`selectForRender`直後に記録される
順序付きの`{token_serial, intent_ordinal, intent_scope}` ledgerである。

[事実] 各runのinventoryは次だった。

```text
scheduler decision producer records          150
FOREIGN_PRE_MEASUREMENT records                2
CURRENT_MEASUREMENT records                  148
CURRENT distinct ordinals                    147
CURRENT min / max diagnostic               0 / 301
CURRENT duplicate ordinal extra diagnostic    1
CURRENT gaps within min..max diagnostic      155
required_measurement_frame_count              300
```

min/max、duplicate、gapはproducer record populationのdiagnosticであり、
required intent set authorityではない。

## authority gap

[事実] fresh-2 schemaには次が無い。

```text
required_intent_ordinals
required_intent_set_exact
decision_qpc / decision_qpc_exact
required_current_membership / exact
decisionごとのmeasurement boundary relation
```

したがって、cardinality 300とexact required identity setのidentityは閉じない。
decision QPCが無いため、measurement arm/start/endとのQPC relationも再生できない。
nearest QPC、順序offset、source frame、Presented min/maxによるfallbackは使用しない。

blocker:

```text
REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING
SCHEDULER_DECISION_QPC_PROVENANCE_MISSING
MEASUREMENT_BOUNDARY_RELATION_UNRESOLVED
```

[exit] `REQUIRED_INTENT_DOMAIN_AUTHORITY_UNRESOLVED`。A/Bのどちらも確立していない。

## 0 / 301 reverse attribution

[事実] 各runでproducer decision sequence 2と3は、ともにordinal `0`だが
distinct tokenを持つ。各tokenはexactに別native Presentへtransportされ、C1 formal
Presented eventへ1対1で到達した。したがって「同一scheduler decisionの二重transport」
ではなく、producer ledger上のdistinct 2 decisionである。

[事実] 各runのsequence 149はordinal `301`、scope `CURRENT_MEASUREMENT`として記録され、
exactにnative PresentとC1 formal Presentedへ到達した。

これらはproducer recordとtransportの事実である。`0`がrequired identityとして重複か、
`301`がrequired domain外かは、required membership authorityが無いため未確定である。

## artifact

```text
build/p2-d5-2-w2-c21-required-intent-domain-fresh-2-20260825-r3.json
```

inventoryとcheckerはいずれもartifactを書き出し／再生成照合した後、authority gapにより
非0で終了する。historical C2 artifactは変更していない。

## C2.1 hardeningとC2.2 instrumentation

[事実] checkerは、decision ledgerから次を独立再集計する。

```text
exact_required_membership_ordinal_set
  = distinct(decision.opportunity_ordinal
             where required_current_membership == true
               and required_current_membership_exact == true)
```

この集合と`required_intent_ordinals` exact setの集合一致を必須とする。同一ordinalの
distinct decisionは許容し、集合比較では1 identityとして扱う。missing membershipと
required set外membershipは`REQUIRED_SET_DECISION_MEMBERSHIP_SET_MISMATCH`でfail-closeする。

[事実] `measurement_arm_qpc`、`measurement_start_qpc`、
`frozen_measurement_end_qpc`をrequired fieldとし、checkerが各`decision_qpc`を
`PRE_MEASUREMENT_ARM`、`ARMED_PRE_MEASUREMENT`、`WITHIN_CURRENT_MEASUREMENT`、
`POST_MEASUREMENT`へ再分類する。producer記録値はconsistency witnessであり、再分類値との
不一致は`MEASUREMENT_BOUNDARY_RELATION_MISMATCH`でfail-closeする。

[事実] scheduler start時点でrequired intent setを生成し、Presented populationや成功した
decision populationからは生成しない。各decisionではQPC、required membership、そのexact
flag、producer-side boundary relationをshadow-only ledgerへ記録する。native Present hook ABI、
performance accounting、frameSwapped retirementは変更していない。

[事実] C2.1とschedulerの対象testは15/15、C1/C2回帰は63/63通過した。

[事実] instrumentation後のfresh 3-runを次へ新規取得した。historical artifactは
上書きしていない。

```text
build/p2-d5-2-w2-c011-fresh-3-20260825-c21
```

C0.1.1 candidate coverageは3/3 PASS、C1 mappingとsealed replayも3/3 PASSだった。
C1 formal Presentedは444件、observed diagnostic populationは889件である。

[事実] C2.1 checkerの再集計結果は各runで同じだった。

```text
required_intent_ordinals exact set       {0, ..., 299} (300)
exact required membership ordinal set                 146
required set missing membership ordinals              154
membership outside required set                         0
CURRENT decisions                                      148
CURRENT distinct ordinals                              147
CURRENT duplicate ordinal extra                          1
CURRENT min / max                                  0 / 301
boundary QPC exact                                    true
boundary relation mismatch                               0
```

ordinal `0`はFOREIGN 1 decisionに加え、membership true/exactのCURRENT 2 decisionsを
持つ。ordinal `301`はCURRENTだがmembership false/exactである。ただしrequired setと
membership setのidentityが閉じていないため、この観測だけでproducer/boundary bugを
確定しない。

[exit] blockerは`REQUIRED_SET_DECISION_MEMBERSHIP_SET_MISMATCH`のみで、verdictは
`REQUIRED_INTENT_DOMAIN_AUTHORITY_UNRESOLVED`。Branch A/Bはいずれも未確立である。

```text
build/p2-d5-2-w2-c14-mapping-replay-fresh-3-20260825-c21.json
build/p2-d5-2-w2-c21-required-intent-domain-fresh-3-20260825-c21.json
```
