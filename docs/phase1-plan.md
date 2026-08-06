# mvm Phase 1 — GPU Preview Engine Spike 計画

> **範囲**: このドキュメントは Phase 1 の **P1（single-video D3D11 zero-copy vertical slice）** のみを規定する。
> P2 以降（二動画 GPU 合成）には着手しない。
>
> Phase 0（MLT スパイク）の結論は [phase0-decision.md](phase0-decision.md) と
> [adr/0001-mlt-adoption.md](adr/0001-mlt-adoption.md) にあり、**本フェーズでは一切変更しない**。
> MLT backend のコードを新実装へ流用しない。新しいコードは `src/media/gpu_preview/`、
> `src/app/preview/`、`apps/preview_spike/` に閉じる。

---

## 1. 目的

**FFmpeg で decode した video frame を、CPU の RGBA buffer へ戻さずに
D3D11 / QRhi 経由で Qt Quick 上へ表示できることを実証する。**

Phase 0 で MLT を不採用としたのは、1080p60 の連続 preview が基準 50 fps に対して
最良 19.85 fps しか出なかったためである（[phase0-decision.md](phase0-decision.md)）。
MLT 経路は「フレームを CPU 上の RGBA へ具現化してから配る」構造であり、
mvm が必要とする preview 帯域を満たせなかった。

P1 が確かめるのは、その逆側の経路が成立するかである。すなわち
**decode 結果の texture を VRAM に置いたまま表示まで運べるか**。
成立しなければ、内製 preview engine の前提そのものが崩れる。

P1 は **可否の判定**であって製品実装ではない。判定に必要のないものを作らない。

## 2. 対象環境

| 項目 | 値 |
| --- | --- |
| OS | Windows 11 (26200) |
| toolchain | MSYS2 UCRT64（Phase 0 と同一。ABI を混在させない） |
| Qt | 6.11.1 (UCRT64) |
| FFmpeg | libavcodec 62 / libavformat 62 / libavutil 60 (UCRT64) |
| graphics API | Direct3D 11 |
| GPU | NVIDIA GeForce RTX 4090（判定用ホスト） |

Phase 0 と同じく、`C:\Users\lambe\sdk\Qt\6.8.3`（MSVC ビルド）は参照しない。
`cmake/mvm_toolchain_guard.cmake` の検査をそのまま継承する。

## 3. 経路の設計

```
  avformat_open_input
        |
  avcodec (hwaccel = d3d11va)          ← AV_HWDEVICE_TYPE_D3D11VA
        |   AVFrame.format = AV_PIX_FMT_D3D11
        |   data[0] = ID3D11Texture2D*   data[1] = array index
        v
  DecodedGpuFrame（境界。FFmpeg の型を外へ出さない）
        |
        v
  PreviewRhiRenderer（Qt render thread）
        |   NV12/P010 → RGB 変換を pixel shader で行い
        |   QQuickRhiItem の color texture へ直接描く
        v
  Qt Quick scene graph → swapchain
```

CPU が触るのは **frame metadata（pts / 解像度 / 色情報 / texture pointer）だけ**である。
画素は VRAM から出ない。

### decoder

- `avformat_open_input` → `avformat_find_stream_info`
- `av_find_best_stream(AVMEDIA_TYPE_VIDEO)` で stream を選ぶ
- `avcodec_get_hw_config` を走査し、`AV_HWDEVICE_TYPE_D3D11VA` かつ
  `AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX` を持つ config を探す
- `get_format` callback で **`AV_PIX_FMT_D3D11` 以外を返さない**
- `avcodec_send_packet` / `avcodec_receive_frame`
- PTS → timeline frame number 変換（§5）
- seek 後は必ず `avcodec_flush_buffers`
- EOF / EAGAIN / decode error を別の状態として区別する
- Windows の UTF-8 path に対応する（FFmpeg は UTF-8 を受けるので、
  Phase 0 の `src/util/mvm_win_utf8.c` と同じ扱いで統一する）

