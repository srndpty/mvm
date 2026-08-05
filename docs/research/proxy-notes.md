# proxy の実測メモ (S7)

対象: 4K60 H.264 (3840x2160, 60/1 fps, 3600 frames, 60.0s)
生成: `scripts/proxy-matrix.ps1`
検証: `mvm_bench proxy-mapping`
FFmpeg は `C:\msys64\ucrt64\bin` のものだけを使う。

---

## 設計: proxy 情報を MLT に持たせない

**proxy 情報を MLT XML にも MLT property にも持たせない。**
どのファイルを開くかは **MLT graph を構築する前**に決める。

解決は `tests/harness/proxy_resolver.h` の `ProxyResolver` が行う。
MLT にも Qt にも依存しない純粋なロジックで、単体テストできる。

```
source id --+--> preview 用 path (proxy が有効なら proxy)
            +--> final 用 path   (必ず original)
```

| 状況 | preview | final |
| --- | --- | --- |
| proxy あり・有効 | **proxy** | **original** |
| proxy 未生成 | original | original |
| proxy 無効 | original | original |
| 未登録の id | **解決しない (fail-closed)** | **解決しない** |

**final が proxy を返したら preview/final 一致 (M11) が構造的に壊れる。**
単体テスト (`proxy_resolver_unit`) はそこを最も厚く検査している。
「見つからなければ元のパスをそのまま使う」という黙った fallback は入れない。
入れると proxy が効いていないことに気づけないまま性能を測ることになる。

Phase 0 の検証用であり、**製品用 Project JSON へ広げない。**

---

## 生成した候補

| 候補 | encoder | 要求 GOP | 正式候補 | 解像度 | fps | frames | duration | SAR | pix_fmt | 音声 | keyframe 間隔(実測) | サイズ | 生成時間 | realtime 比 | M8 速度 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gop12 | h264_nvenc | 12 | はい | 960x540 | 60/1 | 3600 | 60.0s | 1:1 | yuv420p | aac 48000Hz 2ch | 12.0..12.0 | 25.7 MB | 7.16s | **8.38x** | 合格 |
| gop1 | libx264 | 1 | はい | 960x540 | 60/1 | 3600 | 60.0s | 1:1 | yuv420p | aac 48000Hz 2ch | 1.0..1.0 | 35.2 MB | 7.57s | **7.92x** | 合格 |
| x264 | libx264 | 12 | いいえ (対照) | 960x540 | 60/1 | 3600 | 60.0s | 1:1 | yuv420p | aac 48000Hz 2ch | 12.0..12.0 | 7.9 MB | 8.01s | **7.49x** | 合格 |

実際に渡した encoder option:

- `gop12`: `-c:v h264_nvenc -preset p4 -rc vbr -cq 25 -b:v 0 -g 12 -bf 0`
- `gop1`: `-c:v libx264 -preset veryfast -crf 23 -g 1 -bf 0`
- `x264`: `-c:v libx264 -preset veryfast -crf 23 -g 12 -bf 0`

### [事実] h264_nvenc は GOP 長 1 を受け付けない

```
InitializeEncoder failed: invalid param (8):
Gop Length should be greater than number of B frames + 1
```

`-bf 0` にしても条件は `1 > 0 + 1` で偽になるため通らない。`-g 2` は通る。
**したがって all-intra proxy は NVENC では作れず、libx264 で作った。**

**この結果 gop1 と gop12 は encoder が違う。**
両者の差を「GOP の効果」と読んではいけない。
切り分けのために x264-gop12 (同じ encoder・同じ GOP) を対照として残してある。

- `gop1` vs `x264` → **GOP の効果** (encoder は同じ libx264)
- `x264` vs `gop12` → **encoder の効果** (GOP は同じ 12)

サイズはこの切り分けで説明できる。
同じ GOP 12 で NVENC 25.7MB に対し libx264 7.9MB であり、
**差の大半は GOP ではなく encoder に由来する。**

### [事実] 音声を再エンコードすると尺が 1 frame 伸びた

最初は `-c:a aac -b:a 128k` で作っていた。その結果:

