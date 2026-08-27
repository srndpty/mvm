# P2-D5-2-W1 / W1.1 — Formal Accounting Contract v2

`formal_contract_version = "P2-D5-2-v2"`

W0 (`5442dec`) / W0.5 (`57a7ba6`) を固定した上での contract。
**production wiring / threshold / production scheduler はまだ変更しない。**

executable contract:
- `scripts/check-p2-d5-2-formal-v2.ps1`
- `tests/gpu_preview/test-p2-d5-2-formal-v2-contract.ps1`
- `tests/gpu_preview/test-p2-d5-2-formal-architecture-contract.ps1`

---

## 0. 改訂 (W1.1 / W1.2)

### W1.2 — intent satisfaction identity closure

`formal_satisfied_intent_count` の producer contract を固定した (3.6)。
exact identity chain、source uniqueness との非同一視、
authority invalid と performance drop の区別、N1 identity、
および **ABI v4 bump の必要性**を確定した。

`"filled" means occupied by an exact target PresentedEvent` である。
`formal_filled_physical_opportunity_count` は「その VBlank に何らかの画像が
あった」ではなく「その physical ordinal に新しい target PresentedEvent が
対応した」を意味する。

### W1.1

W1 レビューで P1 が2件あり、次を修正した。

```text
1. formal_displayed_count が
       Layer 2 の Presented event 数
       Layer 3 の埋まった physical ordinal 数
   という別集合を融合していた
   -> 廃止し cohort を分離した (3.4 / 3.5)

2. displayed_unique_physical_count <= required_intent_count は
   現定義から導けない
   -> 削除し formal_satisfied_intent_count を導入した (3.6 / S1 / S2)
```

あわせて `OPPORTUNITY_REGRESSION` を retired list へ追加し、
discard reason 内訳は diagnostic のままとした。

---

## 1. authority の4層

```text
1A. workload intent authority
    required_intent_count            既存 formal test contract（60s playback -> 3600）
    source                           test contract。実測ではない。
    役割                             drop-rate の分母

1B. physical opportunity authority
    physical VBlank observer         { physical_vblank_ordinal, qpc }
    source                           display 側の直接観測。DWM 非依存。
    役割                             実在した physical display opportunity domain

2.  presentation outcome authority
    composition token
      -> 1:1 successful native Present (native_present_serial)
      -> 1:1 target app PresentEvent
      -> terminal FinalState
      -> Presented は DisplayedQPC を持つ

3.  physical / source identity authority
    DisplayedQPC
      -> exactly one physical_vblank_ordinal
      -> exact source / composition identity
```

### 1A と 1B を同一視しない

**physical VBlank count が 3600 と一致することを仮定しない。**
表示モードが厳密に 60.000 Hz でなければ、60 秒間の physical VBlank 数は
3600 と一致しない。`required_intent_count` は test contract 由来の分母であり、
`formal_physical_opportunity_count` は実測の domain である。

差は `formal_intent_overhang_count` / `formal_intent_surplus_count` として
別々に記録する。**`tail_true_drop` という単一の名前に押し込まない。**

### 命名は domain を必ず付ける

```text
intent_ordinal            Layer 1A
physical_vblank_ordinal   Layer 1B / Layer 3
native_present_serial     Layer 2
```

`ordinal` を無修飾で使わない。W2 で scheduler ordinal と physical ordinal が
再び混線するのを防ぐ。

### diagnostic only（formal authority ではない）

```text
QQuickWindow::frameSwapped
DwmGetCompositionTimingInfo / cRefresh / qpcVBlank
DWM PresentStart / DWM parent
PresentMode（Independent / Composed）
dependency batch
DXGI GetFrameStatistics oracle
discard reason 内訳（DEPENDENT_PRESENT_SUPERSEDED 等）
```

`cRefresh` は W0.5 の整理どおり **formal authority から降格**する。
independent flip 下での physical refresh ordinal との同値性が未証明であり、
時系列 artifact も残っていないため continuity を検証できない。

---

## 2. Independent / Composed で式を分岐させない