### hardware device

`AV_HWDEVICE_TYPE_D3D11VA`。**expected frame format は `AV_PIX_FMT_D3D11`。**

`AVD3D11VADeviceContext::BindFlags` に
`D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE` を設定する。
`D3D11_BIND_SHADER_RESOURCE` が無いと、decode 出力 texture から
shader resource view を作れず、**CPU へ落とす以外に表示手段が無くなる**。
これは P1 の成否を直接決める設定である。

### presentation

`QQuickRhiItem` / `QQuickRhiItemRenderer`。

## 4. CPU full-frame readback を禁止する理由

`av_hwframe_transfer_data()` や swscale による毎フレーム変換は「動く」。
動いてしまうことが問題である。

1. **P1 の仮説を検証できなくなる。** readback 経路が 1 本でも残っていると、
   device 共有が失敗していても絵は出る。**失敗が失敗として見えない**。
   Phase 0 で最も高くついた事故がこれ（`docs/phase0-findings.md`）であり、
   同じ形の事故を繰り返さない。
2. **帯域が Phase 0 と同じ壁に当たる。** 1080p60 NV12 の readback は
   約 178 MB/s、4K60 では約 712 MB/s。加えて GPU→CPU の同期待ちが入り、
   MLT で観測したのと同種の頭打ちを再現するだけになる。
3. **P2 以降が成立しない。** 二動画 GPU 合成は、両方の frame が
   VRAM に residents していることを前提にする。

したがって P1 では次を **禁止**する。

- `av_hwframe_transfer_data` による毎フレーム CPU 転送
- swscale による毎フレーム RGBA 変換
- `QImage` への毎フレーム copy

**fallback software decode は実装しない。** hardware decode ができない場合は
fail-closed で報告して停止する。Phase 0 の教訓どおり、
暗黙のフォールバックは失敗を静かに成功へ変える。

### 例外として許す readback: marker 帯だけ

frame の正しさ（要求したフレームが本当に表示されたか）は
Phase 0 と同じ焼き込みマーカーで機械判定する（[research/test-media-format.md](research/test-media-format.md)）。
マーカー帯は映像左上の **1216x64 px** に固定されており、
1080p 全画素の **3.7%**、4K の **0.9%** でしかない。

そこで marker 検証時のみ、

1. decode texture を入力に、**表示と同じ変換 shader** で 1216x64 の RGBA へ描く
2. その 1216x64 だけを staging texture へ copy して map する

という経路を使う。これは **full-frame readback ではない**。
計測 JSON では両者を別のカウンタで記録し、混同しない。

| カウンタ | 意味 | exit criteria |
| --- | --- | --- |
| `cpu_full_frame_readback_count` | フレーム全体を CPU へ落とした回数 | **0 でなければ不合格** |
| `marker_band_readback_count` | 1216x64 の帯だけを読んだ回数 | 診断情報。判定に使わない |
| `gpu_copy_count` | GPU 内で行った copy / 変換 pass の回数 | 診断情報 |

「readback していない」ではなく「**どこを何回読んだか**」を出す。
測っていないものを 0 と書かない。

## 5. PTS と frame number

seek 精度とマーカー一致の判定は、すべて frame number 上で行う。

```
frame = round( (pts - start_pts) * time_base * fps )
```

浮動小数を使わず 128bit 整数で計算する。

```
n = (pts - start_pts) * tb.num * fps.num
d = tb.den * fps.den
frame = (n + d/2) / d          （n >= 0 の場合。負は対称に丸める）
```

理由: 60000/1001 のような有理 fps で `double` を経由すると、
長尺の後半で 1 frame ずれる。Phase 0 の proxy で
「尺が 1 frame 伸びる」事故が実際に起きている。

逆変換 `frameNumberToPts` も同じ規則で持ち、
`ptsToFrameNumber(frameNumberToPts(n)) == n` を自動テストで往復検査する。