| | 映像ストリーム | 音声ストリーム | container | MLT が見る frame 数 |
| --- | --- | --- | --- | --- |
| original | 3600 frame / 60.000s | 60.000s | 60.000s | 3600 |
| proxy (aac 再エンコード) | 3600 frame / 60.000s | **60.010s** | **60.010s** | **3601** |

**ffprobe の `nb_frames` は両方 3600 なので、映像だけ見ていると気づかない。**
MLT は container の尺を見るため、proxy だけ 1 frame 長くなる。

proxy と original で尺が 1 frame ずれると、
**proxy を切り替えるだけでタイムラインの長さが変わる。**

対処は `-c:a copy`。音声を触らなければ尺は完全に一致する。
生成の検証にも「duration の差が 1 frame **以上**なら破棄」を入れて、
同じことが黙って再発しないようにした。

### 生成手順の安全性

**[事実]** 生成は必ず `.mvmtmp` へ書き、ffprobe による検証
(解像度・fps・frame 数・duration・音声 sample rate) に成功してから
正規名へ `Move-Item -Force` する。
ffmpeg が失敗した場合も検証に失敗した場合も一時ファイルを削除する。
**失敗した部分ファイルを成果物として残さない。**

`x264` 候補は NVENC 不在時の診断用であり、**正式な proxy 候補にしない。**

### keyframe 間隔は実測する

**[事実]** `-g` に指定した値がそのまま出力に効いているとは限らないので、
`ffprobe -show_entries frame=key_frame` で先頭 600 frame の
keyframe 位置を実測し、最小・最大間隔を記録している。

---

## frame mapping の検証

`mvm_bench proxy-mapping` が以下を検査する。

| 項目 | 判定 |
| --- | --- |
| fps rational | **完全一致**を要求する (60/1 と 60000/1001 を「だいたい同じ」で通さない) |
| frame count | 完全一致 |
| duration | 差が 1 frame 未満 |
| 代表フレーム | 0, 1, 2, 137, 299, 600, 1799, 末尾, 末尾-1 |
| GOP 境界 | 12 の倍数の前後 (`g-1`, `g`, `g+1`) |
| ランダム | seed 20260804 固定で 200 点 |

判定の gate は **marker 値と位置の一致だけ**である。
**画素の完全一致は要求しない。** 解像度もコーデックも違うので当然ずれる。
画質は SSIM などの診断値として分離する (S7 では未測定)。

| 候補 | fps 一致 | frame 数 | duration 差 | 検査 frame 数 | original marker 不一致 | proxy marker 不一致 | 相互不一致 | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gop12 | 完全一致 | 3600 / 3600 | 0s | 232 | 0 | 0 | **0** | 合格 |
| gop1 | 完全一致 | 3600 / 3600 | 0s | 232 | 0 | 0 | **0** | 合格 |
| x264 | 完全一致 | 3600 / 3600 | 0s | 232 | 0 | 0 | **0** | 合格 |

**[事実] 3 候補すべて marker 不一致 0 件。** 検査した 232 frame の内訳は
代表点 (0, 1, 2, 137, 299, 600, 1799, 3598, 3599)、
GOP 境界 (12 の倍数の前後)、seed 20260804 のランダム 200 点である。

### マーカーは縮小に追従させる必要がある

**[事実]** マーカーは解像度に関係なく **64px セルの固定ピクセル**で焼かれている。
3840x2160 を 960x540 へ縮小すると、セル幅は 16px になる。

64px 決め打ちで読むと `w < 64*19 = 1216` で必ず読めず、
**「proxy のマーカーが壊れている」と誤って結論する**ことになる。
`readMarkerScaled` が元の幅からセル幅を割り出して読む。

---

## 4K original / V1-only proxy の preview 比較 (**旧 M8 / partial proxy diagnostic**)

**[注意] この節の測定は V1 だけを proxy 化したものである。**
V2 (1080p HEVC) は original のまま残っており、正式な M8 評価ではない。
正式評価は下の「S7.1」節を参照。