```text
DWM parent == null は display failure ではない
PresentMode は accounting formula に影響しない
```

T2-D0 で 27 run すべてが `Hardware_Composed_Independent_Flip` かつ
`DWM parent = 0` で 900/900 displayed だった。これは設計上の制約とする。

---

## 3. v2 canonical fields

### 3.1 メタ / authority profile

| field | 意味 |
|---|---|
| `formal_contract_version` | `"P2-D5-2-v2"` |
| `formal_authority_profile` | canonical authority 集合の識別子 |
| `formal_runtime_authority_override` | profile から逸脱したか。**PASS には `false` 必須** |
| `formal_authority_valid` | fail-close 条件をすべて満たしたか |
| `formal_authority_error` | 最初の fail-close reason（`NONE` if valid） |

`formal_counter_authority_changed` は **contract-version-relative override
semantics**（W0.5 案1）。「v1 から変わったか」ではなく「宣言された
`formal_contract_version` の canonical authority からこの run が逸脱したか」。
誤解を避けるため v2 では `formal_runtime_authority_override` を canonical とし、
旧 field は **compatibility alias / deprecated** とする。

### 3.2 Layer 1A

| field | 意味 |
|---|---|
| `formal_required_intent_count` | test contract の分母（60s -> 3600） |

### 3.3 Layer 1B

| field | 意味 |
|---|---|
| `formal_physical_vblank_origin_ordinal` | domain 起点の physical_vblank_ordinal |
| `formal_physical_vblank_origin_qpc` | 同 QPC |
| `formal_physical_opportunity_count` | `[measurement_start_qpc, measurement_end_qpc)` に属する physical opportunity 数 |
| `formal_physical_vblank_boundary_bracketed` | 両端の外側に predecessor / successor sample が存在する |
| `formal_physical_output_stable` | adapter / output / HMONITOR / refresh rational が不変 |

### 3.4 Layer 2 — PresentEvent outcome cohort

**submission cohort であり、measurement physical domain とは別集合。**

| field | 意味 |
|---|---|
| `formal_successful_native_present_count` | hook が記録した成功 Present 数 |
| `formal_present_event_count` | 対応した target PresentEvent 数 |
| `formal_presented_event_count` | `FinalState=Presented` かつ `DisplayedQPC` あり |
| `formal_discarded_event_count` | `FinalState` が terminal な非 Presented |
| `formal_present_outcome_unknown_count` | terminal でない / 不明 |

### 3.5 Layer 3 — measurement physical domain occupancy

`formal_displayed_count` は2つの意味を持っていたため **廃止**し分離した。

| field | 意味 |
|---|---|
| `formal_in_domain_presented_event_count` | DisplayedQPC が domain 内に落ちる Presented event 数 |
| `formal_filled_physical_opportunity_count` | 埋まった physical_vblank_ordinal 数 |
| `formal_displayed_unique_physical_count` | 埋まった ordinal のうち source frame が前と異なるもの |
| `formal_repeated_physical_count` | 埋まった ordinal のうち source frame が前と同一のもの |
| `formal_physical_unfilled_count` | `physical_opportunity_count - filled_physical_opportunity_count` |
| `formal_tail_physical_unfilled_count` | physical domain 末尾の連続 unfilled 数 |
| `formal_physical_ordinal_multi_presented_count` | 同一 ordinal へ複数 Presented event が map された数。**0 必須** |

**Layer 2 と Layer 3 の大小関係は要求しない。** 双方が起こりうる。

```text
measurement end 前に Present / DisplayedQPC が domain 外
    -> Layer 2 cohort に入るが Layer 3 domain に入らない

measurement start 前に submit / DisplayedQPC が domain 内
    -> Layer 3 domain に入るが Layer 2 cohort に入らない
```

### 3.6 Layer 1A ↔ Layer 3 の bridge — intent satisfaction identity (W1.2)

`formal_satisfied_intent_count` を算術上の自由変数にしない。**producer contract**
として次の exact identity chain が閉じることを要求する。

