# P2-D5-2 B3-I6A — Source Mapping Authority (design only)

## 1. Scope / status

- 対象: required intent ordinal が **どの時間軸の識別子か** を確定し、intent -> source frame mapping と
  source coverage preflight の authority を閉じること
- phase: **DESIGN ONLY**
- production code: **未変更**。mapping、preflight、queue、join、threshold、denominator、required population、
  source fixture のいずれも変更していない
- canonical W3: **HOLD**。historical W3 verdict は **FAIL / UNCHANGED**
- 機械可読契約: `docs/p2-d5-2-b3-i6a-source-mapping-authority.json`

本 design は W3 を PASS へ寄せるための調整を一切含まない。B3-I5B §17 で露出した
`SOURCE_COVERAGE_INSUFFICIENT` に対し、**source fixture を延長して症状を隠す前に semantics を凍結する**
ことだけを目的とする。

## 2. 観測事実 (B3 §17 / W3 run 1)

```text
required_intent_count   3600        (measure 60s x 60)
source frames           3600        (0..3599)
refresh                 59950/1000  (59.95Hz)
source fps              60/1

targetFor(i) = floor(i * 60000 / 59950)
targetFor(3597) = 3600   -> source domain外 -> SOURCE_COVERAGE_INSUFFICIENT
targetFor(3599) = 3602   -> required set全体の最大target
```

したがって現行 mapping を維持する場合、canonical fixture に必要な source frame 数は **3603** である。
この数値は「延長すれば通る」ことを示すが、**延長してよいか**は semantics が決める。

## 3. 現行 source-level inventory

| authority | producer | layer | 時間軸 |
|---|---|---|---|
| `required_intent_count` | `compositor_spike_controller.cpp` の `measureSeconds * 60` | 1A workload | workload |
| `required intent ordinal` | `RequiredIntentQueue::reserveHead` (B3-I1) | 1A workload | workload |
| `physical_vblank_ordinal` | window output physical VBlank observer | 1B physical | display |
| `target source frame` | `PresentationOpportunityScheduler::targetFor` | **1A ordinal に 1B rate** | display |
| source coverage preflight | `sourceFrameCount >= requiredMeasurementFrameCount` | 1A workload | workload |

`targetFor` だけが Layer 1A の ordinal に Layer 1B の refresh rate を適用している。W3 run 1 の fatal は、
この 1 箇所の混在が required set の末尾で source domain を超えたという事実である。

preflight も `required count` としか比較しておらず、mapping が実際に要求する最大 source frame を
検査していない。したがって preflight は通り、runtime だけが末尾で fail-close した。

## 4. 由来 — なぜ混在したか

pre-B3 の `opportunityOrdinal` は DWM refresh count 由来の **display opportunity 序数**だった。
その時点では「display opportunity i で表示すべき source frame」を求める `targetFor` の refresh 比は
正しい式だった。

B3-I1 で ordinal producer を required-intent queue head へ移し、ordinal は Layer 1A の
**issued intent identity** になった。`targetFor` はそのまま残ったため、意味の異なる ordinal に
display rate が適用され続けている。

## 5. 候補

### A. `WORKLOAD_INTENT_TIME_AXIS`

```text
intent ordinal は workload contract rate (60/s) 上の識別子
target(i) = sourceFrameOffset + floor(i * sourceFps / requiredIntentRate)
canonical: target(i) = i、max target = 3599
source fixture 変更不要、display refresh に依存しない
```

### B. `DISPLAY_OPPORTUNITY_TIME_AXIS`

```text
intent ordinal は display refresh opportunity 上の識別子
target(i) = sourceFrameOffset + floor(i * sourceFps * refreshDen / (sourceFpsDen * refreshNum))
canonical: max target = 3602、必要 source frame = 3603
source fixture sizing が display refresh に依存する
```

## 6. 選定 — A (`WORKLOAD_INTENT_TIME_AXIS`)

根拠は次の 4 点である。いずれも本 slice で新しく決めたものではなく、既に凍結済みの契約である。

