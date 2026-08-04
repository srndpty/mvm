# 合成に関する実測メモ (S5)

対象: MLT 7.36.1 / MSYS2 UCRT64 / profile `atsc_1080p_60`

分類は `docs/phase0-findings.md` と同じ（`[事実]` / `[推測]` / `[未検証]` / `[回避策]`）。

---

## 動いたこと

**[事実]** 以下は動作を確認した。

- `mlt_tractor` + `mlt_playlist` × 5 で 5 トラックのタイムラインを構築できる
- tractor の length は最長トラックの length になる
- V1 のフレーム固有マーカーが合成後も保持され、要求フレームと一致する
- 5 トラック合成に対する任意フレームの取り出しが成功する（214/214）

## 所見 A: `hide` はトラック（playlist）に設定する。producer では効かない

**[事実]** `hide` を clip の producer に設定しても効果がない。
トラックとして tractor に設定した playlist の properties に設定する必要がある。

**効かないだけでは済まない。** 音声トラックを 1 本足しただけで、
上位の映像トラックの合成がまるごと無効化された。V2 も文字も消え、
出力は V1 のみになる。**エラーも警告も出ない。**

再現:

| 構成 | V2 が合成されるか |
| --- | --- |
| V1 + V2（2 トラック） | される |
| V1 + V2 + A1（3 トラック） | **されない** |
| V1 + A1 + V2（順序を変えても） | **されない** |
| V1 + V2 + A1 + A2 + T1（5 トラック） | **されない** |

`hide` をトラック側に設定すると全構成で合成されるようになった。

値: `1` = 映像を隠す、`2` = 音声を隠す、`3` = 両方。

**[推測]** 音声トラックの映像が隠されていないため、tractor の
フレーム合成経路が期待と異なる形で解決されていたと考えられる。
MLT のソースは読んでいないため断定しない。

## 所見 B-2（解決）: 縮小配置は `affine` transition で成立する

**[事実]** 前回「未解決」としていた PiP は解決した。**使用したのは
`affine` transition + `mlt_properties_anim_set_rect` による typed rect** である。

```c
mlt_rect want = {1260, 700, 640, 360, 1.0};   /* x, y, w, h, opacity */
mlt_properties_anim_set_rect(MLT_TRANSITION_PROPERTIES(tr), "rect",
                             want, 0, length - 1, mlt_keyframe_discrete);
```

設定後は `mlt_properties_anim_get_rect(props, "rect", frame, length)` で
frame 0 / 1 / 137 / 最終フレームから読み戻し、x/y/w/h/opacity が
一致することを確認してから配置している（不一致なら構築を失敗させる）。

読み戻した値: `1260/700:640x360:1`（全プローブフレームで一致）。

A/B 差分による実測結果（frame 0 / 1 / 137 / 299 すべて同じ）:

| 指標 | 実測 | 判定 |
| --- | --- | --- |
| PiP 矩形内の差分率 | **1.00** | V2 が矩形を完全に占める |
| PiP 矩形外の差分率 | **0.00** | 矩形外へ 1 画素も漏れていない |
| 差分の外接矩形 | **x=1260, y=700, w=640, h=360** | 期待値と完全一致 |

1920x1080 → 640x360 は同じ 16:9 なので縦横比も維持されている。

**[事実] `affine` にしたことで文字トラックの合成も初めて成立した。**
qtblend では文字も合成されていなかった。

### qtblend transition では成立しない（typed API でも）

**[事実]** 同じ typed rect を `qtblend` transition に設定した場合:

- `mlt_properties_anim_set_rect` は成功を返す
- `mlt_properties_anim_get_rect` で読み戻すと **設定値と完全に一致する**
- しかし実際の描画では **x のオフセットしか効かない**。
  y・w・h と縮小が無視され、等倍のまま切り出して配置される

`rect=0/0:200x200:1` を与えると V2 が全画面を覆い、
`rect=1260/700:640x360:1` を与えると x=1260 から右端まで全高で覆う。

追加で試して効果が無かったもの: `always_active=1` の有無、
`mlt_transition_set_in_and_out` による in/out の明示、`compositing`、`distort`。

**つまり「値が正しく設定・読み戻しできること」は「正しく描画されること」を
まったく保証しない。** 読み戻し照合だけでは不十分で、
描画結果を A/B 差分で確かめる必要がある。

**[未検証]** qtblend transition が rect を無視する理由。
MLT のソースは読んでいない。

### composite transition は使わない

