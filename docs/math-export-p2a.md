# 数式 layer の書き出し高速化 P2-A: Project 全体で残る遅さの原因

2026-10-12。[P1](math-sparse-raster-p1.md) では、Project 全体（v4、6840 frame）が同じ内容の短い区間より
1 frame あたり大きく遅いこと、その原因として keyframe の文字列化が 1 回の stack に見えたことまでが
分かっていた。因果は未確定だった。ここではそれを測って確かめ、最小の試作を全画素比較にかけた。

**製品の経路は変えていない。** 追加したのは既定で無効の診断用の入力と試作だけである
（`MvmExportObserver` の `diagnostic_*`。製品は observer を渡さない）。`timeout_ms` の契約、keyframe の
間引き、P1 までの成果物と FAIL/PASS の記録は変更していない。commit・push はしていない。

証拠は `build/math-export-p2a-20261012/`（run ごとの `identity.json` に source と runtime の SHA-256、
実行した行と回数）。再現は `pwsh scripts/math-export-p2a-matrix.ps1`。

## 判定

| 項目 | 判定 |
| --- | --- |
| 文字列化が Project 全体の遅さの主因か | **PASS（確認）**。区間との差の 81〜93% を説明する（1 件だけ 66%） |
| transition の分割で画素が変わらないか | **PASS**。1320 frame が全画素・完全一致。対照 4 種は検出 |
| transition の分割で Project 全体が速くなるか | **HOLD**。文字列化は消えるが、transition の数が増えた分の別のコストが出て、改善が一定しない |
| 製品の 10 分 timeout | **HOLD**。達成の根拠が無いので、v4 全体の製品条件での実行は行っていない |
| timeout の数え方 | **確認**。`timeout_ms` = 1000 が実時間 3418 ms で timeout した。修正は未着手 |

## 1. 測り方

- **文字列化の時間**: MLT の affine（libmltplus）と qtcrop（libmltqt6）が libmlt から呼ぶ
  `mlt_properties_get`・`mlt_properties_anim_get_rect`・`mlt_properties_anim_get_double` を、process 内で
  import 表の番地を差し替えて計時した（`tests/harness/mlt_property_probe.h`）。MLT は作り直しておらず、
  DLL の file も変えていない。終了時に元の番地へ戻す。`mlt_animation_serialize_cut_tf` は libmlt の内部で
  呼ばれるので直接は計れない。代わりに、animation の付いた property（`rect`・`fix_rotate_x`・
  `fix_shear_x`）を文字列で読む `mlt_properties_get` の時間を「文字列化の時間」とした。この呼び出しは
  毎回 animation 全体を文字列へ直す（`mlt_property.c` の `refresh_animation` が、API で作った animation にも
  文字列から作った animation にも serialiser を付ける）。時間は呼び出した thread の経過時間の和で、
  mutex の待ちを含む。
- **合成の時間**: 合成後の画像の要求から戻りまで（`image_ns_total`）。検査と encoder は含まない。
  render worker が複数のとき、他の worker が lock を持っている間の待ちもここに入る。
- **keyframe の数だけを変える**: `diagnostic_keyframe_stride`。値の変わらない区間で落とす keyframe の一部を
  残す。前後と同じ値なので、評価される値と画素は変わらない（digest で確認した）。
- **graph を縮めずに区間だけ書き出す**: `diagnostic_first_frame`・`diagnostic_frame_count`。graph は
  Project 全体のまま、tractor の seek と out だけを変える。この出力は Project の書き出しとしては不完全なので、
  parity（独立期待値・復号）は判定しない。

## 2. 行列と結果

行列は実行前に固定した（29 行 × 2 回 = 58 run。`scripts/math-export-p2a-matrix.ps1`）。各 run の上限は
40 秒で、58 run すべてが計測に使えた（完走、または上限で進行中に打ち切り）。値は 2 回の平均。

### keyframe の数（1 layer、画素は同じ）

| keyframe | 合成 ms/frame | 文字列化 ms/frame | それ以外 |
| --- | --- | --- | --- |
| 31 | 4.7 | 0.7 | 4.0 |
| 87 | 6.0 | 1.8 | 4.2 |
| 255 | 10.4 | 5.7 | 4.7 |
| 480 | 19.7 | 13.4 | 6.3 |
| 1920 | 113.3 | 96.2 | 17.1 |

