# P2-D5-2-W0.5 — Legacy Pairing + Layer-1 Authority Closure

新規 capture なし。commit `5442dec` 時点の静的追跡。
formal semantics / threshold / production scheduler は変更していない。

W0 の結論（`5442dec`）と historical evidence は不変とする。

---

## A. `RENDER_SWAP_MISMATCH` provenance proof

### A.1 名前の履歴

`RENDER_SWAP_MISMATCH` は **現在の enum には存在しない**。

```text
3b6818a   PresentationOpportunityError::RenderSwapMismatch -> "RENDER_SWAP_MISMATCH"
現在      6つへ分割
          RenderWithoutSwap / SwapWithoutRender / RenderNotCompleted
          RenderOrdinalMismatch / SwapOrdinalMismatch / PresentedFrameMismatch
```

`bench/results/p2-d5-2-formal-3b6818a/playback-run1.json` の
`RENDER_SWAP_MISMATCH` は当時の粗い単一 error であり、現在のどれに相当するかは
その artifact だけからは決まらない。**historical artifact は再解釈しない。**

### A.2 全 producer と invariant

`src/media/gpu_preview/presentation_opportunity_scheduler.cpp` の全 fail site。

| # | error | site | 条件 | 由来する invariant |
|---|---|---|---|---|
| 1 | `RenderWithoutSwap` | `markRenderComplete:113` | `!pendingRender_` | selectForRender 未実行で完了通知 |
| 2 | `RenderWithoutSwap` | `close:332` | close 時に `pendingRender_` | render が選ばれたが swap が来なかった |
| 3 | `RenderOrdinalMismatch` | `markRenderComplete:119` | 二重完了 / `renderEndQpc < renderBeginQpc` / ordinal 不一致 | render callback 側の整合 |
| 4 | `PresentedFrameMismatch` | `markRenderComplete:125` | `renderedSourceFrame != pendingDecision_.targetFrame` | **source-domain 予測 vs 実描画** |
| 5 | `SwapWithoutRender` | `commitSwap:141` | `!pendingRender_` | frameSwapped が pending render なしで到着 |
| 6 | `RenderNotCompleted` | `commitSwap:146` | `!pendingRenderCompleted_` | frameSwapped が render 完了前に到着 |
| 7 | `OpportunityRegression` | `commitSwap:150` | `swapQpc` 非単調 | frameSwapped の QPC |
| 8 | `RenderOrdinalMismatch` | `commitSwap:159` | `renderOrdinal != lastRenderOrdinal_ + 1` | render ordinal の strict +1 |
| 9 | `SwapOrdinalMismatch` | `commitSwap:164` | `swapOrdinal != lastSwapOrdinal_ + 1` | frameSwapped ordinal の strict +1 |
| 10 | `AuthorityDiscontinuity` | `commitSwap:168` | 層1 sample の usable/monotonic | 層1（評価点が frameSwapped） |
| 11 | `PresentedFrameMismatch` | `finalize:250` | `presentedSourceFrame` が定義域外 | source-domain |

ordinal の発生源。

```text
renderOrdinal   compositor_rhi_item.cpp:294  render callback ごとに ++
swapOrdinal     compositor_rhi_item.cpp:1298 recordFrameSwapped (= frameSwapped) ごとに fetch_add
```

`swapOrdinal` は **QQuickWindow::frameSwapped 由来**であることが確定。

### A.3 分類

```text
LEGACY_FRAMESWAPPED_PAIRING_ONLY      #1 #2 #5 #6 #7 #8 #9
    RenderWithoutSwap
    SwapWithoutRender
    RenderNotCompleted
    SwapOrdinalMismatch
    OpportunityRegression (swapQpc 由来のもの)
    RenderOrdinalMismatch (commitSwap 側 #8)

RENDER_SIDE_INTERNAL                  #3
    RenderOrdinalMismatch (markRenderComplete 側)
    -> render callback 内部の整合。frameSwapped に依存しない。

RETAIN_IN_V2                          #4 #11
    PresentedFrameMismatch
    -> scheduler の予測 targetFrame と実際に描画された source frame の一致。
       display outcome authority とは独立した scheduler↔compositor invariant。
       **新 contract でも保持する。**

LAYER1                                #10
    AuthorityDiscontinuity
    -> 層1 の契約。評価点が frameSwapped なだけで、由来は cRefresh/qpcVBlank。
       評価点を新 outcome 到着点へ移す。
```

### A.4 1:1 前提が既に破れている証拠

コードに回避策が実在する。

```text
compositor_rhi_item.cpp:929   formalOpportunityIgnoreNextSwap.store(true)
compositor_rhi_item.cpp:1292  recordFrameSwapped が exchange(false) して1回無視
```

すなわち **「次の frameSwapped を1回だけ数えない」** 例外が必要になっている。
`formalOpportunityDomainReached` も domain 到達後の swap を除外している。

