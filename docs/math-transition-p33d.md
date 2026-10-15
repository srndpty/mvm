# P3-3D: 数式 group を使ってよいかの事前の判定と、並行・寿命の確認

2026-10-12〜13。[P3-3C](math-transition-p33c.md) で残った 3 つのうち、次の 2 つを扱った。

1. 下地に alpha 0 があると、数式 group を使う書き出しが実行時に失敗する。書き出しの前に判定して、
   言えないときは従来の graph で書き出す仕組みが無い。
2. transition と準備済みの画像の寿命・並行を、race の検出器で確かめていない。

3 つ目（v4 の Project 全体 6840 frame の書き出し）は、この phase では行っていない。

製品の書き出しの経路と timeout、MLT の affine、既存の画素 oracle、合成の式、範囲の座標の原点は変えていない。
数式 group は、製品の書き出し（observer が NULL）からは動かない。commit・push はしていない。
P1〜P3-3C の成果物と判定は変更していない（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。

証拠は `build/math-transition-p33d-20261012/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| A. 事前の判定（ELIGIBLE / INELIGIBLE / UNKNOWN） | 実装した。ELIGIBLE のときだけ group を使う graph を作る |
| B. 従来の graph への切り替え | 成立。21 通りの判定が期待どおり。INELIGIBLE・UNKNOWN は従来の graph で書き出して成功し、製品の経路と全 frame 一致 |
| C. 画像の共有と所有の点検 | 共有の条件を「出どころと内容の両方が同じ」に改めた。所有の点検で不具合は見つかっていない |
| D. 並行 | **一部だけ検出器で確認。** 合成器・画像・置き場は ThreadSanitizer・AddressSanitizer で報告 0。MLT の transition と書き出しは検出器の対象外で、構造の点検と stress で確かめた |
| E. 画素の一致 | 成立。判定つきの経路で、25 の fixture・区間（2,640 frame）が製品の経路と全 frame 一致。対照 12 個を検出 |
| F. gate と provenance | 関連 CTest 50/50、lint 通過。最終の 3 つの run の SHA-256（17 file）が作業 tree・binary と一致 |

**全体: この phase の目的（1・2）は、2 の検出器の範囲を除いて達成。** 製品へ入れる前に残るものは 7 にある。

## 1. 事前の判定（Task A）

### 1.1 何を言えば ELIGIBLE か

合成器は、合成を始める前の下地の alpha が 1 以上であることを要求する（P3-3B）。group の下地は、
V1 の画素へ、group より下の layer（従来の affine と、前の group）を重ねたものである。次の 2 つが言えれば、
下地の alpha は 1 以上になる。

- **(O1)** group の member が見えている全 frame で、V1 の画素の alpha が 1 以上。
- **(O2)** 下の layer を重ねても、alpha が 1 以上の画素は 1 以上のまま。

### 1.2 (O1) の確かめ方

- V1 の clip ごとに「alpha の証明」を持たせる（`MvmExportClip::math_group_base_alpha_proof`）。上位の層
  （`timeline_export.cpp`）が、**stage して MLT へ渡す画素を全部走査した**ときだけ、最小値を入れる。
  対象は、stage した静止画（画像・文字・静止の数式）である。
- 証明を入れないもの: 動画、連番（Write・変形・数式の連続・graph）、字幕。全 frame の画素を見ていないので、
  種類や拡張子から alpha を推測しない。
- V1 の隙間は、書き出しが黒の color producer（`#000000`）で埋める。alpha は 255 として扱う。
- V1 の clip に effect・crop・motion があるときは、証明があっても使わない（affine filter や qtcrop が掛かり、
  出す画素を確かめていない）。
- 判定（`mvm_mlt_export.c` の `math_group_preflight`）は、member と同じ frame に見える V1 の clip だけを調べる。

| 判定 | 条件 | 理由の名前 |
| --- | --- | --- |
| INELIGIBLE | group にできる clip が 1 つも無い | `no-eligible-math-clip` |
| INELIGIBLE | 調べた V1 の clip のどれかで、alpha の最小値が 0 だと分かっている | `base-alpha-zero` |
| UNKNOWN | 調べた V1 の clip に、effect・crop・motion のあるものがある | `base-clip-transformed` |
| UNKNOWN | 調べた V1 の clip に、証明の無いものがある | `base-alpha-unproven` |
| ELIGIBLE | 上のどれでもない | `base-alpha-proven` |