[事実] 同じ fixture（480 frame）の 4 通りは pre-YUV が全 480 frame 一致し、1920 frame の 2 通りも比べられた
405 frame が一致した。画素を変えずに keyframe を増やすと、合成時間は文字列化の時間の分だけ増える。
文字列化の時間は keyframe 数の 1.0〜1.4 乗で増える（31→87 で 0.96、255→480 で 1.34、480→1920 で 1.42）。

### layer の数（1 layer の keyframe を 480 に固定）と、Project の長さ

| 条件 | keyframe 合計 | 合成 ms | 文字列化 ms | CPU 時間 / 経過時間 |
| --- | --- | --- | --- | --- |
| 4 layer・最小 | 55 | 47.4 | 1.5 | 6.29 |
| 4 layer・480 ずつ | 6,240 | 218.4 | 144.9 | 2.77 |
| 16 layer・最小 | 287 | 241.8 | 7.3 | 4.55 |
| 16 layer・480 ずつ | 29,280 | 1,263.5 | 606.1 | 2.14 |
| 48 layer・最小 | 3,148 | 763.6 | 86.0 | 3.91 |
| 48 layer・480 ずつ | 59,040 | 2,953.8 | 1,265.8 | 2.14 |
| 16 layer・120 frame・最小 | 151 | 226.0 | 3.7 | 4.61 |
| 16 layer・1920 frame・最小 | 2,766 | 381.0 | 78.9 | 3.68 |

[事実] 文字列化の時間は layer の数にほぼ比例する（1 layer あたり 26〜38 ms）。keyframe を増やすと実効の
並列度が 4〜6 core から 2.1〜2.8 core へ落ちる。Project を長くしただけ（16 layer、120 → 1920 frame）でも、
残る keyframe が 151 → 2,766 に増え、文字列化が 3.7 → 78.9 ms になる。

### 同じ 60 frame を、区間だけの Project と、Project 全体の graph で

| 区間 | 渡し方 | 合成 ms: 区間 → 全体 | 文字列化 ms: 区間 → 全体 | 差のうち文字列化 |
| --- | --- | --- | --- | --- |
| 前半（20 秒） | 全画面 | 310 → 441 | 4.4 → 119.6 | 88% |
| 前半 | 左上原点 | 143 → 293 | 4.2 → 126.5 | 81% |
| 中盤（56.7 秒） | 全画面 | 635 → 925 | 7.2 → 276.1 | 93% |
| 中盤 | 左上原点 | 256 → 687 | 7.1 → 292.6 | 66% |
| 後半（108.3 秒） | 全画面 | 326 → 622 | 14.4 → 265.4 | 85% |
| 後半 | 左上原点 | 176 → 461 | 13.9 → 267.9 | 89% |

[事実] 区間と全体で pre-YUV は一致した（比べられた 60・49・51 frame。全体の run は上限で打ち切った分が
少ない）。左上原点の切り出しと全画面も一致した。同じ画素を作るのに、Project 全体の graph では
合成が 130〜431 ms/frame 長くかかり、その 81〜93% が文字列化の時間である（中盤・左上原点だけ 66%）。
render worker を 1 本にして lock 待ちを除くと、文字列化は合成時間の 29%・38%・45%（前半・中盤・後半）。

## 3. 何で決まるか

| 変数 | 説明できること | 交絡 |
| --- | --- | --- |
| 1 つの transition の keyframe 数 | 文字列化の時間。1.0〜1.4 乗。画素を固定して確認した | なし（1 layer の実験） |
| 有効な layer の数 | 文字列化の時間に比例して効く。画素の合成にも比例して効く | layer を増やすと合成対象の画素も増える。上の表は keyframe 最小の行で分けた |
| Project の長さ | それ自体では効かない。長い clip が持つ keyframe が増えることで効く | 長さと keyframe 数は実 Project では一緒に動く。stride の実験で分けた |
| Project 全体の keyframe 数 | 直接は効かない。効くのは、その frame で有効な transition が持つ keyframe | 全体の数と有効な分の数は実 Project では相関する |
| 合成対象の画素 | 文字列化を除いた残り（全画面で 306〜628 ms、左上原点で 139〜249 ms） | 切り出しでは減らない layer ごとの固定費を含む（P1） |

