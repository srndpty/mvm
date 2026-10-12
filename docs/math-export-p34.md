# P3-4: 数式 group の診断用の経路による、v4 全体の H.264 書き出しの検収

2026-10-13。[P3-3D](math-transition-p33d.md) で残っていた「v4 の Project 全体（6840 frame）の書き出し」を、
判定つきの数式 group（診断用の mode 3）で行い、600 秒以内に終わるか、出力が正しいかを確かめた。

製品の書き出しの経路・timeout・MLT の affine・既存の画素 oracle・合成の式・複製してから書く方針は変えていない。
数式 group は、製品の書き出し（observer が NULL）からは動かない。commit・push はしていない。
P1〜P3-3D の成果物と判定は変更していない（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。

証拠は `build/math-export-p34-20261013/`。再現は `scripts/math-export-p34-verify.ps1`（段階ごとに `-Stage`）。

## 判定

| 項目 | 結果 |
| --- | --- |
| 1. 診断用の経路での全体の書き出しの完走 | **成立。** 6840 frame、終了コード 0、watchdog による打ち切りなし、dump 0 |
| 2. 実時間 600 秒以内 | **成立。** 書き出しの関数の呼び出しから戻りまで 272.4 秒（単調な時計）。外部の時計でも、process の起動から最終 file が現れるまで 276.2 秒 |
| 3. H.264 の出力と復号した frame | **成立。** 6840 frame、1920×1080、60 fps、114.000 秒。時刻は 0 から 1 frame ずつ。全 frame を復号して error 0 |
| 4. encoder 直前の RGBA の一致 | **成立（全 frame）。** 6840/6840 frame が製品の経路と byte 単位で一致。比べていない frame は無い |
| 5. memory と寿命 | 成立。準備済みの画像 2.81 MB、書き出し後に残る transition・callback・画素は 0。process の commit は render の間に約 180 MB 増えた（原因は切り分けていない） |
| 6. 並行の検出 | **未解決のまま。** MLT の統合部分は race の検出器の対象外。今回足したのは全長 1 回分の実行だけ |
| 7. 製品での有効化 | **まだ不可（HOLD）。** 理由は 8 |

**全体: 診断用の経路としての検収は成立。製品の検収ではない。**

## 1. 事前の判定の見直し（Task A）

全長の書き出しの前に、ELIGIBLE を誤って出す道が無いかを見直した。到達できる誤りは見つからなかったが、
呼び出し側の誤りに備える検査が 1 つ欠けていたので足した（1.2）。

### 1.1 見直した点

| 点 | 結果 |
| --- | --- |
| stage した RGBA と、MLT が出す V1 の frame の対応 | [事実] alpha が 1 の背景・全値（1〜255）を散らした背景で、group の callback が受け取った下地の alpha の最小値は、証明の値と同じ 1 だった。不透明な背景は 255（v4 では、下の affine を通った後で 254〜255） |
| PNG の書き・読みと画素の形式 | [事実] stage は RGBA8888 を PNG（8 bit、乗算していない alpha）へ書く。MLT の `qimage` は ARGB32（乗算なし）で読み、大きさが要求と同じなら拡縮しない（`qimage_wrapper.cpp`）。V1 の静止画は書き出しの大きさで stage するので、拡縮は起きない。[推測] 大きさが違う素材を V1 に直接置く道は、上位の層に無い |
| 判定が使う区間 | [事実] V1 の clip と member の区間は半開区間で比べる。直前で終わる・直後に始まる clip は対象外、1 frame でも重なれば対象（単体試験） |
| V1 の隙間 | [事実] 書き出しは、V1 の clip の間と末尾を黒の color producer（`#000000`）で埋める。MLT は 6 桁の色を alpha 255 と解釈し（`mlt_property.c` 457 行）、rgba の要求には alpha 255 の画素を返す（`producer_colour.c`）。V1 に clip が無い fixture と、前半だけ clip がある fixture で、下地の alpha の最小値は 255、全 frame が製品の経路と一致 |
| 数式でない layer と effect | [事実] v4 の従来の layer は 23（字幕 21、申告していない数式 2）。どれも affine の transition で重なる。書き出しは affine に `b_alpha = 0` を明示している。**`b_alpha` が 1 だと、MLT は合成後の alpha を上の画像の alpha で置き換える**（`interp.h` の `is_atop`）ので、(O2) は成り立たない。(O2) は「書き出しが `b_alpha = 0` を設定している」ことに依存する |
| 補間ごとの alpha | [事実] nearest・bilinear・bicubic とも、合成後の alpha は 上 + 下 − 上 × 下（`interp.h`）。bicubic は補間した上の alpha を 0〜255 に丸めてから同じ式へ入れる。P3-3D の検査（上の alpha の全 binary32 値）の範囲に収まる |