計測は `preview-bench` (null consumer, `real_time=-1`, warm-up 5s, wall 60s, 3 回中央値)。
`real_time=-1` を使うのは 1080p の matrix で最も速かった構成だからである。

| 経路 | effective_fps | drop | 起動待ち(ms) | frame 間隔 p50(ms) | p95 | CPU util | WS ピーク(MB) | Priv ピーク(MB) | proxy サイズ | 生成 realtime 比 | M8 fps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4K original | **10.0** | 0 | 191.5 | 121.46 | 142.79 | 2.42 | 962.6 | 1045.9 | — | — | 不合格 |
| proxy GOP12 (NVENC) | **14.88** | 0 | 110.2 | 66.14 | 81.6 | 2.44 | 356.4 | 422.3 | 25.7 MB | 8.38x | 不合格 |
| proxy GOP1 (x264 all-intra) | **15.38** | 0 | 90.5 | 64.2 | 79.72 | 2.52 | 347.8 | 414.3 | 35.2 MB | 7.92x | 不合格 |

### 何が言えるか

**[事実] proxy は効いている。** 4K original に対し

- frame 間隔 p50 が 121.5ms -> 64〜66ms とほぼ半分
- PrivateUsage ピークが 1046MB -> 414〜422MB と 2.5 分の 1
- 起動待ちが 191ms -> 91〜110ms

**[事実] それでも M8 の fps は満たさない。**
基準 50 fps に対し 14.88 (GOP12) / 15.38 (GOP1) である。

**[事実] proxy にしても 1080p native (19.85 fps) を超えない。**
V1 を 540p に落としても、タイムラインには 1080p の V2 (HEVC)・
音声 2 本・文字レイヤが残っており、
**律速はそれらを含む合成全体**である。V1 の解像度だけではない。

**[推測]** したがって proxy は「4K を扱えるようにする」効果はあるが、
**M7 / M8 の fps 基準を満たすには足りない。**
合成コスト自体を下げる (GPU 合成、レイヤ数削減、キャッシュ) か、
別の backend が必要である。

### proxy 候補の選定 (**S7.1 で撤回**)

**[撤回] 「GOP1 を正式候補とする」という S7 の記述を撤回する。**

根拠にした差は preview 14.88 対 15.38 fps、seek p95 460.5 対 452.4ms であり、
いずれも 3 回の中央値どうしの微差である。
しかも **GOP1 は libx264、GOP12 は NVENC で encoder が違う。**
encoder が違う候補間の微差を GOP 差と解釈してはいけない。

S7.1 の all-video proxy は **NVENC GOP12 で統一**して測っており、
そちらで M5 は合格している。GOP1 を選ぶ根拠は現時点で無い。

以下は撤回した比較表であり、記録として残す。

| 観点 | GOP12 (NVENC) | GOP1 (x264 all-intra) | 選択 |
| --- | --- | --- | --- |
| preview fps | 14.88 | **15.38** | GOP1 |
| frame 間隔 p50 | 66.14ms | **64.20ms** | GOP1 |
| 起動待ち | 110.2ms | **90.5ms** | GOP1 |
| Priv ピーク | 422.3MB | **414.3MB** | GOP1 |
| 生成 realtime 比 | **8.38x** | 7.92x | GOP12 |
| ファイルサイズ | **25.7MB** | 35.2MB | GOP12 |

**差はどれも小さい。** preview と seek で GOP1 がわずかに有利、
生成速度とサイズで GOP12 がわずかに有利である。
scrub / seek の結果 (seek-scrub-notes.md) も踏まえて GOP1 を採る。

**[注意] GOP1 は encoder が libx264 である** (NVENC が GOP 長 1 を
受け付けないため)。したがって GOP12 との差は GOP だけの差ではない。

---

# S7.1: all-video proxy による正式 M8 評価

## S7 の M8 は部分 proxy の評価だった

**[事実] S7 で M8 と呼んでいた測定は V1 だけを proxy 化したものだった。**
タイムラインには V2 (1080p HEVC) の original が残っており、
「proxy にした場合の上限」を測っていなかった。

S7.1 では preview で使う **video source を全て proxy 化**して測り直した。

