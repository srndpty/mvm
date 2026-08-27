# P2-D5-2 W4-C2 Scheduler Invocation Ledger — IMPLEMENTED / CAPTURE PENDING

W4-C1では5,223 transition中5,217件をcompleted refresh inputへexact compatibility帰属できたが、
terminal branch executionとexplicit stopの順序は既存ledgerから識別できなかった。W4-C2は
diagnostic flag有効時だけ全`selectForRender()` invocationを記録する。

## implementation

CLI flag:

```text
--w4-c2-scheduler-invocation-ledger
```

flag無効時はinvocation vectorをreserve/pushせず、既存canonical pathへ常時収集負荷を入れない。
flag有効時のschema:

```text
mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1
```

1 invocationごとに次を保存する。

```text
scheduler_invocation_serial / invocation_qpc / input_authority
pre / post scheduler state
result / branch-exact reason
decision fields
required_intent_membership
formal_transport_disposition
state_transition_exact
```

result enumは次だけである。

```text
PRIMARY_DECISION
DUPLICATE_DECISION
OUTSIDE_SOURCE_DOMAIN_DECISION
INVALID_FATAL
```

required-intent domainはresult enumへ混ぜず、`required_intent_membership`と
`formal_transport_disposition`へ分離した。実際、5秒smokeのterminalは次であり、source domain外と
required intent domain外が同時に成立したが、二つは独立fieldとして記録されている。

```text
result                        OUTSIDE_SOURCE_DOMAIN_DECISION
intent_ordinal                301
past_source_domain            true
required_intent_membership    false
formal_transport_disposition SUPPRESS_OUTSIDE_REQUIRED_SET
```

60秒のsealed W3 cohortではterminal primaryがrequired membership内だったため、正式C2 captureでも
特定のmembership値を前提にしない。checkerはmembershipに応じたtransport dispositionの整合だけを
要求する。

## branch coverageとnegative

C++ unit testはprimary、duplicate、source-domain terminal、invalid fatalを固定した。
PowerShell契約は次をnegativeで拒否する。

```text
NegativeInvocationSequenceGap
NegativePrePostStateMutation
NegativeDecisionReasonMutation
NegativeDecisionWithoutTransport
NegativeSourceRequiredDomainConflation
NegativePostTerminalInvocation
NegativeCompletedOrdinalMutation
NegativePerformanceAuthorityPromotion
```

checkerはserial gap、pre/post mutation、result/reason 1:1、anchored
`completed refresh + 1`、terminal後invocation 0件を検査する。

## runtime smoke

dirty worktree上の非authority smokeを3回試した。最初の2回はGUI processへ
`-WindowStyle Hidden`を指定したためmetrics未生成・exit 6となった。artifactは削除していない。
visible windowへ直した3回目は成立した。

```text
invocation_count                 148
PRIMARY_DECISION                 146
DUPLICATE_DECISION                 1
OUTSIDE_SOURCE_DOMAIN_DECISION     1
INVALID_FATAL                      0
terminal invocation serial       148
post-terminal invocation count     0
verdict SCHEDULER_INVOCATION_CONTROL_FLOW_EXACT
```

このsmokeはdirty worktree、5秒条件であり、W4-C root-cause closureには使わない。

## formal capture blocker

正式runnerはfresh checkpoint、clean worktree、binary/source/Qt hash、schema versionをbindし、
既存artifactを上書きしない。現在のworktreeには本変更が未commitで存在するため、runnerはcaptureを
開始する前に次でfail-closedした。

```text
W4-C2 diagnostic captureはclean worktreeから取得してください
output_exists = false
```

ユーザーの明示依頼なしにcommitしない規約のため、正式captureは保留する。

## current verdict

```text
w4_c2_instrumentation_implemented = true
w4_c2_contract_tests_pass = true
w4_c2_runtime_smoke_pass = true
w4_c2_formal_capture_performed = false
canonical_performance_authority = false
historical_w3_verdict_rewritten = false
root_cause_determined = false
```