読んだ MLT の source（tag `v7.36.1`）は `mlt-7.36.1-source/` に置いた。実行時の binary は MSYS2 の配布物で、
source との対応は version の一致による [推測]。

### 1.2 足した検査

- [事実] P3-3D の判定は、V1 の clip に付いた証明を、その clip が静止画かどうかを見ずに信じていた。
  上位の層（`timeline_export.cpp`）は静止画にしか証明を付けないので、今の呼び出しからは誤った ELIGIBLE に
  ならない。しかし、証明は「stage した静止画 1 枚の全画素」についてのものなので、連番・動画・frame hold・
  切り出した素材に付いていれば意味が無い。
- 判定（`mvm_mlt_export_math_group_preflight`）で、静止画でない clip の証明を無いものとして扱い、
  切り出した素材は「変形あり」として扱うようにした。UNKNOWN のときは、途中まで調べた alpha の最小値を
  返さない。
- 判定を、画素も MLT も使わない関数として外へ出し、単体試験（CTest `math_group_preflight_decisions`、
  35 通り）を足した。
- 対照: 静止画の検査を外した変異で、単体試験は「連番・動画・frame hold に付いた証明」の 3 つの期待で
  失敗した（`preflight/mutant-nonstill.log`。同じ log にある 4 つ目の失敗は、その時点の別の誤りで、
  固定の前に直した）。この変異は固定の前に行い、source は元へ戻した。

### 1.3 対照（固定した binary で実行）

`20261013-015037-preflight/`。期待は実行の前に script に書いてある。11 通りすべて期待どおり。

| 場合 | 判定 → graph | 結果 |
| --- | --- | --- |
| V1 に clip が無い（隙間だけ） | ELIGIBLE → 数式 group | 全 frame 一致、下地の alpha の最小 255 |
| V1 の clip が前半だけ | ELIGIBLE → 数式 group | 全 frame 一致、最小 255 |
| 不透明な静止画 | ELIGIBLE → 数式 group | 全 frame 一致、最小 255 |
| alpha 1 の静止画、alpha 1〜255 を散らした静止画 | ELIGIBLE → 数式 group | 全 frame 一致、最小 1 |
| 証明が無い | UNKNOWN → 従来 | 全 frame 一致 |
| 証明が範囲外（999、−7） | UNKNOWN → 従来 | 全 frame 一致 |
| V1 に effect があり、証明は「不透明」 | UNKNOWN → 従来 | 全 frame 一致 |
| alpha 0 があると証明 | INELIGIBLE → 従来 | 全 frame 一致 |
| alpha 0 の背景に「不透明」という誤った証明 | ELIGIBLE → 数式 group | **実行時に合成器が拒否して失敗**（出力なし、残り 0） |

最後の行のとおり、証明そのものが誤っていれば判定は ELIGIBLE になる。そのときは実行時の下地の検査が
書き出しを失敗させる（黙って誤った画素は出ない）。実行時の検査は弱めていない。

## 2. 検収の authority の固定（Task B）

- `freeze.json`（2026-10-13 01:14:09）に、755 file の SHA-256 を固定した。
  - source: `src`・`tests`・CMake の追跡している全 file と、追跡していない新しい file。
  - binary: harness（`0A56214D…BC705C23`）、単体試験 2 つ。
  - runtime: MLT（本体と module の全 DLL）、Qt6Core・Qt6Gui、FFmpeg の library（avcodec-62・avformat-62・
    avutil-60・swscale-9・swresample-6）、libx264-165、ffprobe・ffmpeg（8.1.2）。
  - Project: `quadratic.mvm`、数式の画像の cache、Project が参照する背景の画像。
  - script: `math-export-p34-verify.ps1`。
