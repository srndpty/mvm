# Preview backend 採用決定

- 状態: **Accepted**
- 決定日: 2026-08-10
- 検証済み baseline: `f310e27a27f1d54fb79abe055d81a41ec8c00b39`
- 関連文書: [PreviewEngine 製品契約](preview-engine-contract.md)、[Phase 5 計画](phase5-plan.md)

## 1. 決定

Windows の既定 hardware-accelerated preview architecture として、次の経路を採用する。

```text
FFmpeg D3D11VA decode
  -> same-device D3D11 compositor
  -> QRhi / QQuickRhiItem presentation

FFmpeg audio decode
  -> WASAPI shared event-driven rendering
  -> IAudioClock master scheduling
```

この決定では、次の三つを区別する。

| claim | verdict | 意味 |
| --- | --- | --- |
| architecture spike validated | **YES** | Phase 1～4 の検証対象となった経路が凍結済み契約を満たした |
| suitable for product adoption | **YES** | この経路を Windows の既定 preview backend として製品化へ進める |
| full NLE preview subsystem complete | **NO** | Project Model、編集UI、任意track数、effect等を含む製品機能は未実装・未検証 |

この採用決定により、別の backend spike を追加せずに `PreviewEngine` の製品化を開始できる。

## 2. 歴史的な検証結果

過去の判定は上書きせず、次のとおり保持する。

| contract | historical result |
| --- | --- |
| P2-D4-2 | **FAIL** |
| P3-C-1 | **FAIL** |
| P2-D5-1 | **PASS** |
| P3-C-2 | **PASS** |
| Phase 4 | **FINAL PASS** |

P2-D4-2 と P3-C-1 の失敗は後続contractの成功によって遡及的にPASSへ変更しない。
同様に、Phase 4 FINAL PASSはarchitecture validationの完了を意味するが、full NLE preview
subsystemの完成を意味しない。

根拠となる凍結済み文書は次のとおりである。

- [Phase 1 findings](phase1-findings.md)
- [Phase 2 findings](phase2-findings.md)
- [Phase 3 findings](phase3-findings.md)
- [Phase 4 fixed contract](phase4-plan.md)

## 3. MLT に関する決定の範囲

**MLT 7.36.1 / UCRT64 CPU/RGBA preview path: NOT ADOPTED**

MLTの他の経路・用途の採否はこの決定の対象外である。観測したpreview性能値をMLT固有の
物理上限へ一般化しない。exportの適性は測定・判定していない。

Phase 0で不採用としたのは、測定したMLT 7.36.1 / UCRT64 CPU/RGBA preview pathである。
exportを含む未測定用途へ結果を一般化しない。詳細は
[ADR 0001](adr/0001-mlt-adoption.md) と [Phase 0 findings](phase0-findings.md) に従う。

## 4. 採用architecture

製品化で維持するarchitecture上の性質は次のとおりである。

- FFmpegはD3D11VA hardware frameを生成する
- decodeとcompositionはQt Quickと同じ`ID3D11Device`を使用する
- video frameをper-frameでCPUへ戻さない
- D3D11 compositorが同一device上でlayerを合成する
- QRhi / QQuickRhiItemとの接続はQt専用bridgeへ隔離する
- GPU submission完了までframe、SRV、lifetime payloadをretireしない
- audioはWASAPI shared event-driven renderingを使用する
- video schedulingのmasterは`IAudioClock`とする
- exact source frame、generation、composition identityを検査する
- capability外またはidentity不一致を暗黙fallbackで隠さない

## 5. 正式に検証したenvelope

現在のformal evidenceが直接支えるenvelopeは次のとおりである。

- NVIDIA GeForce RTX 4090
- Windows / Direct3D 11
- **Formally validated topology: two simultaneous 1080p60 video sources.**
- two-source / two-layer composition。各layerは異なるsourceを一度ずつ参照する
- 1920x1080 output
- device pixel ratio 1
- 一つのaudio source
- 48000 Hz / stereoのaudio path
- play、pause、exact seek
- immutable composition snapshotの切り替え

現在の検証済みoutput frame rateは`60/1`である。

このenvelopeはarchitectureの適用範囲やpublic APIの恒久的な上限ではない。特に次は永久的な
API limitとして固定しない。

- video source count
- resolution
- device pixel ratio
- audio source count
- GPU adapter

二本を恒久的なvideo source数またはcomposition layer数の上限とは記述しない。二source／二layerは
現在正式に検証されたtopologyであり、将来のqualificationによって増やせる。同一sourceを複数layerへ
配置する構成は現在のformal evidenceではqualificationされていない。

## 6. 未検証または未契約の能力

次は採用architectureを否定するものではないが、現在のformal evidenceでは保証しない。

- 三本以上の同時active video source
- 複数audio sourceのmixing、volume automation、pan、time stretch
- 複数の4K sourceを含むcomposition
- Intel / AMD adapter、複数GPU、adapter切り替え
- HDR、tone mapping、ICC color management
- D3D11以外のgraphics backend
- device lost後の自動reopen/recovery
- 任意のaudio endpoint format
- 再生中の動的なsource追加・削除
- transition、keyframe、effect graph、text rendering

製品runtimeは現在qualifiedな能力を`PreviewCapabilities`として報告できる。能力外の要求は
`UnsupportedCapability`としてfail-closedで拒否し、型構造だけを根拠に対応済みと扱わない。

## 7. Fallback とdevice lost

採用経路では次へ黙ってfallbackしない。

- software video decode
- CPU composition
- full-frame CPU readback
- QPC master clock
- stale/latest frameの代用
- 古いcomposition snapshotの再利用
- deviceの自動reopen

device lostを検出したらtransportを停止して`ShuttingDown`へ遷移し、安全なshutdown sequenceを
実行する。resource teardown完了後にだけterminal `Error`を公開する。自動復旧は将来の独立contractで扱う。

## 8. 帰結

- P5はbackend選定ではなく、検証済みcomponentを製品境界へ包む作業になる
- `PreviewEngine` public APIにはFFmpeg、D3D11、QRhi、spike stateの型を出さない
- `src/media/gpu_preview`と`src/media/audio_preview`は可能な限りQt非依存を維持する
- Qt private APIへの依存は`src/app/preview`のbridgeへ限定する
- Phase 1～4のexact pairing、generation、GPU lifetime、shutdown semanticsを弱めない
- full NLE機能の完成は、各製品sliceの実装・検証を経た後に別途判定する
