# P5-E4 S2-g5: teardown ordering attribution — race 仮説は棄却

`058ac66` の suite acquisition run-2 (ucrt64-release) で `ownership_soak_100` を
再現し、teardown phase ごとの Event 数を捕捉した。

```text
outcome        REPRODUCED
quartile_avg   1875 -> 1884   (threshold +8)
```

## 1. 結論: 遅延解放は 1 度も起きていない

failing run の全 13 audio iteration で、`close_returned` 以降 Event 数は
まったく動かない。

```text
  it     close_returned   settle_250ms   settle_1000ms   drop
  0      135              135            135             0
  30     133              133            133             0
  60     137              137            137             0
  90     143              143            143             0
  120    140              140            140             0

遅延解放が起きた iteration: 0 / 13
```

**teardown race 仮説は棄却する。**

race であれば、worker thread が close 後に終了して Event を解放するはずであり、
`settle_250ms` か `settle_1000ms` で減少が観測されなければならない。
1000ms 待っても 1 個も解放されない。

これは timing の問題ではなく **cleanup omission (解放されないまま残る object)**
である。

## 2. teardown 各段が解放している数

```text
  it     render中に作られた   stopが解放   closeが解放
  0      +24                 2           1
  10     +18                 2           1
  20     +12                 2           1
  30     +15                 2           0
  40     +18                 3           1
```

consumer の `stop` / `close` が解放するのは 2〜4 個だけである。render 中に
作られる 12〜24 個の大半は、後続の `mvm_mlt_compose_close` まで解放されない。

そのうち **1 個が compose close 後も残る** (S2-g4 で確定した +1/render)。

## 3. before_render の推移

各 render 開始時点の Event 数が単調に増えている。

```text
it0 111  it30 117  it60 121  it90 126  it120 128
```

retention が render をまたいで蓄積していることの直接証拠である。

## 4. 判定の更新

```text
production/resource retention   ESTABLISHED  (変更なし)
resource type                   Event        (変更なし)
rate when fault active          ~1 / audio render  (変更なし)

teardown race                   REFUTED
  遅延解放 0/13。1000ms 待っても解放されない。

root cause                      cleanup omission
  consumer teardown 経路のどこかで Event が destroy されないまま残る。
  どの object かは未特定。
```

私は S2-g4 で teardown race を有力仮説として提示した。**直接計測により棄却された。**
負荷依存に見えたことから timing 起因と考えたが、release の遅れではなかった。

## 5. 残る問い

なぜ条件依存なのか。race でないなら、負荷によって

```text
- 通る code path が変わる (error path / fallback path)
- 生成される object の種類か数が変わる
- consumer 内部の状態遷移が変わる
```

のいずれかが起きていると考えるのが自然である。

`is_stopped_true` 時点の Event 数 (render 中に作られた数) は 12〜24 と大きく
ばらついており、負荷によって consumer が作る object 数自体が変わっている。
このばらつきと retention の関係はまだ見ていない。

## 6. 次に必要な evidence

```text
- passing run と failing run で、同じ phase 系列を比較する
  (今回は failing run のみ。baseline は別 run の 1 iteration しかない)

- render 中に作られる Event 数と retention の相関

- consumer が通る path の差分
  error / fallback path を通っているなら、その経路の cleanup を見る
```

threshold `+8` は引き続き妥当である。test は実在する retention を検出している。