- git の HEAD は `7277de5`。作業 tree の差分は `freeze-git-diff.patch`（SHA-256 も `freeze.json` にある）。
- 各段階は、実行の前に全 file を `freeze.json` と照合し、1 つでも違えば実行しない。full・baseline・compare・
  preflight・safety のどれも「755 file 一致」で始まった。固定の後は build も変異もしていない。
- 実行中の process が実際に load した module（287 個）の SHA-256 は `…-full/candidate/runtime-modules.json`。
  固定した file と重なる 34 個は、すべて同じ値である。
- encoder の設定は source（`configure_mp4_consumer`: libx264、preset medium、CRF 23、yuv420p、faststart）と、
  x264 が出力へ書いた設定（`compare` の `x264_settings`: High、crf=23.0、bframes=3、keyint=250、threads=34）。
- cache の `jobs/`（実行のたびに書き直される tool の version の記録）は固定の対象から外した。
- `trial/`・`trial2/`・`trial3/` は、script の動作確認（120 frame）で、検収には使っていない。`trial` と
  `trial2` は script の誤り（Project を 2 つの process で開けない、cache の除外の誤り）で不成立になった記録である。

## 3. 全体の書き出し（Task C）

`20261013-011412-full/`。1 回だけ実行した（固定の後、全長の書き出しはこの 1 回）。

- 条件: v4 の graph そのまま、1920×1080、6840 frame、区間の指定なし。worker の数・encoder・muxer・
  timeout（600,000 ms）は製品の値のまま。数式 group は mode 3（判定つき）。
- 判定は ELIGIBLE（V1 の clip 1 つ、alpha 255 と証明）。97 clip を 2 つの group にまとめ、affine は 23。

時間（`export_call_wall_seconds` は process の中の単調な時計。外部の値は script の Stopwatch）:

| 測ったもの | 秒 |
| --- | --- |
| 書き出しの関数の呼び出しから戻りまで（**600 秒の判定に使う値**） | **272.4** |
| 外部: process の起動から最終 file が現れるまで（上の値より長い上限） | 276.2 |
| 外部: process の起動から終了まで（書き出しの後の検査を含む） | 288.6 |

- 外部の watchdog は 1500 秒（有限）で、発動していない。製品の `Sleep` の回数で数える timeout は、
  時間の判定に使っていない。
- 600 秒に対する余裕は約 2.2 倍。同じ条件の製品の経路は、baseline の render の合計が約 5,000 秒
  （3 つ同時に動かしたので、単独の時間ではない）。

段階ごとの時間（書き出しの開始から）:

| 段階 | 区間（秒） | 長さ（秒） |
| --- | --- | --- |
| 計画 | 0.00〜0.08 | 0.08 |
| stage（数式の画像の読み込み・PNG の書き出し・数式 group の画像の準備を含む） | 0.08〜11.11 | 11.0 |
| うち数式 group の画像の準備 | — | 0.74 |
| MLT の graph の構築 | 11.12〜11.42 | 0.30 |
| consumer の開始 | 11.42 | 0.0004 |
| render・encode・mux（1 つの区間。MLT の consumer の中で並行に進む） | 11.42〜272.20 | 260.8 |
| consumer を閉じる（encoder の flush、muxer の完了） | 272.20〜272.22 | 0.02 |
| 出力の検証（probe） | 272.22〜272.38 | 0.16 |
| 最終の名前への公開 | 272.40 | 0.0006 |

- encoder と muxer の時間は、render と分けて測れていない（同じ区間の中で並行に進む）[未検証]。
  数えたのは件数で、encoder へ渡した frame・encode した packet・mux した packet は、どれも 6840。
- group の callback は 13,730 回、合計 173.0 秒（worker をまたいだ延べ）。1 回あたり合成 8.3 ms、
  合成先の確保と複製 4.5 ms。