INELIGIBLE は UNKNOWN より優先する（alpha 0 が実際にあれば、ほかに分からない clip があっても INELIGIBLE）。

### 1.3 (O2) の根拠

(O2) は layer ごとには確かめず、MLT の合成後の alpha の式に頼る。

- [事実] MLT の `interp.h` の補間の関数（nearest・bilinear・bicubic）は、どれも
  合成後の alpha = 上 + 下 − 上 × 下 を 255 倍して 8 bit へ切り捨てる（7.36.1 の source）。
- [事実] この式で、下の alpha が 1 のとき、上の alpha（0〜1 の binary32 の **全部の値**、1,065,353,217 個）に
  対して、合成後の alpha は 1 以上だった。下の求め方は、1/255 を掛ける形と 255 で割る形の両方で確かめた
  （CTest `math_group_alpha_floor_lemma`、反例 0）。
- [事実] 下の alpha が 2〜255 のときは、上の値を間引いて確かめ、合成後の alpha の最小は 1 だった。
  **alpha そのものは下がることがある**（下が 255 のとき 254 になる場合がある。下がる幅は最大 1 で、
  間引いた範囲では 255 以外では下がらなかった）。
- [推測] 下の alpha が 2 以上のとき、合成後の値は厳密には下の値以上で、binary32 の丸めの誤差
  （3 回の演算、相対で 10⁻⁷ 程度）を引いても 255 倍して 1 を下回らない。全部の値を計算機で確かめたのは
  下が 1 の場合だけである。

**何が証明で、何が前提か**:

- 証明したこと: V1 の stage した静止画の alpha の最小値（全画素の走査）。
- 前提にしていること: (a) MLT が stage した PNG を、同じ alpha のまま合成へ渡す、(b) 従来の layer の合成が
  上の式に従う、(c) V1 の隙間の color producer の alpha が 255。(a) は、背景の alpha が 254・253・128・1・0 の fixture と、
  全値を散らした背景で、MLT の出力の alpha の最小値が証明の値と同じだったことで裏づけている
  （254・253・128・1・0・1）。
  (b) は source の読解と上の検査、(c) は MLT の色の解釈（6 桁の色は alpha 255）による。
- 前提が崩れた場合の備え: 実行時の下地の検査（合成器が alpha 0 を拒否し、書き出しが失敗する）は、
  判定に関わらず残っている。判定が ELIGIBLE でも、下地に alpha 0 を作る故障を入れると書き出しは
  失敗する（3 の対照）。**黙って誤った画素を出すことはない。**

### 1.4 render せずに決められるか

- 決められる: V1 が stage した静止画・隙間だけで、effect が無い場合（v4 の Project はこれに当たる）。
- 決められない: V1 に動画がある場合、V1 の clip に effect がある場合。全 frame を decode・合成しないと
  alpha の最小値が分からないので、UNKNOWN になり、従来の graph を使う。
- 従来の layer が作る画素の alpha は、render せずには分からない。(O2) により「1 以上のまま」とだけ言う。

### 1.5 graph の選び方

- observer の入力 `diagnostic_math_group = 3` のとき、graph を作る前に判定し、ELIGIBLE なら数式 group の
  graph、INELIGIBLE・UNKNOWN なら **従来の graph（製品の書き出しと同じ code の道）** を作る。
  frame ごとに graph を切り替えることはしない。作った後で切り替えることもしない。
- 判定・理由・実際に作った graph・判定に使った alpha の最小値は、observer へ記録する。
- `diagnostic_math_group = 1`（判定に関わらず group を使う）は、対照のために残してある。
- 製品の書き出しは observer に NULL を渡すので、いつも従来の graph である。**この phase では、製品で
  数式 group を有効にしていない。**

## 2. 従来の graph への切り替え（Task B）

