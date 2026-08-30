# P5-E4 S2-g6b: render 速度と Event retention の相関

marker 付き FAIL は取れなかった (acquisition は run-4 で
`preview_spike_json_contract` が先に落ち `INCONCLUSIVE_OTHER_TEST_FAILED`)。
しかし **FAIL 系列は必須ではなかった**ことが分かった。理由は 3 節に書く。

## 1. run 間

計装済み 8 run (すべて PASS)。

```text
run                      poll min  poll max  Event Δ
g6  r1 debug             355       628       +13
g6  r4 release           476       615       +2
g6  r2 release           582       625       +5
g6  r3 debug             559       622       +1
g6b r1 debug             567       634       +2
g6b r2 release           578       622       +2
g6b r3 debug             554       629       +5
g6b r4 release           560       627       +3
```

poll_count が最も小さい run (355) が最大の retention (+13) を示す。
それ以外は poll min が 554〜582 に固まり retention は +1〜+5 である。

## 2. run 内 (g6 r1、唯一 poll に幅がある run)

```text
  it     poll_count  Event(after_close)  次renderまでのΔ
  0      371         112                 +5
  10     371         117                 -3
  20     382         114                 +2
  30     371         116                 +1
  40     373         117                 +1
  50     383         118                 +1
  60     390         119                 +0
  70     369         119                 +1
  80     355         120                 +1
  90     365         121                 +3
  100    561         124                 +1
  110    628         125                 +0
  120    625         125

  poll <  500   retention avg +1.20  (n=10)
  poll >= 500   retention avg +0.50  (n=2)
```

同一 run 内で it100 を境に poll_count が ~370 から ~560 へ切り替わり、
retention が下がっている。preset / machine state / load を固定した比較で
ある点が run 間比較より強い。

g6b の 48 render は全て poll >= 500 で、pooled retention は +0.25/render。
1 節・2 節と整合する。

```text
poll ~370 (render 約 7.4s)   retention ~+1.2 / render
poll ~600 (render 約 12s)    retention ~+0.2〜0.5 / render
```

**render が速いほど Event を多く残す。** 「負荷が高いと漏れる」という当初の
直感とは逆である。

## 3. FAIL 系列は必須ではない

S2-g6 で判明したとおり、Event retention は PASS/FAIL に関わらず常在し、
test の verdict は total handle delta が +8 を超えるかどうかで決まる。

```text
g6 r1   Event +13 / total +7   PASS   (near-miss)
g4 r1   Event +13 / total +10  FAIL
```

Event 量で見れば g6 r1 と g4 r1 は同等である。したがって leak を研究するのに
必要なのは FAIL 系列ではなく **retention が大きい系列**であり、それは既に
marker 付きで取得できている (g6 r1)。

high-retention (g6 r1, +13, poll ~370) と low-retention (g6 r3, +1, poll ~560)
の対比が、そのまま matched comparison として使える。

## 4. 判定

```text
Event retention                 ESTABLISHED / 常在 / +1〜+13 per run
render 速度との相関             SUPPORTED
  run 間 8 run + run 内 1 run (regime change) で一貫
  ただし causal ではない。fast completion と retention が
  共通の上流原因を持つ可能性は残る。

specific leaked Event object    UNKNOWN
missing cleanup path            UNKNOWN
```

## 5. 次

S2-g7 (Event identity lifecycle tracing) へ進む。相関により scope を
**fast-completion path 周辺**へ絞れる。

追うべきは、

```text
render ordinal
Event create identity / callsite
Event destroy identity
poll_count (= 完了速度)
```

で、fast render で destroy されない identity を直接特定する。
