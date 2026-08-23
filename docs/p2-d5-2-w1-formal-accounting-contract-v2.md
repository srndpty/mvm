# P2-D5-2-W1 — Formal Accounting Contract v2

`formal_contract_version = "P2-D5-2-v2"`

W0 (`5442dec`) / W0.5 (`57a7ba6`) を固定した上での contract 起草。
**production wiring / threshold / production scheduler はまだ変更しない。**
本書と対になる executable contract は
`scripts/check-p2-d5-2-formal-v2.ps1` と
`tests/gpu_preview/test-p2-d5-2-formal-v2-contract.ps1`。

---

## 1. authority の4層

```text
1A. workload intent authority
    required_intent_count            既存 formal test contract（60s playback -> 3600）
    source                           test contract。実測ではない。
    役割                             drop-rate の分母

1B. physical opportunity authority
    physical VBlank observer         { ordinal, qpc }
    source                           display 側の直接観測。DWM 非依存。
    役割                             実在した physical display opportunity domain

2.  presentation outcome authority
    composition token
      -> 1:1 successful native Present
      -> 1:1 target app PresentEvent
      -> terminal FinalState
      -> Presented は DisplayedQPC を持つ

3.  physical / source identity authority
    DisplayedQPC
      -> exactly one physical VBlank ordinal
      -> exact source / composition identity
```

### 1A と 1B を同一視しない

**physical VBlank count が 3600 と一致することを仮定しない。**
表示モードが厳密に 60.000 Hz でなければ、60 秒間の physical VBlank 数は
3600 と一致しない。`required_intent_count` は test contract 由来の分母であり、
`formal_physical_opportunity_count` は実測の domain である。

両者の差は `formal_intent_overhang_count` / `formal_intent_surplus_count` として
別々に記録する。**`tail_true_drop` という単一の名前に押し込まない。**

### diagnostic only（formal authority ではない）

```text
QQuickWindow::frameSwapped
DwmGetCompositionTimingInfo / cRefresh / qpcVBlank
DWM PresentStart / DWM parent
PresentMode（Independent / Composed）
dependency batch
DXGI GetFrameStatistics oracle
```

`cRefresh` は W0.5 の整理どおり **formal authority から降格**する。
independent flip 下での physical refresh ordinal との同値性が未証明であり、
時系列 artifact も残っていないため continuity を検証できない。
diagnostic cross-check としては引き続き有用。

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
| `formal_authority_profile` | `"P2-D5-2-v2"`。canonical authority 集合の識別子 |
| `formal_runtime_authority_override` | この run が profile から逸脱したか。**PASS には `false` 必須** |
| `formal_authority_valid` | fail-close 条件をすべて満たしたか |
| `formal_authority_error` | 最初の fail-close reason（`NONE` if valid） |

`formal_counter_authority_changed` は **contract-version-relative override
semantics** を採る（W0.5 案1）。すなわち「v1 から変わったか」ではなく
「宣言された `formal_contract_version` の canonical authority から
このrunが逸脱したか」を意味する。誤解を避けるため v2 では
`formal_runtime_authority_override` を canonical とし、旧 field は
**compatibility alias / deprecated** とする。

### 3.2 Layer 1A

| field | 意味 |
|---|---|
| `formal_required_intent_count` | test contract の分母（60s -> 3600） |

### 3.3 Layer 1B

| field | 意味 |
|---|---|
| `formal_physical_vblank_origin_ordinal` | domain 起点の physical ordinal |
| `formal_physical_vblank_origin_qpc` | 同 QPC |
| `formal_physical_opportunity_count` | `[measurement_start_qpc, measurement_end_qpc)` に属する physical opportunity 数 |
| `formal_physical_vblank_boundary_bracketed` | 両端の外側に predecessor / successor sample が存在する |
| `formal_physical_output_stable` | adapter / output / HMONITOR / refresh rational が不変 |

### 3.4 Layer 2

| field | 意味 |
|---|---|
| `formal_successful_native_present_count` | hook が記録した成功 Present 数 |
| `formal_present_event_count` | 対応した target PresentEvent 数 |
| `formal_displayed_count` | `FinalState=Presented` かつ `DisplayedQPC` あり |
| `formal_discarded_count` | `FinalState` が terminal な非 Presented |
| `formal_present_outcome_unknown_count` | terminal でない / 不明 |

### 3.5 Layer 3

