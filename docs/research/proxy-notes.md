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

## 4K original / proxy の preview 比較 (M8)

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

### 正式な proxy 候補

**GOP1 (all-intra) を正式候補とする。**

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

## 未検証

- **[未検証]** proxy と original の SSIM / PSNR (画質の診断値)
- **[未検証]** 4K HEVC 10bit からの proxy 生成
- **[未検証]** proxy 生成中の preview 応答 (バックグラウンド生成)
- **[未検証]** proxy と original を切り替えたときの視覚的整合 (目視)
