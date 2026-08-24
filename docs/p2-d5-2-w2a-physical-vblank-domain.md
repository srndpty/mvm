# P2-D5-2-W2-A — Physical VBlank Domain Shadow Wiring

W1 / W1.1 / W1.2 (`Formal Accounting Contract v2`) は **FROZEN**。
本段階は **shadow only** であり、次は一切変更しない。

```text
legacy formal scheduler / counters
legacy shutdown behavior
legacy PASS/FAIL semantics
formal threshold
historical verdict
```

W2-A の責務はひとつだけ。

> measurement window に対する physical VBlank domain を exact に構築できるか

実装:
- `src/media/gpu_preview/physical_vblank_domain.{h,cpp}`
- `apps/compositor_spike/compositor_spike_controller.cpp`
  (`presentation_opportunity.physical_vblank_domain_shadow`)

executable contract:
- `scripts/check-p2-d5-2-w2a-physical-domain.ps1`
- `tests/gpu_preview/test-p2-d5-2-w2a-physical-domain-contract.ps1` (38 case)
- `tests/gpu_preview/test_physical_vblank_domain.cpp` (boundary / property / mutation)

---

## 1. authority の分離

```text
measurement lifecycle authority     既存 formal runner / controller
    measurement_start_qpc
    measurement_end_qpc

physical opportunity authority      window output physical VBlank observer
    { physical_vblank_ordinal, qpc }
```

collector は **独自の end を作らない**。「60 秒経ったから」で domain を閉じる
ことはしない。窓は必ず `CompositorSpikeState::measurementStartQpc` /
`measurementEndQpc`（既存 formal measurement lifecycle が確定した値）をそのまま
使う。これにより W2-E で authority cutover した際も test workload の時間 domain
自体は変わらない。

窓が供給されていない（`start <= 0` / `end <= start`）場合は fail-close する。
`PHYSICAL_VBLANK_MEASUREMENT_WINDOW_INVALID` は observer の問題ではなく
lifecycle 側の問題なので、canonical 射影は `RUNTIME_AUTHORITY_OVERRIDE`。

---

## 2. boundary の定義（コード化済み）

domain member:

```text
measurement_start_qpc <= vblank.qpc < measurement_end_qpc
```

boundary authority（**domain member ではない**）:

```text
predecessor.qpc <  measurement_start_qpc
successor.qpc   >= measurement_end_qpc
```

half-open なので、`measurement_end_qpc` と VBlank QPC が完全一致した場合は
**successor 側**に置かれる。`measurement_start_qpc` と完全一致した VBlank は
**domain member** である。1 tick 動かすと境界が 1 本ずれることを
`halfOpenBoundary` / `sweepProperty` で固定した。

両端の bracket は必須。無いと tail の exact accounting が閉じない。

窓が 2 本の VBlank の間に収まった場合、domain は空（count = 0）だが bracket は
成立する。この場合 `successor.ordinal == predecessor.ordinal + 1` を要求する。

---

## 3. W2-A exit contract（shadow field）

`presentation_opportunity.physical_vblank_domain_shadow`

| field | 意味 |
|---|---|
| `shadow_only` | 常に true |
| `formal_counter_authority_changed` | 常に false |
| `performance_semantics_connected` | 常に false |
| `measurement_window_authority` | `"formal measurement lifecycle"` |
| `physical_opportunity_authority` | `"window output physical VBlank observer"` |
| `domain_relation` | half-open 関係の明文 |
| `measurement_start_qpc` / `measurement_end_qpc_exclusive` | lifecycle 由来の exact QPC |
| `predecessor_valid` / `predecessor_ordinal` / `predecessor_qpc` | 下側 boundary authority |
| `successor_valid` / `successor_ordinal` / `successor_qpc` | 上側 boundary authority |
| `origin_ordinal` / `origin_qpc` | domain 起点。空 domain では -1 |
| `last_ordinal` / `last_qpc` | domain 終点。空 domain では -1 |
| `physical_opportunity_count` | domain の cardinality |
| `sequence_status` | ordinal / QPC の単調連続性 |
| `long_interval_count` / `short_interval_count` | 隣接 VBlank と断定できない interval |
| `ring_overflow_count` / `wait_failure_count` | observer 側 counter |
| `cumulative_consistent` | origin 基準の累積ずれ |
| `output_stable` | adapter LUID / output / HMONITOR / refresh rational 不変 |
| `boundary_bracketed` | predecessor と successor が両方そろった |
| `shadow_authority_valid` / `shadow_authority_error` | W2-A fail-close 結果 |
| `shadow_authority_canonical_reason` | W1 freeze 済み v2 語彙への射影 |
| `required_intent_count` / `intent_overhang_count` / `intent_surplus_count` | shadow only |

checker が強制する identity:

