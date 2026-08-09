# Phase 2 — Two-Video D3D11 GPU Compositor Spike 計画

この文書は P2 実装前の判定契約である。P2 の実装中または計測後に閾値を緩めて
合格扱いにしない。正式判定の唯一の入口は `scripts/p2-matrix.ps1` とし、文書へ
手で転記した数値は判定根拠にしない。

## 1. 目的

2 本の動画を同時に D3D11VA decode し、両方の frame を VRAM 上に保ったまま、
同一 D3D11 device 上の shader で合成して Qt Quick の render target へ表示できるかを
判定する。CPU full-frame readback は一切使わず、P1 の single-video 経路を変更・退行
させない。

P2 は技術スパイクであり、製品コードではない。製品用タイムライン、Project Model、
音声、A/V sync、3 本以上の動画、arbitrary timeline、keyframe animation、transition、
text、export、proxy 生成、device lost recovery、undo/redo、一般 UI は対象外とする。

## 2. 正式構成

| 項目 | 固定値 |
| --- | --- |
| Source A | `tests/assets/benchmark/v1080p60_h264.mp4`、1920x1080、60 fps |
| Source B | `tests/assets/benchmark/v1080p60_hevc.mp4`、1920x1080、60 fps |
| 出力 | 1920x1080、60 fps |
| layer 0 | Source A を全面表示、opacity 1.0 |
| layer 1 | Source B を右下 960x540、opacity 0.75 |
| sampling | linear filtering |
| blend | straight alpha。`out = src * opacity + dst * (1 - opacity)` |

source 順、destination、source UV、opacity は固定値から開始する。layout stress だけで
右下/左上および opacity 0.75/0.5 を決定論的に切り替える。

## 3. thread と ownership

```text
GUI thread
  CompositorController
    | stop/join A, B（両方の join 後だけ teardown 要求）
    v
render thread                         decode thread A / decode thread B
  CompositorRhiRenderer               SourceDecodeWorker A / B
    SharedD3D11Device <-------------- 同一 device を借用
    GpuCompositor                      source-local decoder
    GpuCompletion                     source-local generation
    GpuRetirementQueue                source-local bounded buffer
       ^
       | aggregate lifetime payload
  CompositorCoordinator
    output frame N ごとに A=N と B=N を pair
```

`SourceRegistry` が `SourceId` の一意発行と register/unregister を所有する。
worker は constructor injection された `SourceId` だけを扱い、他 source の buffer、
generation、stop 状態に触れない。各 buffer は bounded とし、空きがなければ decode を
待たせる。無制限の先行 decode は許さない。

## 4. identity と epoch

| 識別子 | 発行者 | 進める時点 |
| --- | --- | --- |
| `SourceId` | `SourceRegistry` | source register |
| `ResourceEpoch` | 各 decoder | open / decode pool 再作成 |
| `SourceGeneration` | 各 source worker | source-local seek / flush |
| `CompositionEpoch` | compositor | layout、source 順、opacity、crop の snapshot 変更 |

seek だけでは `CompositionEpoch` を進めない。composition 採用時に epoch を値で固定し、
renderer が mutable global epoch を後付けしない。display ledger は output frame number、
composition epoch、全 layer の source id / source generation / resource epoch / frame number
を照合する。

## 5. frame pairing

`CompositorCoordinator` は source ごとの要求 generation と current composition snapshot を
保持する。output frame N の要求は、登録済み A と B の両方について次を満たす場合だけ
`ComposedFrame` になる。

- source id が要求集合と完全一致する
- source generation が source ごとの要求値と完全一致する
- A と B の frame number がともに N である
- composition epoch が採用時の current epoch と一致する

A=N/B=N-1、stale/future generation、unknown/missing source、old composition epoch は
fail-closed で拒否する。片方の古い frame を再利用しない。不足は bounded wait の後に
drop として `missing_source_frame_count` へ明示的に数える。mixed pair を成功数や displayed
数へ含めない。

## 6. GPU compositor と lifetime

1 composition は 1 render pass とし、render target の clear は先頭で 1 回だけ行う。
layer を z-order、次いで SourceId で安定 sort し、各 layer を NV12/P010 shader で直接
RGB 化して destination viewport へ描く。source UV crop、source ごとの color metadata、
point/linear sampler、straight-alpha blend state を layer ごとに適用する。2 layer 目以降は
clear しない。各実 texture の owner device を `GetDevice` で取得して shared device と照合し、
申告値だけの一致を成功扱いしない。

