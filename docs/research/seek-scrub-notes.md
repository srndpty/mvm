# seek と scrub の実測メモ (S6)

対象: MLT 7.36.1 / MSYS2 UCRT64
計測対象: S5 の 5 トラックタイムライン（単一 producer ではない）
ビルド: **RelWithDebInfo**（debug の値は判定に使わない）
計測: `std::chrono::steady_clock`。PNG 出力や JSON 整形は latency に含めない

再現:

```powershell
mvm_bench seek-bench  bench/scenarios/s5-five-track.json --random 200 --csv seek.csv

# M6 は matrix スクリプトで測る。単発実行の値を文書へ転記しない。
pwsh scripts/scrub-matrix.ps1 -Runs 3 -Requests 300
```

---

## S6 baseline（loader 修正・所有権修正の反映後）

**最適化は一切入れていない状態の基準値。** proxy も使っていない。
native 素材のみ。以降の改善はここを起点に比較する。

計測条件: 5 トラックタイムライン、RelWithDebInfo、
`--random 1000 --seed 20260804`（固定点 14 を含む 1014 点）を 3 回。

| 実行 | count | mismatch | p50 | p95 | max | mean |
| --- | --- | --- | --- | --- | --- | --- |
| 1 回目 | 1014 | **0** | 161.1 | 223.8 | 476.4 | 148.0 |
| 2 回目 | 1014 | **0** | 154.3 | 227.9 | 275.7 | 147.9 |
| 3 回目 | 1014 | **0** | 128.8 | 219.9 | 261.2 | 130.2 |
| **中央値** | — | **0** | **154.3** | **223.8** | **275.7** | 147.9 |

**[事実]** 3 回合計 3042 回の seek で marker 不一致 **0 件**。
loader 修正と所有権修正を入れても seek 精度は変わらない。

**[事実]** 1 回目の max 476.4ms は外れ値で、2・3 回目は 275.7 / 261.2ms。
初回実行時のファイルキャッシュの影響と考えられる（**[推測]**）。
中央値を代表値とする。

**[exit]** M4 合格 / M5 不合格。

**M5 の max 記録の訂正:** 中央値 275.7ms を「max 基準合格」と扱わない。
**3 回を通して観測した max は 476.4ms であり、基準 400ms を超えている。**
1 回目だけが 476.4ms で 2・3 回目は 275.7 / 261.2ms だが、
外れ値を落として合格とするなら、**warm-up 条件を明示的に定義してから
再測定する必要がある**。現時点ではその定義が無いので、
**p95 と max の両方で不合格**とする。

### scrub baseline（**判定式が壊れていた時期**・参考）

**[重要] 以下の値は使ってはいけない。**
当時の判定は `result.generation < lastAcceptedGeneration` であり、
consumer が generation 昇順に処理する以上この条件は成立しない。
つまり**棄却が一度も働かず、古い結果もすべて表示していた。**
したがって `accepted` と `updates_per_sec` は、
どの契約の値でもない（契約が実装されていなかった）。

`stale_rejected` が全条件 0 だったのは棄却が働いていた証拠ではなく、
判定式が成立しなかったためである。