説明できていない部分: 中盤・左上原点の差の 34%（約 146 ms）と、worker が複数のときに文字列化以外も
増える分（16 layer で 235 → 658 ms）。後者は、文字列化の間に他の worker が同じ transition の lock を
待つ時間が合成時間に入るためと見ているが [推測]、待ち時間そのものは測っていない。

## 4. 最小の試作: transition を区間ごとに分ける

文字列化は MLT の公開 API では避けられない。animation を API で作っても文字列で渡しても、MLT は
property を文字列で読むたびに作り直す。そこで、1 つの transition が持つ keyframe を減らす方法として、
上位 track の clip の affine transition を、値の変わらない区間（同じ値が 8 frame 以上）と変わる区間
（64 frame まで）に分けて植える経路を試作した（`diagnostic_split_transitions`、既定 0）。どの frame にも
その frame の keyframe があるか、値の変わらない区間の中にあるので、評価される値は変わらない。

検討した 3 案:

| 案 | 評価 |
| --- | --- |
| A. transition を時間で分ける | 試作した。画素は完全一致。文字列化は消えるが、transition が増えた分のコストが出る |
| B. 区間ごとに MLT へ見せる keyframe を減らす | 実行中に animation を書き換えることになり、render worker と競合する。A と同じ効果を、より危険な方法で得るだけなので試作していない |
| C. 値を変えずに文字列化そのものを避ける | MLT の API では不可（上記）。MLT を変えるか、数式 layer の合成を MLT の外で行う必要がある |

### 画素（baseline は全画面・分割なし）

[事実] 同じ process で baseline → candidate → baseline の順に書き出し、candidate を baseline と
全 frame・全画素（4 channel）で比べた。15 件・1320 frame がすべて完全一致。未比較 0、2 回目の baseline との
不一致 0。

- 既存の比較 fixture 6 本（整数位置・端数位置・fade・16 層・48 層・v4 の 20〜22 秒・88〜91 秒）と、
  v4 の前半・中盤・後半の区間 3 本: 完全一致。
- Project 全体の graph のまま前半・中盤・後半を書き出した 6 件（全画面と左上原点）: 完全一致。
- 分類ごとの frame 数: 端数位置の静止 811、補間の途中 329、整数位置 120、fade の途中 90、
  拡縮する layer を含む 119、複数 layer 1080。すべて完全一致。
- fixture の最終 frame と、区間の境界（transition の in/out が変わる frame）を含む。同じ値の keyframe が
  続く区間は「変わらない区間」として両端だけを置いている。

対照（意図的に壊して、比較が検出することを見る）:

| 対照 | 結果 |
| --- | --- |
| baseline の 1 画素を 2 階調ずらす | 1 階調超で検出（1 frame） |
| 分割で keyframe を 1 つおきに落とす | 1 階調超で検出（22 frame、最大差 21） |
| 分割で不透明度を半分にする | 1 階調超で検出（120 frame、最大差 127） |
| 分割で keyframe の位置を 1 frame ずらす | **期待と違う形で落ちた**。書き出しそのものが失敗し、比較は不成立（49 frame が 1 階調超、71 frame は未比較）。壊れた出力が成立扱いにはなっていないが、「1 階調超で検出」という期待（終了コード 14）とは一致しなかった（15）。書き出しが失敗した理由は調べていない |
| 既存の 4 種（観測の欠落・期待値のずれ・検査の拒否・取消） | 期待どおり |

### 性能

[事実] 同じ process の 2 回目の baseline との比。v4 全体の graph（transition 120 → 766、keyframe 19,632 → 22,172）:

| 区間 | 渡し方 | 文字列化 ms | 合成 ms | render 秒 |
| --- | --- | --- | --- | --- |
| 前半 | 全画面 | 118.0 → 4.3 | 353 → 412 | 30.2 → 36.3（0.83 倍） |
| 前半 | 左上原点 | 127.4 → 4.2 | 286 → 338 | 23.1 → 30.1（0.77 倍） |
| 中盤 | 全画面 | 288.2 → 6.2 | 816 → 395 | 53.4 → 34.4（1.55 倍） |
| 中盤 | 左上原点 | 326.0 → 7.6 | 886 → 387 | 38.6 → 34.1（1.13 倍） |
| 後半 | 全画面 | 274.7 → 9.3 | 566 → 488 | 46.8 → 42.5（1.10 倍） |
| 後半 | 左上原点 | 172.1 → 9.2 | 258 → 391 | 22.0 → 34.4（0.64 倍） |

