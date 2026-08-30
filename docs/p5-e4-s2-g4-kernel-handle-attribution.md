# P5-E4 S2-g4: kernel handle type attribution — owner 特定

`f0655cc` の diagnostic cohort run-1 (ucrt64-debug) で `ownership_soak_100` を
suite 条件で再現し、種別内訳を捕捉した。acquisition としては目的達成である。

```text
outcome        REPRODUCED
failure        handle 数が増加しています 1875 -> 1885
quartile_avg   handles_first 1875 -> handles_last 1885  (threshold +8)
warmup 30 / measured 100 / total 130
```

## 1. 増えているのは Event だけである

audio iteration ごとの `after_close` 内訳:

```text
  it     Event   File  Thread  IoCompletion  Semaphore  Section  Mutant  Key
  0      113     63    46      12            24         14       12      1360
  30     114     63    45      12            24         14       12      1360
  60     118     63    46      12            24         14       12      1360
  90     123     63    47      13            24         14       12      1360
  120    126     63    47      13            24         14       12      1360

it0 -> it120 差分
  Event         +13
  Thread        +1
  IoCompletion  +1
  File          0
  Semaphore     0   Section 0   Mutant 0   Key 0   Timer 0
```

`Event` 以外は実質不変である。**owner domain は Event handle に確定した。**

## 2. audio render 1 回あたり約 +1 の net retention

```text
  it     Event: before -> after -> close   across_audio  net(前回closeとの差)
  0        111 ->  133 ->  113             +22
  30       117 ->  132 ->  114             +15           +1
  60       121 ->  137 ->  118             +16           +1
  90       126 ->  142 ->  123             +16           +1
  120      129 ->  143 ->  126             +14           +1

Event after_close  113 -> 126
net +13 / 12 audio iteration = +1.08 per audio render
```

各 audio render は Event を 14〜22 個作り、大半を解放するが **毎回 1 個だけ
残す**。130 iteration には audio iteration が 13 回あるため net +13 となり、
threshold +8 を超える。

これは measurement artifact ではない。**production 側の resource retention で
ある。**

## 3. どこか

`mvm_mlt_compose_render_audio` (src/media/mlt/mvm_mlt_compose.c:1118) は
avformat consumer を毎回生成し、

```c
mlt_consumer_start(consumer);
while (!mlt_consumer_is_stopped(consumer)) { Sleep(20); ... }
mlt_consumer_stop(consumer);
mlt_consumer_close(consumer);
```

で閉じている。呼び出し順序自体は正しい。

Event handle は MLT が使う pthread の condition variable / mutex 実装
(winpthreads) が確保する。consumer 内部の worker thread が完全に終了する前に
`close` が進むと、その 1 個が解放されずに残る、という筋が最も整合する。

**これは仮説である。** 確定には MLT 内部か winpthreads 側の追跡が必要である。

## 4. 条件依存であることの説明

これまで isolated 6 run / suite 5 run では Event が平坦だった (117 -> 116 等)。
同じ code path が漏らしたり漏らさなかったりする。

teardown race の仮説はこれと整合する。

```text
suite 条件 = 負荷が高い
  -> consumer worker thread の終了が遅れる
  -> close が先行する
  -> Event が 1 個残る

isolated 条件 = 負荷が低い
  -> thread が先に終了する
  -> 解放される
```

観測されている発生分布とも合う。

```text
full ordinary suite   約 16 run 中 3 回失敗
isolated / targeted   約 26 run 中 0 回
```

## 5. 判定の更新

```text
production/resource retention   ESTABLISHED
                                Event handle, audio render あたり約 +1

owner domain                    Event (kernel handle)
                                File / Section / Semaphore / Mutant / Key は無関係

発生条件                        負荷依存 (suite 条件でのみ観測)
根本原因                        未確定 (consumer teardown race が有力仮説)

measurement-domain defect       別途 ESTABLISHED のまま
                                (quartile 有効標本数 2-3、S2-g1c)
                                ただし今回の failure はこれでは説明できない
```

S2-g0 で `production leak = NOT_ESTABLISHED` と書き、S2-g3 でも再現しなかった
ため OPEN のままとしていた。**今回 owner まで特定できたので ESTABLISHED へ
更新する。**

## 6. threshold について

`+8` を上げる案は引き続き採らない。今回の +10 は measurement artifact ではなく
実際の retention であり、test は正しく検出している。

閉じるべきは test ではなく retention 側である。