## 6. Qt / FFmpeg の D3D11 device 共有（P1 の主要仮説）

**P1 の主要仮説はここにある。**
「Qt Quick が使う D3D11 device と、FFmpeg の decode device を同一にできるか。」

同一でなければ、decode texture を Qt の renderer から直接 sample できず、
shared texture + keyed mutex 経由の GPU copy か、最悪 CPU readback になる。

### 第一候補（採用する順序）

1. Qt Quick の graphics API を `QSGRendererInterface::Direct3D11` へ**固定**する
   （`QQuickWindow::setGraphicsApi`。既定値に依存しない）
2. **render thread 上で** `QRhi::nativeHandles()` → `QRhiD3D11NativeHandles` から
   `ID3D11Device*` / `ID3D11DeviceContext*` / feature level / adapter LUID を取得する
3. その context に `ID3D10Multithread::SetMultithreadProtected(TRUE)` を設定する
4. `av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)` で AVHWDeviceContext を作り、
   `AVD3D11VADeviceContext::device` / `device_context` に **Qt の実体を AddRef して渡す**
   （`video_device` / `video_context` は FFmpeg に導出させる）
5. `lock` / `unlock` / `lock_ctx` に自前の再帰 mutex を設定し、
   renderer 側が D3D11 を触るときも同じ mutex を取る

`ID3D11DeviceContext` は thread-safe ではない。decode thread と Qt render thread が
同じ immediate context を共有する以上、**`SetMultithreadProtected` と自前 lock の
二重の直列化が必要**である。片方だけでは足りない
（FFmpeg 内部の video context 呼び出しは lock コールバックしか通らない）。

### 同一 device 共有が困難な場合

原因を記録したうえで、次の順に降りる。**CPU readback へは逃げない。**

1. shared texture (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) + keyed mutex
2. 同一 adapter 上での D3D11 GPU copy

どこまで降りたかは完了報告に明記する。「zero-copy か GPU-copy か」を曖昧にしない。

### 検査する

実際に使った device pointer / adapter LUID / feature level を JSON へ記録し、
**Qt 側と FFmpeg 側の adapter が一致していること**を機械で照合する。

- `qt_d3d11_device` / `ffmpeg_d3d11_device`（ポインタ値）
- `qt_adapter_luid` / `ffmpeg_adapter_luid`
- `same_device`（**device pointer 一致**。zero-copy が成立している）
- `same_adapter`（**LUID 一致**。device が別でも GPU copy で繋げる）

`same_device` と `same_adapter` を同じ式で計算しない。
同じ値を 2 つの名前で出すと、どちらが成立したのかが分からなくなり、
「zero-copy だった」と「同じ GPU だった」を取り違える。

FFmpeg 側の LUID は **decode 結果の texture から `GetDevice` で遡って**取る。
設定値ではなく実体を見る（Phase 0 の `mvm_guard_qt` と同じ考え方）。

`same_adapter` が false の run は、他の数値がどれだけ良くても不合格とする。

## 7. QQuickRhiItem / QRhi の互換保証と隔離

`QRhi` は Qt の **private API** である（`QtGui/<version>/QtGui/rhi/qrhi.h`）。
`QQuickRhiItem` 自体は public だが、
patch release 間でもソース互換・バイナリ互換が保証されていない。

さらに QRhi には次の制約がある。

- NV12 / P010 のような planar YUV format を `QRhiTexture` として扱えない
- texture array の特定 slice を指す view を作る API が無い

decode 出力は **NV12 の texture array**（array index が data[1]）なので、
QRhi の texture 抽象にはそのまま載らない。

そこで P1 では次の形にする。

- NV12/P010 → RGB の変換は **生の D3D11 pixel shader** で行い、
  出力先は `QQuickRhiItem` の color texture（`QRhiTexture::nativeTexture()` で実体を取る）