`pwsh scripts/math-transition-p33d-verify.ps1`。candidate は mode 3。fixture ごとの期待する判定と理由は、
実行の前に script に書いてある。candidate の全 frame を、同じ process で先に書き出した製品の経路と
byte 単位で比べる。

[事実] `20261012-230213-final-decisions/`。21 通りすべてが期待どおり。

| fixture | 判定（理由） | 作った graph | 製品の経路との比較 |
| --- | --- | --- | --- |
| 既存の 9 fixture（背景は不透明な画像） | ELIGIBLE（`base-alpha-proven`、V1 の alpha 255） | 数式 group | 全 frame 一致 |
| `alpha-255`・`254`・`253`・`128`・`1`・`all-nonzero` | ELIGIBLE（V1 の alpha 255・254・253・128・1・1） | 数式 group | 全 frame 一致 |
| `alpha-0`・`alpha-ramp`（背景に alpha 0） | INELIGIBLE（`base-alpha-zero`） | 従来 | 全 frame 一致（書き出しは成功） |
| `cmp-region-scale`（従来の layer と混ざる） | ELIGIBLE | 数式 group（34 clip、affine 4） | 全 frame 一致 |
| `geom-mixed`（16 層のうち拡縮・回転・crop が 1 層ずつ） | ELIGIBLE | 数式 group（13 clip を 4 つの group、affine 3） | 全 frame 一致 |
| `geom-all-unsupported`（全層を回転） | INELIGIBLE（`no-eligible-math-clip`） | 従来 | 全 frame 一致 |
| `base-transformed`（V1 を 110% に拡大） | UNKNOWN（`base-clip-transformed`） | 従来 | 全 frame 一致 |
| `cmp-layers-16`、証明を無いものとして扱う | UNKNOWN（`base-alpha-unproven`） | 従来 | 全 frame 一致 |

- INELIGIBLE・UNKNOWN の 5 通りは、group を 1 つも使わず、affine の数が製品の経路と同じ（16）である。
- `alpha-0` は、判定に関わらず group を使う mode（P3-3B・P3-3C）では書き出しが失敗していた。mode 3 では
  従来の graph で成功する。
- 「証明が得られない素材」は、V1 に動画を置いた fixture ではなく、証明を無いものとして扱う入力で
  作った（v4 の Project に動画が無いため）。動画を V1 に置いた実際の書き出しは試していない [未検証]。
- 対象外の配置（拡縮・回転・crop）の clip は、従来の affine のまま書き出されている。

出力 file（主の書き出しそのものを mode 3 で行う。`20261012-233529-final-controls/`）:

| 場合 | 判定 → graph | 書き出し | 最終 file | 一時 file |
| --- | --- | --- | --- | --- |
| 不透明な背景 | ELIGIBLE → 数式 group | 成功 | ある | 無い |
| 証明なし | UNKNOWN → 従来 | 成功 | ある | 無い |
| 背景に alpha 0 | INELIGIBLE → 従来 | 成功 | ある | 無い |
| 実行時に下地を拒否（故障） | ELIGIBLE → 数式 group | 失敗（終了コード 6） | 無い | 無い |
| 途中で取消 | ELIGIBLE → 数式 group | 取消（終了コード 11） | 無い | 無い |

最終 file ができるのは、書き出しが成功を返したときだけである。

## 3. 画素の一致と対照（Task E）

[事実] 判定つきの経路（mode 3、準備済みの画像は範囲だけ）:

- 上の 21 通り（2,400 frame）と、Project 全体の graph の 4 区間（240 frame。ELIGIBLE、97 clip、affine 23、
  画像 2.81 MB）が、製品の経路と全 frame・全 byte 一致した。欠落・比べなかった frame は 0。
- 並行の確認の 6 通り（4）でも、candidate を 3 回ずつ書き出して、どの回も全 frame 一致した。

対照（`cmp-layers-16`。mode 3）:

| 対照 | 期待 | 結果 |
| --- | --- | --- |
| 合成を 1 frame 飛ばす | 画素の不一致 | 検出（119/120 一致） |
| 1 つ後の frame の値を使う | 画素の不一致 | 検出（91/120） |
| 不透明度を半分にする | 画素の不一致 | 検出（0/120） |
| 別の member の画像を使う | 画素の不一致 | 検出（0/120） |
| 一番上の layer を落とす | 画素の不一致 | 検出（0/120） |
| 下地に alpha 0 の画素を作る（mode 3。判定は ELIGIBLE） | 実行時に拒否して失敗 | 検出 |
| 同上（mode 1） | 実行時に拒否して失敗 | 検出 |

合成器の変異（最終の source で、最終の build の前に実行）:

- P3-3C の 4 つ（範囲の原点のずれ、補間の縁の欠落、範囲の外を不透明として読む、alpha の割り算）:
  単体試験の意図した検査と、実際の MLT との比較（画素の不一致）の両方で検出。
- P3-3B の 5 つ（alpha の割り算、下限を 0 にする、下限の検査を外す、最小値の走査が最後の行を見ない、
  範囲を 1 列短くする）: 単体試験の意図した検査で検出。
- どちらも source は SHA-256 が一致する形で復元した。

## 4. 画像の共有と所有の点検（Task C）

### 4.1 共有の条件を改めた

P3-3C は、成果物の path・見た目を決める入力・前処理が同じなら、**画素を比べずに**同じ画像を使っていた。
同じ path の成果物が途中で書き換わっていれば、別の内容に対して前の画素を使ってしまう。

- 置き場（`PreparedMathRasterPool`）を作り、共有するのは **出どころ（上の文字列）が同じで、かつ内容が
  完全に同じ**ときだけにした。内容は、大きさ・持ち方・範囲・照合用の値に加えて、保存した全 byte を比べる
  （照合用の値だけでは決めない）。
- 画像は clip ごとに必ず準備し、置き場へ入れるときに照合する。
- 単体試験: 同じ出どころで 1 byte だけ違う画像は別に持つ（同じ path の成果物の書き換え）。内容が同じでも
  出どころ・前処理が違えば共有しない。内容が違った後も、それぞれの内容ごとに共有する。
- v4 の Project では、97 clip に対して 81 枚で、P3-3C と同じである（出どころが同じで内容が違った回数は 0）。

[未検証] 「同じ path の成果物が、書き出しの途中で実際に書き換わる」場合を、書き出し全体では試していない
（単体試験だけ）。

### 4.2 所有と寿命の点検（code の読解）

| 見たもの | 分かったこと |
| --- | --- |
| 準備済みの画像 | 作った後に変える口が無い。画素は参照数つきで共有し、置き場が書き出しの関数から戻るまで持つ |
| group の transition が持つ参照 | 画像への pointer と、frame ごとの値の複製。値は作った後に書き換えない |
| frame が持つ参照 | `process` で frame に transition の参照を 1 つ持たせ、frame が閉じるときに返す。同じ frame で 2 回呼ばれても増減は釣り合う |
| 取消の間の callback | 待ち loop が consumer を止め、consumer は worker を join する。その後で tractor を閉じる。実行中の callback が 0 になってから画像が解放される |
| consumer と tractor を閉じる順 | consumer（frame を持つ）が先、tractor（transition を持つ）が後。順が逆でも、frame が参照を持つので transition の状態は残る |
| 失敗・早い return の道 | group の配列を確保した後の失敗は、すべて解放の処理を通る。transition の作成・植え付けの失敗は、状態を解放して数を戻す |
| observer の counter | worker からは interlocked で足す。最初の失敗の記録は、内容を書いてから flag を立て、読む側は flag を見てから読む |
| 画像の置き場 | `intern` は書き出しを始める前に 1 つの thread から呼ぶ。worker は読むだけ |

- 上の点検で、直すべき不具合は見つかっていない。
- **限界**: これは読解による確認で、検出器による確認ではない。書き出しの後の counter（生きている
  transition・実行中の callback・生きている画素）が 0 であることは、解放の漏れが無いことの確認であって、
  「解放の後に参照していない」ことの証明ではない。後者は、MLT が consumer を閉じた後に callback を
  呼ばない、という MLT の挙動に依っている（P3-3A の 2.5）。

## 5. 並行の確認（Task D）

### 5.1 検出器で確かめた範囲