| field | 意味 |
|---|---|
| `formal_displayed_unique_physical_count` | 埋まった physical ordinal のうち source frame が前と異なるもの |
| `formal_repeated_physical_count` | 埋まった physical ordinal のうち source frame が前と同一のもの |

### 3.6 accounting

| field | 定義 |
|---|---|
| `formal_physical_unfilled_count` | `physical_opportunity_count - displayed_count` |
| `formal_tail_physical_unfilled_count` | physical domain 末尾の連続 unfilled 数 |
| `formal_intent_overhang_count` | `max(required_intent_count - physical_opportunity_count, 0)` |
| `formal_intent_surplus_count` | `max(physical_opportunity_count - required_intent_count, 0)` |
| `formal_true_drop_count` | `max(required_intent_count - displayed_unique_physical_count, 0)` |
| `formal_presented_frame_mismatch_count` | W0.5 で保持と決めた invariant の違反数。**0 必須** |

### 3.7 deprecated（v2 で authority を持たない）

```text
formal_swapped_composition_count        frameSwapped 回数
formal_superseded_candidate_count       同一 opportunity 内の複数 frameSwapped
formal_opportunity_origin_refresh_count cRefresh 由来
tail_true_drop                          1A/1B 未分離の旧定義
formal_counter_authority_changed        alias（意味は 3.1 のとおり）
```

旧 field へ **新 authority を黙って再割当てしない**。残す場合は
`deprecated` / `not authoritative in contract v2` と明示する。

`formal_refresh_numerator` / `_denominator` は display metadata として残すが、
**ordinal authority ではない**。

---

## 4. accounting identity（checker が強制する）

```text
I1  displayed_count + physical_unfilled_count == physical_opportunity_count
I2  displayed_count == displayed_unique_physical_count + repeated_physical_count
I3  present_event_count == displayed_count + discarded_count + unknown_count
I4  successful_native_present_count == present_event_count
I5  0 <= displayed_unique_physical_count <= min(required_intent_count,
                                                physical_opportunity_count)
I6  0 <= tail_physical_unfilled_count <= physical_unfilled_count
I7  intent_overhang_count == max(required_intent_count - physical_opportunity_count, 0)
I8  intent_surplus_count  == max(physical_opportunity_count - required_intent_count, 0)
I9  true_drop_count == max(required_intent_count - displayed_unique_physical_count, 0)
I10 intent_overhang_count == 0 または intent_surplus_count == 0
```

**`true_drop == physical_unfilled + intent_overhang` という恒等式は要求しない。**
`physical > required` の場合に成立しないためである（I7/I8/I9 で個別に閉じる）。

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
```

W0.5-A で `render callback ↔ QQuickWindow::frameSwapped` 1:1 が
旧 Layer-2 専用 invariant だと証明済み。v2 では **意図的に retire** する。
checker はこれらの出現自体を契約違反として拒否する。

`RENDER_ORDINAL_MISMATCH` は `markRenderComplete` 側（render callback 内部の
整合）だけが残り、`commitSwap` 側は retire される。

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
ordinal が strictly consecutive

measurement_start_qpc より前に predecessor sample が存在
measurement_end_qpc  より後に successor sample が存在

すべての DisplayedQPC が exactly one physical ordinal へ一意に map できる
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
    physical VBlank ordinal domain を固定      <- token ではなく domain を freeze
    ↓
その domain に関係する native Presents / PresentEvents の
terminal outcome を closure window で待つ
    ↓
DisplayedQPC を physical ordinal へ map
    ↓
physical ledger を finalize
    ↓
gap / tail / total accounting
```

**freeze する対象は token ではなく opportunity domain。**
token が生成されなかった最後の opportunity こそ drop なので、
token を基準に freeze すると domain から消えてしまう。

domain inclusion は **record 到着時刻ではなく DisplayedQPC / physical ordinal**
で決める。

```text
PresentEvent が measurement end 後に到着し
DisplayedQPC が domain 内            -> count する

Present が measurement end 前に開始し
DisplayedQPC が domain 外            -> in-domain display として count しない

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

W1 レビュー後に W2（wiring）。W2 では
`PresentationOpportunityScheduler` を **schedule / ordinal authority として残し**、
display / drop 判定部分を本 contract の Layer 2/3 へ差し替える。
W3 で固定 clean SHA の fresh formal acquisition。