```text
intent_ordinal
  -> scheduler decision / expected targetFrame
  -> actual rendered source            (PresentedFrameMismatch で保証)
  -> composition token
  -> successful native Present         (native_present_serial)
  -> exact PresentEvent
  -> DisplayedQPC
  -> in-domain physical_vblank_ordinal
```

```text
formal_satisfied_intent_count
    = measurement intent domain 内で上記 chain が閉じ、かつ in-domain physical
      opportunity に表示された distinct intent_ordinal の個数
```

**source frame uniqueness と intent satisfaction を同一視しない。**
30fps source を 60Hz で表示する場合、

```text
intent 100 -> source frame 50
intent 101 -> source frame 50
```

の2つの distinct intent が同一 source frame を正しく表示しうる。このとき

```text
displayed_unique_physical_count = 1
satisfied_intent_count          = 2
```

でも矛盾しない（`GoodSourceHalfRate` で固定）。

#### authority invalid と performance drop の区別

```text
scheduled intent に in-domain Presented outcome が無い
    -> performance drop（authority invalid ではない）

観測された display の intent identity が欠損 / 曖昧
    -> authority invalid
```

#### 追加 field

| field | 意味 |
|---|---|
| `formal_intent_identity_missing_count` | display に intent identity が無い。**0 必須** |
| `formal_intent_identity_ambiguous_count` | intent identity が曖昧。**0 必須** |
| `formal_intent_ordinal_out_of_domain_count` | intent_ordinal が domain 外。**0 必須** |
| `formal_intent_duplicate_display_count` | 同一 intent が複数回 in-domain 表示。**0 必須** |
| `formal_in_domain_presented_foreign_intent_count` | 前 measurement 由来 intent の in-domain Presented event |
| `formal_unsatisfied_intent_count` | `required - satisfied`。performance drop |

#### N1 identity

```text
N1  satisfied_intent_count + in_domain_presented_foreign_intent_count
      == in_domain_presented_event_count
```

これにより Layer 3 occupancy と intent satisfaction が閉じる。
3集合（Layer 2 cohort / Layer 3 occupancy / Layer 1A satisfaction）は
互いに異なってよい。

```text
measurement 前 submit / DisplayedQPC domain 内 / intent は前 measurement 由来
    -> physical opportunity は埋めるが satisfied には寄与しない

intent は今回 domain 内 / Present は end 前 / DisplayedQPC は domain 外
    -> Layer 2 では Presented だが satisfied ではない
```

#### ABI v4 が必要（W2 前提）

現行 ABI v3 の `MvmNativePresentCompositionToken` は
`tokenSerial / compositionEpoch / compositionState / outputFrameNumber /
sources[] / propagationSerial` のみで、**intent identity を持たない**。

```text
token.outputFrameNumber = frame.outputFrameNumber
formal 有効時は output = formalDecision.targetFrame   ← source frame 番号
```

`targetFrame` は source frame 番号なので、30fps source では 2つの distinct
intent が同一値を持つ。したがって **`outputFrameNumber` は intent を同定できない**。

**source frame 番号や QPC から intent を推測してはならない。**
W2 で `intentOrdinal`（および必要なら `schedulerDecisionId`）を token へ
明示追加し、**native present hook ABI を v4 へ bump する**。
v2/v3 の mismatch を hard reject する既存設計をそのまま使う。

| field | 定義 |
|---|---|
| `formal_intent_overhang_count` | `max(required_intent_count - physical_opportunity_count, 0)` |
| `formal_intent_surplus_count` | `max(physical_opportunity_count - required_intent_count, 0)` |
| `formal_true_drop_count` | `required_intent_count - satisfied_intent_count` |
| `formal_presented_frame_mismatch_count` | W0.5 で保持と決めた invariant の違反数。**0 必須** |

`displayed_unique_physical_count` は physical 側の observational counter であり
**`required_intent_count` では縛らない**（`physical > required` なら
`unique > required` になりうる）。intent を満たしたかは
`satisfied_intent_count` が担う。

`satisfied_intent_count == displayed_unique_physical_count` を要求したいなら、
source / intent identity contract として**別途証明する**。本 contract では
要求しない。

