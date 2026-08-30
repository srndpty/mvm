# P5-E4 S2-g1a: warmup boundary の実測

assertion / threshold は未変更。`--dump-all-samples` は emission 専用の
診断 flag であり判定に影響しない。soak は直列実行した。

## 1. 前提の訂正

`samples` は元から **毎 iteration** 記録されている
(mvm_bench_compose.cpp:2490)。10 回ごとなのは stderr 進捗と JSON 出力だけで
ある。したがって metric の quartile は

```text
quartile_size = 100 / 4 = 25
first quartile = iteration 0..24
last  quartile = iteration 75..99
```

S2-g0 で dip 位置を 10-sample 系列から再計算した表は、metric の窓とは異なる
粒度で計算していた。authoritative な `quartile_avg` 値は JSON 由来なので
影響しないが、窓の記述は誤っていた。

## 2. 取得結果 (100 sample / run)

```text
run                q_first  q_last  delta  range(all)  range(60-99)
ucrt64-debug-1     1872     1868    -4     10          2
ucrt64-debug-2     1872     1869    -3     5           2
ucrt64-debug-3     1872     1871    -1     3           2
ucrt64-release-1   1873     1870    -3     5           2
ucrt64-release-2   1870     1866    -4     6           2
```

5/5 PASS。delta は全て負である。

## 3. handle は audio iteration でのみ動く

値が変化する iteration 位置:

```text
debug-1    10, 20, 50, 61, 70
debug-2    10, 15, 22, 50, 61
debug-3    10, 18, 24, 58, 60, 70, 90
release-1  10, 30, 60, 70, 90
release-2  10, 27, 60, 70
```

大半が 10 の倍数である。soak は `audioThisIter = doAudio && (it % 10 == 0)`
(mvm_bench_compose.cpp:2468) で 10 回に 1 度だけ audio 処理を行う。handle 数は
ほぼ audio iteration でのみ階段状に変化しており、iteration ごとに漸増する
性質ではない。

## 4. warmup を入れた場合の効果 (実測)

各 run の先頭 N iteration を捨てた場合の quartile delta:

```text
warmup=0    deltas [-4, -3, -1, -3, -4]   max|d| 4
warmup=10   deltas [-2, -3, -1, -2, -4]   max|d| 4
warmup=20   deltas [-3, -4, -1, -3, -5]   max|d| 5
warmup=30   deltas [-3, -5, -2, -4, -6]   max|d| 6
warmup=40   deltas [-2, -4, -2, -4, -6]   max|d| 6
warmup=50   deltas [ 0, -1, -1, -3, -5]   max|d| 5
warmup=60   deltas [ 1,  0,  0, -1, -2]   max|d| 2
```

**warmup 20〜40 はむしろ悪化する。** 改善するのは warmup=60 以降であり、
これは 100 iteration の 60% にあたる。

## 5. 結論: warmup 仮説は支持されない

S2-g0 では「startup transient が first quartile に混入した」と説明したが、
per-iteration データはこれを支持しない。

```text
startup transient モデル       支持されない
  先頭 10 iteration が特別なのではなく、
  値は run 全体を通じて audio iteration ごとに動く。
  安定するのは iteration 60 以降である。

warmup による除去              費用対効果が成立しない
  有効な warmup は 60 以上。
  「warmup 60 + measured 100」は実行時間を 1.6 倍にする。
  transient が run の 60% を占めるなら、それは transient ではない。
```

## 6. 未解決 / 反証されていない点

```text
- 今回の 5 run はいずれも delta が負であり、cohort failure の +9 を
  再現していない。したがって「warmup=60 なら +9 を防げた」は未検証である。

- cohort failure の q_last は 1877 で、今回の 5 run の q_last (1866〜1871) より
  高い。S2-g0 で「q_last は clean range 内」と書いたが、それは同日の別 4 run と
  比較した結果であり、run 間の baseline drift (1866〜1877) を跨いだ比較には
  なっていない。head の低下だけが原因という説明は確定していない。

- handle が audio iteration で動く理由 (何が確保/解放されるか) は未特定。
  種別内訳を取るには GetGuiResources 等の計装が必要である。
```

## 7. 推奨

warmup 導入を S2-g1b として実装することは、現時点の evidence では推奨しない。
根拠は 4 節の実測 (warmup 20〜40 は悪化、有効域は 60 以上) である。

metric 自体が run 内 wander に対して過敏である可能性が高いが、それを結論づける
には cohort failure と同じ条件 (full ordinary suite 実行中) での per-iteration
取得が必要である。今回の 5 run は isolated 実行であり、failure 条件を再現して
いない。

次に取るべきは threshold や metric の変更ではなく、
**failure 条件下での per-iteration 取得**である。