```text
B1  predecessor.qpc <  measurement_start_qpc
B2  successor.qpc   >= measurement_end_qpc

count > 0 のとき
D1  physical_opportunity_count == last_ordinal - origin_ordinal + 1
D2  predecessor.ordinal + 1 == origin_ordinal
D3  successor.ordinal == last_ordinal + 1
D4  measurement_start_qpc <= origin_qpc <= last_qpc < measurement_end_qpc

count == 0 のとき
D5  origin_ordinal == last_ordinal == -1
D6  successor.ordinal == predecessor.ordinal + 1

X1  intent_overhang_count == max(required - physical, 0)
X2  intent_surplus_count  == max(physical - required, 0)
X3  overhang == 0 または surplus == 0
```

---

## 4. `required_intent_count` と physical count を比較して verdict を出さない

```text
required_intent_count      = 3600
physical_opportunity_count = 3597
```

は **W2-A では正常**である。`3 opportunities missing` と判定してはならない。

```text
Layer 1A = workload contract       required_intent_count
Layer 1B = physical observation    physical_opportunity_count
```

W2-A で確認するのは **physical domain が正しく観測できたか**だけ。
`intent_overhang` / `intent_surplus` は shadow 出力するが performance semantics
には接続しない。checker は `drop_rate` / `performance_pass` /
`true_drop_count` 等の verdict field が shadow に混入していたら
`RUNTIME_AUTHORITY_OVERRIDE` で拒否する。

---

## 5. fail-close reason（W2-A 固有語彙と canonical 射影）

W1 の reason 語彙は変更しない。W2-A では observer invalid を分解し、
**freeze 済み v2 語彙への射影**を同時に出力する。W2-E cutover ではこの射影を
使う。

| W2-A reason | canonical (v2 frozen) |
|---|---|
| `NONE` | `NONE` |
| `PHYSICAL_VBLANK_MEASUREMENT_WINDOW_INVALID` | `RUNTIME_AUTHORITY_OVERRIDE` |
| `PHYSICAL_VBLANK_OBSERVER_UNAVAILABLE` | `PHYSICAL_VBLANK_OBSERVER_INVALID` |
| `PHYSICAL_VBLANK_RING_OVERFLOW` | `PHYSICAL_VBLANK_OBSERVER_INVALID` |
| `PHYSICAL_VBLANK_WAIT_FAILURE` | `PHYSICAL_VBLANK_OBSERVER_INVALID` |
| `PHYSICAL_VBLANK_OBSERVER_STALL` | `PHYSICAL_VBLANK_OBSERVER_INVALID` |
| `PHYSICAL_VBLANK_SEQUENCE_BREAK` | `PHYSICAL_VBLANK_SEQUENCE_BREAK` |
| `OUTPUT_OR_MODE_CHANGED` | `OUTPUT_OR_MODE_CHANGED` |
| `PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED` | `PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED` |

precedence は決定的（上記表の順ではなく実装順）:

```text
MeasurementWindowInvalid
  -> ObserverUnavailable
  -> RingOverflow
  -> WaitFailure
  -> SequenceBreak
  -> OutputOrModeChanged
  -> BoundaryNotBracketed
  -> ObserverStall
```

### [P1予防] 取りこぼしと「本当に長い物理 interval」を混同しない

strict contract（`long_interval_count == 0` / `short_interval_count == 0`）は
維持する。ただし、

```text
実 display が長い interval だった
observer が起きられなかった
```

はどちらも **authority invalid** であって **performance FAIL ではない**。
`PHYSICAL_VBLANK_OBSERVER_STALL` は両者を区別せず、区別できないことを名前で
明示している。以前 observer thread preemption の run を invalid にした実績と
同じ扱いである。

### interval 検査の範囲

interval / 累積の検査は **bracketed closed range `[predecessor, successor]`** で
行う。domain の exact accounting に必要な sample はこの範囲に閉じており、
warmup 中の observer hiccup を domain authority の判定に混ぜない
(`stallOutsideBracketIsIgnored`)。

`ring_overflow_count` / `wait_failure_count` は observer 全体の counter なので
範囲を切れない。非 0 なら無条件に fail-close する。

`sequence_status` は ring 全体で評価する（ordinal は observer の自前 counter で
あり、途中に gap があれば domain の ordinal 算術そのものが信用できない）。

---

## 6. live shadow acquisition (W2-A-LIVE)

`scripts/p2-d5-2-w2a-live-shadow.ps1`

```text
3 run x (warmup 3s + measure 5s)
formal-preflight        使わない
incremental mapper      使わない
performance evaluation  行わない
```

legacy formal path は `RENDER_SWAP_MISMATCH` 系で早期 shutdown し得るため、
W2-A shadow の確認に混ぜない。measurement lifecycle と VBlank observer が同じ
実経路を使う短い non-formal acquisition で足りる。

結果 (`bench/results/p2-d5-2-w2a-live/`, artifact は commit しない):

```text
3/3  shadow_authority_valid = true / error = NONE
     sequence_status OK, long/short/overflow/wait = 0
     cumulative_consistent, output_stable, boundary_bracketed = true
     predecessor.qpc < start / successor.qpc >= end
     physical = 299, ordinal 算術 exact (1..299, pred 0, succ 300)
     required = 300 -> intent_overhang = 1  (verdict にしない)

verdict: PHYSICAL_VBLANK_DOMAIN_SHADOW_EXACT
```

