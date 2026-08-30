# P5-E4 S2-g3: sustained handle growth の attribution

metric / threshold / measured workload / warmup 30 は未変更。追加したのは
診断専用の計装 (gdi / user / audio / h_before_audio / h_after_audio) のみである。

## 1. 出発点

`ffe0e0c-r2` cohort の `run-2-ucrt64-release-2` で、warmup 30 を入れた後の
measured domain 内に持続的な増加を観測した。

```text
it30 1873  it40 1876  it50 1881  it60 1882  it70 1883
it80 1884  it90 1885  it100 1884 it110 1885 it120 1885
measured q_first 1875 -> q_last 1884  delta +9  (threshold +8)
```

startup bias では説明できないため `production leak = NOT_ESTABLISHED` を撤回し
`production/resource retention = OPEN` へ戻した。

## 2. 再現しなかった

計装後、isolated 6 run と full ordinary suite 5 run を取得した。

```text
isolated (6 run)   deltas -5, -6, +3, +4, +0, -1     max +4
suite    (5 run)   deltas +0, +2, -6, -2, +2         max +2
```

**11 run のいずれでも sustained growth は再現しなかった。** measured domain の
block 系列はすべて ±5 以内で平坦であり、r2 の `1873 -> 1885` のような単調な
climb は現れていない。

## 3. 方法上の誤り (記録)

最初の 6 run は isolated で取得した。しかし本 failure は full ordinary suite
実行中にしか観測されていない。

```text
isolated / targeted   約 26 run   失敗 0
full ordinary suite   約 15 run   失敗 2  (f07dcb6 +9 / ffe0e0c-r2 +9)
```

isolated 取得では構造的に再現し得ない。S2-g0 で同じ誤りをして g1c で修正した
にもかかわらず繰り返した。suite 条件での取得を追加したが、そちらでも 5 run では
発火しなかった。

## 4. 分かったこと

### GDI / USER は owner ではない

全 11 run で一定である。

```text
gdi    3        (全 run / 全 iteration で不変)
user   22-28    (振れ幅は6以内、系統的増加なし)
```

process handle が動いても GDI/USER は動かない。したがって増加分が発生する場合、
その owner は file / event / thread / section 等の **kernel handle** である。
`GetGuiResources` はこれ以上の分解能を持たない。

### 通常時の iteration 収支は均衡している

```text
audio 呼び出し前後   平均 +1.5 〜 +4.1  (最大 +17、時折大きな負)
compose close 後     平均 -58 〜 -62
```

1 iteration あたり約 60 handle を確保し、`mvm_mlt_compose_close` で解放している。
通常時は net で蓄積しない。

### warmup 30 は典型ケースには効いている

suite 条件の delta 分布:

```text
warmup 導入前 (S2-g1c)   +0, +1, -1, +4, +5    max +5
warmup 導入後 (S2-g3)    +0, +2, -6, -2, +2    max +2
```

ただし r2 の +9 は warmup 30 が入った状態で発生している。**warmup は典型的な
measurement sensitivity を下げるが、この稀な事象を防いでいない。**

## 5. 分類の結論

```text
A. audio iteration ごとに増えて後で解放      該当しない (通常時は均衡)
B. 蓄積して固定 N で plateau                 r2 では該当するが再現しない
C. workload に比例して増え続ける             該当しない (r2 も plateau した)
D. process handle は増えるが GDI/USER は不変  成立 (ただし増加時に限る)
```

**現時点で A/B/C を確定できない。** 事象が再現しないためである。
D については GDI/USER を owner 候補から除外できた。

## 6. 現在の判定

```text
production/resource retention   OPEN (未確定)
sustained growth の再現性        11 run で 0 回
GDI / USER owner                EXCLUDED
kernel handle 種別              未特定 (計装不足)
S2-g1b (warmup 30)              実装維持 / validation OPEN
                                典型 max を +5 -> +2 へ下げたが +9 は防げていない
発生頻度の推定                   suite 条件で約 15 run 中 2 回
                                isolated では約 26 run 中 0 回
```

## 7. 次に取り得る選択肢

```text
(a) suite 条件での取得を継続する
    1 run 約 10 分。頻度が 15 分の 2 なら数十 run 必要になり得る。

(b) kernel handle の種別 attribution を計装する
    NtQuerySystemInformation(SystemHandleInformation) 等が必要。
    診断専用でも production harness への追加としては重い。

(c) 事象が捕捉できるまで ordinary gate を開けたままにする
    現状の方針。ただし closure が無期限に遅れる。
```

threshold を +12 / +16 へ上げる案は採らない。r2 は measured domain で
net +9 > +8 であり、契約上は正当な FAIL である。plateau したことは
免責理由にならない。
