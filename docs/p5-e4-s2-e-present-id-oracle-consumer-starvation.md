# P5-E4 S2-e — Present-ID oracle consumer starvation closure

## 1. 変更

`p2_present_id_oracle_live` の `statisticsThread` を、`Sleep(0)` による
yield busy-waitからpublication eventベースの取得へ変更した。

```text
VBlank observer
  ring publication
    -> opt-in auto-reset event
    -> statistics sampler wake

statistics sampler
  TIME_CRITICAL priority
  publicationごとに3/4周期のbounded poll
  cycle completionをproducerへ通知

Present producer
  frame-latency waitable object
  maximum frame latency = 1
  sampler cycle completion barrier
  previous Present-IDまでのbounded ACK
```

swapchain起動直後のflip-mode遷移は固定12 Presentのwarmup domainとして明示した。
warmupの完了、失敗0、最終warmup ID、測定先頭IDとの連続性をartifactとcheckerで
fail-closeにした。測定domainへ入ったsuccessful Presentは従来どおり全件exact joinする。

`WindowOutputVBlankObserver` のpublication signalは
`waitForPublishedCount()`を呼んだobserverだけがopt-inする。通常利用時のcanonical
observer hot pathには`SetEvent`を追加しない。

次は変更していない。

```text
ORACLE_SAMPLING_GAP許容             なし
poll threshold                     max_poll_interval_qpc * 2 < nominal_period_qpc
test skip / delete                 なし
production presentation semantics  変更なし
```

## 2. 修正前

attribution時のordinary runは次だった。

```text
configured submissions             900
statistics transitions             406
sampler_vblank_gap_count             51
final observed PresentCount         898
poll threshold                      FAIL
oracle_status                       INVALID
```

Present、GetLastPresentCount、GetFrameStatistics、statistics disjoint、VBlank ring overflow、
VBlank waitのfailureはすべて0であり、substrate failureではなくconsumer starvationだった。

## 3. checkerの空振り防止

checkerへ、event trigger、TIME_CRITICAL priority、frame-latency mode、warmup、
sampler ACK/cycle timeout、各wait failureのfail-close assertionを追加した。

negative harnessは各mutationの適用前後JSON fingerprintを比較し、checkerが返す
violation messageの完全一致を要求する。

```text
Good                              1/1 PASS
negative                         15/15 PASS
mutation application            15/15 verified
intended violation exact-match  15/15 verified
timeout                           0
```

## 4. live closure evidence

採取条件:

```text
checkpoint  cefb3841368abc4039dd17e416fdf72edfc32ddc + S2-eの未コミット差分
build       build/ucrt64-release
date        2026-08-29
```

900 Presentの明示runを4回連続で取得し、さらにordinary CTestの登録live testも通した。
最終明示runは次だった。

```text
warmup_complete                              true
present submissions / oracle records        900 / 900
sampler_vblank_gap_count                     0
max_poll_interval_qpc / nominal_period_qpc   44216 / 166805
poll_interval_valid                          true
observed_ids_complete                        true
final_drain_complete                         true
oracle_status                                VALID

present_failure_count                        0
get_last_present_count_failure_count         0
frame_latency_wait_failure_count             0
sampler_ack_timeout_count                    0
sampler_cycle_timeout_count                  0
statistics_failure_count                     0
statistics_disjoint_count                    0
vblank_ring_overflow_count                   0
vblank_wait_failure_count                    0
sampler_vblank_wait_failure_count            0
```

4明示runの最大poll間隔は`45521 / 44354 / 44178 / 44216`で、全runが
frozen thresholdを満たした。

## 5. ordinary regression gate

```powershell
ctest --test-dir build\ucrt64-release `
  -LE 'performance|stability' `
  --output-on-failure --timeout 180 --parallel 8
```

```text
1332/1332 PASS
timeout 0
unexplained failure 0
real time 406.13 sec
```

repository-wide lintは既存debtのためcleanではない。未整形6ファイルと
PSScriptAnalyzer warning 6件は、いずれもS2-eの変更対象外である。
今回変更したC++はclang-format適用済みで、変更したPowerShellに新規warningはない。

## 6. 判定

```text
S2-e                                  CLOSED
Present-ID oracle ordinary failure    1 -> 0
sampler VBlank gap                    51 -> 0
explicit live                         4/4 PASS (900/900 each)
registered ordinary live              PASS
checker Good + negative               16/16 PASS
threshold relaxation                  0
skip / delete                         0
production presentation semantics     unchanged

ordinary regression debt              7 -> 0
P5-E4 ordinary regression gate        CLOSED
```

P5-E4全体のfinal closureはordinary gateだけでは決めない。
`docs/phase5-e-plan.md` §6の残るfrozen P2 correctness/performance、P3-C-2、P4、
artifact/provenance/docs監査をcurrent closure checkpointで完了した後に最終判断する。
