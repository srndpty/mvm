# P3-1.2: 6 frame の画素の反例の解明と修正

2026-10-12。[P3-1.1](math-compositor-p311.md) で残った反例（`cmp-layers-48` の 6 frame、各 1 画素）を調べた。
**結果は PASS。** 原因を特定して合成器を 1 か所直し、対象の全 frame が MLT の pre-YUV と byte 単位で一致した。

製品の書き出しの経路、timeout、既存の画素 oracle（`tests/harness/mlt_rgba_oracle.h`）とその基準は変えていない。
合成器は製品から呼ばれていない。MLT への組み込みはしていない。commit・push はしていない。
P1〜P3-1.1 の成果物と判定（P3-1.1 の HOLD を含む）は変更していない。

証拠は `build/math-compositor-p312-20261012/`。

| 場所 | 中身 |
| --- | --- |
| `diagnosis/` | 原因の調査（修正前の全不一致の記録、layer ごとの MLT の途中の値、逆アセンブル） |
| `*-mutation/`、`*-mutation-previous-divide-destination-alpha/` | 変異の対照 |
| `*-all-frames/` | 最終の source での全 frame 比較と対照。`identity.json` に binary・source・runtime の SHA-256 |

## 判定

| 項目 | 結果 |
| --- | --- |
| A. 比較の結果の分離 | 成立。COMPARED / UNSUPPORTED / ERROR を分け、対照 4 つが ERROR・欠落として検出された |
| B. 準備済みの画像の所有 | 成立。関数内の static な cache をやめ、1 回の比較 run が所有する文脈へ移した |
| C. 不一致の記録 | 成立。6 画素すべてを記録した（上限 256 件、打ち切りなし） |
| D. 最初に違う layer | 成立。6 画素とも、下の alpha が 254 の画素へ重ねた layer で初めて違う |
| E. 最小の修正と回帰 | 成立。1 行の修正で 6 frame が一致。回帰の試験は修正前の計算で落ちる |
| F. 範囲と下地の alpha | 成立。範囲は外から渡せない型にした。不透明でない下地は拒否する |
| G. gate と provenance | 下の「gate」を参照 |

## 1. 原因（Task D）

[事実] 合成器は下の画素の alpha を `alpha / 255` で 0〜1 にしていた。MLT の実際の binary
（`libmltplus.dll` の `interpBL_b32`）は **`alpha × (1/255)`**（binary32 の定数 `0x3b808081` との掛け算）で
求めている。逆アセンブルで確かめた（`diagnosis/interpBL_b32-disassembly.txt`、`mulss %xmm4,%xmm1`。
`%xmm4` は `0x38446d4e8` の 1/255）。

- 下の alpha が 255 のときは、どちらも 1.0 で同じ。0〜255 のうち 126 個の値（254 を含む）で、割り算と
  掛け算の結果が binary32 の末尾の桁で違う（`diagnosis/destination-alpha-arithmetic-check.txt`）。
- MLT は、不透明な下地へ半透明の texel を重ねた画素の alpha を 254 にする（切り捨てのため）。
  その画素へ次の layer が重なると、下の alpha 254 を使う。ここで末尾の桁が違い、重みが 1 ulp ずれ、
  色の計算結果が整数の境界をまたぐ画素でだけ 1 階調ずれる。
- R だけがずれたのは、反例の画素では下の R と上の R が近く（どちらも 204 付近）、混合の結果が
  整数ちょうどの近くに来るためである。G・B は境界から離れている。
- 既存の oracle（`mltSourceOver`）は、もともと掛け算で書かれている（「実 DLL は定数除算を逆数へ変換する」
  と注記がある）。P3-1 で合成器を書いたときに、下の alpha だけ割り算にしていた。合成器の側の誤りである。

layer ごとの確認（`diagnosis/layer-sequence.json`）。反例の画素に掛かる layer だけを下から 1 枚ずつ増やした
fixture（`--layer-select`）を製品の経路で書き出し、MLT の途中の値を読んだ（16 本）。

