# P2-D5-2 W4-B Producer Semantics Attribution — Contract (FROZEN)

W4-A は 5574 件の unsatisfied がすべて primary scheduler decision の生成以前に集中することを
exact に閉じた。W4-B は「どの producer semantic が `NO_PRIMARY_SCHEDULER_DECISION` を作るか」
という別レイヤの原因帰属に入る。

**本 contract は producer-field 集計コードを書く前に freeze する。**

## 位置づけと禁止事項

W4-B の closure は次に限定する。

```text
isolated missing と double-missing boundary に対応する
producer-side exact semantic transition を識別できた
```

この段階では次のいずれも結論にしない。

```text
30fps source が原因
Qt callback cadence が原因
scheduler bug
instrumentation overhead
```

artifact に固定する flag:

```text
root_cause_determined            = false
new_capture_performed            = false
producer_instrumentation_changed = false
```

新規 capture は取得しない。W3 で保存済みの同一 sealed cohort だけを offline 再評価する。

## 母集団

母集団は W4-A と同じ **exact required current intent set = 10800** である。
missing set は W4-A の `missing_primary_decision_ordinals` と完全一致しなければならない。
W4-A が `M = required_set - primary_decision_ordinal_set` を集合差そのものから作っている点を
そのまま維持する。

per required ordinal で持つもの:

```text
required_ordinal
primary_decision_present

if primary exists:
  そ の primary decision の exact producer fields

if primary absent:
  previous_primary_ordinal
  next_primary_ordinal
  previous / next の exact producer record
  gap_width
```

### missing ordinal 自身に存在しない field を補間しない

これが W4-B の最重要禁止事項である。missing ordinal には decision record が存在しない。
present 側の近傍から内部状態を推測すると heuristic に戻る。

```text
禁止:
  targetFrame  を previous/next から推定する
  decision_qpc を midpoint にする
  repeat       を周囲の値から推測する
  nearest decision QPC で代用する
```

missing 側は「前後に実在した record」と「gap 幅」だけを持つ。値は作らない。

## cohort 定義 (ordinal pattern だけで exact に固定する)

分類は local pattern matching ではなく **maximal consecutive missing run** から行う。
これにより double run の 2 つの missing ordinal をそれぞれ pattern scan して
「double event を 2 件」と誤カウントする余地が無くなる。

```text
maximal missing run length = 1   -> ISOLATED_MISSING
maximal missing run length = 2   -> DOUBLE_MISSING_BOUNDARY
maximal missing run length > 2   -> LONGER_MISSING_RUN

required-set boundary に接する run -> HEAD_EDGE / TAIL_EDGE
上記のいずれでもない             -> OTHER_PATTERN
```

precedence (先に一致したものを採る):

```text
1. HEAD_EDGE / TAIL_EDGE
2. LONGER_MISSING_RUN
3. DOUBLE_MISSING_BOUNDARY
4. ISOLATED_MISSING
5. OTHER_PATTERN
```

6 つは排他的である。今回の実測では run-length が 1 と 2 しかないため
`LONGER_MISSING_RUN = 0` になるはずだが、checker contract としては存在させ、
出現したら fail-close できるようにする。

期待値 (run ごと):

```text
isolated event count   1626    isolated intent count   1626
double   event count    116    double   intent count    232
                               1626 + 232 = 1858 missing intents
```

### double-missing は event と intent を分けて数える

116 箇所の double missing は、missing intent 数としては 232、phase-shift event としては 116 である。

```text
double_missing_event_count  = 116 / run
double_missing_intent_count = 232 / run
```

混同すると producer event frequency との比較で factor 2 のズレが出る。artifact には両方を持たせ、
`event_count * 2 == intent_count` を identity として検査する。

### cross-run neighbor splice の禁止

run 末尾と次 run 先頭を近傍としてつなぐことを禁止する。近傍探索は run 内で閉じる。
run 境界に接する missing は `HEAD_EDGE` / `TAIL_EDGE` へ入る。

## producer field は「値」より「transition」を中心に

情報量が最も高いのは present → present の間に何が変わるかである。

```text
isolated missing  primary(n-1) -> primary(n+1)   ordinal jump = 2
double missing    primary(n-1) -> primary(n+2)   ordinal jump = 3
```