**[事実]** `composite` transition に `geometry` を設定すると
アクセス違反（0xC0000005）でプロセスが落ちる。
`geometry` と `always_active` だけの最小構成でも再現する。
**この経路は使用しない。**

---

## 所見 B（旧・解決済み）: 縮小配置（PiP）は未解決だった

**[事実]** 3 通り試し、いずれも目的を達成できなかった。

| 方法 | 結果 |
| --- | --- |
| `qtblend` transition、`rect` **なし** | b_track がまったく合成されない（エラーなし） |
| `qtblend` filter に `rect` を設定して producer へ attach（+ transition） | そのトラックが合成結果から消える（エラーなし） |
| `composite` transition + `geometry` | **アクセス違反でクラッシュ**（0xC0000005） |
| `qtblend` transition + `rect`（現在の実装） | 合成される。ただし**拡縮しない** |

現在の実装（`qtblend` transition + `rect="1260 700 640 360 1"`）で観測した挙動:

- x と幅はおおむね反映される
- **高さは反映されない**（画面上端から下端まで占める）
- **縮小されない**。等倍のまま切り出されて配置される

つまり「1/3 に縮小して右下へ」ではなく「等倍の右側 640px 幅を右半分に貼る」
という結果になる。見た目は「何かが重なっている」ので、
画素統計を取らないと成功したと誤認しやすい。

**[未検証]** 未確認の候補:
- `affine` transition の `geometry`
- `affine` filter の `transition.rect` 系プロパティ
- `qtblend` filter を transition なしで単独使用
- `frei0r.cairoblend` transition
- Shotcut / Kdenlive が実際に使っている組み合わせ（未調査）

**[exit]** **M3 は現時点で未達。** 5 トラックの構造・音声・文字・マーカー保持は
できているが、「V2 を縮小して右下へ配置」が実現できていない。

原因の分類:
- **MLT 自体の限界ではない**（Shotcut は同じことをしている）
- **mvm_bench 実装（API の使い方）の問題**である可能性が最も高い
- 回避策の見積り: 上記候補を順に試す作業で 0.5〜1 日

## 所見 C: 検証が空振りしやすい

**[事実]** 実装中に、通っていた検査が 2 回とも偽陽性だった。

1. **text 領域の分散検査**: 閾値 `variance > 5.0` は、背景の testsrc2 の
   カラーバーだけで 3605 に達する。文字が 1 文字も描かれていなくても通る
2. **V2 合成の色距離検査**: `v2_inside` と `v1_only` は元々別内容の領域なので、
   V2 が合成されていなくても距離が開く

さらに根本的な問題として、**V1（H.264）と V2（HEVC）が同一の
testsrc2 から生成されていた**ため、「全画面で重ねた」のか
「縮小して重ねた」のかを画素から区別できなかった。

**[回避策]** HEVC 素材の背景パターンを `smptehdbars` に変更し、
V1（testsrc2）と視覚的に区別できるようにした
（`scripts/make-testmedia.ps1` の `-Pattern`）。

**教訓**: 合成の検証では「検証対象が存在しないときに、その検査が本当に落ちるか」を
先に確かめること。テスト素材が互いに区別可能であることも前提条件である。

## 所見 D: 文字レイヤ

**[事実]** `qtext` filter を `color` producer に attach する構成で、
単独トラックとしては描画される（黄色の画素、mean_alpha 26）。

**[未検証]** 5 トラック合成の中で文字が描画されるかは**未確認**。
所見 A の修正後に再確認したが、所見 C の偽陽性のため
「描画されている」と断定できていない。

**[未検証]** `qtext` と `dynamictext` の比較。
両者の property は実測済み（`docs/research/mlt-notes.md`）だが、
描画結果の比較は行っていない。

実測した property（推測ではない）:

| filter | text を渡す property | 既定値 |
| --- | --- | --- |
| `qtext` | `argument` | `text` |
| `dynamictext` | `argument` | `#timecode#` |

共通: `geometry`（`0%/0%:100%x100%:100%`）、`family`、`size`、`weight`、
`style`、`fgcolour`、`bgcolour`、`olcolour`、`pad`、`halign`、`valign`、
`outline`、`opacity`。`qtext` にのみ `pixel_ratio` と `typewriter.*` がある。

`qtext` には producer 版もあり、`text` / `encoding=UTF-8` / `align` を持つ。

## 所見 F: 文字 service の比較（qtext / dynamictext）

同じ文章・同じフォント（`C:/Windows/Fonts/meiryo.ttc` / family `Meiryo`）・
同じ矩形（96/240 1500x320）で 5 トラック合成を実行した。

