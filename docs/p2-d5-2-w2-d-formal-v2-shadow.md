# P2-D5-2 W2-D Formal-v2 Shadow Integration

## 位置づけ

W2-D は新しい性能判定を作る段ではない。W2-A/A.1、B1、B2、C1、C2.1、C2 で
既に閉じた authority を、**1 つの noncanonical formal-v2 shadow artifact から
再生できるようにする**段である。canonical cutover は W2-E であり、W2-D では行わない。

artifact には次を固定している。

```text
shadow_only                                     = true
canonical_authority                             = false
performance_threshold_evaluated                 = false
canonical_verdict_evaluated                     = false
frame_swapped_retirement_changed                = false
source_frame_identity_used                      = false
nearest_qpc_or_tolerance_used                   = false
layer1a_layer1b_count_difference_is_not_a_verdict = true
```

checker はこれらを再構築比較より**先**に検査し、true/false が反転していれば
reject する。`drop_rate` / `effective_fps` / `performance_pass` /
`canonical_verdict` などの field が artifact・run・record のどこかに現れた場合も
reject する。

## record

formal-v2 の中心 record は freeze 済み chain をそのまま 1 行へ束ねたものである。
新しい identity は導入していない。identity key は C1 / C2 と同じ
`exact_event_key = "<etw_sequence>|<displayed_qpc>"` であり、source frame は
identity にも record field にも入れない。

```text
exact_event_key
intent_ordinal
intent_scope
required_intent_membership
composition_token_serial
native_present_serial
etw_sequence
final_state
displayed_qpc
physical_vblank_ordinal
in_measurement_physical_domain
intent_satisfied
```

`composition_token_serial` / `native_present_serial` は C1 candidate 側と
B2 terminal 側の双方から取り、値が割れていれば transport splice として
fail-close する。`final_state` は B2 terminal outcome であり、formal-v2 母集団は
`Presented` のみである。

## checker が再計算するもの

aggregate をコピーせず、sealed source から records を再構築したうえで数え直す。

Layer 1A:

```text
required_intent_count = |C2.1 exact required intent set|
satisfied_intent_count = |unique in-domain CURRENT_MEASUREMENT intent_ordinal ∈ required set|
unsatisfied_intent_count = |required set のうち satisfy されていない ordinal|

required = satisfied + unsatisfied
```

`unsatisfied` は `required - satisfied` の引き算では作らず、required set 側から数える。

Presented accounting (C2 N1 identity):

```text
satisfied_intent_count
  + in_domain_presented_foreign_intent_count
  = in_domain_presented_event_count
```

physical fill:

```text
filled_physical_opportunity_count
  = unique(in-domain formal Presented physical_vblank_ordinal)
```

Layer 1B:

```text
physical_vblank_opportunity_count = W2-A domain cardinality
                                  = last_ordinal - origin_ordinal + 1
```

W2-A checker が同じ run artifact から読んだ domain と、checker が traced-app から
読み直した domain が一致しない場合は「別 run の physical domain」として fail-close する。

### Layer 1A と Layer 1B の差は verdict ではない

W2-D で初めて Layer 1A と Layer 1B が同じ artifact に並ぶ。

```text
required_intent_count != physical_vblank_opportunity_count
```

これは **INVALID でも performance FAIL でもない**。W1 で freeze した通り両者は
異なる母集団である。core には両者を比較する blocker が無く、architecture test が
それを固定している。

## authority replay と provenance binding

checker / runner は hash 一致だけを authority にしない。次を実際に再実行する。

```text
W2-A / W2-A.1  physical VBlank domain     (run ごと、status=PASS まで確認)
W2-B2          terminal shadow            (run ごと、内部で W2-B1 を再実行)
W2-C1.4        sealed mapping replay
W2-C2.1        required intent domain
W2-C2          intent satisfaction ledger
W2-C2.4        formal transport policy
```

そのうえで cross-cohort splice を fail-close する。

```text
source_c1_proof_sha256
source_c21_proof_sha256
source_c2_proof_sha256
source_upstream_inventory_proof_sha256
run ごとの sealed_source_sha256
  traced_app / present_history_raw / b2_terminal_shadow / upstream_inventory_proof
```

- C2.1 が参照する C1 と、W2-D が渡された C1 が別なら reject
- C2 が参照する C1 / C2.1 と、W2-D が渡されたものが別なら reject
- C1 / C2 / C2.1 が別 run の sealed source を見ていれば hash で割れて reject
- W2-A physical domain が別 run のものなら reject

W2-D は C2 ledger の値をコピーせず独立に再計算し、C2 と数が割れた場合は
`C2_LEDGER_INTEGRATION_DIVERGENCE` で integration を fail-close する。