各 event について exact に出す:

```text
primary_decision_intent_ordinal_delta
decision_qpc_delta
render_begin_qpc_delta
target_frame_delta
last_finalized_opportunity_ordinal_delta

repeat_before / repeat_after
past_source_domain_before / past_source_domain_after
```

`duplicate_callback` は primary transition field に**含めない**。primary decision の定義が
`duplicate_callback == false` である以上、before/after はどちらも恒等的に false であり
比較に意味が無い。phase-shift 近傍の duplicate callback は別 diagnostic として、
primary(before) と primary(after) の **open interval 内**に存在する producer ledger record の
うち `duplicate_callback == true` の件数を exact に数える。

```text
interval_diagnostics.duplicate_callback_record_count
```

これなら primary の意味を汚さず、duplicate callback が phase shift と共起するかを
実際に検査できる。

`last_finalized_opportunity_ordinal` の遷移が特に重要である。double-missing boundary でだけ
通常と異なる遷移が出れば、phase shift を scheduler 内部の ordinal advancement に結びつけられる。
逆に target frame だけが変わっても、それだけでは intent identity の原因とは言えない。

### raw field 名と semantic interpretation を分離する

producer ledger に `opportunity_ordinal` という独立 field は存在しない。存在しない raw field を
artifact 上で存在するように見せないため、実値と意味づけを分けて保存する。

```text
raw source field   = intent_ordinal
derived delta      = primary_decision_intent_ordinal_delta
semantic meaning   = opportunity ordinal delta
authority          = frozen W2-B1 producer identity
```

artifact には次を宣言する。

```text
semantic_interpretation =
  SCHEDULER_OPPORTUNITY_ORDINAL_DELTA_VIA_W2_B1_INTENT_IDENTITY
```

W2-B1 で `intentOrdinal = formalDecision.opportunityOrdinal` が単一 producer callsite として
固定されているため、この読み替えは frozen authority に基づく。

### exactness の前提

`decision_qpc_delta` は両端で `decision_qpc_exact == true` を必須とする。片方でも exact で
なければ delta を「計算可能な数値」として出さず、W4-B attribution INVALID にする。
同様に primary-neighbor record には `producer_semantics_exact == true` を要求する。
missing 側へ値を補間しないという freeze と同じ思想である。

### event ごとの artifact 構造

```text
run
cohort
missing_ordinals

before_primary:
  intent_ordinal / decision_qpc / render_begin_qpc / target_frame
  repeat / past_source_domain / last_finalized_opportunity_ordinal / token_serial

after_primary:
  (同じ field)

transition:
  primary_decision_intent_ordinal_delta
  decision_qpc_delta
  render_begin_qpc_delta
  target_frame_delta
  last_finalized_opportunity_ordinal_delta

interval_diagnostics:
  duplicate_callback_record_count
```

**missing ordinal 自身には producer semantic field を置かない。**

## run-level time-domain diagnostic (amendment)

event-local transition (isolated vs double) と直交する、**producer decision stream 全体の
exact な時間構造**も W4-B に残す。これは補助統計ではなく producer semantics attribution の
一部である。

### measurement window を producer から作らない

W4-B core は decision stream から measurement window を推定してはならない。W3 で確立済みの
**W2-A physical measurement window** を upstream authority から受け取るだけにする。

```text
producer 由来:
  first / last primary decision QPC

physical authority 由来 (W2-A):
  measurement_start_qpc
  measurement_end_qpc_exclusive
  qpc_frequency
```

この 2 ソースを分けて bind し、次を計算する。

```text
head gap = first_decision_qpc - measurement_start_qpc
tail gap = measurement_end_qpc_exclusive - last_decision_qpc
```

### field

```text
primary_decision_first_qpc
primary_decision_last_qpc
primary_decision_active_span_qpc
primary_decision_active_span_seconds

primary_decision_count
primary_decision_interdecision_cadence_hz

measurement_window_seconds
primary_decision_active_span_fraction

head_without_primary_decision_seconds
tail_without_primary_decision_seconds

first_primary_intent_ordinal
last_primary_intent_ordinal
first_required_intent_ordinal
last_required_intent_ordinal
trailing_missing_required_intent_count
```