`scripts/math-transition-p33d-sanitizers.sh`（WSL の Ubuntu 22.04、g++ 11.4）。この環境の Windows の
toolchain（MinGW の gcc）には ThreadSanitizer が無いので、Linux で build できる部分だけを対象にした。

| 対象 | 検出器 | 結果 |
| --- | --- | --- |
| 同時の合成（8 thread × 200 回。置き場の画像を共有して読む） | ThreadSanitizer | 報告 0 |
| 単体試験の全部（置き場、範囲だけを持つ画像、同時の合成を含む） | ThreadSanitizer | 報告 0 |
| 対照: 2 thread が同じ合成先へ書く（意図した race） | ThreadSanitizer | 報告 4（data race として検出。検出器は働いている） |
| 単体試験の全部・同時の合成 | AddressSanitizer + UndefinedBehaviorSanitizer | 報告 0 |

- 対象は `src/media/math/math_group_compose`（合成器、準備済みの画像、置き場）だけである。
- **対象外**: MLT の transition（`mvm_mlt_math_group.c`）、書き出し（`mvm_mlt_export.c`）、上位の層
  （`timeline_export.cpp`）、MLT そのもの。Windows の API と MLT に依存するので、この検出器では build できない。
- Linux の g++ と Windows の MinGW では、生成される code が違う。検出器が見たのは Linux の binary である。

### 5.2 検出器の対象外を、stress で確かめた範囲

[事実] `20261012-233529-final-controls/`。procdump の下で実行し、未処理の例外があれば full dump を採る。

| 内容 | 回数 | 結果 |
| --- | --- | --- |
| worker 1・4・8 本 × `cmp-layers-48`・`cmp-region-scale`。同じ process で candidate を 3 回続けて書き出す | 6 通り（candidate 18 回） | 全部の回で全 frame 一致。失敗 0、残り 0、dump 0 |
| すぐの取消: 合成済みの画像が 0・1・8 枚の時点 × worker 1・4・8 本 | 9 通り | 全部が取消で終わり、transition・callback・画素の残り 0、出力 file なし、dump 0 |
| 画像の準備の失敗（上限 50 MB、全画面の持ち方） | 1 | graph を作る前に失敗を返す。残り 0、dump 0 |
| consumer の終了（正常終了） | 上の全部 | 残り 0 |

- 取消が合成の前に効いた場合（合成した frame 0）と、途中で効いた場合（1〜25 frame）の両方が含まれる。
- 同じ frame が複数回要求される場合（120 frame に対して callback は 145 回前後）は、どの書き出しにも含まれる。
- crash は 1 回も起きていない。dump は 0 件。

[未検証]

- MLT の transition と書き出しの data race を、検出器では見ていない。
- stress は、起きうる並行の順序の一部を実行しただけである。「race が無い」ことの証明ではない。
- `math_equation_sequence_product_ui` の SegFault（P3-3A）は、この phase でも再現しておらず、原因は
  分かっていない。UNKNOWN のままである。

## 6. memory と時間（退行の確認）

[事実] `20261013-000613-final-timing/`。Project 全体の graph の各区間（60 frame）を、別々の process で
B（製品の経路）、R（mode 3）、R、B の順に書き出した。

| 区間 | render 秒 B / R | 書き出し全体の秒 B / R | process の peak（MB）B / R |
| --- | --- | --- | --- |
| 1200〜 | 36.4・36.5 / 2.8・2.8 | 47.1・47.3 / 13.2・13.3 | 1,434 / 1,239・1,241 |
| 3400〜 | 63.9・49.1 / 1.8・1.8 | 74.8・55.8 / 8.1・8.1 | 1,559・1,561 / 1,239 |
| 5280〜 | 31.3・31.7 / 3.4・3.3 | 37.9・38.4 / 9.7・9.8 | 1,446・1,445 / 1,276・1,278 |
| 6500〜 | 41.9・49.2 / 3.0・3.1 | 48.8・60.3 / 13.6・13.8 | 1,421・1,431 / 1,225 |

- 4 区間とも判定は ELIGIBLE で、数式 group の graph になった。B・R の出力の全 frame の digest は一致し、
  encoder へ渡した frame と mux した packet は 60 ずつである。