| scenario | V1 | V2 | 位置づけ |
| --- | --- | --- | --- |
| `s7-4k-original.json` | 4K original | 1080p HEVC original | native 基準 |
| `s7-4k-proxy-gop12.json` | 540p proxy | **1080p HEVC original** | **partial proxy diagnostic** (旧 M8。正式評価に使わない) |
| `s7-4k-proxy-all-gop12.json` | 540p proxy | **540p proxy** | **正式 M8 評価** |

## required / optional source の契約

「全 source を proxy 必須」にはできない。WAV や音声専用 source が混ざるためである。
2 種類に分けた。

| 種別 | 指定 | 未解決のとき |
| --- | --- | --- |
| **required** | `--require-proxy-ids "id;id"` | **exit 4 で fail-closed** |
| optional | 指定しない | original のまま。**id と件数を必ず報告** |

required id ごとに以下を全て検査する。

- scenario にその id が実在する
- `--map` に登録されている
- preview では**全出現が**置換される (同じ素材が複数 clip にあっても)
- final では **1 件も** proxy にならない

旧 `--require-proxy` (「1 件以上置換されれば成功」) は契約として弱すぎる。
**V1 だけ proxy 化されて V2 が original のままでも通ってしまい、
実際に S7 の M8 はその状態で「proxy 評価」として報告されていた。**
diagnostic 専用へ降格し、判定には使わない。

### 置換の証拠 (all-video proxy scenario の生成時)

```
required_proxy_ids            : v4k60_h264.mp4, v1080p60_hevc.mp4
resolved_required_ids         : v4k60_h264.mp4, v1080p60_hevc.mp4
missing_required_ids          : (なし)
resolved_occurrences          : 2
unregistered_optional_sources : v1080p60_h264.mp4 (A1 音声), wav_48k.wav (A2)
```

## V2 proxy のメタデータ

| 項目 | 値 |
| --- | --- |
| encoder / GOP | h264_nvenc / 12 (実測 keyframe 間隔 12..12) |
| 解像度 | 960x540 |
| fps | 60/1 (元と完全一致) |
| frames | 3600 (元と一致) |
| duration | 60.0s (差 0) |
| SAR / pix_fmt | 1:1 / yuv420p |
| 音声 | aac 48000Hz 2ch (copy) |
| サイズ / 生成 | 34.4 MB / 7.4s (**realtime 比 8.11x**) |

frame mapping: fps 完全一致、frame 数一致、duration 差 0、
232 frame 検査で **marker 不一致 0**。

all-video scenario の合成でも frame 0 / 1 / 137 / 299 / 600 / 1799 / 3599 が
**すべて marker 一致**。

## 正式 M8: preview 比較

計測は `preview-bench` (null consumer, `real_time=-1`, warm-up 5s, wall 60s, 3 回中央値)。

| 経路 | effective_fps | drop | 起動待ち | frame 間隔 p50 | p95 | CPU | Priv ピーク | M8 fps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4K original (proxy なし) | **10.0** | 0 | 191.5ms | 121.46ms | 142.79ms | 2.42 | 1045.9MB | 不合格 |
| **V1-only partial proxy** (旧 M8) | **14.88** | 0 | 110.2ms | 66.14ms | 81.6ms | 2.44 | 422.3MB | 不合格 |
| **all-video proxy** (正式 M8) | **19.67** | 0 | 57.2ms | 50.09ms | 65.72ms | 2.64 | 289.2MB | 不合格 |

**[事実] 全 video を proxy 化すると 14.88 -> 19.67 fps へ改善する。**
V1 だけの proxy 化では V2 の 1080p HEVC デコードが残っており、
**旧 M8 は proxy の効果を過小評価していた。**

**[事実] それでも基準 50 fps には届かない。** 19.67 は 39% である。

**[事実] all-video proxy でも 1080p native (19.85 fps) をわずかに下回る。**
4K を 540p に落としても、**1080p native と同程度が上限**である。
解像度ではなく合成そのものが律速していることの直接的な証拠である。

