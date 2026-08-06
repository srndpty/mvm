# Phase 1 / P1 の所見

記述は Phase 0 と同じ規則で分類する。混ぜない。

| 印 | 意味 |
| --- | --- |
| `[事実]` | 実際に実行して観測した。再現手順を併記する |
| `[推測]` | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない |
| `[回避策]` | 現在の対処。恒久策とは限らない |
| `[exit]` | exit criteria への影響 |

**数値の出典は `build/ucrt64-release/p1-matrix/summary.json` である。**
このファイルの数値は `scripts/p1-matrix.ps1` が生 JSON から再計算したものであり、
本文へは要点だけを引く。判定は summary.json だけで行う。

再現:

```powershell
pwsh scripts/build.ps1
pwsh scripts/p1-matrix.ps1
```

---

## 1. Qt と FFmpeg の D3D11 device 共有

**[事実] Qt Quick の `ID3D11Device` をそのまま FFmpeg の decode device にできる。**

`QRhi::nativeHandles()` を render thread 上で `QRhiD3D11NativeHandles` へ落とし、
`dev` / `context` を `AVD3D11VADeviceContext::device` / `device_context` に渡した。
9 run すべてで `same_device == true`（device ポインタが完全一致）。

観測した device ポインタは Qt 側と FFmpeg 側で同一の値であり、
FFmpeg 側の値は **decode 結果の texture から `GetDevice` で遡って**取っている。
設定値の照合ではない。

**[事実] `ID3D10Multithread::SetMultithreadProtected(TRUE)` は必須である。**

decode thread と Qt render thread が同じ immediate context を共有する。
加えて `AVD3D11VADeviceContext::lock` / `unlock` に自前の
`std::recursive_mutex` を渡し、renderer 側も同じ mutex を取る。
二重に直列化している。片方だけで足りるかは **[未検証]**。
外して壊れることを確かめていないので、「必要だから入れた」ではなく
「安全側に倒して両方入れた」が正確である。

**[事実] `AVD3D11VADeviceContext::BindFlags` に
`D3D11_BIND_SHADER_RESOURCE` を入れないと表示できない。**

入れないと decode 出力 texture から shader resource view を作れず、
CPU へ落とす以外の手段が無くなる。
`mvm_test_gpu_decode no-shader-bind` がこの検査が効いていることを確かめている
（BindFlags 無しの texture を渡すと変換パスが失敗し、CPU readback へ退避しない）。

## 2. zero-copy の範囲

**[事実] decode 出力の画素は CPU へ 1 度も降りない。**

9 run すべてで `cpu_full_frame_readback_count == 0`。

経路は次のとおり。

```
NV12 texture array (FFmpeg decode pool, BIND_DECODER|BIND_SHADER_RESOURCE)
  -> Texture2DArray SRV (Y=R8_UNORM / UV=R8G8_UNORM, FirstArraySlice=index)
  -> pixel shader で YUV->RGB
  -> QQuickRhiItem の color texture (RGBA8) へ直接描画
```

**[事実] これは「zero-copy」だが「pass 0」ではない。**
NV12 のまま Qt Quick の scene graph に載せる手段が無いので、
**GPU 上で 1 pass の変換を必ず通る**。60 秒あたり約 4,700〜4,900 回の
`gpu_copy_count` はこの変換 pass の回数である（表示 3,600 + marker 7 + warm-up 分）。

CPU 転送は 0、GPU pass は 1。この 2 つを同じ言葉で報告しない。

**[事実] marker 検証だけは 1216x64 の帯を CPU へ読む。**
1080p 全画素の 3.7%、4K の 0.9%。`marker_band_readback_count` として
full-frame とは別に数えており、判定には使わない。
計測区間中は marker 検証を止めているので、fps の測定には混ざらない。

## 3. seek の着地（P1 で最も高くついた所見）

**[事実] `AVSEEK_FLAG_BACKWARD` は「目標以前のキーフレームへ飛ぶ」ことを保証しない。**

1080p60 HEVC（benchmark, 60 秒 / 3600 frame）で frame 299 を要求すると
**300 に着地した**。1000 点のランダム seek のうち **65 点 (6.5%)** が
同じ形で 1 frame 行き過ぎた。marker 代表点では 299 と 1799 が外れた。