- 文字列化は 118〜326 ms から 4〜9 ms に減った。見込みどおりである。
- しかし合成時間は 338〜488 ms に揃い、区間によっては分ける前より遅い。分けた後は、全画面でも左上原点でも
  ほぼ同じ時間になっている。
- [推測] transition が 120 から 766 に増え、MLT が毎 frame すべての transition を通すコストが
  新しい下限になっている。transition の数を変えて測ってはいない。
- baseline 自体のぶれも大きい（後半・左上原点の baseline は、行列では合成 461 ms、この比較の 2 回目では
  258 ms）。この表だけで改善率を決められない。

見込みと実測: 文字列化が消えれば区間の fixture と同じ速さ（前半 310、中盤 635、後半 326 ms）になると
見込んだが、実測は 412・395・488 ms で、前半と後半は見込みに届かなかった。

**分割はこのままでは採れない（HOLD）。** 画素は保てるが、Project 全体での利益を示せていない。

### 製品水準の性能

v4 全体を製品の timeout で書き出す実行は行っていない。前半・中盤・後半の実測（分割後で render
0.50〜0.71 秒/frame、60 frame の区間）から、10 分で終わる根拠が無いためである。区間の値から
10 分の合否を言うこともしない。

## 5. timeout の数え方

[事実] MLT 層の待ち loop は `Sleep(20)` を 1 回まわるたびに 20 を足して経過時間としている
（`mvm_mlt_export.c` の 2 か所。sequential と tractor）。loop の 1 回には `Sleep(20)` の他に、進行 callback と
観測の時間が入る。`Sleep` も指定より長く眠る。したがって数えた時間は実時間より遅く進む。

`mvm_test_mlt_export_capacity timeout-accounting` で測った。進行 callback を 30 ms 眠らせ、
`timeout_ms` = 1000 で書き出すと、**実時間 3418 ms（3.42 倍）で timeout した**。loop は 50 回まわり、
1 回は最低 50 ms かかるので、2500 ms 以上になることは `Sleep` の性質から決まる。
CTest には `mlt_export_timeout_accounting_deviation`（label `stability`、期待する終了コード 3 = 遅れて
timeout した）として現状を記録した。P1 で v4 全体が 720 秒を超えても timeout しなかったのも同じ原因である。

最小の修正案（未着手）:

1. loop に入る前に単調増加の時刻（`QueryPerformanceCounter`）を取り、毎回「いま − 開始 ≥ timeout_ms」で
   判定する。`waited += 20` をやめる。
2. 意味は変えない。開始からの絶対時間で、進行中でも失敗にする。進行が止まったときだけ失敗にする方式へは
   変えない（それは別の決定）。
3. 取消の確認（進行 callback）は今と同じく loop の毎回で行い、timeout より先に見る順序も保つ。
4. 直したら、上の CTest の期待を 0（1.5 倍以内に timeout）へ変え、通常の実行へ入れる。
5. 影響: 実時間で数えると、いま 10 分を超えても通っている書き出しが失敗するようになる。
   既定の 600000 ms を保つか延ばすかは、この修正とは別に決める必要がある。

## 6. 次の P2 の候補

1. **transition の数と keyframe の数の釣り合いを測る**（P2-B）。分割の閾値を変え、transition 1 つの
   固定費と、transition あたりの keyframe による文字列化を、同じ window で測る。transition の数を増やさずに
   keyframe を抑えられる分け方があるかを、全画素比較つきで判断する。
2. それで足りなければ、**数式 layer の合成を MLT の affine から外す**案を検討する。文字列化、lock による直列化、
   layer ごとの全画素の走査がまとめて無くなるが、pre-YUV は現在と完全一致しなくなる（P1 の記録を参照）。
   parity の基準をどう置くかを先に決める必要がある。
3. **timeout の数え方の修正**は独立に進められる。