**[exit] M8 preview: 不合格。**
生成速度 (realtime 比 8.11〜8.38x) と frame mapping (mismatch 0) は合格である。

## all-video proxy の M5

| 経路 | mismatch | p50 | **p95** | max 中央値 | **観測 max** | M5 |
| --- | --- | --- | --- | --- | --- | --- |
| all-video proxy | **0** | 57.7ms | **81.7ms** | 232.5ms | **285.6ms** | **合格** |

**[事実] M5 は合格した。** 基準 p95 <= 150ms / 観測 max <= 400ms に対し
p95 81.7ms、観測 max 285.6ms である。

V1-only proxy では p95 452ms で不合格だったので、
**V2 を proxy 化したことが seek の合否を分けた。**

300 点で基準から大幅に外れてはいないが、**M8 が不合格なので 1000 点の最終確認は行っていない。**

## ablation (原因の切り分け)

15 秒 preview、`real_time=-1`、3 回中央値。基準は B (all-video proxy)。
**フレームを作らない構成で速く見せることはしていない。**
全ケースが 1920x1080 RGBA を毎フレーム生成し、`rendered=0` が 1 件でもあれば採用しない。

| ケース | 内容 | fps | B との差 | frame 間隔 p50 | CPU | Priv ピーク | 製品で使えるか |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | V1 proxy のみ (V2 は 1080p HEVC original) | **16.39** | -5.14 | 60.6ms | 3.1 | 421.1MB | いいえ (V2 が original) |
| B | V1 + V2 proxy、全 5 トラック (基準) | **21.53** | +0 | 45.73ms | 3.47 | 289.3MB | はい |
| C | B から qtext を除去 | **33.65** | +12.12 | 28.79ms | 2.41 | 267.2MB | いいえ (文字は必須機能) |
| D | B から音声 2 トラックを除去 | **22.45** | +0.92 | 43.99ms | 3.57 | 267.6MB | いいえ (音声は必須) |
| E | B から V2 / affine を除去 | **35.84** | +14.31 | 27.09ms | 4.21 | 235.4MB | いいえ (PiP は必須) |
| F | B の qtext を事前描画 PNG へ置換 | **31.1** | +9.57 | 31.59ms | 2.45 | 269.6MB | はい (静的文字なら) |
| G | V2 proxy を 640x360 にした構成 | **24.47** | +2.94 | 39.72ms | 3.41 | 506.2MB | はい |

### 何が言えるか

**[事実] 単独の低コスト回避策で 50fps へ届く根拠は無い。**

- 製品で使える最良は **F (文字を事前描画 PNG 化) の 31.1 fps**。基準の 62% である
- 必須機能を削ったケースでも最良は **E (PiP 除去) の 35.84 fps**
- **PiP を丸ごと捨てても 50fps に届かない。**

コスト内訳 (B からの差):

| 除去したもの | 効果 | 製品で外せるか |
| --- | --- | --- |
| V2 / affine 合成 | +14.31 fps | 外せない (PiP は必須) |
| qtext の毎フレーム描画 | +12.12 fps | 事前描画なら外せる (F = +9.57) |
| 音声 2 トラック | +0.92 fps | 外せない。**そもそも効果が小さい** |
| V2 proxy を 640x360 へ | +2.94 fps | 外せるが効果が小さい |

**[推測]** 合成コストは V2 の affine 合成と qtext 描画に集中している。
両方を外して初めて 30〜36 fps であり、**残り 14〜20 fps 分の説明がついていない。**
1920x1080 RGBA の生成と転送そのものが下限を作っている可能性があるが、
**MLT 内部のどこかは特定していない。**

**[方針] 追加の最適化探索は打ち切る。**
private API の cache purge、`EmptyWorkingSet`、閾値緩和、
フレームを作らない構成による偽高速化はいずれも行わない。

---

## 未検証

- **[未検証]** proxy と original の SSIM / PSNR (画質の診断値)
- **[未検証]** 4K HEVC 10bit からの proxy 生成
- **[未検証]** proxy 生成中の preview 応答 (バックグラウンド生成)
- **[未検証]** proxy と original を切り替えたときの視覚的整合 (目視)