composition ごとに原則 1 submission serial を発行する。両 source の AVFrame lifetime、
decode texture、SRV holder、中間 resource を 1 個の aggregate payload にまとめ、その serial
が完了するまで `GpuRetirementQueue` が保持する。一方だけを先に解放しない。追跡不能 serial
では payload を解放せず fatal shutdown とする。

## 7. 正しさの検査

per-source marker は各 decode texture を合成前に表示と同じ色変換 shader で 1216x64 の
診断 target へ描き、その小領域だけを readback する。A requested == A marker、B requested ==
B marker、output == A == B を source 別に検査する。

合成結果は既知の color patch に対し、A-only、B-only、overlap、PiP 境界の内外、opacity
0.75 の小領域だけを readback する。期待 RGB は shader 実装を呼ばず、色規格の標準式と
straight-alpha 式から独立計算する。8-bit RGB の許容誤差は各 channel ±3 と固定する。
full-frame staging texture、`QImage`、`sws_scale`、`av_hwframe_transfer_data` は禁止する。

## 8. seek と layout stress

dual seek は要求前の display baseline を取り、A/B の source generation を別々に進め、同じ
target frame へ exact seek する。両方の target frame が揃った composition が baseline より後に
実 display された時点までを latency とする。decode-ready は完了ではない。正式 seek は seed
固定の 1000 回を 3 process で実施する。

layout stress は正式性能測定と別フェーズにする。右下、左上、opacity 0.75、opacity 0.5 の
各 snapshot 採用時だけ `CompositionEpoch` を進め、旧 epoch 表示、mixed epoch、epoch regression、
source generation の相互干渉がすべて 0 であることを検査する。

## 9. 計測値の定義

warm-up 終了後から measurement 終了までの QPC 実測時間を `elapsed_seconds` とする。

- `displayed_composition_count`: shader draw、GPU submission serial の追跡、display ledger 記録を
  完了した unique output composition 数。present call、decoded frame、paired frame は含めない。
- `effective_fps = displayed_composition_count / elapsed_seconds`
- `scheduled_output_count`: measurement 区間の 60 fps clock が要求した output frame 数。
- `dropped_output_count`: scheduled output のうち bounded deadline までに正しい A/B pair を display
  できなかった数。mixed/stale/missing を表示したことにして分母から除かない。
- `drop_rate = dropped_output_count / scheduled_output_count`。分母が 0 なら契約違反で不合格。

decoded frame 数、delivery 数、pair 形成数、present call 数を fps と呼ばない。

## 10. 正式 matrix

`scripts/p2-matrix.ps1` が raw JSON 保存、契約検査、raw からの `summary.json` 生成まで行う。

- warm-up 5 秒、measurement 60 秒、3 independent processes
- §2 の H.264 + HEVC、1920x1080、固定 PiP、fence backend
- seed 固定、1000 deterministic dual-source seeks × 3 run
- layout stress、forced event_query short soak、open/close soak は正式 fps phase と分離
- P1 の `mvm_preview_spike` と `scripts/p1-matrix.ps1` は変更しない

正式 matrix が不合格でも閾値を変更しない。切り分けは各 15 秒 × 3 run の A: A 単独、
B: dual decode + pair のみ、C: 固定 2 texture 合成、D: dual decode + full compositor に限定する。

## 11. P2 exit criteria（全項目 MUST）

- `effective_fps >= 55`
- `drop_rate <= 0.02`
- Source A marker mismatch `== 0`
- Source B marker mismatch `== 0`
- output probe mismatch `== 0`
- dual seek displayed p95 `<= 150 ms`
- 全 run の dual seek displayed observed max `<= 400 ms`
- seek failure `== 0`
- mixed source frame count `== 0`
- mixed generation count `== 0`
- stale composition epoch count `== 0`
- missing source frame count `== 0`
- CPU full-frame readback count `== 0`
- software frame reject count `== 0`
- device mismatch `== 0`
- untracked submission `== 0`
- completion poll failure `== 0`
- payloads released before completion `== 0`
- retirement timeout `== 0`
- lifecycle order violation `== 0`
- retirement depth after drain `== 0`
- teardown success `== true`
- crash `== 0`
- device lost `== 0`
- release/debug の通常 CTest が全通過
- P1 formal contract が退行しない

いずれか 1 項目でも未測定または不合格なら P2 は合格ではない。P3 へは進まない。
