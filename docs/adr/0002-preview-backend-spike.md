# ADR 0002: 内製 GPU preview backend（FFmpeg D3D11VA + Qt Quick / QRhi）を検証する

- 状態: **Proposed**（提案。製品採用は決定しない）
- 日付: 2026-08-06
- 対象: mvm Phase 1 / P1（GPU Preview Engine Spike, single-video vertical slice）
- 根拠文書: [../phase1-plan.md](../phase1-plan.md)
- 前提: [0001-mlt-adoption.md](0001-mlt-adoption.md)（MLT 不採用）

## 決定

**FFmpeg (D3D11VA hardware decode) と Qt Quick (QRhi / Direct3D 11) を
同一の `ID3D11Device` 上で結び、decode 結果の texture を CPU へ戻さずに
表示する経路を、mvm の preview backend 候補として検証する。**

この ADR は **検証すること**を決めるものであり、
**採用を決めるものではない**。採用可否は P1 の実測結果を得たうえで、
別の ADR（0003 を想定）で決める。

## 文脈

[ADR 0001](0001-mlt-adoption.md) で MLT 7 を preview engine として不採用とした。
理由は 1080p60 / 5 トラックの連続 preview が基準 50 fps に対して
最良 19.85 fps しか出ず、proxy 化しても改善しなかったことである。

MLT 経路の構造上の性質は「フレームを CPU 上の RGBA として具現化してから配る」ことである。
ただし Phase 0 では **MLT 内部のどの段が律速かを特定していない**。
したがって「CPU / RGBA 経路だから遅い」と断定はできない。
言えるのは、CPU / RGBA 経路である MLT が基準を満たさなかったこと、
および mvm が必要とする preview 帯域（1080p60 で 55 fps 以上）を
CPU readback 込みで確保できる根拠を、現時点で持っていないことである。

そこで **画素を VRAM から出さない経路が成立するか**を、
最小の縦切り（1 本の動画を decode して表示するだけ）で確かめる。

## 選択肢

### A. FFmpeg D3D11VA + Qt Quick / QRhi 同一 device（本 ADR で検証する）

decode 出力の `ID3D11Texture2D` を、Qt Quick が使っているのと
同じ device 上の shader から直接 sample する。

- 利点: CPU 転送が 0。P2（GPU 合成）へ素直に伸びる。依存が FFmpeg と Qt だけ
- 欠点: `QRhi` は Qt private API であり互換保証が弱い。
  NV12 texture array を QRhi の抽象に載せられないため、生 D3D11 を混ぜる必要がある

### B. Qt Multimedia / QVideoSink に載せる

- 利点: 実装量が最小
- 欠点: フレーム単位の決定論的な seek と frame number の同定が制御できない。
  Phase 0 で確立した marker 検証（要求フレームと表示フレームの一致）を
  そのまま適用できない。編集用 preview には粒度が粗すぎる

### C. Media Foundation / DXVA を直接使う

- 利点: Windows ネイティブ。D3D11 との親和性が高い
- 欠点: 対応 codec・container が FFmpeg より狭い。Phase 0 の素材・proxy 資産と
  検証手段（ffprobe / manifest）が繋がらない。Windows 専用に固定される

### D. CPU decode + GPU upload

- 利点: 実装が単純で、device 共有の問題が起きない
- 欠点: **Phase 0 で不合格になった構造と同じ**。
  1080p60 NV12 で約 178 MB/s、4K60 で約 712 MB/s の転送が毎フレーム発生する。
  これを退避経路として残すと、A の失敗が「絵は出ている」ことで隠れる

**A を検証する。D は P1 のコードから明示的に排除する**（fallback を実装しない）。

## この ADR で確定させる技術的前提

1. hardware device type は `AV_HWDEVICE_TYPE_D3D11VA`、
   期待する frame format は `AV_PIX_FMT_D3D11` に固定する
2. `AVD3D11VADeviceContext::BindFlags` に `D3D11_BIND_SHADER_RESOURCE` を含める。
   含めないと decode texture から SRV を作れず、CPU へ落とす以外の手段が無くなる
3. Qt Quick の graphics API は `Direct3D11` に**明示的に固定**する。既定値に依存しない
4. Qt に依存するコードは `src/app/preview/` の `PreviewRhiItem` / `PreviewRhiRenderer`
   に隔離する。`src/media/gpu_preview/` は Qt を include しない。
   Phase 0 で「MLT のヘッダを include してよいのは `src/media/mlt/` だけ」と
   決めたのと同じ隔離規則を、QRhi に対して適用する
5. software decode fallback を実装しない。hardware decode 不可なら fail-closed で報告する

## 検証で否定されうる仮説（P1 の主要仮説）

**「Qt Quick の `ID3D11Device` と FFmpeg の decode device を同一にできる」**

これが成立しない場合、次の順で降りる。CPU readback へは降りない。

1. shared texture + keyed mutex
2. 同一 adapter 上の D3D11 GPU copy

どこまで降りたかを完了報告に明記する。
「zero-copy」と「GPU-copy」を同じ言葉で報告しない。

## 判定条件

[phase1-plan.md §12](../phase1-plan.md) の exit criteria を **MUST 全充足**で判定する。

要点のみ再掲する。

- H.264 / HEVC 両方で表示できる
- marker mismatch 0
- Qt と FFmpeg が同一 adapter
- `cpu_full_frame_readback_count == 0`
- 1080p60 effective fps >= 55 / drop rate <= 0.02
- seek p95 <= 150 ms、観測 max <= 400 ms
- crash 0 / device lost 0
- 通常 CTest 全通過

**不合格でも閾値を変更しない。** 原因を記録して停止する。

## 帰結

### 肯定的

- 成立すれば、mvm は preview engine の中核（decode → 表示）を自前で持つ。
  MLT のような外部フレームワークの性能特性に縛られない
- P2（二動画 GPU 合成）が同じ device 上の shader 合成として素直に伸びる
- decode / 表示 / 検証が Phase 0 で作った marker・素材・proxy の資産に乗る

### 否定的・リスク

- `QRhi` は private API であり、Qt の patch release で壊れうる。
  隔離しても「壊れないこと」は保証できず、Qt 更新のたびに再検証が要る
- 生 D3D11 と QRhi の状態管理を混在させる。
  `beginExternalCommandBuffer` の契約に依存する
- device lost（driver 更新・TDR）からの復帰を自前で持つ必要がある。
  P1 では**検出のみ**とし、復帰は実装しない
- NVIDIA / RTX 4090 の 1 台でしか測っていない状態で判定することになる。
  Intel / AMD の内蔵 GPU、および複数 GPU 環境は P1 の範囲外である。
  **「Windows で動く」と一般化して書かない**

### 破棄の条件

P1 が不合格で、かつ shared texture / GPU copy へ降りても閾値を満たさない場合、
D3D11 zero-copy 前提の内製 preview を破棄し、
別の選択肢（B / C、あるいは preview の要求水準そのものの見直し）へ戻る。

## 未決定のまま残すこと

- 製品としての preview backend の採用（**この ADR では決めない**）
- encode / export の経路（NVENC を使うか）
- 複数 GPU / adapter 切り替え
- HDR / 色管理
- D3D12 への移行時期