これは `render callback ↔ frameSwapped` が構造的に 1:1 でないことを、
実装自身が認めている箇所である。

### A.5 A の結論

```text
RENDER_SWAP_MISMATCH (およびその後継6分類のうち7件) は
    render callback ↔ QQuickWindow::frameSwapped
という旧 Layer-2 invariant だけに由来する。

例外は2つ。
    PresentedFrameMismatch  -> source-domain invariant。v2 でも保持。
    AuthorityDiscontinuity  -> 層1 invariant。評価点のみ移設。
```

したがって W1 では次のとおり **意図的に retire し置換する**。
「新 authority にしたら偶然出なくなった」とは扱わない。

```text
LEGACY INVARIANT RETIRED
    render ↔ frameSwapped 1:1
    (RenderWithoutSwap / SwapWithoutRender / RenderNotCompleted /
     SwapOrdinalMismatch / commitSwap 側 RenderOrdinalMismatch /
     swapQpc 由来 OpportunityRegression)

NEW INVARIANTS
    composition token ↔ successful native Present   (1:1)
    native Present ↔ exactly one target PresentEvent
    PresentEvent outcome is terminal and unambiguous
    Displayed outcome ↔ physical identity

RETAINED
    predicted targetFrame ↔ rendered source frame   (PresentedFrameMismatch)
    layer-1 continuity                              (AuthorityDiscontinuity)
```

新 fail-close reason（案）。

```text
COMPOSITION_TOKEN_PRESENT_MISSING
COMPOSITION_TOKEN_PRESENT_AMBIGUOUS
PRESENT_EVENT_MISSING
PRESENT_EVENT_AMBIGUOUS
PRESENT_OUTCOME_UNKNOWN
DISPLAYED_QPC_MISSING
PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS
```

`formalOpportunityIgnoreNextSwap` / `formalOpportunityDomainReached` の
frameSwapped 例外は、v2 では **不要になるはず**である。もし新 invariant でも
同種の例外が必要なら、それは新 pairing の欠陥であり W1 で明示する。

---

## B. Layer-1 `cRefresh` — ACCEPTED ではなく CANDIDATE

### B.1 表記の訂正

W0 の「cRefresh は display の refresh clock」は強すぎた。
Microsoft の `DWM_TIMING_INFO` は `cRefresh` を **"The DWM refresh counter"** と
定義しており、`rateRefresh` = monitor refresh rate、`qpcVBlank` = vertical blank
前の QPC である。API 文書だけから「independent flip 中も physical refresh ordinal
と完全同値」は保証されない。

```text
Layer 1 candidate authority   DwmGetCompositionTimingInfo.cRefresh
purpose                       refresh-domain intent ordinal
NOT used as                   presentation outcome authority
acceptance pending            independent-flip continuity contract
```

層1を捨てる話ではなく、W2 の規模を広げる前に小さく実証する。

### B.2 現在の検証は不足

`presentationAuthorityUsable` / `presentationAuthorityMonotonic` は
`available` と単調性しか見ていない。

### B.3 W1 で固定する continuity contract（案）

```text
HRESULT == S_OK                       (現在は FAILED で {} を返すだけ)
cRefresh 非減少
qpcVBlank 非減少
ΔcRefresh = k のとき
    ΔqpcVBlank ≈ k × refreshPeriod    (許容幅つき)
rateRefresh / refreshPeriod の不意の変更を検出
```

サンプリングなので **`cRefresh` の +1 を毎回要求してはならない**。
呼び出し間に複数 refresh が進むのは正常。

### B.4 `AuthorityDiscontinuity` の trigger 定義

```text
discontinuity とする
    refresh counter regression
    impossible refresh/QPC relationship
    display timing domain change
    authority API unavailable

discontinuity としない
    PresentMode transition (Independent <-> Composed)
```

層1は presentation path と独立でなければならない。
`Independent -> Composed` が起きても continuity が保たれていれば authority は継続。

### B.5 検証方法と、より強い候補の発見

**`cRefresh` / `qpcVBlank` の時系列は artifact に保存されていない。**
app json が持つ refresh 系は次だけである。

```text
formal_opportunity_origin_refresh_count   (origin の1点のみ)
formal_refresh_numerator / _denominator
formal_opportunity_authority_valid
```

したがって cRefresh continuity を offline で実証することはできない。

一方、**physical VBlank observer が `ordinal` + `qpc` を直接持っている**。

```text
presentation_opportunity.physical_vblank
    samples[]           { ordinal, qpc }
    sequence_status
    long_interval_count / short_interval_count
    ring_overflow_count / wait_failure_count
    cumulative_consistent
    window_output_stable
    window_output_start / _end   (adapter LUID / monitor / refresh rational)
```

これは DWM ではなく display 側の観測であり、`check-p2-vblank-shadow.ps1` が
既に許容 0 件の fail-closed contract を課している（F3-C3-A3-T2-B で
observer thread が 8.5ms preempt された run を実際に棄却した実績がある）。

