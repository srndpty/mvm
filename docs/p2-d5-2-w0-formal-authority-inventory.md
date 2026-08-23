# P2-D5-2-W0 — Formal Display Authority Inventory

コード変更なし・新規取得なしの棚卸し。commit `967a838` 時点。

前段の archaeology は次で停止済みであり、これ以上の window intervention 探索は行わない。

```text
EXACT_HISTORICAL_RUNTIME_UNAVAILABLE
REBUILD_PROBE_NOT_EVALUABLE
historical BAD           preserved
current reproducibility  NOT ESTABLISHED
historical cause         NOT ESTABLISHED
```

## 0. 結論（先に）

```text
現在の formal display outcome authority =
    QQuickWindow::frameSwapped  +  DwmGetCompositionTimingInfo(cRefresh)

新 authority chain との接続 = ゼロ
```

`src/` と `apps/` の C++ 全体で `FinalState` / `DisplayedQPC` / `displayed_qpc` の
参照は **0 件**。つまり formal counter は PresentMon/ETW の display outcome を
一度も見ていない。

一方で危険語のうち DWM parent 系は C++ に存在しない。

| 危険語 | src/ + apps/ 出現数 | 所在 |
|---|---|---|
| `DwmParent` / `attached_dwm_parent` / `dwm_parent` | **0** | PowerShell checker のみ（diagnostic） |
| `frameSwapped` | 3 | うち実結線は `apps/compositor_spike/main.cpp:141` の1つ |
| `synthetic_deadline` | 1 | `diagnostic_synthetic_deadline_drop_count`（診断名のみ） |
| `nearest_slot` / `physical_overlap` / `vblank_shadow` | 0 | — |

したがって W2 で触るべき対象は **DWM parent の除去ではなく、
`frameSwapped` を display outcome authority から降格させること**である。

## 1. authority を3層に分ける

### 層1: intent / denominator authority（現状維持でよい）

```text
producer   PresentationOpportunityScheduler
source     DwmGetCompositionTimingInfo -> cRefresh (+ qpcVBlank)
役割       opportunity ordinal と分母（60s × 60Hz = 3600 相当）
```

`presentation_refresh_authority.h` の設計コメントどおり、ordinal は
`sample.refreshCount - originRefreshCount` そのもので、QPC 差分は continuity の
cross-check に留めている。丸めや +1 は入っていない。

**D0 はこの層を否定していない。** cRefresh は display の refresh clock であって
DWM の Present/parent 有無ではない。independent flip 中も clock は進む。

ただし W1 で明示的に決めるべき残課題がある。

- independent flip 中の `DwmGetCompositionTimingInfo` 可用性・単調性を
  fail-closed に検証しているか（現状は `available` と monotonic のみ）。
- `AuthorityDiscontinuity` が independent/composed 遷移時に誤発火しないか。

### 層2: presentation outcome authority（**要差し替え**）

```text
現状 producer   QQuickWindow::frameSwapped -> commitSwap()
現状 source     Qt render loop の swap 完了通知
問題            swap 完了は「表示された」ことの証拠ではない
```

`commitSwap(swapQpc, postSwapAuthority, swapOrdinal)` が finalize を駆動し、
そこから次がすべて派生している。

