# seek と scrub の実測メモ (S6)

対象: MLT 7.36.1 / MSYS2 UCRT64
計測対象: S5 の 5 トラックタイムライン（単一 producer ではない）
ビルド: **RelWithDebInfo**（debug の値は判定に使わない）
計測: `std::chrono::steady_clock`。PNG 出力や JSON 整形は latency に含めない

再現:

```powershell
mvm_bench seek-bench  bench/scenarios/s5-five-track.json --random 200 --csv seek.csv
mvm_bench scrub-bench bench/scenarios/s5-five-track.json --requests 300 --pattern linear
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

### scrub baseline（**stale 判定修正前**・参考）

**[重要] 以下の値は使ってはいけない。**
当時の判定は `result.generation < lastAcceptedGeneration` であり、
**decode 中に新しい要求が来た古い結果を accept していた。**
したがって `accepted` と `updates_per_sec` には、
表示すべきでない stale 結果が含まれている。

`stale_rejected` が全条件 0 だったのは棄却が働いていた証拠ではなく、
判定式が成立しなかったためである。

削除せず参考として残す。

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

## stale 判定の修正（S6 correctness closure）

### 修正前

```cpp
if (res.generation < lastAcceptedGeneration)  // ← 誤り
    staleRejected++;
else
    accept(res);
```

consumer は 1 件ずつ順に処理するため、結果は常に generation 昇順で返る。
`lastAcceptedGeneration` は直前に accept した値なので、この条件は**成立しない**。
結果として **decode 中に新しい要求が来た古い結果をすべて accept** していた。

### 修正後

判定の基準を `latestSubmittedGeneration` にした。

| 条件 | 判定 |
| --- | --- |
| `result.generation < latestSubmittedGeneration` | **RejectStale** |
| `result.generation == latestSubmittedGeneration` | **Accept** |
| `result.generation > latestSubmittedGeneration` | **InvalidFutureGeneration**（契約違反 / fail-closed） |
| in-flight と一致しない generation | **NotInFlight**（二重 complete を含む） |

判定ロジックは `tests/harness/scrub_coalescer.h` の `ScrubCoalescer` に
分離した。MLT にもスレッドにも実時間にも依存しない純粋な状態機械であり、
決定論的に単体テストできる。

`lastAcceptedGeneration` との比較は削除した。

### busy wait の除去

pending が無いときの `std::this_thread::yield()` ループを
`std::condition_variable` に置き換えた。consumer は
「pending が投入された」か「done になった」で起床する。
終了は producer が done を立て、pending も in-flight も無くなった時点。
最終要求が必ず処理される契約は、done を立てる前に
`cv.wait(lk, [&]{ return !hasPending() && !hasInFlight(); })` で保証している。

### counter invariant

scrub 終了時に以下を検査し、破れたら失敗にする。

- `submitted == superseded + decoded`
- `decoded == accepted + stale_rejected + decode_failed`
- accepted の generation が単調増加
- `final displayed generation == latest submitted generation`
- `accepted == 0` を成功扱いしない
- 契約違反カウンタが 0

marker 不一致と latency p50/p95 は **accepted 結果だけ**を対象に集計する。
stale 結果の decode 時間は `stale_decode_diagnostic` として別に残す。

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

実装したモデル:

- producer 側は連続的に要求を投入する
- consumer 側は、処理中でない未処理要求を **最新 1 件だけ** 保持する
- 新しい要求が来たら古い pending を supersede する
- decode 中の処理は中断しない
- 各要求に generation を付け、**現在の最新受理より古い結果は棄却する**

**[事実]** 300 要求（投入間隔 0）での結果:

| 指標 | linear | random |
| --- | --- | --- |
| submitted | 300 | 300 |
| decoded | 1 | 1 |
| superseded | 299 | 299 |
| accepted | 1 | 1 |
| stale rejected | 0 | 0 |
| marker mismatch | **0** | **0** |
| updates/sec | 2.92 | 3.22 |
| latency p50 / p95 / max (ms) | 342 / 342 / 342 | 309 / 309 / 309 |
| 最終要求と最終表示の一致 | **true** | **true** |

### 何が言えるか

**[事実] coalescing 自体は正しく動いている。**

- 最終要求が必ず表示される（`final_matches: true`）
- stale な結果を表示していない
- marker 不一致 0 件

**[事実] updates/sec は基準に遠く届かない。** 基準 15 に対し約 3。

**[事実] この計測条件では decoded が 1 件しかない。**
投入間隔 0 で 300 件を一気に投入したため、consumer が 1 件目を取り出す前に
299 件が supersede された。**coalescing としては正しい挙動だが、
scrub の実性能を測れていない。**

**[exit] M6: 不合格。** ただし不合格の理由は coalescing ではなく、
**1 フレームの取得に 130〜340ms かかること**である。
1 フレーム 300ms なら、理論上の上限が約 3 updates/sec であり、
15 updates/sec には 1 フレーム 67ms 以下が必要になる。

つまり **M6 は M5 と同じ原因で不合格**であり、
seek レイテンシを改善しない限り達成できない。

**[未検証]** 現実的な投入間隔（`--submit-interval-us` で
マウスドラッグ相当の 8000〜16000µs）での再計測。
今回は間隔 0 でしか測っていない。

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