| service | 結果 |
| --- | --- |
| **`qtext`** | **成功。** A/B 差分で text 矩形内 0.5685 / 矩形外 0.00。frame 0/1/137/299 すべて同じ |
| `dynamictext` | **アクセス違反（0xC0000005）でクラッシュ** |

**[事実] mvm の第一候補は `qtext` とする。** dynamictext は
この構成（color producer に attach し affine で合成）ではクラッシュするため使わない。

**[未検証]** dynamictext のクラッシュ原因。別の使い方（producer への
直接 attach など）なら動く可能性はあるが追っていない。

### 日本語・数学記号の描画（目視確認）

`qtext` で以下がすべて正しく描画されることを目視で確認した。

```
第1回　微分積分＆線形代数
極限 lim(x→0) sin(x)/x = 1
日本語・English・123・（）「」±×÷
```

- 豆腐・文字化けなし
- **全角空白が保持されている**（「第1回」と「微分積分」の間）
- 数学記号が描画される（`→` `±` `×` `÷`）
- 改行が 3 行として扱われる
- 指定した黄色（`0xffff00ff`）と半透明黒背景（`0x000000c0`）が反映される

フォントは実ファイルの存在を構築前に検査し、無ければ失敗させる
（別フォントへ無言で fallback しない）。

## 所見 G（未解決）: 音声バッファの解釈

**[未解決]** `mlt_frame_get_audio` から得たバッファを読むと、
RMS が `nan` や `1e+33` になり、さらに **heap 破壊（0xC0000374）** が起きる。

試した組み合わせ:

| format | samples 入力 | 結果 |
| --- | --- | --- |
| `mlt_audio_float`（planar と解釈） | 計算値 | 値が `1e+34` |
| `mlt_audio_f32le`（interleaved） | 0 | `nan` + heap 破壊 |
| `mlt_audio_f32le` | `mlt_audio_calculate_frame_samples` の値 | `nan` + heap 破壊 |

戻り値の format 検査は通る（`mlt_audio_f32le` = 5 が返る）。
`frequency=48000 channels=2 samples=800` も妥当な値が返る。
それでもバッファの中身が期待と合わない。

**[回避策]** 原因を特定できていない以上、バッファを読むこと自体が危険なので
**読まずに失敗させている**。誤った RMS を「音が出ている」と誤認するより、
検証できていないことを明示する方が安全である。

**したがって A1 / A2 が両方 mix されているかは未実証である。**

素材側の準備は完了している。A1（映像に埋めた音声）は L 1000Hz / R 500Hz、
A2（WAV）は **L 1500Hz / R 750Hz** と別周波数にしたので、
バッファ問題が解決すれば「4 周波数すべてが検出されるか」で
片方だけでは通らない検査ができる。

**次バッチの最優先項目。**

## 所見 E: 音声の mix

**[事実]** `mix` transition には gain 相当の property が無い。
実測した property は `in` / `out` / `a_track` / `b_track` / `_transition_type=2` のみ。

**[回避策]** トラックごとの音量は `volume` filter の `level`（dB 文字列）で設定する。

**[未検証]** A1 と A2 が実際に両方とも出力へ含まれるかは**未測定**。
Goertzel 法による検証コードは実装済みだが、実行していない。

## producer の共有

**[事実]** 同一の producer オブジェクトを 2 つの playlist に append しても
構築は成功し、各 playlist は独立した length を持つ（180 と 60）。

**[回避策]** それでも clip ごとに producer を新規に開いている。
複数トラックが同時刻に別位置を要求したときの読み取り位置の競合は
構築時ではなく再生時に出るため、構築が通ったことを根拠にしない。

**[未検証]** 共有した場合に実際に競合が起きるかは測っていない。

## playlist の blank

**[事実]** `mlt_playlist_blank(pl, out)` は `out + 1` フレームの空白を作る。
`blank(59)` の後に 120 フレームを append すると playlist の length は 180 になる。

`mlt_playlist_get_clip_info` が返す blank の `length` は 15000 と表示されるが、
`frame_out` は 59 であり、playlist 全体の length には 60 として寄与する。

## producer の in/out と length

**[事実]** `mlt_producer_set_in_and_out(p, 30, 149)` の後:

- `length` は **300 のまま**（素材の長さ）
- `in` = 30、`out` = 149
- `playtime` = 120（= out - in + 1）

つまり `length` は「素材の尺」、`playtime` が「使用範囲の尺」である。
