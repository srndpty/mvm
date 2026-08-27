# P2-D5-2 W4-B Producer Semantics Attribution — CLOSED

W4-A は 5574 件の unsatisfied がすべて primary scheduler decision の生成以前に集中することを
exact に閉じた。W4-B は isolated missing と double-missing boundary に対応する
**producer-side exact semantic transition** を識別した段である。

```text
W4-B  CLOSED
      attribution_exact        = true
      verdict                  = PRODUCER_SEMANTICS_ATTRIBUTION_EXACT
      root_cause_determined    = false
      new_capture_performed    = false
      producer_instrumentation_changed = false
```

契約は [p2-d5-2-w4-b-producer-semantics-contract.md](p2-d5-2-w4-b-producer-semantics-contract.md)
に freeze してある。新規 capture は取得していない。W3 で保存済みの同一 sealed cohort を
offline 再評価した。

## cohort 分類 (3 run 合計)

maximal consecutive missing run + precedence で排他的に分類した。

```text
HEAD_EDGE                event=0     intent=0
TAIL_EDGE                event=3     intent=3
LONGER_MISSING_RUN       event=0     intent=0
DOUBLE_MISSING_BOUNDARY  event=348   intent=696
ISOLATED_MISSING         event=4875  intent=4875
OTHER_PATTERN            event=0     intent=0
                         intent sum  5574 = W4-A unsatisfied
```

3 run で完全一致する (各 run: isolated 1625 / double 116 / tail_edge 1)。

`event x 2 = intent` (double) と `event = intent` (isolated) の identity が成立している。
isolated が 1626 でなく 1625 なのは、1 件が precedence により TAIL_EDGE へ入るためであり
契約どおりの動作である。

## A. ordinal-domain — exact な反転

```text
                                          ISOLATED (1625/run)   DOUBLE (116/run)
primary_decision_intent_ordinal_delta     2 (全件)              3 (全件)
target_frame_delta                        2 (1622), 3 (3)       3 (全件)
last_finalized_opportunity_ordinal_delta  2 (1507), 3 (116)     2 (全件)
repeat_before / repeat_after              全件 false            全件 false
past_source_domain_before / after         0 / 1                 0 / 0
interval duplicate_callback_record_count  1                     0
```

[事実] double-missing boundary では intent ordinal が +3 進むのに
`lastFinalizedOpportunityOrdinal` は +2 しか進まない。一方 isolated 側にはちょうど
**116 件** (double の件数と同数) だけ lastFinalized が +3 進むものがある。
位相のずれと戻りが 1:1 で対応している。

`duplicate_callback` は primary transition field に含めていない。primary decision の定義が
`duplicate_callback == false` である以上 before/after は恒等的に false であり比較に意味が
無いためである。代わりに primary(before) と primary(after) の open interval 内に存在する
producer ledger record のうち `duplicate_callback == true` の件数を数えている。

## B. time-domain (run ごと、3 run 完全一致)

```text
primary_decision_active_span_seconds       29.0575384
primary_decision_interdecision_cadence_hz  59.9156      (N-1)/span
measurement_window_seconds                 60.0         W2-A physical authority
primary_decision_active_span_fraction      0.4843
head_without_primary_decision_seconds       0.0
tail_without_primary_decision_seconds      30.9425
first / last primary intent ordinal        0 / 3598
first / last required intent ordinal       0 / 3599
trailing_missing_required_intent_count      1
legacy_measurement_elapsed_seconds_diagnostic  29.0575384
legacy_elapsed_minus_producer_span_seconds     3.55e-15
legacy_measurement_elapsed_used_as_authority   false
decision_span_used_as_measurement_window       false
```

[事実] primary decision の発行レートは 59.92/s であり refresh rate そのものである。
隔回に間引かれてはいない。間隔 > 25ms は run 全体で 1 件のみ (最大 33.5ms)。
head gap 0 で measurement window 開始と同時に始まり、29.06 秒で止まり、
末尾 30.94 秒は decision が 1 件も無い。

[事実] legacy `measurement_elapsed_seconds` は producer active span と一致した
(差 3.55e-15 秒 = double 丸め誤差のみ)。これは W2-E で diagnostic へ降格した legacy 値が
何を測っていたかの裏付けになる。ただし **correlation として記録するだけで canonical
時間 authority へは戻していない**。tolerance 付きの match bool は作らず、差そのものを保存
している。

### measurement window は producer から作らない

`measurement_window_seconds` は W2-A physical authority
(`measurement_start_qpc` / `measurement_end_qpc_exclusive` / `qpc_frequency`) から受け取る。
decision stream から推定しない。head/tail gap はこの 2 ソースを分けて bind して計算する。

### cadence の式

