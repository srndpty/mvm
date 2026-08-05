# Phase 0 判定書: MLT 7 の採否

作成日: 2026-08-06
対象: mvm Phase 0（技術スパイク）S0〜S7.1 の実測結果
位置づけ: 計画 §10 の exit criteria に基づく **早期 S16** の判定書

---

## 1. 判定

> **MLT 7.36.1 / MSYS2 UCRT64 の現行 CPU・RGBA 経路を、
> mvm の統合編集・リアルタイム preview engine として採用しない。**

この一文が判定のすべてである。以下は範囲の限定であり、緩和ではない。

### この判定が言っていないこと

判定を広く読み替えないために、明示的に否定しておく。

- **「MLT が一般に使えない」とは言っていない。**
  Shotcut / Kdenlive が実運用されている事実と矛盾しない。
  測ったのは *この構成* (7.36.1 / UCRT64 / CPU / RGBA / 5 トラック 1080p60) だけである。
- **「書き出し (export) に使えない」とは言っていない。** 書き出しは**測っていない**。
- **「オフラインレンダに採用する」とも言っていない。** これも**測っていない**。
  M9〜M11 (エフェクト / 書き出し / preview-final 一致) を実施していないので、
  採用の判断材料も不採用の判断材料も無い。**未評価**である。
- **「別の MLT 経路 (GPU / SDL / movit) なら不可」とは言っていない。** 測っていない。

判定が及ぶ範囲は「**リアルタイム preview engine としての採否**」に限られる。

---

## 2. 判定の根拠 (実測)

計画 §10 の判定規則は **MUST の全充足**である。1 つでも欠ければ
「不採用」または「条件付き採用 (回避策付き)」となる。

### 2.1 MUST の充足状況 (S7.1 完了時点)

| # | 基準 | 閾値 | 実測 | 判定 |
| --- | --- | --- | --- | --- |
| M1 | 再現ビルド | 2 回連続成功 | prebuilt では成功。from-source (S13) 未実施 | **部分** |
| M2 | 素材読み込み | ffprobe と一致 | H.264 / HEVC / PNG(alpha) / WAV すべて一致 | **合格** |
| M3 | 5 トラック合成 | 日本語含め意図どおり | PiP・文字・音声 mix すべて実測確認 | **合格** |
| M4 | seek 精度 | 100% 一致 | mismatch **0** (3003 サンプル) | **合格** |
| M5 | seek 速度 | p95 ≤150ms / max ≤400ms | p95 **82.7ms** / 観測 max **2998.0ms** | **不合格** |
| M6 | scrub | ≥15 ups / 表示 p95 ≤200ms | partial proxy で 8 条件中 4 合格。all-video では**未測定** | **保留** |
| **M7** | **1080p60 preview** | **実効 ≥50 fps** | 最良 **19.85 fps** (`real_time=-1`) | **不合格** |
| **M8** | **4K proxy preview** | **≥50 fps** / 生成 ≥2x | preview **19.67 fps** / 生成 **8.11〜8.38x** | **不合格** (fps) |
| M9〜M16 | エフェクト〜安定性 | — | **未実施** | **未評価** |

### 2.2 判定を決めた 2 つの数字

**[事実] M7: 1080p60 / 5 トラックの連続 preview は最良 19.85 fps。基準 50 の 40%。**

これは Qt を一切含まない、**MLT が RGBA を作り終えるまで**の値である
(null consumer、`real_time=-1`、warm-up 5s、wall 60s、3 回中央値)。
スレッドを増やすと悪化した。

**[事実] M8: preview で使う video source を全て proxy 化しても 19.67 fps。**

| 構成 | effective_fps |
| --- | --- |
| 4K original | 10.00 |
| V1 だけ proxy (partial) | 14.88 |
| **V1 + V2 を proxy (all-video)** | **19.67** |
| 1080p native (proxy 不要) | 19.85 |

**素材を 4K から 540p へ落としても、1080p native と同じ 20 fps 付近が上限だった。**
律速は素材の解像度ではなく、5 トラック合成と 1920x1080 RGBA 生成そのものである。

### 2.3 M5 は 1000 点で不合格になった

300 点では観測 max 285.6ms で暫定合格だったが、
**1000 点 x 3 回 (seed 20260804 固定、run ごとに別プロセス) では不合格**だった。

| 区分 | 件数 | p50 | p95 | max |
| --- | --- | --- | --- | --- |
| 全サンプル | 3003 | 60.9ms | **82.7ms** | **2998.0ms** |
| warm のみ | 3039※ | 61.1ms | 85.2ms | 132.0ms |
| cold (各 run の 1 回目) | 3 | — | — | 225.1 / 230.3 / **2998.0** ms |

※ warm の件数が 3003 を超えるのは、CSV に warm-up 用の追加サンプルが含まれるため。