| formal field | producer | 現 authority | formal/diag | PASS必須 | independent-flip互換 | DWM parent要 | frameSwapped要 | 物理VBlank observer要 | D5-2後の予定 authority |
|---|---|---|---|---|---|---|---|---|---|
| `formal_displayed_unique_count` | scheduler | frameSwapped + cRefresh | formal | ○ | 未検証 | × | **○** | × | PresentEvent FinalState/DisplayedQPC |
| `formal_repeated_opportunity_count` | scheduler | 同上 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_gap_true_drop_count` | scheduler | 同上 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_tail_true_drop`（`tail_true_drop`） | scheduler | 同上 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_true_opportunity_drop_count` | scheduler | 上2つの和 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_forward_reconciliation_count` | scheduler | 同上 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_lost_opportunity_count` | scheduler | 同上 | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_superseded_candidate_count` | scheduler | 同一 opportunity 内の複数 swap | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_swapped_composition_count` | scheduler | swap 回数 | formal | ○ | 未検証 | × | **○** | × | native Present 成功数へ |
| `formal_finalized_opportunity_count` | scheduler | ledger records | formal | ○ | 未検証 | × | **○** | × | 同上 |
| `formal_opportunity_ledger` | scheduler | 上記の記録 | formal | ○ | 未検証 | × | **○** | × | 同上 + DisplayedQPC 列を追加 |
| `formal_opportunity_anchored` / `_origin_refresh_count` | scheduler | cRefresh | formal(層1) | ○ | 要検証 | × | × | × | **維持** |
| `formal_refresh_numerator/denominator` | state | display mode | formal(層1) | ○ | ○ | × | × | × | **維持** |
| `formal_qpc_frequency` | QPC | — | formal(層1) | ○ | ○ | × | × | × | **維持** |
| `formal_opportunity_authority_valid` / `_error` | scheduler | 内部整合 | formal | ○ | 要検証 | × | ○ | × | 維持（判定条件は更新） |
| `formal_first_reconciliation_event` | scheduler | 初回不一致 | diag | × | 未検証 | × | ○ | × | 維持（diagnostic） |
| `formal_counter_authority_changed` | 固定 `false` | — | guard | ○ | — | — | — | — | **維持** |
| `formal_contract_version` | 固定 `"P2-D5-2"` | — | meta | — | — | — | — | — | bump 対象 |
| `diagnostic_synthetic_deadline_drop_count` | scheduler周辺 | 合成 deadline | **diag** | × | ○ | × | × | × | diagnostic 明示 |

`presentation_opportunity_attribution.h:32` のコメントどおり、render callback と
frameSwapped hook は別 array へ書く single-writer 構成になっている。
差し替え時もこの分離は保つ。

### 層3: physical / source identity authority（**未実装**）

```text
DisplayedQPC -> physical display opportunity / ordinal -> source identity
```

現状 C++ 側に該当コードなし。`presentation_opportunity_mapper.{h,cpp}` が
近い役割だが、formal counter へは結線されていない（要確認項目）。

## 2. DWM parent の位置づけ

C++ formal path には存在しないので**降格作業は不要**。現状すでに
diagnostic 専用である。

```text
diagnostic として可      composed-path attribution / DWM cadence / dependency batch
formal authority として不可  displayed? / drop? / physical opportunity existed?
```

これは T2-D0 以降、実験結果ではなく設計上の制約とする。
PowerShell checker（`check-p2-c3-a3-t1-condition.ps1` 等）は診断用であり
formal PASS/FAIL を決めていないことを W1 で明文化する。

## 3. `p2_present_id_oracle_live` の判定 → **分岐 B（legacy oracle）**

根拠。

```text
probe      apps/p2_present_identity_probe/main.cpp
source     DXGI GetFrameStatistics / GetLastPresentCount のポーリング
検査内容   submission ごとの PresentCount 遷移が連続で観測できること
失敗       ORACLE_SAMPLING_GAP = ポーリングが遷移を取りこぼした
```

- `GetFrameStatistics` / `GetLastPresentCount` の利用は
  **`apps/p2_present_identity_probe/main.cpp` のみ**。compositor spike にも
  canonical acquisition path にも存在しない。
- canonical/formal checker（`check-p2-c0-native-etw.ps1`,
  `check-p2-c3-a3-t1-condition.ps1`, `check-p2-c3-a3-t2-update-chain.ps1`）は
  `present_id` を一切参照していない。

したがって新 formal chain
`composition token -> native Present -> PresentEvent -> FinalState/DisplayedQPC -> physical identity`
の依存ではない。**D5-2 の blocker ではない。**

ただし次を守る。

- **緩めない・PASS 扱いしない・skip しない・削除しない。**
- `ordinary suite = FAIL 654/655` という事実はそのまま保存する。
- suite migration を行うなら、先に新 canonical authority test が
  この oracle 以上の保証を持つことを示してから、別作業として行う。

`ORACLE_SAMPLING_GAP` はサンプリング観測に内在する取りこぼしであり、
「gap を許容する」修正は authority を弱めるので行わない。

## 4. W1 で固定する contract（案）

```text
scheduled opportunity authority
    = PresentationOpportunityScheduler + DwmGetCompositionTimingInfo(cRefresh)