### 3.7 deprecated（v2 で authority を持たない）

```text
formal_displayed_count                  Layer2/Layer3 融合。廃止
formal_swapped_composition_count        frameSwapped 回数
formal_superseded_candidate_count       同一 opportunity 内の複数 frameSwapped
formal_opportunity_origin_refresh_count cRefresh 由来
tail_true_drop                          1A/1B 未分離の旧定義
formal_counter_authority_changed        alias（意味は 3.1 のとおり）
```

旧 field へ **新 authority を黙って再割当てしない**。
`formal_refresh_numerator` / `_denominator` は display metadata として残すが、
**ordinal authority ではない**。

---

## 4. accounting identity（checker が強制する）

```text
E1  present_event_count
      == presented_event_count + discarded_event_count + unknown_count
E2  successful_native_present_count == present_event_count

P1  filled_physical_opportunity_count + physical_unfilled_count
      == physical_opportunity_count
P2  filled_physical_opportunity_count
      == displayed_unique_physical_count + repeated_physical_count
P3  in_domain_presented_event_count == filled_physical_opportunity_count
      (1 ordinal あたり Presented event は 0 or 1。
       2件以上は PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS)
P4  0 <= tail_physical_unfilled_count <= physical_unfilled_count

U1  0 <= displayed_unique_physical_count <= physical_opportunity_count

S1  0 <= satisfied_intent_count <= required_intent_count
N1  satisfied_intent_count + in_domain_presented_foreign_intent_count
      == in_domain_presented_event_count
S2  true_drop_count == required_intent_count - satisfied_intent_count

X1  intent_overhang_count == max(required_intent_count - physical_opportunity_count, 0)
X2  intent_surplus_count  == max(physical_opportunity_count - required_intent_count, 0)
X3  intent_overhang_count == 0 または intent_surplus_count == 0
```

**要求しない恒等式。**

```text
true_drop == physical_unfilled + intent_overhang
    physical > required で成立しない

presented_event_count と in_domain_presented_event_count の大小関係
    Layer 2 cohort と Layer 3 domain は別集合であり
    どちら向きの差も boundary で正当に発生する
```

---

## 5. fail-close reasons

すべて `formal_authority_valid = false` にする。performance FAIL ではない。

```text
COMPOSITION_TOKEN_PRESENT_MISSING
COMPOSITION_TOKEN_PRESENT_AMBIGUOUS
PRESENT_EVENT_MISSING
PRESENT_EVENT_AMBIGUOUS
PRESENT_OUTCOME_UNKNOWN
DISPLAYED_QPC_MISSING
PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS
PHYSICAL_VBLANK_SEQUENCE_BREAK
PHYSICAL_VBLANK_OBSERVER_INVALID
PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED
OUTPUT_OR_MODE_CHANGED
PRESENTED_FRAME_MISMATCH
INTENT_IDENTITY_MISSING
INTENT_IDENTITY_AMBIGUOUS
INTENT_ORDINAL_OUT_OF_DOMAIN
INTENT_DUPLICATE_DISPLAY
ETW_LOSS
RING_OVERFLOW
ACCOUNTING_IDENTITY_VIOLATION
RUNTIME_AUTHORITY_OVERRIDE
```

### 5.1 retired（v2 に出現してはならない）

```text
RENDER_SWAP_MISMATCH
RENDER_WITHOUT_SWAP
SWAP_WITHOUT_RENDER
RENDER_NOT_COMPLETED
SWAP_ORDINAL_MISMATCH
OPPORTUNITY_REGRESSION      (swapQpc 由来。W0.5-A で legacy と分類済み)
```

W0.5-A で `render callback ↔ QQuickWindow::frameSwapped` 1:1 が
旧 Layer-2 専用 invariant だと証明済み。v2 では **意図的に retire** する。
checker はこれらの出現自体を契約違反として拒否する。

`RENDER_ORDINAL_MISMATCH` は `markRenderComplete` 側（render callback 内部の
整合）だけが残り、`commitSwap` 側は retire される。reason string だけでは
provenance を区別できないため、**architecture test で
`frameSwapped -> formal commit` が v2 canonical path に存在しないことを固定する**。