- 生 D3D11 の発行は `QRhiCommandBuffer::beginExternalCommandBuffer()` /
  `endExternalCommandBuffer()` で挟む（QRhi の状態追跡を明示的に無効化させる）

**Qt に依存するコードは `src/app/preview/` の
`PreviewRhiItem` / `PreviewRhiRenderer` だけに閉じる。**
`src/media/gpu_preview/` は Qt を一切 include しない。
Qt の非互換が来ても、壊れる範囲を 2 ファイルに限定する。
これは Phase 0 で「MLT のヘッダを include してよいのは `src/media/mlt/` だけ」と
決めたのと同じ隔離である。

## 8. 境界（最小限の interface）

**巨大な `IMediaEngine` も、製品用 timeline model も作らない。**

### `DecodedGpuFrame`

| フィールド | 内容 |
| --- | --- |
| `frameNumber` | timeline frame number |
| `pts` | 元の PTS |
| `timeBase` | PTS の time base（有理数のまま） |
| `width` / `height` | 表示解像度 |
| `pixelFormat` | NV12 / P010 |
| `texture` | `ID3D11Texture2D*` |
| `arrayIndex` | texture array index（= subresource） |
| `colorSpace` / `colorRange` | 色空間・レンジ |
| `lifetime` | lifetime token（§9） |
| `generation` | seek generation（§9） |

### `IVideoDecoder`

`open` / `requestFrame` / `seek` / `flush` / `close`。

`requestFrame` は `Ok` / `Eof` / `Again` / `Error` を区別して返す。
「取れなかった」を一括りにしない。EOF と decode error を同じ扱いにすると、
壊れた素材が「最後まで再生できた」ことになる。

### `IPreviewSurface`

`submitFrame` / `clear` / `displayedFrameNumber`。

## 9. 寿命と所有権

### frame lifetime

`DecodedGpuFrame::lifetime` は `AVFrame` の参照を保持する
`std::shared_ptr<void>` である。decode texture は FFmpeg の pool 内の
array texture の 1 slice なので、**AVFrame を解放した瞬間に
別のフレームが同じ slice へ decode されうる**。

renderer は submit された frame を **GPU command が完了するまで**保持する。

> **P1.1 で方式を変えた。** P1 は「直近 N 枚を retain する」深さ方式だったが、
> これは「たぶん GPU は読み終わっているだろう」という推測でしかなく、
> 負荷が上がれば保証が崩れる。P1.1 では `ID3D11Fence`（無ければ
> `D3D11_QUERY_EVENT`）の **完了 serial** を根拠にする。詳細は §15。

### stale generation rejection

`seek` / `flush` のたびに decoder の `generation` を増やす。
surface は自分が知る最新 generation より古い frame を **拒否**する。
seek 直後に、飛ぶ前のフレームが 1 枚遅れて到着して表示される事故を防ぐ。

### thread

| スレッド | 責務 |
| --- | --- |
| GUI thread | QML、操作、`PreviewRhiItem` の property |
| decode thread | `IVideoDecoder`。D3D11 呼び出しは lock 下 |
| Qt render thread | `PreviewRhiRenderer`。device の取得元。D3D11 呼び出しは lock 下 |

GUI thread と render thread の受け渡しは `QQuickRhiItemRenderer::synchronize()`
（両スレッドが停止している時点）でのみ行う。

## 10. 色空間

NV12 / P010 → RGB の変換は shader で行う。係数は frame metadata から選ぶ。

- 少なくとも **BT.709 limited** と **BT.709 full** を区別する
- BT.601 / BT.2020 (non-constant luminance) も係数を持つ
- metadata が未指定 (`AVCOL_SPC_UNSPECIFIED`) のときは
  解像度で推定する（幅 <= 1024 かつ 高さ <= 576 なら BT.601、それ以外は BT.709）
- range 未指定は limited とみなす（H.264 / HEVC の既定）