## fresh-7 offline 統合結果

fresh capture は取得していない。producer / measurement semantics を変更していないため、
fresh-7 sealed authority の offline 統合で足りる。

入力:

```text
build/p2-d5-2-w2-c14-mapping-replay-fresh-7-20260825-c24.json
build/p2-d5-2-w2-c21-required-intent-domain-fresh-7-20260825-c24.json
build/p2-d5-2-w2-c2-intent-satisfaction-fresh-7-20260825-c24.json
```

出力:

```text
build/p2-d5-2-w2-d-formal-v2-shadow-fresh-7-20260825.json
```

[事実] 3 run を統合し、records から次を再集計した。

```text
required intents                          900 (300 x 3 run)
satisfied intents                         438 (146 x 3 run)
unsatisfied intents                       462 (154 x 3 run)
formal Presented                          438
in-domain Presented                       438
in-domain foreign Presented                 0
filled physical opportunities             438
physical VBlank opportunities             897 (299 x 3 run)
```

[事実] 次の identity はすべて成立した。

```text
900 required = 438 satisfied + 462 unsatisfied
438 satisfied + 0 foreign = 438 in-domain formal Presented
438 filled = 438 unique in-domain physical ordinal
897 physical opportunity = Σ (last_ordinal - origin_ordinal + 1)
```

[事実] `required 900 != physical opportunity 897` だが、これを blocker にしていない。
異なる母集団の count 差であり、W2-D では verdict へ接続しない。

[事実] W2-A、B1/B2、C1.4、C2.1、C2、C2.4 の各 checker を再実行したうえで
statement を再構築し、artifact と完全一致した。

[exit] fresh-7 formal-v2 shadow は `FORMAL_V2_SHADOW_INTEGRATION_EXACT`。
drop rate、fps、threshold、canonical PASS/FAIL は評価しておらず、
frameSwapped / DWM authority の retirement も canonical artifact の置換も行っていない。

## negative

W2-D では mutation より authority mixing が重要である。`p2_d5_2_w2d_authority_mixing`
が次を fail-close することを確認している。

```text
NegativeDifferentC1ForC2            別 C1 を参照する C2
NegativeDifferentC21ForC2           別 C2.1 を参照する C2
NegativeRequiredIntentSetMutation   C2 構築後に required intent set を差し替えた C2.1
NegativeDifferentPhysicalDomainRun  別 run の physical VBlank domain
NegativeMissingUpstreamAuthority    upstream authority の欠落
NegativeSealedSourceMutation        sealed source の事後書き換え
NegativeCanonicalFlagTrue           canonical_authority = true の注入
NegativeCanonicalVerdictEvaluated   canonical_verdict_evaluated = true の注入
NegativePerformanceVerdictInjected  drop_rate 等 performance field の注入
NegativeSatisfiedIntentMutation     record の intent_satisfied 改変
NegativePhysicalOrdinalMutation     record の physical ordinal 改変
NegativeAggregateMutation           aggregate の改変
```

`p2_d5_2_w2d_formal_v2_shadow_*` は integration core 単体の fail-close を固定している
(required set mutation / satisfaction mutation / physical ordinal mutation /
foreign-current mutation / scope mutation / final state mutation / chain provenance 欠落 /
duplicate event key / physical domain cardinality / filled > opportunity)。

同じ source frame が複数 intent を満たす fixture は C2 で固定済みである。W2-D では
source frame を identity key に含めないことを architecture test で確認するに留める。

## 再現

```powershell
pwsh scripts/build-p2-d5-2-w2-d-formal-v2-shadow.ps1 `
  -C1Proof build/p2-d5-2-w2-c14-mapping-replay-fresh-7-20260825-c24.json `
  -C21Proof build/p2-d5-2-w2-c21-required-intent-domain-fresh-7-20260825-c24.json `
  -C2Proof build/p2-d5-2-w2-c2-intent-satisfaction-fresh-7-20260825-c24.json `
  -Output build/p2-d5-2-w2-d-formal-v2-shadow-<new-name>.json

pwsh scripts/check-p2-d5-2-w2-d-formal-v2-shadow.ps1 `
  -Proof build/p2-d5-2-w2-d-formal-v2-shadow-fresh-7-20260825.json

ctest --test-dir build/ucrt64-release `
  -R 'p2_d5_2_w2d_' --output-on-failure --timeout 300
```

runner は既存 artifact を上書きしない。

## W2-D で行っていないこと

```text
drop rate / fps / threshold 判定
canonical PASS/FAIL
old frameSwapped authority との優劣判定
frameSwapped / DWM authority の retirement
canonical artifact の置換
W3 acquisition
historical FAIL/INVALID の書き換え
```

W2-E canonical cutover 設計はここから始める。
