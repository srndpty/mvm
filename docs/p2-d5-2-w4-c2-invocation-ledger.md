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

## formal capture attempt

checkpoint `8b0640b`で最初のformal runを開始したが、runnerの既定warmupが2秒で、sealed W3の
5秒条件と一致していなかった。run 1は約72秒後にexit 6となりmetricsを生成せず、残りrunは
fail-closedで実行していない。ledger無効controlも同じexit 6だったため、C2 ledger固有の失敗ではない。
両artifactは保存し、正式cohortには含めない。warmup差をexit 6の原因とは断定しない。

```text
build/p2-d5-2-w4-c2-formal-20260827-8b0640b
build/p2-d5-2-w4-c2-control-no-ledger-20260827-8b0640b
classification = PROTOCOL_INVALID_CONFIGURATION
root_cause_of_exit_6 = NOT_ESTABLISHED
```

W3との比較可能性を回復するため、formal runnerの既定値を`warmup=5, measure=60`へ修正し、
architecture mutation testで固定する。

修正後のW3一致条件でもrun 1は同じexit 6となった。したがってwarmup差はexit 6の原因ではない。
同一失敗が3回連続したためretryを停止し、既存のexit 6分岐を区別するdiagnostic stderrを追加する。

```text
build/p2-d5-2-w4-c2-formal-20260827-b1f63fb
formal_capture = FAILED_CLOSED
root_cause_of_exit_6 = NOT_ESTABLISHED
W4_C_LINK_A_TO_B = NOT_ESTABLISHED
```

停止stage診断では次をexactに観測した。

```text
stage = REQUESTED
worker_a_joined = 1
worker_b_joined = 1
render callback after teardown request = 0
```

worker join待ちやGPU drainではなく、measurement/capture gate close後のteardown要求がrender callbackへ
配送されていない。itemと所有windowの両方への`update()`およびrender jobによるwakeを診断的に試したが、
いずれも次frameを起動しなかった。この試行は撤回し、正式captureには含めない。

追加診断でterminal callbackは`RENDER_CALLBACK_EXITED`まで到達し、render jobも実行されなかった。
`nativePresentEnvelopeStopped`がcallback内部でpublishされた直後、GUI timerがcallback退出前に
`performShutdown()`を開始し、次frame要求が進行中frameへcoalesceされる順序と整合する。
したがってwake jobは撤回し、`CaptureEnvelopeStopWait`がrender callback退出を観測してから
teardownを要求するbarrierへ置き換える。

barrierだけではGUI側updateの配送は回復しなかった。native envelope停止callbackからrender threadの
`update()`で次callbackを予約し、controllerのteardown要求到着まではscheduler/native Presentより前の
gateで再予約するbridgeを追加する。したがってbridge callbackはscheduler invocation ledgerや
native Present ledgerへ混入しない。

bridge確認runではteardownとmetrics生成が完了し、C2 checkerは1743 invocation、terminal serial
1743、post-terminal 0でPASSした。一方、W2 physical VBlank successorがtimeoutし、mapping support
closeだけがexit 6となった。observer ring overflowは245,023,994であり、C2 scheduler証跡とは別の
authority failureである。

```text
build/p2-d5-2-w4-c2-post-envelope-bridge-smoke-20260827-e34768c
C2 checker = SCHEDULER_INVOCATION_CONTROL_FLOW_EXACT
process exit = 6 (PHYSICAL_VBLANK_SUCCESSOR_TIMEOUT)
formal cohort inclusion = false
```

正式C2 modeではphysical VBlank successor / mapping supportを要求せず、schemaにも両authorityが
falseであることを固定する。measurement stop QPCとnative envelope close QPCはraw値として残すが、
Link A→Bやexplicit stop順序の判定はC3まで行わない。

successor判定を外した最初のrunは、observer threadの停止joinで120秒timeoutした。physical mapping
authorityを持たないC2でobserverを開始してから無視する設計自体が不適切なので、C2 modeではobserverを
起動しない。DWM completed-refresh scheduler authorityとnative envelopeは引き続き有効である。

observer非起動後の最初の60秒runも120秒timeoutし、metricsとstderrを生成しなかった。既存診断は
shutdown到達後しか出力しないため、このartifactだけでは停止phaseを識別できない。同じ長時間runの
再試行は行わず、C2 modeに限りcontroller phase遷移をstderrへ一度ずつflushする診断を追加した。

```text
build/p2-d5-2-w4-c2-no-physical-observer-smoke-20260827-d7e9629
classification = DIAGNOSTIC_TIMEOUT_PHASE_NOT_OBSERVABLE
formal cohort inclusion = false
```

phase trace付きの5秒短縮runは`CAPTURE_ENVELOPE_STOP_WAIT`まで到達して45秒timeoutした。
observerを起動しない場合、measurement stop callback退出前にGUI側のenvelope stop updateが
coalesceされ、次callbackが失われる。observer待機は従来この順序を偶然回避していた。

```text
build/p2-d5-2-w4-c2-phase-trace-short-20260827-b0fec49
last controller phase = CAPTURE_ENVELOPE_STOP_WAIT
formal cohort inclusion = false
```

measurement stop後はscheduler/native Present token生成より前のgateでrender callbackを橋渡しし、
stop requestを同じgateで処理する。これによりterminal後のscheduler invocationを増やさない。
またnative envelope stopが15秒以内に完了しなければexit 6でfail-closeし、外部runnerのtimeoutまで
無期限に待たない。

## formal C2 capture result

checkpoint `4f2b950`のclean worktreeから、sealed W3と同じ`warmup=5`、`measure=60`で
3 runを取得した。runnerのpost provenance checkと各runのinvocation checkerはすべてPASSした。

```text
build/p2-d5-2-w4-c2-formal-20260827-4f2b950
runs = 3/3 PASS
verdict = SCHEDULER_INVOCATION_CONTROL_FLOW_EXACT (3/3)
invocations = 1743 / 1743 / 1743
PRIMARY_DECISION = 1741 / 1741 / 1741
DUPLICATE_DECISION = 1 / 1 / 1
OUTSIDE_SOURCE_DOMAIN_DECISION = 1 / 1 / 1
INVALID_FATAL = 0 / 0 / 0
terminal serial = 1743 / 1743 / 1743
terminal intent ordinal = 3598 / 3598 / 3598
required_intent_membership = true / true / true
past_source_domain = true / true / true
formal_transport_disposition = TRANSPORT / TRANSPORT / TRANSPORT
post_terminal_invocation = 0 / 0 / 0
measurement elapsed seconds = 29.0574383 / 29.0574563 / 29.0573773
canonical_performance_authority = false
```

source-frame terminalとrequired-intent membershipが同時に別fieldとして記録され、3 runとも
`past_source_domain=true`かつ`required_intent_membership=true`だった。したがって両domainを
同一視していないこともruntimeで確認できた。

C2はterminal branch executionとpost-terminal invocation 0件を閉じるが、terminal invocationから
`finishMeasurement()`のcapture gate closeへ至った因果edge、および`EXPLICIT_STOP`が先にpublish
されていないことをbranch-exact fieldとして持たない。terminal invocation QPCとmeasurement stop QPCは
一致するが、QPC一致だけを因果joinに使わない。よって現時点の判定は次のままとする。

```text
Cause B DOMAIN_TERMINAL = BRANCH_EXECUTION_EXACT
Cause B PLANNED_WINDOW_END = EXACT_INCOMPATIBLE
Cause B EXPLICIT_STOP = NOT_OBSERVABLE
Link A -> B = NOT_ESTABLISHED
root_cause_determined = false
attribution = PARTIAL
```
