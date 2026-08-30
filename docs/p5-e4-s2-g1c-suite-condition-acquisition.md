# P5-E4 S2-g1c: suite 条件での handle acquisition と g1a の訂正

`--dump-all-samples` のみ付与。threshold / metric / window / scheduling は未変更。
これは gate cohort ではなく failure reproduction acquisition である。

## 1. 取得結果

```text
planned runs   5 (debug 3 / release 2)
outcome        NOT_REPRODUCED
全 run         1343/1343 PASS
```

`ownership_soak_100` は suite 条件でも再現しなかった。ただし **系列は捕捉できた**
ので attribution としては前進である。

## 2. suite 条件と isolated 条件で delta 分布が系統的に違う

```text
isolated (S2-g1a)  deltas [-4, -3, -1, -3, -4]   mean -3.0
suite    (S2-g1c)  deltas [ 0, +1, -1, +4, +5]   mean +1.8
                                                  shift +4.8
```

suite 条件は delta を系統的に正方向へ押し上げる。cohort failure の +9 は
この分布の裾として説明できる位置にある (suite 観測最大 +5、threshold +8)。

## 3. 機構: first quartile の有効標本数は 25 ではなく 3

handle は audio iteration (`it % 10 == 0`) でのみ階段状に変化する。
したがって 25 iteration の窓に含まれる「独立な値」は 2〜3 個しかない。

```text
run                      q_first distinct  q_last distinct
run-1-ucrt64-debug-1     3                 1
run-2-ucrt64-release-1   2                 2
run-3-ucrt64-debug-2     3                 2
run-4-ucrt64-release-2   3                 2
run-5-ucrt64-debug-3     3                 3
```

実例 (run-5, delta +5):

```text
q_first 窓 (0..24)   1868 x10, 1870 x10, 1876 x5   -> 1870
q_last  窓 (75..99)  1874 x5,  1875 x10, 1876 x10  -> 1875
```

q_first は先頭 2 block にほぼ支配される。先頭 block は startup 側の値であり、
suite 条件ではこれが低く出る。結果として delta が正へ振れる。

## 4. S2-g1a の結論を訂正する

**g1a で「warmup は支持されない」と結論したのは誤りである。**

assertion は片側である。

```cpp
if (handleLast > handleFirst + 8)
```

したがって評価すべき統計量は `max|delta|` ではなく `max(signed delta)` である。
g1a では両側の `max|delta|` を使ったため、安全側 (負) の振れを危険と誤認し、
warmup 20〜40 を「悪化」と判定していた。

正しい片側統計で 10 run (isolated 5 + suite 5) を再計算した結果:

```text
warmup    isolated max   suite max   combined max
0         -1             +5          +5
10        -1             +4          +4
20        -1             +1          +1
30        -2             +1          +1
40        -2             +1          +1
50        +0             +3          +3
60        +1             +4          +4
```

**warmup 20〜40 が有効域である。** combined max が +5 から +1 へ下がる。
g1a が「有効域は 60 以上」としたのは統計量の誤りによるものである。

isolated 条件では warmup の有無にかかわらず max は +1 以下であり、warmup を
入れても悪化しない。

## 5. 現在の判定

```text
production leak              NOT_ESTABLISHED
  suite 条件 5 run でも tail の継続上昇や
  audio iteration ごとの net retention は観測されない。

measurement-domain defect    ESTABLISHED
  first quartile の有効標本数が 2〜3 しかなく、
  startup block に支配される。
  実行条件 (isolated / suite) で delta 分布が +4.8 動く。

startup transient model      再度支持される
  g1a での否定は統計量の誤りによるものであり撤回する。

warmup remedy                支持される (warmup 20〜40)
```

## 6. 推奨

S2-g1b を再開し、次を実装する。

```text
warmup 30 iterations   (有効域 20〜40 の中央。±10 の誤差に耐える)
  handle sample を measurement domain から除外
  functional correctness (crash / value mismatch / ownership) は warmup 中も有効

measured 100 iterations  (現行の workload contract を減らさない)

threshold +8             変更しない
metric                   変更しない
```

warmup は retention の許容量ではなく measurement-domain boundary である。

## 7. 未解決

```text
- cohort failure (+9) は本 acquisition では再現しなかった。
  したがって「warmup 30 なら +9 を防げた」は直接には未検証である。
  根拠は分布 (suite max +5 -> +1) であって、当該 run の再現ではない。

- handle が audio iteration で何を確保/解放しているかは未特定。
  種別内訳には GetGuiResources 等の計装が必要。

- 標本は 10 run である。warmup 20/30/40 の差は分解できていない。
```