#### independent flip 下での実測

T2-D1-B3a の 3 run（すべて `GOOD_INDEPENDENT`、DWM PresentStart 0）で
physical VBlank は完全に健全だった。

| run | seq | samples | long | short | overflow | waitFail | cumulative | output stable |
|---|---|---|---|---|---|---|---|---|
| CLEAN_STATIC | OK | 305 | 0 | 0 | 0 | 0 | true | true |
| FOREIGN_WINDOW_OVERLAP | OK | 305 | 0 | 0 | 0 | 0 | true | true |
| OVERLAP_THEN_REMOVE | OK | 305 | 0 | 0 | 0 | 0 | true | true |

#### 含意

層1の候補は2つある。

```text
(i)  DwmGetCompositionTimingInfo.cRefresh
     現 formal scheduler が使用。DWM refresh counter。
     independent flip 下の同値性は API 文書からは保証されない。
     時系列が artifact に無く offline 実証不可。

(ii) physical VBlank observer ordinal
     display 側の直接観測。DWM に依存しない。
     既に fail-closed contract 済み。
     independent flip 下で健全性を実測済み（上表）。
```

**(ii) の方が層1 authority として強い。** 層3（DisplayedQPC → physical
display opportunity / ordinal）とも同じ domain であり、
`DisplayedQPC` を physical ordinal へ map する際に別 domain を跨がずに済む。

W1 では次を決める。

- 層1を (ii) へ移すか、(i) を残して (ii) を cross-check に使うか。
- (i) を残すなら、`cRefresh` / `qpcVBlank` の時系列を artifact へ出す
  diagnostic-only の追加が必要（そうしないと continuity を検証できない）。

私の見立ては **層1を (ii) へ移し、(i) は diagnostic として残す**である。
これなら層1と層3が同一 physical domain で閉じ、DWM への依存が
formal path から完全に消える。ただし `requiredFrameCount` の分母定義
（60s × 60Hz = 3600）が physical ordinal 基準でも同じになることの確認が要る。

---

## C. `formal_counter_authority_changed` の semantics

現状は常に `false` を出力しているだけで、意味が定義されていない。
W0 の「維持」は**撤回する**。

2案。

```text
案1  contract-version-relative な runtime override flag
     「この run が formal_contract_version の canonical authority から
       runtime override されたか」
     -> v2 へ bump した上で false のままでよい
        formal_contract_version = P2-D5-2-v2
        formal_counter_authority_changed = false

案2  D5-2 当初からの authority 変更を表す
     -> v2 では true でなければならない
```

**案1 を推奨する。** authority 変更は contract version で表し、この flag は
「宣言された contract からの逸脱がない」ことの guard として使う方が、
既存の全 checker（`formal_counter_authority_changed=false` を要求している）と
整合する。W1 で定義を文章化してから値を決める。

## D. v2 field の rename / add / deprecate

旧 field へ黙って別 authority を再割当てしない。

```text
ADD (v2 canonical)
    formal_successful_native_present_count
    formal_present_event_count
    formal_displayed_count            (FinalState=Presented かつ DisplayedQPC あり)
    formal_discarded_count
    formal_present_outcome_unknown_count

REDEFINE 不可 / DEPRECATE
    formal_swapped_composition_count   (frameSwapped 回数)
        -> deprecated / legacy_alias / not authoritative in contract v2
    formal_superseded_candidate_count
        -> 「同一 opportunity 内の複数 frameSwapped」と
           「複数 PresentEvent が同じ physical ordinal へ map」は
           似ているが同一概念ではない。v2 では後者を新 field で表し、
           旧 field は deprecate する。

KEEP (層1)
    formal_opportunity_anchored / _origin_refresh_count
    formal_refresh_numerator / _denominator
    formal_qpc_frequency

KEEP (source-domain)
    PresentedFrameMismatch 由来の invariant
```

**旧 counter 名を守るより accounting invariant を守る。**

---

## W0.5 exit 状況

| 項目 | 状態 |
|---|---|
| A. `RENDER_SWAP_MISMATCH` = legacy frameSwapped-only か | **CLOSED**。7件が legacy、`PresentedFrameMismatch` と `AuthorityDiscontinuity` は保持 |
| B. Layer-1 cRefresh | **CANDIDATE のまま**。時系列が artifact に無く offline 実証不可。より強い候補（physical VBlank observer ordinal）を発見し、independent flip 下での健全性を実測済み |
| C. `formal_counter_authority_changed` | 案1（contract-version-relative）を推奨、W1 で確定 |
| D. v2 field 方針 | add / deprecate 一覧を提示、W1 で確定 |

## 次

B の実証方法（既存 artifact で足りるか、小 probe が要るか）と、
C の案1採用可否を確認してから W1 contract を固定する。