decision が `N` 件なら interval は `N-1` 個である。

```text
cadence_hz = (N - 1) / ((last_decision_qpc - first_decision_qpc) / qpc_frequency)
```

## W4-B closure で言えること / 言わないこと

```text
言える:
  primary decisions は約 60Hz で継続的に発行されたが、
  その decision stream は canonical 60s window の約半分 (29.06s / 48.4%) で終了している。
  各 decision 間で intent ordinal はおおむね +2 進んでいる。

言わない:
  ordinal を +2 したから 29 秒で required domain を使い切って停止した
  30fps source が原因 / Qt callback cadence が原因 / scheduler bug / instrumentation overhead
```

A と B をまだ因果で結んでいない。

## checker

checker は artifact の aggregate を信用しない。

- W4-A checker を再実行する (その中で W3 / W2-E / W2-D / 各 upstream も再実行される)
- W4-A proof の SHA を bind し、別 proof を指す splice を reject
- cohort identity (intent sum / event x2 = intent) を artifact 上でも先に検査
- **time-domain も独立再計算する。** sealed producer ledger の primary decision QPC と
  W2-A physical window から `active_span` / `cadence` / `head_gap` / `tail_gap` /
  `fraction` / `legacy delta` を再計算し、artifact と field 単位で照合する
- sealed producer records から event を再構築し artifact 全体を比較
- `root_cause_determined` / `new_capture_performed` / `producer_instrumentation_changed` /
  `legacy_measurement_elapsed_used_as_authority` / `decision_span_used_as_measurement_window`
  が true の artifact を reject

```text
P2-D5-2 W4-B checker: PASS missing=5574 isolated=4875 double=348
```

## negative

```text
NegativeMissingSetSplice                        W4-A missing setと違う集合
NegativeIsolatedClassifiedAsDouble              cohort分類の取り違え
NegativeDoubleEventCountMutation                event x2 = intent の破れ
NegativeMissingOrdinalInterpolatedProducerField missing側fieldの補間
NegativeNearestDecisionQpcUsed                  exactでないdecision_qpcでの代用
NegativeCrossRunNeighborSplice                  run境界をまたぐ近傍接続
NegativeRootCauseDeclared                       root_cause_determined=true の注入
NegativeAggregateOnlyTransitionForgery          transition tableのaggregateだけ偽造
NegativeDecisionSpanUsedAsMeasurementWindow     decision spanをmeasurement windowにする
NegativeLegacyElapsedPromotedToAuthority        legacy elapsedを時間authorityへ昇格
NegativeTailGapMutation                         tail gapの改変
```

positive として `GoodEdgeCohorts` / `GoodLongerMissingRun` を置いた。`LONGER_MISSING_RUN` は
実測 0 件なので、これが無いと「0 件」が検出能力ゼロの結果と区別できない。

### `NegativeCrossRunNeighborSplice` の設計

当初「run 内に近傍 primary が無い missing」を作ろうとしたが、**missing set が整合していれば
interior の近傍欠落は構造的に発生し得ない** (run 境界は非 missing ordinal で定義され、
非 missing なら必ず primary を持つため)。そこで「run 末尾の missing が TAIL_EDGE に入り、
`after_primary` と `transition` が null のままである」= splice しないことを検証する形にした。
cross-run で埋めていれば `after_primary` が非 null になる。

## artifact

```text
build/p2-d5-2-w4-b-producer-semantics-20260826.json   (7.3 MB)
```

全 event の before/after primary snapshot、transition、interval diagnostics、
run-level time-domain summary を保存している。missing ordinal 自身には producer semantic
field を置いていない。

## 再現

```powershell
pwsh scripts/build-p2-d5-2-w4-b-producer-semantics.ps1 `
  -W4AProof build/p2-d5-2-w4-a-attribution-20260826.json `
  -Output build/p2-d5-2-w4-b-producer-semantics-<new-name>.json

pwsh scripts/check-p2-d5-2-w4-b-producer-semantics.ps1 `
  -Proof build/p2-d5-2-w4-b-producer-semantics-20260826.json

ctest --test-dir build/ucrt64-release -R 'p2_d5_2_w4b_' --output-on-failure --timeout 300
```

`-SkipW4AReplay` は開発用の逃げ道であり closure の証拠には使わない。

## 次 — W4-C

W4-B が閉じたので、次は初めて因果帰属に入る。

```text
なぜ約60Hzでdecisionを出しながら、intent ordinalが主に+2で進み、
約29秒でdecision streamが終わるのか
```

これをコード経路と scheduler semantics に沿って追う段になる。producer instrumentation の
変更や新規 capture が必要になる可能性があるため、着手前に W4-C の契約を freeze する。