削除せず参考として残す。現在の契約での値は
「[scrub と request coalescing — M6](#scrub-と-request-coalescing--m6)」を参照。

1000 要求 × 4 パターン × 投入間隔 2 種。native 素材のみ、proxy なし。

| pattern | interval | submitted | decoded | superseded | accepted | stale | updates/sec | p50 | p95 | final一致 | marker不一致 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| linear | 8000µs | 1000 | 488 | 512 | 488 | 0 | **30.57** | 31.3 | 120.3 | ✓ | 0 |
| random | 8000µs | 1000 | 188 | 812 | 188 | 0 | **11.86** | 102.3 | 139.1 | ✓ | 0 |
| fine | 8000µs | 1000 | 301 | 699 | 301 | 0 | **18.96** | 29.9 | 126.6 | ✓ | 0 |
| jump | 8000µs | 1000 | 976 | 24 | 976 | 0 | **61.55** | 13.6 | 14.4 | ✓ | 0 |
| linear | 16000µs | 1000 | 727 | 273 | 727 | 0 | **25.83** | 24.1 | 125.6 | ✓ | 0 |
| random | 16000µs | 1000 | 337 | 663 | 337 | 0 | **11.97** | 105.8 | 143.3 | ✓ | 0 |
| fine | 16000µs | 1000 | 549 | 451 | 549 | 0 | **19.62** | 28.9 | 125.1 | ✓ | 0 |
| jump | 16000µs | 1000 | 984 | 16 | 984 | 0 | **35.48** | 14.8 | 17.7 | ✓ | 0 |

**測定は全条件で成立**している（decoded / accepted とも 20 件以上、
final_matches 真、marker 不一致 0、stale を accept していない）。

**[事実] M6 の判定は条件によって割れる。**

- **request→display p95 は全条件で基準内**（最大 143.3ms ≤ 200ms）
- **updates/sec は 8 条件中 6 条件で基準を満たす**（≥ 15）
- **`random` パターンだけが 11.9 前後で基準を下回る**（両 interval とも）

**[推測]** `jump`（61 ups）が速いのは、少数の位置を往復するため
直前の decode 結果が効きやすいからと考えられる。
`random` が遅いのは毎回 GOP 内の前方 decode が必要になるためで、
seek の p50 が 102〜106ms とほぼ 1 フレーム分の decode コストに一致する。

**ボトルネック候補**（今回は改善しない）:

1. 1 フレームあたりの合成コストそのもの（seek p50 ≈ 128〜161ms）
2. GOP 構造（素材は `-g 60`。最悪 59 フレーム分の前方 decode）
3. proxy 未使用（S7 の範囲）
4. consumer による先読みを使わず `mlt_service_get_frame` を直接呼んでいる
5. 毎フレーム 1920x1080 の RGBA を全面コピーしている

---

## 表示契約の変更（S6 correctness closure）

### 経緯 1: stale 判定が成立していなかった

```cpp
if (res.generation < lastAcceptedGeneration)  // ← 誤り
    staleRejected++;
else
    accept(res);
```

consumer は 1 件ずつ順に処理するため、結果は常に generation 昇順で返る。
`lastAcceptedGeneration` は直前に accept した値なので、この条件は**成立しない**。
判定式が死んでいた。

### 経緯 2: strict latest-only は使い物にならなかった

そこで判定基準を `latestSubmittedGeneration` に変え、
**`generation == latestSubmitted` のときだけ表示する**契約にした。
これは論理的には筋が通っているが、実測すると
**投入間隔より decode が遅い間、入力が止まるまで画が 1 枚も出ない。**

間隔 0 で 300 要求を投入した測定では decoded が 1 件しかなく、
updates/sec は約 3 だった。coalescing は正しく動いていたが、
**この契約のもとでは「スクラブ中に画が更新される」という
UI 上の要求そのものが満たせない。**

「latestSubmitted より古いものはすべて stale」という言い方は、
この契約に固有の言い方だったので廃止した。

### 現在の契約: monotonic display

保証するのは**「表示が巻き戻らないこと」**であって
**「常に最新であること」ではない。**

| 条件 | 判定 |
| --- | --- |
| `result.generation > latestSubmitted` | **InvalidFutureGeneration**（契約違反 / fail-closed） |
| in-flight と一致しない generation | **NotInFlight**（二重 complete を含む） |
| `decodeOk == false` | **DecodeFailed** |
| `result.generation <= lastDisplayed` | **RejectRegression**（表示すると巻き戻る） |
| `result.generation == latestSubmitted` | **DisplayLatest** |
| `lastDisplayed < result.generation < latestSubmitted` | **DisplayLagging** |

**DisplayLagging は捨てない。** 新しい要求が pending であっても、
現在表示中より新しい decode 結果なら表示する。これによりスクラブ中も
画が更新され続ける。追従の遅れは `generation_lag` として別に測る。

維持しているもの:

- pending は最新 1 件だけ保持する（latest-only coalescing）
- decode 中の処理は中断しない
- 入力停止後、最終要求は必ず `DisplayLatest` される

### generation の採番

generation は `ScrubCoalescer` が内部で単調採番する（`submit(frame)`）。
外部で採番する場合は `submitWithGeneration()` を使うが、
**`latestSubmitted` 以下の generation は受理せず、契約違反に数える。**
呼び出し側が採番を巻き戻すと `InvalidFutureGeneration` が
「未投入の generation」ではなく「一度は投入した generation」を
指しうるようになり、契約が意味を失うためである。

`InvalidFutureGeneration` の判定は in-flight 判定より**先**に行う。
未投入の generation を受け取ること自体が、
in-flight の不一致より重い異常だからである。この順序により
両方の分岐が単体テストから到達可能になっている。

### busy wait の除去

pending が無いときの `std::this_thread::yield()` ループを
`std::condition_variable` に置き換えた。consumer は
「pending が投入された」か「done になった」で起床する。
終了は producer が done を立て、pending も in-flight も無くなった時点。
最終要求が必ず処理される契約は、done を立てる前に
`cv.wait(lk, [&]{ return !hasPending() && !hasInFlight(); })` で保証している。

### counter invariant

scrub 終了時に以下を検査し、破れたら失敗にする。

- `submitted == superseded_pending + decoded`
- `decoded == display_latest + display_lagging + reject_regression + decode_failed`
- 表示した generation が単調増加（巻き戻りが 1 件でもあれば失敗）
- `final displayed generation == latest submitted generation`
- `displayed_total == 0` を成功扱いしない
- 契約違反カウンタが 0
- `display_updates_per_sec == displayed_total / elapsed_sec`（集計の自己整合）

marker 不一致と latency p50/p95 は **表示した結果だけ**を対象に集計する。

### テスト

| テスト | 対象 | 内容 |
| --- | --- | --- |
| `scrub_coalescer_unit` | 状態機械のみ | スレッドも実時間も使わない。全 decision 分岐の**到達回数を数え、0 回の分岐を「テスト済み」と呼ばない** |
| `scrub_coalescer_threaded` | 実 mutex / condition_variable | sleep を使わない。cv ハンドシェイクで take と complete の順序を固定し、`DisplayLagging` が起きる interleaving を確実に作る |

`RejectRegression` は防御的分岐であり、
`submitWithGeneration` が逆行を拒否する現在の設計では通常経路から到達しない。
**到達しないことを到達回数 0 として記録している**（「テスト済み」とは書かない）。

---

## seek 精度 — M4

**[事実] 214 回中 214 回一致。不一致 0 件。**

要求フレーム集合:

- 固定点: 0、1、最終フレーム(299)、最終の 1 つ前(298)、中央(150)
- GOP 付近: 59、60、61、119、120、121、179、180、181（素材は `-g 60` で生成）
- 固定 seed(20260804) のランダム 200 点

判定は素材に焼き込んだフレーム固有マーカーで行う
（`docs/research/test-media-format.md`）。**再試行はしていない。**
retry で隠すと M4 の判定が意味を失うため、初回結果だけを記録している。

**[exit] M4: 合格。**

## seek レイテンシ — M5

**[事実]** 単位はミリ秒。

| 区分 | count | p50 | p95 | max | mean | stddev |
| --- | --- | --- | --- | --- | --- | --- |
| 全体 | 214 | 128.97 | **232.26** | 307.04 | 126.93 | 70.42 |
| cold | 1 | 235.84 | 235.84 | 235.84 | 235.84 | 0 |
| warm | 213 | 126.43 | 230.68 | 307.04 | 126.42 | 70.19 |
| 前方 seek | 101 | 164.63 | 244.46 | 307.04 | 136.99 | 77.42 |
| 後方 seek | 111 | 111.29 | 213.46 | 260.27 | 117.87 | 60.82 |

**[exit] M5: 不合格。** 基準は p95 ≤ 150ms、max ≤ 400ms。
max は基準内だが **p95 が 232ms で基準の 1.5 倍**である。

### 原因の分類

**[推測]** 主因は 1 フレームあたりの合成コストである。以下が毎回発生する。

- 1080p の avformat producer 2 本の decode（H.264 と HEVC）
- 音声 producer 2 本の decode
- 文字レイヤの描画
- transition による合成
- 1920x1080 の RGBA へのフォーマット変換とコピー

分類:

| 候補 | 該当するか |
| --- | --- |
| MLT 自体の限界 | **不明**。キャッシュと proxy を使っていない状態での値であり、限界とは言えない |
| テスト素材 / GOP 構造 | 一部該当。`-g 60` なので最悪 59 フレーム分の前方 decode が要る |
| timeline 構築方法 | 該当の可能性。producer を clip ごとに開いており共有していない |
| キャッシュ / purge 方法 | **該当の可能性が高い**。現在は毎回 `mlt_producer_seek` + `mlt_service_get_frame` のみで、MLT のキャッシュ設定を一切調整していない |
| mvm_bench 実装 | 該当の可能性。毎フレーム RGBA 全面コピーをしている |
| ホスト負荷 | 小。計測中に他の重い処理は動かしていない |

**[未検証]** 以下は今回実施できていない。M5 の再判定にはこれらが必要である。

- proxy（S7 の範囲だが、scrub 性能に直結する）
- `mlt_consumer` を用いた先読み。現在は consumer を使わず `mlt_service_get_frame` を直接呼んでいる
- MLT のキャッシュ設定（`noimagecache`、`mlt_service` の cache size）
- 前方 seek が後方より遅い理由の切り分け

**回避策の見積り**: 上記の切り分けに 1〜2 日。
proxy 込みで再測定すれば基準を満たす可能性は十分あるが、
**現時点の数値では M5 は不合格である。**

## scrub と request coalescing — M6

**monotonic display 契約での再測定。** 旧契約の値は上に理由付きで残してある。

計測: `mvm_bench scrub-bench`、実行: `scripts/scrub-matrix.ps1`
**8 条件（pattern 2 種 × 投入間隔 4 種）× 3 回。表の値は 3 回の中央値。**
run ごとに別プロセスで実行する。300 要求、native 素材、proxy なし。

表の値はすべて `scripts/scrub-matrix.ps1` が生 JSON から再計算している。
手で転記していない。各 run で counter invariant と
`display_updates_per_sec == displayed_total / elapsed_sec` を検査し、
1 件でも破れたらスクリプトが停止する。

### 投入間隔について

**間隔 0 は実際のスクラブ操作ではない。** 人が触るスライダの更新は
60Hz 前後が上限なので、**16667µs（60Hz）が基準条件**である。
0 は上限側の負荷条件として残している。

### linear（連続スクラブ）

| 条件 | submitted | superseded_pending | decoded | display_latest | display_lagging | displayed_total | reject_regression | decode_failed | updates/sec | p50 | p95 | max | gen_lag p95 | catchup(ms) | elapsed(s) | marker不一致 | M6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| linear-0 | 300 | 299 | 1 | 1 | 0 | 1 | 0 | 0 | **2.82** | 353.4 | 353.4 | 353.4 | 0.0 | 353.4 | 0.35 | 0.0 | 不合格 |
| linear-8333 | 300 | 157 | 143 | 2 | 142 | 143 | 0 | 0 | **29.61** | 33.5 | 69.8 | 250.0 | 4.0 | 80.0 | 4.82 | 0.0 | 合格 |
| linear-16667 | 300 | 104 | 196 | 66 | 126 | 196 | 0 | 0 | **21.66** | 57.1 | 88.3 | 250.6 | 2.0 | 97.5 | 9.13 | 0.0 | 合格 |
| linear-33333 | 300 | 48 | 252 | 62 | 194 | 252 | 0 | 0 | **17.33** | 70.3 | 102.4 | 259.0 | 2.0 | 70.5 | 14.59 | 0.0 | 合格 |

### random（無作為な飛び）

| 条件 | submitted | superseded_pending | decoded | display_latest | display_lagging | displayed_total | reject_regression | decode_failed | updates/sec | p50 | p95 | max | gen_lag p95 | catchup(ms) | elapsed(s) | marker不一致 | M6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| random-0 | 300 | 299 | 1 | 1 | 0 | 1 | 0 | 0 | **2.95** | 338.6 | 338.6 | 338.6 | 0.0 | 338.6 | 0.34 | 0.0 | 不合格 |
| random-8333 | 300 | 257 | 43 | 1 | 42 | 43 | 0 | 0 | **8.67** | 116.0 | 239.5 | 338.3 | 14.0 | 245.0 | 4.96 | 0.0 | 不合格 |
| random-16667 | 300 | 233 | 67 | 1 | 66 | 67 | 0 | 0 | **7.47** | 140.3 | 255.8 | 344.7 | 8.0 | 193.8 | 9.05 | 0.0 | 不合格 |
| random-33333 | 300 | 197 | 103 | 3 | 100 | 103 | 0 | 0 | **6.98** | 155.5 | 275.8 | 357.9 | 5.0 | 172.2 | 14.28 | 0.0 | 不合格 |

### 何が言えるか

**[事実] 契約の変更で linear の updates/sec が約 3 → 17〜30 になった。**
同じ decode 速度のまま、`DisplayLagging` を捨てずに表示するようにしただけである。
旧契約の約 3 updates/sec は **decode 速度ではなく表示契約が作っていた数字**だった。

**[事実] linear は投入間隔 8333 / 16667 / 33333µs のすべてで M6 基準を満たす。**
基準は ≥ 15 updates/sec かつ request→display p95 ≤ 200ms。

**[事実] random はどの間隔でも基準を満たさない。**
7.0〜8.7 updates/sec、p95 は 239〜276ms。
`generation_lag` p95 が linear の 2〜4 に対し random は 5〜14 で、
**表示が要求から大きく遅れている。**

**[事実] 間隔 0 は両パターンとも decoded が 1 件しかない。**
300 件を一気に投入すると、consumer が 1 件目を取り出す前に
299 件が supersede される。coalescing としては正しい挙動だが、
**この条件は scrub の性能を測っていない。**

**[事実] marker 不一致は全条件・全 run で 0 件。**
表示したフレームは常に要求したフレームである。

**[事実] 表示 generation の巻き戻りは全条件・全 run で 0 件。**
`reject_regression` も 0 件だった（in-flight が 1 件しかない現在の構造では、
古い結果が後から返ることが起こらないため）。

**[事実] final catchup（入力停止から最終表示まで）は
linear で 70〜98ms、random で 172〜245ms。**
最終要求は全条件・全 run で表示されている。

### 判定

**[exit] M6: 条件付き。**

- **linear（連続スクラブ、8333〜33333µs）: 合格**（3 / 8 条件）
- **random（無作為な飛び）: 不合格**（updates/sec が基準の約半分）
- **間隔 0: 測定条件として不適**（decoded 1 件）

実際のスクラブ操作は linear に近い。しかし
**マーカー間のジャンプやクリップ境界への移動は random に近く、
その場合に基準を満たさない。**
条件を選べば合格する、という書き方で M6 を合格にはしない。

**[推測]** random が遅い原因は M5 と同じで、
毎回 GOP 内の前方 decode が必要になることである。
random の displayed latency p50（116〜156ms）は
seek の p50（129ms）とほぼ一致する。

**[未検証]** proxy を入れた場合の再測定。これは S7 の範囲である。
**M6 の最終判定は S7 の proxy 導入後に行う。**

## MLT のキャッシュと purge

**[未検証]** 今回の要求項目のうち、以下の比較実験は**実施できていない**。

- seek のみ
- seek + consumer purge
- timeline 再構築
- producer 再オープン

`MvmSeekMode` の enum と設定 API（`mvm_mlt_compose_set_seek_mode`）は
用意したが、`MVM_SEEK_WITH_PURGE` の実装は入っていない。
`mlt_consumer_purge` は consumer を使う設計にしないと呼べないため、
consumer ベースの取得経路を作るところから必要になる。

これは M5 / M6 の原因究明に直結するため、次のバッチの最優先項目とする。
