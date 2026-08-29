# P5-E4 S2-g0: ownership_soak_100 handle attribution

assertion は変更していない。既存 test をそのまま複数回取得した diagnostic
acquisition である。PASS cohort ではない。

## 1. 取得

```text
checkpoint   f07dcb6
debug        3 run
release      1 run
実行形態     serial (mvm_bench 同時実行を guard で禁止)
結果         4/4 PASS
```

## 2. 系列

`q` は quartile size。sample 数 10〜11 なので `q=2`。

```text
run                    min   min位置      末尾   q_first  q_last  delta
clean debug-1   PASS   1870  idx3(it30)  1877   1877     1877    +0
clean debug-2   PASS   1871  idx3(it30)  1873   1877     1873    -4
clean debug-3   PASS   1872  idx3(it30)  1874   1875     1874    -1
clean release-1 PASS   1869  idx3(it30)  1876   1875     1876    +1
COHORT debug    FAIL   1866  idx1(it10)  1878   1868     1877    +9
```

## 3. 機構

**全 run に ~6〜8 handle の transient dip が存在する。** 差は dip の位置だけである。

```text
clean 4 run   dip は idx3 (iteration 30)
              = first quartile (idx 0..1) の外側
              → q_first は steady state のまま
              → delta は -4 .. +1

failed run    dip は idx1 (iteration 10)
              = first quartile の内側
              → q_first が 1868 まで押し下げられる
              → delta が +9 になり threshold +8 を 1 超える
```

末尾の steady state は fail した run を含め全 run で 1873〜1878 であり、
**retention 側に差はない**。failed run の `q_last`(1877) は clean run の範囲内で
ある。delta を作ったのは `q_first` が異常に低かったことだけである。

つまり現在の metric は

```text
last-quartile 平均 - first-quartile 平均
```

であるため、**transient dip がどの位置に落ちたか**を測っている。retention を
測っていない。

## 4. 分類

```text
case A (iterationに比例して増加)      否定
                                      末尾値に run 間差がない
case B (同位置で一度だけ +N)          否定
                                      clean run では dip 位置が一定(idx3)だが
                                      増加ではなく減少方向の transient である
case C (noise / 位置ばらつき)         該当
                                      ただし単なる noise ではなく機構が特定できた
```

より正確には C の下位ケースであり、**startup/early transient が measurement
window の先頭に混入したこと**が原因である。

## 5. 結論

```text
production leak              NOT_ESTABLISHED
  steady state は pass/fail で同一。末尾 1873〜1878。
  iteration 比例の増加なし。

measurement-domain defect    ESTABLISHED
  first quartile が early transient を含む。
  metric が retention ではなく transient 位置を測っている。
  threshold +8 の当否以前の問題である。

+9 handle の owner            未特定
  harness は process 全体の handle 数しか出さず、
  sampling も 10 iteration ごとである。
  種別内訳 (GDI/USER/kernel) を取るには diagnostic 計装が必要。
```

## 6. 推奨する修正方向

threshold 変更 (`+8` → 緩和) は根拠がない。metric を monotonicity へ置き換える
のも適切でない (実 leak は allocator noise で strict monotonic にならない)。

evidence が支持するのは **measurement window の修正**である。

```text
warmup を measurement 前へ出す
  → transient を measurement domain から隔離
  → frozen steady-state window で測る
  → 既存の handle delta assertion (+8) はそのまま維持できる
```

これは contract 変更として最小である。metric 自体の再設計 (growth-vs-step
classifier) は不要である可能性が高い。

## 7. 未解決

```text
- warmup 何 iteration で steady state に入るかは未測定
  clean run では idx3 (it30) 付近まで transient が続く
- dip がなぜ起きるか (何が一時的に handle を手放すか) は未特定
- suite 実行時に dip 位置が前倒しになる理由は未特定
  failed run は full suite -j8 中、clean run は isolated である

なお当該 failed run では render_audio_* / verify_audio_* との重なりは
無かった (soak window 525..2499 行に audio test の Start なし)。
したがって audio fixture 競合はこの失敗の原因ではない。
```

## 8. 併せて確認された別 defect (本失敗の原因ではない)

`ownership_soak_100` は `--wav-dir "${MVM_AUDIO_OUT}"` を使うが
`FIXTURES_REQUIRED mvm_audio` も `RESOURCE_LOCK` も持たない。
`render_audio_*` (FIXTURES_SETUP) / `verify_audio_*` (FIXTURES_REQUIRED) と
同一 directory を共有しながら fixture の外にいる。

`-j8` の schedule 次第では同時実行しうる。実際、本 attribution の初回試行で
soak を 2 本同時に走らせた際、

```text
iteration 80: WAV 読めず: RIFF/WAVE ヘッダがありません
```

が発生した。今回の cohort failure の原因ではないが、hermeticity defect として
独立に存在する。
