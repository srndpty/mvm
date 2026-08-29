# P5-E4 S2-e2: Present-ID oracle の authority 分離

## 1. 問題

`p2_present_id_oracle_live` は CTest registration 上 `LABELS "p2;workstation"` で
あり、ordinary gate は `performance|stability` だけを除外する。したがって debug
preset でも ordinary authority に入っていた。

一方 AGENTS.md は timing を exit criteria に使う場合 release で測ることを求める。
oracle の判定はこの2種類を混在させていた。

```text
exact correctness   900/900, gap=0, exact join, final drain, ordering
timing              max_poll_interval_qpc * 2 < nominal_period_qpc
```

さらに probe の `oracleValid` が `pollIntervalValid` を correctness と同じ論理積へ
入れていたため、exit code 4 が両者を区別できなかった。

## 2. 3層の authority

```text
A. EXACT_CORRECTNESS        debug + release
   configured submission population complete (900/900)
   Present ID exact +1 / observed completeness / exact join
   gap = 0 / final drain exact / API失敗 0 / output identity stable

B. ACQUISITION_LIVENESS     debug + release
   900件をboundedに取得できること
   budgetは性能判定ではない

C. TIMING_AUTHORITY         release only
   max_poll_interval_qpc * 2 < nominal_period_qpc
   release frozen threshold は変更していない
```

`900/900` 自体は correctness prerequisite なので debug でも hard requirement の
まま。「何秒で 900 件取れるか」だけが timing である。

## 3. authority mode は明示的に渡す

build type から checker が推測しない。CTest registration が provenance を持つ。

```cmake
-AuthorityMode "$<IF:$<CONFIG:Debug>,CORRECTNESS_ONLY,FULL_RELEASE>"
```

artifact には次を記録する。

```text
authority_mode                 FULL_RELEASE | CORRECTNESS_ONLY
correctness_verdict            PASS | FAIL
timing_verdict                 PASS | FAIL | NOT_AUTHORITY_IN_DEBUG
acquisition_liveness_verdict   PASS | FAIL
acquisition_wait_budget_ms     1000 (release) / 8000 (correctness-only)
```

`NOT_AUTHORITY_IN_DEBUG` は PASS の代替ではなく scope 外の明示である。
checker は CORRECTNESS_ONLY で timing threshold を**緩めるのではなく判定しない**。

## 4. acquisition budget

debug の flake は timing threshold ではなく acquisition liveness だった。
submission loop は次の3箇所で `break` し、population を 900 未満に切り詰める。

```text
frame latency waitable  timeout
sampler cycle           timeout
sampler ack             timeout
```

いずれも 1 秒固定だった。CORRECTNESS_ONLY では 8 秒へ広げる。これは threshold の
緩和ではない。timing verdict には一切入らず、900/900 の要求も変わらない。

## 5. negative authority

```text
GoodCorrectnessOnly                CORRECTNESS_ONLY で timing 範囲外でも PASS
NegativeAuthorityModeMissing       provenance 無し           => reject
NegativeDebugTimingApplied         debug で timing PASS 主張 => reject
NegativeReleaseTimingDisabled      release で timing 無効化  => reject
NegativeCorrectnessRelaxedInDebug  debug で observed gap     => reject
```

最後のものが重要である。`correctness_verdict` field は PASS のままにし、checker が
transition 列を再計算して `ORACLE_SAMPLING_GAP` を捕まえることを要求する。producer
の自己申告を信用しないことと、debug split で correctness が緩んでいないことを
同時に示す。

## 6. 結果

```text
checker contract   21/21 PASS (release / debug 両方)
live release       FULL_RELEASE / correctness PASS / timing PASS / 900 / gap NONE
live debug         CORRECTNESS_ONLY / correctness PASS /
                   timing NOT_AUTHORITY_IN_DEBUG / 900 / gap NONE
threshold relaxation  0
skip / delete         0
```

## 7. 実行環境の前提

S2-e2 の実装中、machine load が高い状態では release / debug とも
`sampler_ack_timeout_count=1` で 45〜266 件目に break する現象を観測した。
**HEAD の probe でも同一症状が再現したため S2-e2 起因ではない。**

負荷が収まると両 preset とも 900/900 で PASS する。

これは ordinary 実行時に観測されていた
`UNIDENTIFIED_INTERMITTENT_FAILURE` の有力な候補である。live oracle は
`RESOURCE_LOCK mvm_gpu` と `RUN_SERIAL` を持つが、同時に走る他 test の CPU 負荷
までは排除しない。stability cohort を取る際は連続実行の負荷を考慮する必要がある。