| frame | 画素 | 最初に違う layer | その layer の下の画素 | MLT | 修正前 | 修正後 |
| --- | --- | --- | --- | --- | --- | --- |
| 13 | (866, 556) | 44 | (204, 194, 194, 254) | (203, 174, 174, 254) | (204, 174, 174, 254) | (203, 174, 174, 254) |
| 14 | (911, 510) | 44 | (204, 204, 204, 254) | (204, 180, 180, 254) | (203, 180, 180, 254) | (204, 180, 180, 254) |
| 20 | (879, 367) | 44 | (204, 204, 204, 254) | (203, 170, 170, 254) | (204, 170, 170, 254) | (203, 170, 170, 254) |
| 23 | (554, 440) | 18 | (0, 0, 0, 254) | (57, 9, 9, 254) | (58, 9, 9, 254) | (57, 9, 9, 254) |
| 32 | (605, 404) | 32 | (204, 204, 204, 254) | (203, 170, 170, 254) | (204, 170, 170, 254) | (203, 170, 170, 254) |
| 52 | (841, 544) | 44 | (13, 2, 2, 254) | (191, 31, 31, 254) | (190, 31, 31, 254) | (191, 31, 31, 254) |

- 6 画素とも、最初に違う layer より下の layer では、修正前の合成器と MLT が一致している。
  最初に違う layer の下の alpha は、どれも 254 である。
- 最初に違う layer は、どれも qtcrop の付かない clip である。qtcrop の描き直しは原因ではない。
- frame 13・14 について、修正前後の式を binary32 で手計算し、割り算なら修正前の値、掛け算なら MLT の値に
  なることを確かめた（上の check の出力）。

調べて否定した仮説（`diagnosis/02-serialized-values/`）: 「MLT は animation の値を文字列（`%g`、6 桁）へ直して
読み直している」。合成器へ渡す位置と不透明度を `%g` 経由にすると、不一致は 6 画素から 763,978 画素
（120 frame すべて）へ増えた。MLT は倍精度の値をそのまま使っている。

[未検証] trace が残すのは、素材座標・補間の重み・qtcrop の後の texel・不透明度（倍精度と binary32）・
layer の前後の画素である。qtcrop の前の texel と、式の途中の binary32 の値そのものは trace に残していない
（後者は frame 13・14 の手計算だけ）。

## 2. 修正と回帰（Task E）

修正は `src/media/math/math_group_compose.cpp` の 1 行（下の alpha を `× (1/255)` で求める）。
字形の無い画素を飛ばしてよいかの判定（`mathGroupAlphaRoundTripIsIdentity`）も同じ式へ揃えた
（掛け算でも 0〜255 のすべてで恒等）。

- [事実] この 1 行だけを直した時点（`diagnosis/03-minimal-fix/`）で、`cmp-layers-48` の 120 frame が
  すべて一致した。
- 回帰の試験（`math_group_compose_oracle`）:
  - MLT の pre-YUV で観測した値（frame 13・14 の最後の layer）を、下の alpha 254 の画素でそのまま再現する。
  - 下の alpha 254・255 について、色・texel の alpha・不透明度を総当たりして既存の oracle と比べる
    （573,352 byte。oracle が定義域の外として拒否する 22 画素は比べず、数を残す）。
- [事実] 修正前の計算（割り算）へ戻す変異で、この試験は「下の alpha が 254 の画素が MLT の観測値と一致する」
  で落ち、実際の MLT との全 frame 比較も元の 6 frame・6 画素の不一致へ戻った（下の 5）。

## 3. 比較の結果の分離（Task A・B・C）

検証 harness（`mvm_math_score_export_parity --compositor-all 1`）を直した。

- 1 frame の結果を 3 つに分ける。
  - **COMPARED**: 期待値を作り、MLT の pre-YUV と全画素を比べた。
  - **UNSUPPORTED**: 事前に決めた対象外だけ。拡縮・回転・shear・crop（`unsupported-scale-rotation-shear-crop`）、
    Write（`unsupported-math-write`）、変形（`unsupported-math-transform`）、数式でない layer
    （`unsupported-non-math-layer`）。frame 番号をすべて report に残す。
  - **ERROR**: それ以外のすべて。背景・成果物・mapping・計画の値・画像の準備の失敗、合成器の想定外の拒否、
    pre-YUV の大きさの違い。1 つでもあれば比較は不成立になる（`verdict` が `error`）。
  P3-1.1 の harness は、期待値を作れなかった frame を理由によらず「対象外」に数えていた。