### W2-A.1 適用後の再取得

```text
3/3  shadow_authority_valid = true / error = NONE
     prestart_vblank_preroll_completed = true, timeout = false
     preroll sample.qpc < measurement_start_qpc  (3/3)
     preroll wait = 65k / 90k / 100k QPC tick (= 6.5 / 9.0 / 10.0 ms)
     pred 1 -> origin 2 .. last 300 -> succ 301,  physical = 299
```

preroll sample は V0、実際の predecessor は V1 になった。preroll sample が
そのまま predecessor である必要はない。**preroll sample.qpc < 窓を arm した時刻
<= measurement_start_qpc** なので、ring には必ず start より前の sample が
存在する。これで下側 bracket は race ではなく構造的に保証される。

### 【解消済み】下側 bracket が race で成立していた問題

`requestMeasurementStart()` が VBlank observer を start し、その直後の render
callback が `measurementStartQpc` を stamp する。つまり predecessor は
「observer の最初の `WaitForVBlank` が render callback より先に返った」ときだけ
存在する。上の 3 run はいずれも `predecessor_ordinal = 0` であり、
**observer の最初の 1 本がそのまま下側 bracket になっている**（余裕は 247〜956
QPC tick）。

W2-A.1 でこれを解消した。

---

## 6.1 W2-A.1 — Lower Boundary Preroll

physical VBlank authority を要求する acquisition path に限り、次の順序を固定した。
**通常 product path の measurement start semantics は変更していない。**

```text
baseline = ring.publishSerial()      <- observer start より前に取る
observer start                        (ring reset。publishSerial は戻さない)
publishSerial > baseline を bounded wait
measurementStartRequested を arm
render callback が measurementStartQpc を stamp
```

`ring が空でない` ではなく **monotonic publish serial の前進**で判定する。
`VBlankRing::reset()` は count / overflow を 0 に戻すが `publishSerial` は戻さない
ので、start/stop 再利用時に reset 前の stale sample を「新しく publish された」と
誤認しない。この reset invariant は
`publishSerialIsMonotonicAcrossReset` で固定している。

timeout は **acquisition liveness timeout** であり performance threshold ではない
(500 ms = 60 Hz で約 30 VBlank 分。observer start timeout と同じ桁)。timeout 時は
**measurement を開始せず** `beginShutdown` で fail-close する。

追加 reason (canonical 射影はどちらも `PHYSICAL_VBLANK_OBSERVER_INVALID`):

```text
PHYSICAL_VBLANK_PREROLL_TIMEOUT           preroll が成立しなかった
PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START  preroll sample.qpc >= measurement_start_qpc
```

`PHYSICAL_VBLANK_PREROLL_TIMEOUT` は stage reason 名であり、
`prestart_vblank_preroll_timeout=true` を必ず意味するものではない。
observer の停止などを含む、preroll 成立不能全般をこの stage reason で表す。

precedence 上は ObserverUnavailable の直後、sequence 判定より前に置く。

追加 shadow field (shadow-only provenance):

```text
prestart_vblank_preroll_completed
prestart_vblank_preroll_timeout
prestart_vblank_sample_ordinal
prestart_vblank_sample_qpc
prestart_wait_elapsed_qpc
```

**確認する不変量は ordinal が 0 かどうかではなく
`prestart_vblank_sample_qpc < measurement_start_qpc`** である
(`GoodPrerollOrdinalNonZero` で固定)。

architecture invariant は
`test-p2-d5-2-formal-architecture-contract.ps1 -Phase W2A1` が静的に固定する。

```text
baseline serial 取得  ->  observer start  ->  prerollNewSample  ->  measurement arm
preroll 失敗経路は beginShutdown + return で閉じている
```

W2-A.1 適用後、下側 bracket が落ちうるのは
**渡された sample 列から preroll sample が失われた場合だけ**になった
(`boundaryNotBracketed` の no-predecessor case はこの形で表現している)。

---

## 7. 次

```text
W2-A  physical VBlank domain                              <- 本段階
W2-B  intent / token / native Present / PresentEvent
      terminal closure, ABI v4                            (shadow only)
W2-C  DisplayedQPC -> physical_vblank_ordinal
      intent satisfaction ledger                          (shadow only)
W2-D  actual shadow artifact -> v2 executable checker
      legacy canonical path remains active
W2-E  canonical cutover / frameSwapped formal authority retirement
W3    clean fixed SHA / fresh formal acquisition
```

W2-B では `intentOrdinal` を composition token と native Present record の
**両方**に入れ、native hook の 1:1 copy invariant に含める。

```text
token.intentOrdinal == nativePresent.intentOrdinal
```

を ABI-level provenance contract にする。`outputFrameNumber` は source frame
identity であり 30fps→60Hz では非単射なので、**QPC / source frame からの
fallback は作らない**。sentinel が必要なら `intentOrdinalValid`（または明確な
invalid sentinel）を定義し、formal mode では valid 必須、non-formal では無視と
する。ABI は v4 へ bump し、mismatch は hard reject する。