1. **W1 formal accounting contract v2 §1** が `intent_ordinal` を Layer 1A、
   `physical_vblank_ordinal` を Layer 1B と明示し、「1A と 1B を同一視しない」を凍結している。
   `ordinal` の無修飾使用も禁止している。B は Layer 1A の ordinal に 1B の rate を与えるため、
   この凍結に正面から反する。
2. **W1 は `required_intent_count` を「test contract 由来の分母」**と定義し、実測 display 量ではないと
   している。分母が workload 契約であるなら、その分母を数える ordinal も workload 軸である。
3. **B3-I1** で ordinal の producer は required-intent queue head になった。issuance identity は
   display opportunity ではなく immutable required set の位置である。
4. B を採ると **canonical input asset の sizing が測定機の refresh に依存**する。59.95Hz なら 3603、
   60.000Hz なら 3600、他の refresh ならまた別になり、canonical fixture が再現しない。

したがって選定は A である。**この選定は product semantics の確定であり、実装は本 slice では行わない。**

### A を選んだ場合の帰結

```text
required set                3600            変更なし
drop-rate 分母              3600            変更なし
frozen threshold            55fps / 2%      変更なし
canonical source fixture    3600 frames     変更不要 (max target = 3599)
target mapping              要修正 (I6C)
source coverage preflight   要修正 (I6C)
```

**source を 3603 へ延ばすことは A の下では修正ではなく症状の隠蔽である**。B3 §17 の fatal は
mapping 側の欠陥として扱う。

## 7. 凍結する invariant

```text
REQUIRED_INTENT_ORDINAL_IS_LAYER_1A
TARGET_MAPPING_INDEPENDENT_OF_DISPLAY_REFRESH
INTENT_RATE_HAS_SINGLE_PRODUCER
SOURCE_COVERAGE_PREFLIGHT_USES_MAX_TARGET_OVER_REQUIRED_SET
MAX_TARGET_OVER_REQUIRED_SET_LESS_THAN_SOURCE_FRAME_COUNT
REQUIRED_SET_SIZE_UNCHANGED
DROP_RATE_DENOMINATOR_UNCHANGED
SOURCE_COVERAGE_FAILURE_IS_PROTOCOL_FATAL_NOT_PERFORMANCE_DROP
```

`INTENT_RATE_HAS_SINGLE_PRODUCER` は、現在 controller の `measureSeconds * 60` に literal として
埋まっている intent rate を、required count と target mapping の両方が参照する 1 つの authority に
することを要求する。現状は required count 側だけがこの rate を持ち、mapping 側は refresh を見ている。

## 8. 禁止する解法

```text
EXTEND_SOURCE_FIXTURE_TO_HIDE_MAPPING_DEFECT
SHRINK_REQUIRED_SET
CHANGE_FROZEN_THRESHOLD
CHANGE_DROP_RATE_DENOMINATOR
SKIP_TAIL_INTENTS
CLAMP_TARGET_TO_LAST_SOURCE_FRAME
TREAT_SOURCE_COVERAGE_FATAL_AS_PERFORMANCE_DROP
DERIVE_INTENT_ORDINAL_FROM_PHYSICAL_VBLANK_ORDINAL
```

`CLAMP_TARGET_TO_LAST_SOURCE_FRAME` は末尾 intent を静かに repeat へ変え、drop 判定を歪めるため
特に禁止する。

## 9. preroll mapping への影響

preroll producer は `requiredFrameCount = repeatedFrame + 1` / `sourceFrameOffset = repeatedFrame` /
`sourceFps = 1` で start し、同一 source frame を繰り返す。A の式でも preroll の実効 range では
target は `repeatedFrame` のままであり、挙動は変わらない。I6C はこの不変を negative test で固定する。

## 10. 本 slice で行っていないこと

```text
production code 変更            なし
mapping 修正                    I6C
source coverage preflight 修正  I6C
fatal/token publication atomicity  I6B (本 slice の semantics とは独立)
canonical fixture 変更          なし
fresh W3 再取得                 HOLD
```

historical `COMPOSITION_TOKEN_MISMATCH` は `UNRESOLVED_HISTORICAL_RUNTIME_FAILURE` のまま保持し、
I6 mapping failure へ再分類しない。
