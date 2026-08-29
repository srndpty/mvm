# P5-E4 S2-d — W4-C0 contract 再導出 closure

## 1. 変更

旧W4-C0 static guardを、B3前のphysical refresh counter semanticsから
`docs/p5-e4-s2-d-w4-c0-current-control-flow-contract.md`に定義したcurrent semanticsへ
再導出した。

削除されたliteralの期待件数を0へ下げる方法は採っていない。代わりに次を構造で固定した。

```text
ordinal producer
  RequiredIntentQueue::reserveHead()
    -> reservation.intentOrdinal
    -> decision.opportunityOrdinal

forbidden ordinal reconstruction
  physical refresh / completed + 1
  source / target frame
  callback / render ordinal
  QPC

selectForRender
  9/9 return -> finishInvocation

lastFinalizedOrdinal_
  writer 1
  applyPendingOpportunityFinalization内
  finalizePendingOpportunity -> prepare -> apply

source coverage error
  SourceCoverageInsufficient
  -> InvalidFatal / PastSourceDomain

DOMAIN_TERMINAL
  successful completion不可

normal completion owner
  PLANNED_WINDOW_ENDのみ
```

production source、threshold、W3 / P3-C-2 / P4 contractは変更していない。

## 2. 修正前

```text
GoodStaticInventory     FAIL 1
negative                 5
false-green negative     4
```

Goodは旧`ordinal = completed + 1` assertionで失敗していた。そのため次の4件は
自身のmutationと無関係に同じ旧assertionでthrowしてPASSしていた。

```text
NegativeUnclassifiedLastFinalizedWriter
NegativeUnclassifiedNoDecisionReturn
NegativeSecondIntentProducer
NegativeIndirectOrdinalReconstruction
```

## 3. closure evidence

採取条件:

```text
checkpoint  e74c87b83c78f9e4a72798bd78188db03355f3e4 + S2-d の未コミット差分
build       build/ucrt64-release
date        2026-08-29
```

各caseを個別実行し、source fingerprintの差でmutation適用を確認した後、
次の固有violation messageへ到達した。

```text
NegativeUnclassifiedOrdinalWriter
  未分類のopportunity ordinal writerがあります (actual=2 expected=1)

NegativeUnclassifiedLastFinalizedWriter
  未分類のlast finalized writerがあります (actual=2 expected=1)

NegativeUnclassifiedNoDecisionReturn
  invocation ledgerを迂回するinvalid/no-decision returnがあります (actual=1 expected=0)

NegativeSecondIntentProducer
  intent transport producerが単一ordinal直結ではありません (actual=0 expected=1)

NegativeIndirectOrdinalReconstruction
  target/source/callback/QPCからintent ordinalを間接再構築しています
```

CTest登録経路:

```powershell
ctest --test-dir build\ucrt64-release `
  -R '^p2_d5_2_w4c0_static_control_flow_' `
  --output-on-failure --timeout 60
```

```text
6/6 PASS (Good 1 + negative 5)
timeout 0
```

termination semanticsの独立negative authorityも確認した。

```text
p2_b3_i1_required_intent_queue_guard_negativeplannedenddropsactive       PASS
p2_b3_i1_required_intent_queue_guard_negativedomainterminalcompletion   PASS
p2_b3_i1_required_intent_queue_guard_negativesourcecoverageskipped      PASS

3/3 PASS
timeout 0
```

B3-I1のruntime unit testとarchitecture Goodも2/2 PASSした。

## 4. 判定

```text
S2-d                                  CLOSED
W4-C0 ordinary failures               1 -> 0
observed W4-C0 false-green negatives  4 -> 0
negative mutation application         5/5 verified
negative intended violation           5/5 verified
current termination negative authority 3/3 PASS
B3-I1 positive authority               2/2 PASS
production semantics changes          0
assertion / threshold relaxation      0
```

S2-dは閉じた。P5-E4 ordinary regression gateは、S2-eが未完了のためOPENのままである。