- memory は P3-3C と同じである: 準備済みの画像 2.81 MB（81 枚。97 clip のうち 16 回は共有）、
  process の peak は製品の経路より約 150〜320 MB 少ない。書き出しの後に生きている画素は 0。
- 画像の準備は 467〜719 ms（P3-3C は 575〜593 ms）。clip ごとに必ず準備して内容を照合する形にした分は、
  この幅の中に収まっている。
- **この run の時間は、P3-3C より揺れが大きい。** 同じ区間の 2 回の B が 3 割違うものがあり（3400〜の
  63.9 秒と 49.1 秒）、区間 3400・5280 は全体に速い。run の途中で機械の状態が変わったと考えられる
  [推測]。R の render（1.8〜3.4 秒）は P3-3C（2.8〜5.4 秒）より遅くなってはいないが、この表は退行が
  無いことの確認であって、速さの新しい計測としては使わない。
- 1 回の callback: 合成 5.9〜8.7 ms、合成先の確保と複製 4.0〜4.9 ms（P3-3C と同じ程度）。

## 7. 製品へ入れる前に残るもの

1. **v4 の Project 全体（6840 frame）の書き出しでの確認**（この phase では行っていない）。
2. **MLT の統合部分の race の検出**。検出器の下で build する手段（別の toolchain など）が要る。
3. ELIGIBLE の判定が、MLT の挙動についての前提（1.3 の (a)〜(c)）に依っている。前提が崩れたときは
   実行時に書き出しが失敗する。その場合に従来の graph で自動的にやり直すかどうかは決めていない。
4. V1 に動画がある Project は UNKNOWN になり、数式 group を使えない（速くならない）。
5. 合成先の確保と複製（1 回 約 5 ms）は変えていない。
6. `math_equation_sequence_product_ui` の SegFault は UNKNOWN のままである。
7. 製品の書き出しで数式 group を有効にする入口は、まだ無い（observer の入力だけ）。

## 8. gate と provenance

| gate | 結果 |
| --- | --- |
| 合成器の単体試験（`math_group_compose_oracle`。置き場・同時の合成を含む） | 通過 |
| alpha の下限の性質（`math_group_alpha_floor_lemma`。2,263,259,004 通り） | 通過（反例 0） |
| 判定と parity（21 通り + 4 区間） | 通過 |
| 対照（12 個）、出力 file（5 通り）、stress（16 通り） | 通過 |
| 合成器の変異（P3-3C の 4 つ、P3-3B の 5 つ） | 検出 |
| 検出器（ThreadSanitizer、AddressSanitizer + UBSan。Linux、合成器の部分だけ） | 報告 0。対照の race は検出 |
| 関連の CTest（`math`・`mlt_export`・`m7b_3_product_export`・`timeline_export` に合う 50 件） | 50/50 通過（`ctest-final.log`） |
| lint | 通過 |
| 全 CTest | 実行していない |

provenance:

- source を書き換える変異の script は、最終の build の **前** に実行した（`20261012-225123-mutation/`・
  `20261012-225922-mutation/`）。変異の後の build が最終の build で、その後は build していない。
- 最終の 3 つの run（`20261012-230213-final-decisions/`、`20261012-233529-final-controls/`、
  `20261013-000613-final-timing/`）は、同じ binary（harness の SHA-256 `5DC0099B…86A9EC93`）で実行した。
  `identity.json` の 17 file（harness と単体試験の binary、MLT・Qt の DLL、Project、関係する source、
  script）は、3 つとも、gate の後の作業 tree・binary と一致している。
- 検出器の run は、同じ source を WSL の g++ で build したもの（`sanitizers/identity.txt` に source の
  SHA-256 と compiler）。Windows の binary ではない。
- CTest は最終の binary で実行した。
- git の HEAD は `dd22535`（P3-3C）で、P3-3D の変更は commit していない。
- `trial/` は、script を用意する前の試しの run。
- P3-3C の最終の run の harness の binary が、その後の build で変わっていた件（P3-3C の文書）は、
  そのままである。今回は同じことを避けた。