- 準備済みの画像は、1 回の比較 run が所有する文脈（`CompositorRun`）が持つ。関数内の static な cache は
  なくした。識別は、成果物 file の SHA-256・数式の入力（記法・式・文字の大きさ・色・背景色）・出力の大きさ・
  前処理（qtcrop の有無）で決まる。run の終わりに、成果物 file と準備済みの画素の SHA-256 が準備の時から
  変わっていないことを確かめる。
- 不一致は、画素ごとに frame・座標・違う channel・両方の RGBA・重なる layer の一覧（clip の ID・順序・
  計画の位置と不透明度・成果物と準備済みの画像の SHA-256・qtcrop の有無）・layer ごとの途中の値を残す。
  上限は 256 画素で、超えた場合は `mismatches_truncated` が立つ（数は全部数える）。
  修正前の 6 画素は、打ち切りなしで全部残っている。

## 4. 範囲と下地の alpha（Task F）

合成器の公開の入口を変えた（`src/media/math/math_group_compose.h`）。

- layer の画像は `PreparedMathRaster` で渡す。`prepare` だけが中身を作り、画素を複製して持ち、字形の範囲を
  画素から求める。作った後に画素も範囲も変える口が無く、外から範囲（矩形）を渡す口も無い。
- `composeMathGroup` は、先にすべての入力を確かめ、対象外が 1 つでもあれば **何も書かずに** 拒否する。
  - 準備していない画像、出力と大きさの違う画像（拡縮）、範囲外の不透明度。
  - 合成を始める前の下地に、不透明でない画素（alpha が 255 でない）が 1 つでもある場合。
    透明・半透明の下地は実際の MLT と比べていないので、対象外として拒否する。別の計算で代わりに
    合成することはしない。
- 合成の途中で alpha が 254 になるのは対象の中である（MLT と同じ結果で、全 frame の比較が通っている）。
- 範囲と下地を確かめない入口は `detail::composeMathGroupUnchecked` として残した。単体試験
  （誤った範囲を渡す対照、下の alpha 254 の検査）と、harness の画素の trace だけが使う。

[未検証] 下地の検査は、呼び出しごとに全画素の alpha を 1 回走査する。この分のコストは計っていない。

## 5. 対照

変異（`pwsh scripts/math-compositor-p312-mutations.ps1`）。合成器の source を 1 か所だけ書き換え、
最後に元へ戻した（SHA-256 が変異の前と一致、復元後の試験は通過）。5 つとも build は成功し、
意図した検査で落ちた。

| 変異 | 落ちた検査 |
| --- | --- |
| 下の alpha を 255 で割る（修正前の計算） | 「下の alpha が 254 の画素が MLT の観測値と一致する」。MLT との比較も 6 frame・6 画素の不一致 |
| alpha を四捨五入で 8 bit にする | 「整数位置で oracle と全 byte 一致する」 |
| 不透明度を 255 で割る | 同上 |
| 準備が求める範囲を 1 列短くする | 「字形の範囲は画素から求まる」 |
| 下地の alpha の検査を外す | 「不透明でない下地を拒否し、何も書かない」 |

比較そのものの対照（`cmp-integer` へ故障を入れる）は 6 の表にある。

## 6. 全 frame の比較（最終の source）

`pwsh scripts/math-compositor-p312-verify.ps1`。

[事実] `20261012-134955-all-frames/`。

| fixture | 想定 | 観測 | COMPARED | 欠落 | UNSUPPORTED | ERROR | 不一致の frame | 対象外の frame |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `cmp-integer` | 120 | 120 | 120 | 0 | 0 | 0 | 0 | なし |
| `cmp-fraction-fade` | 120 | 120 | 120 | 0 | 0 | 0 | 0 | なし |
| `cmp-layers-16` | 120 | 120 | 120 | 0 | 0 | 0 | 0 | なし |
| `cmp-layers-48` | 120 | 120 | 120 | 0 | 0 | 0 | **0** | なし |
| `cmp-region-motion` | 120 | 120 | 120 | 0 | 0 | 0 | 0 | なし |
| `cmp-region-scale` | 180 | 180 | 61 | 0 | 119 | 0 | 0 | 61〜179 |
| `p2-window-early` | 60 | 60 | 60 | 0 | 0 | 0 | 0 | なし |
| `p2-window-middle` | 60 | 60 | 60 | 0 | 0 | 0 | 0 | なし |
| `p2-window-late` | 60 | 60 | 60 | 0 | 0 | 0 | 0 | なし |