### 5.2 authority invalid は performance を評価しない

```text
AUTHORITY_INVALID
    performance_evaluated = false
    performance_pass      = null
    drop_rate             = null
```

`performance_pass = false` にしない。threshold 超過の正式な performance FAIL と
区別できなくなる。状態は三値相当。

```text
AUTHORITY_INVALID / PERFORMANCE_PASS / PERFORMANCE_FAIL
```

---

## 6. Layer 1B の fail-closed contract

```text
sequence_status == OK
ring_overflow_count == 0
wait_failure_count == 0
long_interval_count == 0
short_interval_count == 0
cumulative_consistent == true
window_output_stable == true

adapter LUID / output / HMONITOR 不変
refresh rational 不変
physical_vblank_ordinal が strictly consecutive

measurement_start_qpc より前に predecessor sample が存在
measurement_end_qpc  より後に successor sample が存在

すべての DisplayedQPC が exactly one physical_vblank_ordinal へ一意に map できる
```

**両端の bracket は必須。** これがないと tail の exact accounting が閉じない。

`AuthorityDiscontinuity` は cRefresh ではなく Layer 1B に結び直す。

```text
discontinuity とする
    VBlank sequence break
    observer timing invalid（long/short interval, overflow, wait failure）
    output / display mode change
    boundary bracket missing

discontinuity としない
    PresentMode transition（Independent <-> Composed）
```

---

## 7. measurement boundary と closure

```text
measurement end
    ↓
1A: required intent domain を固定
1B: [measurement_start_qpc, measurement_end_qpc) の
    physical_vblank_ordinal domain を固定    <- token ではなく domain を freeze
    ↓
その domain に関係する native Presents / PresentEvents の
terminal outcome を closure window で待つ
    ↓
DisplayedQPC を physical_vblank_ordinal へ map
    ↓
physical ledger を finalize
    ↓
gap / tail / total accounting
```

**freeze する対象は token ではなく opportunity domain。**
token が生成されなかった最後の opportunity こそ drop なので、
token を基準に freeze すると domain から消えてしまう。

domain inclusion は **record 到着時刻ではなく DisplayedQPC /
physical_vblank_ordinal** で決める。

```text
PresentEvent が measurement end 後に到着し
DisplayedQPC が domain 内            -> Layer 3 に count する

Present が measurement end 前に開始し
DisplayedQPC が domain 外            -> Layer 3 に count しない
                                        (Layer 2 cohort には残る)

FinalState = Discarded               -> physical opportunity を fill しない
FinalState = Unknown / closure timeout
                                     -> performance drop ではなく
                                        PRESENT_OUTCOME_UNKNOWN で authority invalid
```

---

## 8. threshold（変更しない）

```text
effective_fps >= 55
drop_rate     <= 2%

drop_rate = formal_true_drop_count / formal_required_intent_count
```

threshold は W3 の fresh formal acquisition で評価する。
**閾値も historical failure の再解釈も行わない。**

---

## 9. p2_present_id_oracle_live

legacy oracle / D5-2 non-blocker のまま変更しない。
`ORACLE_SAMPLING_GAP` は緩めない・skip しない・削除しない・PASS 扱いしない。
`ordinary suite = FAIL 654/655` の事実も保存する。

---

## 10. 次

W1.1 レビュー後に W2（wiring）。W2 では
`PresentationOpportunityScheduler` を
**workload intent / source scheduling authority として残す**。
physical ordinal authority は physical VBlank observer であり scheduler ではない。

W2 の到達点は
`tests/gpu_preview/test-p2-d5-2-formal-architecture-contract.ps1` の
`-Phase PostW2` で固定する。現在は `-Phase PreW2` のみ登録しており、
W2 完了時に PostW2 へ切り替える。PostW2 は次を要求する。

```text
recordFrameSwapped が formal commitSwap を呼ばない
formalOpportunityIgnoreNextSwap が存在しない
```

W3 で固定 clean SHA の fresh formal acquisition。