presentation outcome authority
    = composition token
      -> native Present identity (hook, ABI v3)
      -> target app PresentEvent
      -> FinalState / DisplayedQPC
      -> physical display / source identity

diagnostic only
    = DWM PresentStart / DWM parent / PresentMode / dependency batch
      / frameSwapped / DXGI GetFrameStatistics
```

fail-closed 条件（列挙案）。

```text
composition token join missing
native Present identity missing / serial 不連続
PresentEvent ambiguous / missing
FinalState Unknown
ETW Lost / buffers lost / overflow
DisplayedQPC 必須なのに欠損
physical identity ambiguous
ring overflow
measurement-boundary accounting 不整合
```

**Independent Flip と Composed Flip で accounting formula を変えない。**
`DWM parent == null` は display failure ではない（D0 で 900/900 displayed を実証済み）。

## 5. formal path は現在 dormant であり、blocker は層2にある

### 5.1 T2/D 系 acquisition では formal scheduler が一度も動いていない

```text
formalOpportunitySchedulerEnabled =
    config_.formalPreflight
 && config_.mode == Playback
 && config_.diagnosticCase == None
```

`invoke-p2-c0-native-run.ps1` は `--formal-preflight` を渡していない。実測。

```text
T2-D1-B3a probe-01-clean_static / traced-app.json
    formal_preflight                    False
    formal_opportunity_authority_valid  False
    formal_displayed_unique_count       0
    formal_finalized_opportunity_count  0
```

含意は2つ。

- T2-D0/D1 の presentation path evidence は PresentMon/ETW のみに依存しており、
  formal scheduler に汚染されていない。**過去の verdict は安全。**
- 逆に formal counter は現行 runtime で一度も検証されていない。
  W3 の fresh formal acquisition は未踏領域である。

### 5.2 最後の formal acquisition は層2で失敗していた

`bench/results/p2-d5-2-formal-3b6818a/playback-run1.json`

```text
formal_preflight                    True
formal_contract_version             P2-D5-2
formal_opportunity_error            RENDER_SWAP_MISMATCH
formal_opportunity_authority_valid  False
formal_displayed_unique_count       103
measurement_elapsed_seconds         0
shutdown_reason                     P2-D5-2 render↔swap authority失敗: RENDER_SWAP_MISMATCH
```

**P2-D5-2 が BLOCKED である直接の理由は render↔swap pairing の失敗であり、
それはまさに `frameSwapped` 駆動の層2で起きている。**

`selectForRender` / `markRenderComplete` / `commitSwap` の三点を
render callback と `frameSwapped` で 1:1 に対応させる前提が成立していない。
`RenderWithoutSwap` / `SwapWithoutRender` / `RenderOrdinalMismatch` /
`SwapOrdinalMismatch` はすべてこの前提の破れを表す error である。

つまり **formal blocker と authority 問題は同じ問題**である。
層2を PresentEvent / FinalState / DisplayedQPC に差し替えることは、
authority を正すと同時に blocker を解く作業になる。

### 5.3 解決済みの確認項目

| 項目 | 結果 |
|---|---|
| `presentation_opportunity_mapper` は formal path に結線されているか | **未結線**（参照 0 件） |
| formal scheduler の有効化条件 | `--formal-preflight` + Playback + diagnosticCase None |
| T2 系で有効だったか | **いいえ**（`formal_preflight=False`） |

### 5.4 W1 着手前に残る確認項目

1. independent flip 中の `DwmGetCompositionTimingInfo` の可用性・単調性を
   fail-closed に検証しているか（層1の健全性）。
2. `AuthorityDiscontinuity` が independent/composed 遷移で誤発火しないか。
3. `tail_true_drop` / measurement boundary の accounting が
   新 outcome authority でも同じ式で書けるか。
4. `RENDER_SWAP_MISMATCH` の再現条件（層2差し替えで自然に消えるのか、
   別の pairing 欠陥が残るのか）。

## 6. 次

W0 のレビュー後に W1（accounting contract を文章 + checker test で固定）、
W2（wiring）、W3（固定 clean SHA で fresh formal acquisition、threshold は
`fps >= 55` / `drop_rate <= 2%` のまま変更しない）へ進む。