推定した場合は「推定した」ことを JSON に残す。黙って決め打ちにしない。

## 11. 計測プロトコル

既存の benchmark 素材（60 秒 / 3600 frame）を使う。新規に作らない。

| 素材 | 扱い |
| --- | --- |
| `v1080p60_h264.mp4` | **判定対象** |
| `v1080p60_hevc.mp4` | **判定対象** |
| `v4k60_h264.mp4` | 診断のみ。判定に使わない |

各素材について:

- warm-up **5 秒** → 測定 **60 秒**
- **3 run**。**run ごとに別プロセス**（プロセス内の状態を持ち越さない）
- seek は seed 固定で **1000 点**
- marker 代表点: **frame 0 / 1 / 137 / 299 / 600 / 1799 / 3599**

判定は release ビルドで行う（Phase 0 の規約どおり、debug の数値は使わない）。

```powershell
pwsh scripts/build.ps1                       # release ビルド
pwsh scripts/test.ps1 -Preset ucrt64-release # 通常 CTest
pwsh scripts/test.ps1 -Preset ucrt64-debug   # 通常 CTest (debug)
pwsh scripts/p1-matrix.ps1                   # **正式な計測。合否はこれだけで決める**
pwsh scripts/p1-matrix.ps1 -Quick            # 短縮 (経路確認用。判定に使えない)
```

`scripts/p1-matrix.ps1` が生 JSON から集計し、
`scripts/check-p1-contract.ps1` が run ごとの契約を検査する。
契約検査は 1 実装だけに置き、CTest と matrix の両方がこれを呼ぶ。

### JSON に出す項目

adapter LUID / Qt D3D11 device / FFmpeg D3D11 device / `same_device` /
decoded frames / displayed frames / dropped frames / effective fps /
frame interval p50 / p95 / max / startup latency / seek p50 / p95 / max /
marker mismatch / CPU full-frame readback count / GPU copy count /
CPU utilization / GPU engine utilization（取得可能な場合） /
WorkingSet / PrivateUsage / device lost count / errors

### 指標の定義（先に決めておく）

数えた後に定義を選ぶと、都合のよい定義を選んでしまう。先に決める。

| 指標 | 定義 |
| --- | --- |
| `decoded_frames` | 計測区間で decode したフレーム数 |
| `displayed_frames` | 計測区間で **実際に描画した** ユニークなフレーム数 |
| `dropped_frames` | **表示期限を過ぎて意図的に捨てた数**（P1.1 §7 で定義を締めた） |
| `pending_at_end` | 計測終了時に queue に残っていた数。**drop ではない** |
| `drop_rate` | `dropped / decoded` |
| `repeated_presents` | 新しいフレームが無く、前の絵をもう一度出した present の回数 |
| `repeat_rate` | `repeated / present_calls` |
| `effective_fps` | `displayed_frames / 実測経過秒` |
| `present_rate_hz` | `present_calls / 実測経過秒` |

**queue に残っている frame を drop と呼ばない (P1.1 §7)。**
計測を止めた瞬間に表示待ちが数枚残るのは正常であり、
それを drop に数えると「正常終了が不合格」になる。
drop は「表示期限を過ぎたので捨てた」ものだけを指す。

`dropped` と `repeated` を **同じ指標にしない**。
表示のリフレッシュが素材の fps より速ければ `repeated` は必ず出る
（60fps 素材を 72Hz で回せば 1/6 は繰り返しになる）。
これを drop と呼ぶと、正常な状態が不合格になる。

再生は実時間クロックで刻んでいない。表示が追いつかないときは
decode 側に backpressure がかかる（落とさない）。したがって
`effective_fps` は「この経路が持続的に出せる表示スループット」であり、
リアルタイム再生の可否は `effective_fps >= 55` で判定する。