**同じ time_base (1/15360)・同じ fps (60/1) の H.264 では 1 件も起きない。**
素材の生成条件も尺も同じで、違うのは codec と GOP / B frame 構造だけである。

**[推測]** mp4 の index は DTS で並んでおり、B frame があると PTS != DTS になる。
backward seek が選んだキーフレームの **表示時刻**が目標より後になりうる。
ソースを読んで確かめてはいない。

**[回避策] 着地を完全一致のみ成功とし、行き過ぎたら手前へ戻して decode し直す。**

戻し幅は 1 秒相当から 4 倍ずつ広げ、先頭まで戻っても届かなければ失敗として報告する。
戻した回数は `seek_backoff_count` として JSON に出す（隠さない）。
HEVC では 1000 点あたり 67 回この経路を通っている。

**[exit] 当初の実装は `f.frameNumber >= target` を成功としていた。**
この条件では 1 frame ずれが成功として飲み込まれ、marker 検査も通ってしまう。
**編集点が 1 frame ずれるという、最も気づきにくい形の不具合になる。**
`gpu_seek_exact_landing_hevc` / `gpu_marker_exact_landing_hevc` を
回帰テストとして CTest に入れた（60 秒素材があるときのみ登録。
5 秒の smoke 素材では再現しない）。

## 4. 表示レートの上限

**[事実] `effective_fps` は swapchain の present に律速される。**

判定ホストのリフレッシュレートは 59.95 Hz で、
9 run すべてで `present_rate_hz` も `effective_fps` も 59.87〜59.96 に収まった。

したがって `effective_fps >= 55` は
**「vsync のほぼ毎回に新しいフレームを間に合わせた」**の意味であり、
**この経路の最大スループットではない。**

**[未検証] 余力（headroom）は測っていない。** vsync を外した最大 fps、
および複数動画を同時に decode したときの限界は P2 以降の課題である。
4K60 H.264 も 59.87 fps 出ているが、これも上限に張り付いているだけで、
「4K に余裕がある」と読んではいけない。

## 5. 4K60 H.264（診断のみ・判定に使わない）

**[事実] 表示は 1080p と同じく上限に張り付く。marker も 21/21 一致。**

**[事実] seek だけが明確に遅い。** seek p95 は 1080p の 3〜4 倍。
P1 の判定対象ではないので閾値判定はしていないが、
**4K を原寸で編集する構成は seek 応答の面で成立しない**ことを示している。
Phase 0 の結論（4K は proxy 前提）と矛盾しない。

## 6. 色空間

**[事実] 同じ内容の素材でも、encoder によって metadata が違う。**

`v1080p60_h264` は `bt709`、`v1080p60_hevc` は `bt601` と申告する。
どちらも 1920x1080 であり、内容は同じスクリプトで生成している。

P1 は metadata をそのまま信じ、未指定のときだけ解像度から推定して
`color_space_inferred` を立てる。**推定を黙って確定値にしない。**

**[未検証] 表示色が正しいかは目視でも測定でも確かめていない。**
marker は白 235 / 黒 16 の高コントラストなので、
係数が多少ずれても読めてしまう。色の正しさは P1 の判定対象外である。

## 7. 10bit / P010

**[事実] 4K HEVC 10bit も同じ経路で表示でき、marker も読める。**
`gpu_marker_4k_hevc10_diagnostic` が確認している。
SRV format を R16_UNORM / R16G16_UNORM に切り替え、
10bit を 16bit の上位へ詰める分（65535/65472）を shader で補正している。

**[未検証] 10bit の表示品質**（バンディング、丸め）は評価していない。
判定対象は 8bit である。

## 8. 測っていないこと

「動くはず」を「動く」と書かないために、明示しておく。

- **NVIDIA RTX 4090 の 1 台でしか測っていない。** Intel / AMD の内蔵 GPU、
  複数 GPU 環境、ノート PC の切り替え可能 GPU は範囲外。
  **「Windows で動く」と一般化しない**
- device lost からの復帰。**検出しかしていない**（9 run とも発生 0）
- 長時間再生（数十分〜数時間）でのリーク。60 秒 x 3 run しか測っていない
- 音声、A/V 同期、複数トラック、export
- Qt の patch release をまたいだときの QRhi 互換
- vsync を外した最大スループット（§4）
- 表示色の正しさ（§6）
