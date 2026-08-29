# P5-E4 S2-d — W4-C0 current control-flow contract

## 1. 状態

```text
contract                         CURRENT
w4_c0_current_contract           true
ordinal_authority                REQUIRED_INTENT_QUEUE_RESERVATION
source_coverage_failure          PROTOCOL_FATAL
domain_terminal_success          false
normal_completion_owner          PLANNED_WINDOW_END
```

この文書はB3後のcurrent runtimeを対象にする。旧W4-C0が固定していた
physical refresh counter由来のordinal semanticsを復活させない。

## 2. required intent ordinal producer

`PresentationOpportunityScheduler::selectForRender()`におけるsemantic producerは
次の1経路だけである。

```text
RequiredIntentQueue::reserveHead()
  -> queueDecision.reservation.intentOrdinal
  -> local ordinal
  -> decision.opportunityOrdinal
  -> nativePresentToken.setFormalIntentOrdinal(decision.opportunityOrdinal)
```

次のidentityからintent ordinalを再構築してはならない。

```text
physical refresh ordinal / DWM counter
source frame / target frame
callback index / render ordinal
QPC
```

旧`presentationOpportunityOrdinal(originRefreshCount_, ...)`と
`ordinal = completed + 1`はsupersededであり、current contractでは禁止する。

## 3. selectForRender invocation closure

`selectForRender()`の全returnは`finishInvocation()`を通り、invocation ledgerの
pre/result/reason/decision/postを閉じる。裸の`return {}`や未分類returnを許可しない。

current branch partitionは次である。

```text
PrimaryDecision                  1
DuplicateDecision                1
RequiredQueueExhaustedDecision   1
InvalidFatal                     6
OutsideSourceDomainDecision      0
total finishInvocation return    9
```

`RequiredQueueExhaustedDecision`はrequired setを全て発行済みであることを表す
valid decisionであり、normal measurement completion authorityではない。

## 4. last finalized writer

`lastFinalizedOrdinal_`のsemantic writerは
`applyPendingOpportunityFinalization()`内の次の1箇所だけである。

```text
lastFinalizedOrdinal_ = prepared.record.actualOpportunityOrdinal
```

`finalizePendingOpportunity()`は
`preparePendingOpportunityFinalization()`成功後に必ず
`applyPendingOpportunityFinalization()`を呼ぶ。swap commitが前のpending opportunityを
advanceする場合も同じapply関数を使い、writerを複製しない。

## 5. source coverage とtermination

queue reservation後のtargetがsource domain外なら、次の順でfail-closeする。

```text
decision.pastSourceDomain = true
pastSourceDomain_ = true
fail(SourceCoverageInsufficient)
decision.valid = false
finishInvocation(InvalidFatal, PastSourceDomain, decision)
```

これはprotocol fatalであり、performance drop、successful domain terminal、
required set縮小へ変換しない。`OutsideSourceDomainDecision`の旧success pathは
supersededである。

`DOMAIN_TERMINAL`はnormal successful completionになれない。
`finishMeasurement(... DomainTerminal ...)`と
`formalOpportunityDomainReached.store(true, ...)`はcurrent sourceに存在してはならない。

normal completion ownerは`PLANNED_WINDOW_END`だけである。

```text
intervalEnded
  -> claimStopCause(PlannedWindowEnd)
  -> finishMeasurement(... PlannedWindowEnd ...)
  -> closePlannedWindow()

ExplicitStop / Fatal / その他
  -> closeWithoutNormalCompletion()
```

## 6. negative authority

W4-C0固有の5negativeは、ordinal writer、last-finalized writer、裸return、
transport producer、間接再構築をそれぞれ1件ずつ破壊する。各caseはmutation適用と
固有violation messageの完全一致を要求する。

source coverage、DOMAIN_TERMINAL、planned-end ownershipは既存のB3-I1 authorityも
独立にnegative検証している。

```text
NegativeSourceCoverageSkipped
NegativeDomainTerminalCompletion
NegativePlannedEndDropsActive
```

W4-C0 guardはこれらのcurrent invariantを再確認するが、旧counter semanticsの
occurrence countを現在値へ合わせる方法では契約しない。