**`effective_fps` には上限がある。** 表示は swapchain の present に
同期するので、`effective_fps` はディスプレイのリフレッシュレートを超えない。
判定ホストは約 59.95 Hz なので、この経路がどれだけ速くても
**60 fps 付近が上限**である。したがって

- `effective_fps >= 55` は「**vsync のほぼ毎回に新しいフレームを間に合わせた**」の意味であり、
  「この経路の最大スループットが 55 fps である」という意味ではない
- 余力（headroom）は測っていない。`display_refresh_hz` と `present_rate_hz` を
  JSON に併記して、上限に張り付いていることが分かるようにする

計測区間の値は **区間が終わった時点で確定させる**。
そのあとの marker 検査と seek 計測でも decode は進むので、
JSON を書く時点で読むと計測区間の値ではなくなる（実際に混ざった）。

Phase 0 の教訓をそのまま持ち込む。

- **配信数を fps と呼ばない。** 実際に描画された frame だけを
  `displayed` として数え、`decoded` と分けて出す
- 経過時間は `QueryPerformanceCounter` の実測値を使う
- 中央値の max ではなく、**全 run を通した観測 max** も出して判定に使う
- 集計は生 JSON から機械で再計算し、自己整合（`effective_fps == displayed/elapsed`）を検査する
- **計測値を文書へ手で転記しない**

GPU engine utilization は取得できない場合がある。
その場合は 0 と書かず、`null` と「取得できなかった理由」を書く。

## 12. P1 exit criteria

**MUST 全充足**で合格。1 つでも欠ければ不合格。

| # | 条件 |
| --- | --- |
| 1 | H.264 / HEVC の**両方**で表示できる |
| 2 | marker mismatch **0** |
| 3 | Qt と FFmpeg が**同一 adapter** |
| 4 | `cpu_full_frame_readback_count` **== 0** |
| 5 | 1080p60 `effective_fps` **>= 55** |
| 6 | `drop_rate` **<= 0.02**（decode したのに表示されなかった割合。§11 の定義） |
| 7 | seek **displayed** p95 **<= 150 ms**（P1.1 §5。decode-ready ではない） |
| 8 | seek **displayed** 観測 max **<= 400 ms** |
| 9 | crash **0** / device lost **0** |
| 10 | 通常の release / debug CTest が**全通過** |

**不合格でも閾値を変更しない。** 原因を記録して停止する。
Phase 0 で MLT を不採用にしたときと同じ規則である。
閾値を動かして合格にすることは、判定そのものを無意味にする。

## 13. 自動テスト

通常 CTest（短時間）で回すもの:

- timestamp / frame 変換（往復・境界・有理 fps）
- seek 後の flush が行われること
- color metadata mapping
- frame lifetime（token が生きている間 texture が解放されない）
- stale generation rejection
- device mismatch negative（別 device の texture を拒否する）
- software frame rejection（`AV_PIX_FMT_D3D11` 以外を拒否する）
- CPU readback counter（帯 readback は band を増やし full を増やさない）
- invalid media negative（破損素材を成功扱いしない）
- missing hardware decoder negative（hwaccel が無い場合に fail-closed）
- marker 代表点

長時間の性能試験には **`performance` ラベル**を付け、通常 CTest へ混ぜない。

Phase 0 の規約を継承する。

- `WILL_FAIL` は使わない。`scripts/expect-exit.ps1` で終了コードを厳密に照合する
- 新しい検査を足したら、**その検査が無ければ落ちる negative test** を同時に足す
- 対象 0 件のテスト群を「全部通った」と報告しない

## 14. P1 で実装しない項目

判定に必要が無いので作らない。

