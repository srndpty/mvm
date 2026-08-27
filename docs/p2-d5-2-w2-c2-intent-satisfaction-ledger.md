# P2-D5-2 W2-C2 Intent Satisfaction Ledger

## authority

C2 の入力は C1 checkpoint `5034bfcd41dd9f5c860827a9594b604be5db7446` で固定した。
C2 runner / checker は最初に C1.4 checker を実行し、次に sealed C0 inventory、
B2 terminal records、physical VBlank samplesからformal Presented集合とphysical mappingを
再生する。C1 artifactのrecord値だけをコピーしない。

初期実装はLayer 1Aの`required_measurement_frame_count`から`[0, count)`を構成していた。
W2-C2.1でこのgapを検出したため、以下のfresh-2結果は**provisional count-derived
domain解釈下のhistorical diagnostic**として保存する。現行runner / checkerはC2.1
checkerを先に実行し、C2.1 artifactの`required_scheduler_intent_ordinals` exact setを
直接consumeする。Presented min/maxまたはcountからidentity setを復元しない。

## recordとaccounting

各formal Presented recordは次を持つ。

```text
exact_event_key
etw_sequence
intent_scope / intent_scope_exact
intent_ordinal / intent_ordinal_valid / intent_ordinal_exact
physical_vblank_ordinal
in_measurement_physical_domain
classification
```

source frame identityは保持・参照しない。satisfactionはin-domainのexactな
`CURRENT_MEASUREMENT intent_ordinal` identityだけで数える。
`FOREIGN_PRE_MEASUREMENT`はphysical fillには寄与するがcurrent satisfactionには寄与しない。

C2 checkerはrecordsから次を再集計する。

```text
satisfied_intent_count
  + in_domain_presented_foreign_intent_count
  = in_domain_presented_event_count

filled_physical_opportunity_count
  = unique(in-domain formal Presented physical_vblank_ordinal)
```

後者と`in_domain_presented_event_count`の一致は、C1の
one-formal-Presented-per-physical-ordinal authority成立時だけのderived identityとして扱う。

## fresh-2 offline適用結果

入力:
`build/p2-d5-2-w2-c14-mapping-replay-fresh-2-20260825.json`

出力:
`build/p2-d5-2-w2-c2-intent-satisfaction-fresh-2-20260825-r3.json`

[事実] sealed fresh-2へoffline適用し、次を再集計した。

```text
formal Presented                         374
required current intents                 900 (300 x 3 run)
in-domain Presented                     374
filled physical opportunities           374
satisfied current intents               368
in-domain foreign Presented               0
duplicate current intent satisfaction     3
current intent outside [0, 300)            3
missing / ambiguous intent provenance      0 / 0
multiple Presented per physical ordinal    0
```

[事実] provisional `[0, 300)`解釈では、各runに
`CURRENT_MEASUREMENT intent_ordinal=0`のin-domain Presentedが2件あり、各runに
domain外と分類される`CURRENT_MEASUREMENT intent_ordinal=301`が1件あった。

[exit] C2 ledgerは次のblockerで`INTENT_SATISFACTION_LEDGER_INVALID`となり、
closure候補ではない。

```text
DUPLICATE_CURRENT_INTENT_SATISFACTION
CURRENT_INTENT_OUTSIDE_REQUIRED_DOMAIN
INTENT_SATISFACTION_ACCOUNTING_IDENTITY_VIOLATION
```

physical fill identityは成立している。provisional解釈下のN1は
`368 + 0 != 374`のため不成立である。6件を確定root causeとは扱わない。
required intent identity authorityが確定するまでA/B分岐は未確定である。
threshold、fps、canonical PASS/FAIL、frameSwapped retirementには接続していない。

## 再現

```powershell
pwsh scripts/build-p2-d5-2-w2-c2-intent-satisfaction-ledger.ps1 `
  -C1Proof build/p2-d5-2-w2-c14-mapping-replay-fresh-7-20260825-c24.json `
  -C21Proof build/p2-d5-2-w2-c21-required-intent-domain-fresh-7-20260825-c24.json `
  -Output build/p2-d5-2-w2-c2-intent-satisfaction-<new-name>.json

ctest --test-dir build/ucrt64-release `
  -R 'p2_d5_2_w2c2_' --output-on-failure --timeout 120
```

fresh-2へのrunner終了コードは、上記blockerを検出するため非0になる。artifactはfail前に保存する。

## C2.4後 fresh-7 closure候補

[事実] C2.4後のfresh-7 sealed C1と、同じC1を参照するC2.1 exact required setから
C2 ledgerを再構築した。runner / checkerの双方がC1.4とC2.1 checkerを独立に再実行し、
C2.1と別C1のhashを組み合わせた入力はnegative integrationでrejectする。

```text
formal Presented                              438 (146 x 3 run)
required current intents                      900 (300 x 3 run)
in-domain Presented                           438
satisfied current intents                     438
in-domain foreign Presented                     0
filled physical opportunities                 438
duplicate current intent satisfaction           0
current intent outside exact required set       0
missing / ambiguous intent provenance           0 / 0
multiple Presented per physical ordinal         0
```

[事実] recordsから再集計した次のidentityは両方成立した。

```text
438 satisfied + 0 foreign = 438 in-domain formal Presented
438 filled = 438 unique in-domain formal Presented physical ordinals
```

後者はC1 one-formal-Presented-per-physical-ordinal成立時のderived identityであり、
一般契約へ拡張しない。

```text
build/p2-d5-2-w2-c2-intent-satisfaction-fresh-7-20260825-c24.json
```

[事実] closure review後、membership provenance検査をduplicate suppressionより先へ移し、
C2.4 checkerでsuppression reasonとeligibilityをproducer semanticsへexact-bindした。fresh-7を
再取得せずoffline再検証し、C1、C2.1、C2.4、C2 checkerはすべてPASSした。C2 ledger値と
accounting identityは変化していない。

[exit] fresh-7 C2 ledgerは`INTENT_SATISFACTION_LEDGER_EXACT`。W2-C2はPASS / CLOSED。
required 900件のうち462件はPresentedされておらずunsatisfiedのままであり、C2ではdrop rate、
fps、threshold、canonical PASS/FAILを評価していない。