### cadence の式を freeze する

decision が `N` 件なら interval は `N-1` 個である。`N / span` では意味がずれる。

```text
cadence_hz = (N - 1) / ((last_decision_qpc - first_decision_qpc) / qpc_frequency)
```

### legacy elapsed との一致は correlation であって authority ではない

`measurement_elapsed_seconds` は W2-E で diagnostic へ降格済みである。今回 producer active
span と一致したことは強い裏付けだが、**canonical 時間 authority へ復帰させない**。

```text
legacy_measurement_elapsed_seconds_diagnostic
producer_active_span_matches_legacy_elapsed
legacy_measurement_elapsed_used_as_authority = false
```

### 2 種類の diagnostic をまだ因果で結ばない

```text
A. ordinal-domain      required / primary ordinal domain、count、missing、tail-edge
B. time-domain         active span、canonical window、fraction、tail gap、cadence
```

W4-B closure で言えるのはここまでである。

```text
primary decisions は約 60Hz で継続的に発行されたが、その decision stream は
canonical 60s window の約半分で終了している。
各 decision 間で intent ordinal はおおむね +2 進んでいる。
```

次は **root-cause statement なので W4-B では言わない**。

```text
ordinal を +2 したから 29 秒で required domain を使い切って停止した
```

### 追加 flag

```text
run_level_time_domain_diagnostic_present     = true
legacy_measurement_elapsed_used_as_authority = false
decision_span_used_as_measurement_window     = false
```

## provenance と handoff boundary

W4-B は **W4-A proof を直接 consume し、その SHA を bind** する。W4-A は missing set を集合差から
exact に確定し、原因判定を意図的にしておらず、ordinal set 自体を W4-B 用に保存している。
ここを正式な handoff boundary とする。

checker の手順:

```text
W4-A checker 再実行
  -> W4-A missing set 取得
  -> sealed producer ledger から W4-B event を独立再構築
  -> artifact 全体比較
```

## closure 条件

```text
1.  source population = exact W4-A required set
2.  missing set = W4-A missing_primary_decision_ordinals と完全一致
3.  isolated / double / edge / other が排他的
4.  pattern bucket sum = missing count
5.  double event count x 2 = double missing intent count
6.  3 run の pattern counts を独立再計算
7.  producer fields は existing exact records のみ
8.  missing 側 field の補間・nearest-QPC・推定は禁止
9.  isolated vs double の transition table を artifact に保存
10. causal / root-cause verdict はまだ出さない
11. run-level primary-decision time-domain summary は exact producer QPC と
    canonical physical window だけから再構築する
12. time-domain summary は diagnostic only である
    root-cause verdict を出さない / legacy elapsed を authority にしない /
    missing 側の timestamp を推定しない
```

## negative

```text
NegativeMissingSetSplice                        W4-A missing setと違う集合を使う
NegativeIsolatedClassifiedAsDouble              cohort分類の取り違え
NegativeDoubleEventCountMutation                event count x 2 = intent count の破れ
NegativeMissingOrdinalInterpolatedProducerField missing側fieldの補間
NegativeNearestDecisionQpcUsed                  nearest QPCでの代用
NegativeCrossRunNeighborSplice                  run境界をまたぐ近傍接続
NegativeRootCauseDeclared                       root_cause_determined=true の注入
NegativeAggregateOnlyTransitionForgery          transition tableのaggregateだけ偽造
NegativeDecisionSpanUsedAsMeasurementWindow    decision spanをmeasurement windowにする
NegativeLegacyElapsedPromotedToAuthority       legacy elapsedを時間authorityへ昇格
NegativeTailGapMutation                        tail gapの改変 (sealed QPCから再計算してreject)
```

## この後に来る可能性のある W4-C

producer ledger だけでは missing 側の原因状態が存在せず、

```text
present側の前後recordが isolated と double で同じ
```

という結果になった場合、それ自体が重要な結論である。その場合は W4-C として
**scheduler invocation / no-decision path instrumentation** が必要になる。
これは producer 変更と新規 capture を伴う可能性があるため、W4-B の結果を見てから設計する。
