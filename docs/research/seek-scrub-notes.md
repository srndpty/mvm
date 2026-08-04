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