**[事実] 400ms を超えたのは cold の 1 回目 1 件だけ**である。
**[推測] cold が 225ms〜2998ms と 13 倍ばらつく原因は特定していない。**
**「cold を除けば合格」とは書かない。** 利用者は実際にその時間を待つ。

ただし **M5 の合否はこの判定を左右しない。**
M5 が仮に合格でも、M7 / M8 の fps は変わらない。

---

## 3. 回避策を探索し、打ち切った

> **[注意] この節の ablation の値は参考値であり、判定の根拠ではない。**
> 2026-08-06 の再測定で、同じ scenario が run ごとに 2.5 倍ばらつく現象が出た
> (ホスト側の要因。原因は特定していない。
> 詳細は [research/proxy-notes.md](research/proxy-notes.md))。
> **§2 の M7 / M8 はこの表に依存しない。**
> M8 の all-video proxy は独立に 9 回測って 18.74〜19.83 fps に収まり、再現している。
> ablation スクリプトには、run 間のばらつきが 1.25 倍を超えたケースを
> 「判断に使わない」と記録する検査を入れた。

「何を外せば 50fps に届くか」を preview x 3 回で測った (ablation)。
**フレームを作らない構成で速く見せることはしていない。**
全ケースが 1920x1080 RGBA を毎フレーム生成し、
`rendered=0` が 1 件でもあるケースは採用していない。

| ケース | 内容 | fps | B との差 | 製品で使えるか |
| --- | --- | --- | --- | --- |
| A | V1 だけ proxy | 16.39 | -5.14 | いいえ |
| **B** | **all-video proxy / 全 5 トラック (基準)** | **21.53** | — | はい |
| C | qtext を除去 | 33.65 | +12.12 | いいえ (文字は必須) |
| D | 音声 2 トラックを除去 | 22.45 | +0.92 | いいえ。効果も小さい |
| E | V2 / affine 合成を除去 | **35.84** | +14.31 | いいえ (PiP は必須) |
| F | qtext を静的 RGBA overlay の代用物へ置換 | 31.10 | +9.57 | 未確認 |
| G | V2 proxy を 640x360 へ | 24.47 | +2.94 | はい |

**[参考] 必須機能 (文字・PiP) を丸ごと捨てても最良 35.84 fps で、基準 50 に届かない。**
**この行は上の注意のとおり参考値であり、再現性を確認できていない。**
判定に使っているのは M7 / M8 だけである。

**[注意] ケース F は「文字を事前描画した画像」ではない。**
使っているのは既存の `png_alpha.png` で、文字は入っていない。
測ったのは「qtext の毎フレーム描画を、同じ位置・同じ大きさの
**静的 RGBA overlay 1 枚**に置き換えた場合の上限」だけである。
実際の文字画像で同じ fps になる保証は無く、視覚的な同等性も確認していない。

**[推測]** コストは V2 の affine 合成と qtext 描画に集中している。
両方外して 30〜36 fps であり、**残り 14〜20 fps 分の説明はついていない。**
1920x1080 RGBA の生成と転送そのものが下限を作っている可能性はあるが、
**MLT 内部のどこかは特定していない。**

**[方針] 単独の低コスト回避策で 50fps へ届く根拠が無いため、追加の最適化探索は打ち切った。**
private API の cache purge、`EmptyWorkingSet`、閾値緩和、
フレームを作らない構成による偽高速化はいずれも行っていない。

---

## 4. S12 (Qt/QML 経路) を実施せずに判定した理由

計画では M7 の最終判定に S12 (Qt texture 転送を含む end-to-end) を含めていた。
これを**実施せずに**判定する。理由は 1 つである。

**[事実] Qt 経路は MLT が作った RGBA を受け取る側であり、供給側より速くならない。**

M7 の 19.85 fps は `mlt_frame_get_image` が RGBA を返し終えるまでの値である。
S12 で足されるのは `QSGTexture` へのアップロード、UI スレッドのフレーム時間、
音声クロック追従、垂直同期であり、**いずれも引き算にはならない。**

したがって S12 を「M7 を確認するため」に実施しても結果は変わらない。
S12 に意味があるのは、**`mlt_frame_get_image` の RGBA コピーを経ずに
GPU テクスチャへ直接書く経路 (zero-copy / 別 GPU 経路) を作る**場合だけであり、
それは MLT の外側を作り直すことであって Phase 0 の範囲ではない。

計画 §実行順序の
「S6 / S7 / S12 のいずれかが早期に MUST を大きく割った場合、
残りを打ち切って S16 に進む判断を許容する」に該当する。

---

## 5. 測っていないこと (未評価)

**判定の射程外である。「問題なし」ではなく「調べていない」。**

