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

## 所見 B: 縮小配置（PiP）は未解決。M3 は未達

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
