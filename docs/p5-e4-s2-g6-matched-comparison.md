# P5-E4 S2-g6: PASS/FAIL matched comparison — Event retention は常時存在する

`76cc871` の acquisition は run-4 で `p2_present_id_oracle_live` が先に落ちて
`INCONCLUSIVE_OTHER_TEST_FAILED` になったが、run-1〜3 で **計装済みの PASS 系列
3 本**を取得できた。既存の FAIL 系列 2 本と突き合わせる。

## 1. 結果

`after_close` (= `mvm_mlt_compose_close` 後) の Event 数と、soak metric が見る
total handle quartile delta を並べる。

```text
run                  q_first  q_last  total Δ  Event Δ  verdict
PASS g6 r1 debug     1877     1884    +7       +13      PASS
PASS g6 r2 release   1872     1871    -1       +5       PASS
PASS g6 r3 debug     1877     1873    -4       +1       PASS
FAIL g5 r2 release   1875     1884    +9       +9       FAIL
FAIL g4 r1 debug     1875     1885    +10      +13      FAIL

threshold = +8 (total handles)
```

after_close Event 系列:

```text
PASS r1  [112,117,114,116,117,118,119,119,120,121,124,125,125]
PASS r2  [110,106,111,111,113,114,114,114,114,114,113,114,115]
PASS r3  [115,114,115,115,115,113,113,113,114,115,115,115,116]
FAIL g5  [115,116,113,115,116,117,118,121,122,123,124,124,124]
FAIL g4  [113,116,113,114,116,117,118,119,122,123,124,125,126]
```

## 2. S2-g4 の枠組みを訂正する

**Event retention は failing run に固有ではない。** PASS r1 は Event +13 で
FAIL g4 と同じ大きさである。それでも PASS したのは total handle delta が +7 で
threshold +8 を下回ったからにすぎない。

```text
誤り (S2-g4 の含意)
  Event retention が failure を説明する

正しい
  Event retention は常時存在し、大きさが +1 〜 +13 と変動する
  test が落ちるのは total handle delta が +8 を超えたときだけ
  Event Δ は常に total Δ 以上であり、他の型が部分的に相殺している
```

つまり **defect は intermittent ではない。常在していて大きさが変わる。**
intermittent なのは「metric が threshold を超えるかどうか」だけである。

PASS r1 (Event +13 / total +7) は near-miss であり、同じ defect が
threshold の内側に収まっただけである。

## 3. 新しい相関: render が速いほど retention が大きい

path marker は PASS 3 本にしかない (FAIL 系列は marker 導入前)。

```text
run                  poll avg  waited avg  Event Δ
PASS g6 r1 debug     426       8529 ms     +13
PASS g6 r2 release   602       12040 ms    +5
PASS g6 r3 debug     605       12092 ms    +1
```

`poll_count` は `is_stopped` になるまでの 20ms poll 回数である。
**render が速く終わるほど Event retention が大きい。**

r1 と r3 はどちらも debug preset なので preset 差ではない。

これは「負荷が高いと漏れる」という直感とは逆である。consumer が早く
`is_stopped` に達する経路で cleanup が取りこぼされている可能性がある。

ただし **n=3 であり、FAIL 系列に marker がないため相関は未確認**である。

## 4. 判定

```text
production/resource retention    ESTABLISHED
resource type                    Event
発生                             常在 (passing run にも存在)
大きさ                           +1 〜 +13 / run (変動)
test failure との関係            Event Δ 単独では決まらない
                                 total handle delta > +8 のときだけ落ちる

delayed-release teardown race    REFUTED (S2-g5)
observable cleanup omission      ESTABLISHED
specific leaked object           UNKNOWN
render 速度との相関              SUGGESTIVE (n=3, FAIL 側に marker なし)
```

## 5. 次に必要なもの

marker 付きの **FAIL 系列**が要る。現状は

```text
PASS 3 本   marker あり
FAIL 2 本   marker なし (g4 / g5 は marker 導入前)
```

であり、poll_count と retention の相関を FAIL 側で確認できていない。

同一 checkpoint で acquisition を継続し、marker 付き FAIL を 1 本取れば
ケース A/B/C の判別ができる。