- CPU 時間は 1,064 秒で、render の区間の平均は約 4 core 分（32 論理 core の機械）。CPU 時間の多い thread は
  1 本が 197 秒、4 本が約 62 秒ずつ。
- この時間には診断の負荷（全 frame の digest、callback ごとの下地の alpha の全画素の集計、muxer の記録）が
  含まれる。製品の経路には無い負荷なので、時間は長い側へ寄っている [推測]。

## 4. 出力の検査（Task D）

`20261013-015018-compare/`。harness の集計とは別に、ffprobe・ffmpeg で調べた。

| 項目 | 結果 |
| --- | --- |
| codec・大きさ・fps | h264（High、yuv420p）、1920×1080、60/1 |
| frame 数 | container の記録 6840、読めた frame 6840 |
| 尺 | 114.000 秒（= 6840 / 60） |
| 表示の時刻 | 復号した 6840 frame の時刻が、0 から 256 刻み（time base 1/15360）で 1 つずつ増える |
| packet | 6840 個、時刻の重複なし、0〜6839 frame 分を過不足なく覆う |
| 全 frame の復号 | ffmpeg が 6840 frame を復号、終了コード 0、error の行 0 |
| encoder への投入 | 6840（encode 6840、mux 6840） |
| 公開 | 最終 file あり、一時 file なし。SHA-256 `FF8EB4D5…9A8BF6C0`、2,062,352 byte |
| 音声 | stream なし（v4 に音声の clip が無い） |

- encoder 直前の RGBA は、frame 0〜6839 のすべてを 1 回以上観測した（欠け 0）。最後の frame だけ 26 回
  観測されているが、encoder へ渡ったのは 6840 frame である（60 frame の区間でも同じ挙動）。
- 「同じ内容の frame が 2 回入り、別の frame が抜ける」ことが無いことは、RGBA の観測（frame ごとの digest が
  baseline と一致）と件数・時刻による。復号した画素を frame ごとに RGBA と突き合わせてはいない [未検証]。

失敗・取消の後（`20261013-015730-safety/`）:

| 場合 | 結果 | 残った file |
| --- | --- | --- |
| v4 全体で、1,204 枚を合成した時点で取消 | 取消（終了コード 11） | なし |
| v4 全体で、実行時に下地を拒否（故障の注入） | 失敗（終了コード 6） | なし |
| 誤った証明（alpha 0 の背景） | 失敗（終了コード 6） | なし |
| 外部の watchdog が 45 秒で process を止める | 打ち切り | **`v4-full.mp4.mvmtmp`（262,192 byte）が残る** |

- 書き出しが自分で失敗・取消を返す場合は、最終 file も一時 file も残らない。
- process を外から止めた場合は、後始末が走らないので一時 file が残る。最終の名前の file はできない
  （未完成の file が最終の名前で残ることはない）。残った一時 file を次の起動で片づける仕組みは確かめていない。

## 5. 画素の一致（Task E）

- baseline: 製品の経路（数式 group なし、affine 120）で、同じ graph の全 frame の RGBA の digest を採った
  （`20261013-011914-baseline/`）。1 process では 600 秒で終わらないので、570 frame ずつ 12 区間に分け、
  別々の process で書き出した。timeout だけを上書きしている（harness が「製品の条件ではない」と記録する）。
  Project は 1 つの process しか開けないので、3 つ同時に動かすために Project と cache の写しを 2 つ使った
  （写しの SHA-256 が元と同じことを、実行の前に確かめている）。
- 比較: candidate（全長を 1 回で書き出したもの）の frame ごとの digest と alpha の最小値を、baseline と比べた。

| | frame |
| --- | --- |
| 一致 | **6840** |
| 不一致 | 0 |
| candidate に無い | 0 |
| baseline に無い（比べていない） | 0 |
| 区間どうしの重なり | 0 |

- 比べたのは frame 0〜6839 の全部で、比べていない区間は無い。
- 比べたのは、frame 全体（1920×1080 の RGBA）の 64 bit の digest と alpha の最小値である。画素を 1 つずつ
  比べたのではない。digest が偶然一致する可能性は残る（P3-3D までの 2,640 frame は画素そのものを比べている）。
