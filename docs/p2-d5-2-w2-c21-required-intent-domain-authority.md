# P2-D5-2 W2-C2.1 Required Intent Domain Authority Attribution

## 目的

`required_measurement_frame_count`から`[0, count)`を生成せず、scheduler sole producerが
記録したrequired current intent identity setを確定する。producer、historical C2、
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