| 項目 | 対応する MUST | 状態 |
| --- | --- | --- |
| エフェクト 6 種と時間アニメーション | M9 | **未評価** (S8 未実施) |
| H.264/AAC 書き出しと原子的リネーム | M10 | **未評価** (S9 未実施) |
| preview と final render の一致 (SSIM / A/V 同期) | M11 | **未評価** (S9 未実施) |
| NVENC 書き出しとオプション pass-through | S1 / S2 | **未評価** |
| clean VM への配置と起動 | M13 | **未評価** (S14 未実施) |
| UI 応答性 (UI スレッドのフレーム時間) | M14 | **未評価** (S12 未実施) |
| `IMediaEngine` 抽象化と `NullEngine` 差し替え | M15 | **未評価** (S11 未実施) |
| 30 分連続再生の安定性 / リーク | M16 | **未評価** (S15 未実施) |
| MLT の from-source ビルドと 2 回連続再現 | M1 の残り | **未評価** (S13 未実施) |
| all-video proxy での M6 (scrub) 正式 8 条件 | M6 | **未測定** |
| Qt 転送を含む end-to-end の fps | M7 | **未測定** (§4 の理由で実施しない) |

**とくに「オフラインレンダ / 書き出しに MLT を使えるか」は
M9〜M11 を一つも実施していないため、肯定も否定もできない。**

---

## 6. 次に検討する代替案

計画 §9 の順位を、今回の所見に基づいて更新する。

**落ちた理由は「アーキテクチャ的な性能上限」であって、
Windows のビルド・配布・ABI ではなかった。**
MSYS2 UCRT64 統一でビルドも実行も問題なく通っている。
日本語パスも読み書きとも動いた。この事実が順位を決める。

| 順位 | 案 | 今回の所見との対応 |
| --- | --- | --- |
| **1** | **libavcodec / libavfilter ベースの自作エンジン + GPU 合成** | 落ちた理由が CPU 合成と RGBA 生成の上限だったので、そこを置き換える案が最も直接的である。mvm はトラック数が少なく解説動画に特化するため、必要な機能集合は小さい |
| 2 | 自作の軽量 preview エンジン + final render は FFmpeg `filter_complex` | preview 性能が理由で落ちたので該当する。ただし preview / final の一致 (M11) が自前の責務になる |
| 3 | GStreamer + GES | **順位を下げる。** GES の利点は公式 MSVC ビルドだが、**今回 ABI / 配布は問題になっていない。** 合成の性能上限が GES で改善する根拠は無い |
| 4 | MLT を fork して vendoring + パッチ | **順位を下げる。** 欠陥が局所的ではなく、合成経路全体の上限である。パッチで閉じる見込みが薄い |

**Phase 0 の成果は代替案でもそのまま使える。**
検証素材、フレーム固有マーカー、`mvm_bench` の計測手法、
proxy path resolver の required/optional 契約、scenario 形式は
いずれも backend 非依存に作ってある。

---

## 7. 再現手順

判定に使った数値は以下で再現できる。

```powershell
# 素材と proxy
pwsh scripts/make-testmedia.ps1 -Mode Benchmark
pwsh scripts/proxy-matrix.ps1

# scenario (partial diagnostic と正式 all-video の両方を生成し、7 条件を機械検査)
pwsh scripts/make-proxy-scenarios.ps1

# M7 / M8
pwsh scripts/preview-matrix.ps1

# M5 (最終確認: 1000 点 x 3 回)
pwsh scripts/seek-matrix.ps1 -Only proxy-all-gop12 -Random 1000 -Runs 3 -Seed 20260804

# 回避策の切り分け
pwsh scripts/ablation-matrix.ps1
```

生データ:

| 内容 | 場所 |
| --- | --- |
| preview (M7 / M8) | `build/<preset>/preview/*/matrix.json` |
| seek (M5) | `build/<preset>/seek-all-1000/` |
| ablation | `build/<preset>/ablation/` |
| proxy メタデータ | `tests/assets/benchmark/_proxy/proxy-matrix-*.json` |

---

## 8. 関連文書

| 文書 | 内容 |
| --- | --- |
| [adr/0001-mlt-adoption.md](adr/0001-mlt-adoption.md) | この判定の ADR |
| [phase0-findings.md](phase0-findings.md) | 実測結果の全体。事実 / 推測 / 未検証を区別 |
| [research/preview-performance-notes.md](research/preview-performance-notes.md) | `real_time` の意味と preview 経路の実測 |
| [research/proxy-notes.md](research/proxy-notes.md) | proxy 生成・resolver 契約・正式 M8 |
| [research/seek-scrub-notes.md](research/seek-scrub-notes.md) | M5 の 1000 点最終確認と M6 |
| [phase0-plan.md](phase0-plan.md) | 計画と exit criteria |