- baseline は区間ごとに書き出している。「区間の途中から書き出した frame は、最初から書き出した frame と同じ」
  ことが前提で、candidate（最初から 1 回）と全 frame が一致したことは、この前提とも矛盾しない。
- alpha の最小値は、両方とも 254。
- 既存の画素 oracle（代表 frame）は変えていない。全長の run では frame 600 が許容の範囲内（差は最大 1）。

## 6. 資源と寿命（Task F）

| 項目 | 値 |
| --- | --- |
| process の working set の peak | 1,656 MB（harness の中と外部の観測が一致） |
| commit（private）の peak | 1,956 MB |
| 準備済みの画像 | 81 枚、2.81 MB（97 clip のうち 16 回は共有、内容の食い違い 0） |
| 準備の前後の working set | 104.1 → 108.6 MB |
| 書き出し後に生きている画素 | 0 byte |
| 書き出し後に残る transition・実行中の callback | 0・0 |
| group の失敗 | 0 |
| crash・dump | 0（procdump を起動後に attach。未処理の例外なし） |

- commit は、render が始まった直後（30 秒）の 1,776 MB から、終わり（275 秒）の 1,954 MB まで増えた。
  増え方は後半ほど緩い（200 秒で 1,938 MB、260 秒で 1,946 MB）。**MLT の pool の大きさは直接測っていない**ので、
  増えた分が pool か encoder かは分からない [未検証]。書き出しの後の working set は 379 MB である。
- procdump は、処理された C++ の例外（`CacheOverflowException`）を 1 回記録した。未処理ではなく、dump は無い。
  120 frame の trial でも、起動から約 15 秒（stage の途中）に出ている。
- thread は最大で約 96 本。
- 取消と watchdog は 4 のとおり。

## 7. 並行の検出の範囲

- 検出器（ThreadSanitizer・AddressSanitizer・UBSan）で見たのは、P3-3D と同じく合成器・準備済みの画像・
  置き場だけである。合成器の source（`math_group_compose`）は P3-3D から変えていないので、検出器は
  回し直していない。
- MLT の transition・書き出し・MLT そのものは、今回も検出器の対象外である。
- 今回足した証拠は、全長 1 回（callback 13,730 回、worker は製品の本数）が、失敗 0・残り 0・crash 0 で、
  全 frame が製品の経路と一致したことである。起きうる順序の一部を実行しただけで、race が無いことの
  証明ではない。
- UI の SegFault は再現していない。UNKNOWN のままである。

## 8. 製品で有効にできるか

**まだ有効にしない（HOLD）。** 診断用の経路が 1 回、条件を満たしたことと、製品の検収は別である。

1. 全長の書き出しは 1 回だけである。時間のばらつきと、繰り返したときの安定は見ていない。
2. MLT の統合部分を race の検出器の下で動かせていない。
3. 製品から数式 group を使う入口が無い（observer に診断用の入力を渡す形でしか動かない）。
4. 判定が ELIGIBLE で実行時に拒否されたとき、従来の graph でやり直す仕組みが無い（書き出しが失敗する）。
5. 製品の timeout は `Sleep` の回数で数えるので、実時間の 600 秒と一致しない（今回は変えていない）。
6. process を外から止めると一時 file が残る。
7. V1 に動画がある Project は UNKNOWN になり、従来の経路のまま（600 秒では終わらない長さがある）。
8. UI の SegFault（UNKNOWN）。

次に行うなら、最も小さい範囲は「全長の書き出しを複数回くり返して時間と安定を見ること」と
「MLT の統合部分を検出器の下で build する手段を作ること」である。

## 9. gate

| gate | 結果 |
| --- | --- |
| 判定の単体試験（`math_group_preflight_decisions`、35 通り） | 通過 |
| 関連の CTest（`math`・`mlt_export`・`m7b_3_product_export`・`timeline_export` に合う 51 件） | 51/51 通過（`ctest-final.log`。固定の直前に実行。binary は固定したものと同じ） |
| lint | 通過 |
| 全 CTest | 実行していない |
| 判定の対照（11 通り）・失敗時の安全（4 通り） | 期待どおり |