- 比べた 841 frame がすべて全画素・4 channel で一致した。欠落 0、ERROR 0。
- 対象外は `cmp-region-scale` の frame 61〜179 の 119 frame だけで、理由はすべて
  `unsupported-scale-rotation-shear-crop`（拡縮する layer）。一致には数えていない。
- 数式の画素を含む frame は、`cmp-fraction-fade` が 119（1 frame は不透明度 0）、他は比べた全 frame。
- 使った入力（成果物と準備済みの画像）は、どの run でも準備の時から変わっていない。

比較そのものの対照（`cmp-integer` へ故障を入れる。どれも不成立にならなければならない）:

| 対照 | 終了 | verdict | COMPARED | 欠落 | UNSUPPORTED | ERROR |
| --- | --- | --- | --- | --- | --- | --- |
| 成果物なし | 8（不成立） | `error` | 0 | 0 | 0 | 120 |
| 成果物を読めない | 8 | `error` | 0 | 0 | 0 | 120 |
| mapping なし | 8 | `error` | 0 | 0 | 0 | 120 |
| frame の欠落（1 frame） | 8 | `error` | 119 | 1 | 0 | 0 |

4 つとも、壊れた入力は UNSUPPORTED にも一致にも数えられなかった。

## 7. gate（Task G）

| gate | 結果 |
| --- | --- |
| 合成器の単体試験（`math_group_compose_oracle`） | 通過。oracle と 1,064,872 byte を比べて一致 |
| 実際の MLT の pre-YUV との全 frame 比較（9 fixture） | 通過（上の 6） |
| 丸め・範囲・mapping・欠落の対照 | 変異 5 つ、比較の対照 4 つをすべて検出 |
| 関連の CTest（名前が `math`・`mlt_export`・`m7b_3_product_export`・`timeline_export` に合う 49 件） | 48 通過、1 件が 1 回落ちた（下） |
| lint | 通過 |

- [事実] 49 件を続けて実行したとき、`math_equation_sequence_product_ui` が開始 0.72 秒で SegFault になった。
  同じ binary でこの試験だけを 3 回実行すると 3 回とも通過した（各 約 166 秒）。
  [未検証] 落ちた原因は分かっていない。この試験は合成器を呼ばない（合成器は製品から呼ばれていない）が、
  今回の変更と無関係だとは確かめていない。
- 全 CTest は実行していない。
- 最終の比較（`20261012-134955-all-frames/`）の `identity.json` にある 14 file の SHA-256 は、gate の後の
  作業 tree・binary・runtime と一致している（合成器の source は `89E89DF1…3532C68`）。
  変異の run は、復元後の SHA-256 が変異の前と一致している。
- `diagnosis/` の調査の run は、harness の binary の SHA-256 だけを残している（source の SHA-256 は無い）。
  修正前の 6 画素の完全な記録は、source の SHA-256 も残る
  `20261012-134629-mutation-previous-divide-destination-alpha/` にもある。
- 変異の正しい run は `20261012-134606-mutation/` である。`20261012-134006-…`・`20261012-134046-…`・`20261012-134250-…`・`20261012-134312-…` は、検証 script 自身の不具合
  （表示用の関数の添字、出力の取り込み）で途中終了・誤判定した run である。結果として使わず、消さずに残した。

## 8. 残っていること

- 拡縮する layer（`cmp-region-scale` の 119 frame）は対象外のままである。合成器は対応していない。
- 透明・半透明の下地は対象外（拒否）で、MLT との一致は確かめていない。
- 合成器の対象は、等倍の静止の数式で、motion のある clip には qtcrop と同じ描き直しを前処理として
  通す、という P3-1.1 の契約のままである。
- MLT への組み込み（P3-3）は、この phase では行っていない。