- 製品用 NLE、タイムライン UI、Project Model、undo / redo
- 複数トラック、GPU 合成（**P2 の対象**）
- 音声（再生・同期・波形）
- 文字・字幕・エフェクト・キーフレーム
- export / encode
- software decode fallback
- D3D12 / Vulkan / OpenGL backend
- 色管理（HDR / tone mapping / ICC）。BT.709 / BT.601 の係数選択までに留める
- 10bit 表示品質の検証（P010 の経路は通すが、判定対象は 8bit）
- proxy 生成・切り替え（Phase 0 の resolver をそのまま使う）
- インストーラ / 署名 / clean environment 検証
- 複数 GPU / adapter 切り替え / device lost からの復帰（**検出はする**）
- vsync を外した最大スループットの計測（余力の測定）。
  P1 は「60 fps に間に合うか」までしか見ない

## 15. P1.1: GPU 寿命・並行性・計測契約のクローズ

P1 の縦切りは成立したが、P2（二動画 GPU 合成）へ広げる前に
**正しさの面で開いたままの穴**を閉じる。P1.1 で閉じたのは次の 7 点である。

| # | 穴 | 閉じ方 |
| --- | --- | --- |
| 1 | frame の解放が「直近 N 枚保持」という**推測**だった | GPU の完了 serial を根拠にする（`GpuCompletionTracker` / `GpuRetirementQueue`）。fence を第一候補、event query を fallback |
| 2 | GUI が decoder 内部を**無排他で読んでいた** | `DecoderSnapshot` の値コピーだけを返す。mutable object への const 参照を廃止 |
| 3 | generation の照合が片側だけだった | 未来 generation を拒否、逆行を拒否、同値は no-op（pending を破棄しない） |
| 4 | SRV cache の key が (texture, index) だけだった | key に `resource_epoch` と pixel format を含め、旧 epoch は GPU 完了後に retire |
| 5 | seek を **decode 完了**までしか測っていなかった | `seek_decode_ready_ms` と `seek_displayed_ms` に分け、**判定は displayed** |
| 6 | 色の正しさを marker 一致で代用していた | 既知 YUV patch の fixture を作り、表示と同じ shader で照合 |
| 7 | `initialize()` の再入で古い device を使い続けうる | 毎回 native device / context を照合し、変化したら drain して再初期化。件数を JSON へ出す |

### GPU 完了に基づく retire（§1 の契約）

- draw を発行するたびに submission serial を得る
- 描画に使った lifetime token / SRV は、その serial が完了するまで解放しない
- seek / clear / stop でも即解放せず retirement queue へ移す
- **per-frame で GPU 完了を blocking wait しない。** render ごとに poll する
- shutdown だけ有限 timeout で drain する。timeout は fail-closed
- **`Flush` を毎 frame 呼んで見かけ上解決しない**

`frames_released_before_completion` は **必ず 0**。
0 でなければ「GPU が読み終わる前に手放した」ことを意味するので不合格とする。

### source_generation と composition_epoch

P1.1 では decoder は 1 本だが、**単一の global generation に固定しない**。

- `source_generation`: ある source の seek / flush で進む
- `composition_epoch`: decode pool / device / resource が作り直された世代

比較は composition_epoch を上位、source_generation を下位とする辞書式で行う。
P2 で source が増えても、この形のまま拡張できる。

### 色検査の位置づけ

正式な色検査は `mvm_test_gpu_decode color` が
`tests/assets/color/` の fixture に対して行う（CTest 登録済み）。
期待 RGB は `scripts/make-color-fixtures.ps1` が
**標準式から独立に**計算しており、実装の関数は呼んでいない。

`preview_spike` 側の `--color-patch` は診断であり、合否には使わない
（どの行列 / レンジが選ばれたかを JSON へ残すだけ）。

## 16. P1 の後

実測所見は [phase1-findings.md](phase1-findings.md) に、
事実 / 推測 / 未検証を区別して記録する。

P1 の結果は [adr/0002-preview-backend-spike.md](adr/0002-preview-backend-spike.md) に
**Proposed** として記録する。**この時点では製品採用を決定しない。**

P2（二動画 GPU 合成）へ進めるかどうかだけを判断し、そこで停止する。
