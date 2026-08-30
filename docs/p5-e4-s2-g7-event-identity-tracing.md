# P5-E4 S2-g7: Event identity lifecycle tracing (中間凍結)

## 1. 計装

`CreateEvent` / `CloseHandle` を hook せず、handle table 上の
`(HandleValue, Object, GrantedAccess)` を render 前後で集合差分する方式。
`NtQueryObject(ObjectNameInformation)` で名前も引く。

```text
diagnostic-only
verdict / threshold / metric / window influence = 0
```

## 2. 初回観測 (PASS run / release)

```text
  it     retained  handle values
  0      7         1452, 6544, 9208, 9756, 10444, 12692, 12700
  10     3         6280, 8440, 9304
  20     1         5376
  30-70  0
  80     1         9220
  90     1         3280
  100-110 0
  120    1         6268

合計 14 / 13 render (平均 +1.08、count ベースの計測と整合)
distinct handle value 14、複数 render への再出現 0
access mask は全て 2031619 (EVENT_ALL_ACCESS)
name は全て空 = 匿名 (pthread primitive と整合)
```

## 3. **重要: これは exact identity ではない**

現在の tracer は handle value の集合差分である。

```cpp
before.insert(e.handleValue);
if (before.count(e.handleValue)) continue;
```

Windows の handle value は再利用される。同一 render 中に

```text
before        handle 500 -> Event A
render 中     CloseHandle(500) / CreateEvent -> handle 500 を再利用
after         handle 500 -> Event B
```

が起きると、Event B は「before から存在した」と誤認され retained set から
落ちる。`Object` は非 elevated 実行のため `0x0` で返るので、
`(handleValue, Object)` による世代識別も現状できない。

したがって観測値の位置づけは次である。

```text
14 retained handle values
  = handle-value 差分による retained-event の
    lower-bound / candidate set

「render 中に作られ destroy されなかった Event identity が正確に 14 個」
  とは言えない
```

## 4. 時間構造 — 確定部分と仮説部分

```text
確定
  retention は一様でなく bursty である
  先頭 3 render (it0/10/20) に 11 candidate
  measured domain (it30+) は 3 candidate (約 0.3/render)

仮説 (未確定)
  front-loaded burst = legitimate one-time initialization
  scattered remainder = genuine leak
```

前半 11 個が次のどれなのかは未分類である。

```text
- process-lifetime の正当な cache
- bounded な MLT initialization
- early phase に集中しただけの cleanup omission
- handle-value 再利用による誤検出
```

## 5. 現在の判定

```text
Event retention                  ESTABLISHED
persistent / variable magnitude  ESTABLISHED
render-speed correlation         SUPPORTED
retention temporal structure     ESTABLISHED (front-loaded + scattered)

front-loaded = initialization    HYPOTHESIS
scattered = genuine leak         HYPOTHESIS
exact Event identity             NOT ESTABLISHED (handle-value 差分の限界)
Event owner / callsite           UNKNOWN
cleanup omission source          UNKNOWN

delayed-release teardown race    REFUTED
observable cleanup omission      ESTABLISHED
```

## 6. 再開時の next action

> **S2-g7a: handle-reuse-safe な Event lifecycle tracing を入れ、その後に
> high-retention と low-retention の対を取得する。**

handle value 差分のまま high-retention run を大量取得しても、
再利用による誤差を含んだ candidate set が増えるだけである。先に identity を
確定させる。

必要なのは create 時点で付ける独自 serial である。

```text
CREATE  serial=417 handle=6268 render=120 callsite=...
DESTROY serial=417 handle=6268 render=120
```

これなら handle value が再利用されても

```text
serial 417 handle=500 DESTROY
serial 418 handle=500 CREATE
```

を区別でき、`serial N CREATE / DESTROY missing` を直接主張できる。

hook 範囲は全プロセスの `CloseHandle` を detour する前に、winpthreads / MLT が
Event を作る wrapper 層へ絞る。`pthread_cond_*` / `pthread_mutex_*` /
thread lifecycle のどれが Event を保持するかまで分かれば source fix に近づく。

取得対象は PASS/FAIL ではなく **high-retention (poll ~370) と
low-retention (poll ~560-600) の対**である (S2-g6b)。

## 7. gate への含意

`ownership_soak_100` は誤検出していない。常在する Event retention を
total handle aggregation が threshold を超えたときに検出している。
したがって **production cleanup defect を閉じるまで ordinary gate は OPEN**
であり、cohort の反復では閉じない。
