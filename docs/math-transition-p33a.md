# P3-3A: 数式 group transition の組み込みの準備

2026-10-12。[P3-1.2](math-compositor-p312.md) の合成器を、MLT の transition として書き出しの graph へ
入れる試作（診断専用）と、その前提の確認。

製品の書き出しの経路、timeout、既存の画素 oracle は変えていない。試作は
`MvmExportObserver::diagnostic_math_group`（製品では observer が NULL）が 0 以外のときだけ動く。
commit・push はしていない。P1〜P3-1.2 の成果物と判定は変更していない。

証拠は `build/math-transition-p33a-20261012/`。

## 判定

| 終了の条件 | 結果 |
| --- | --- |
| 1. pre-YUV の RGBA が完全に一致する | 成立。9 fixture の 960 frame と、Project 全体の graph の 4 区間 240 frame が全画素一致（3 回） |
| 2. track の順序と frame の対応を保つ | 成立。従来の経路の layer と混ざる fixture でも一致。対応・画像・layer の欠落の対照を検出 |
| 3. 所有と寿命の安全 | 成立（1 点を除く）。buffer は複製へ合成、解放の確認は 0。**data race は検出器で確かめていない** |
| 4. 対照が意図どおりに検出される | 成立。7 つすべて |
| 5. 繰り返して測れる速度の利益 | **fixture による**。数式が 16 層以上重なる区間は 7.6〜16 倍（3 回とも）。Project 全体の graph では 1.17〜1.39 倍（各 1 回） |
| 6. 製品の動作を変えない | 成立。observer が NULL なら graph は従来どおり。関連 CTest 49/49 |
| 7. 最終の source・runtime の provenance | 成立。最終の 5 run の SHA-256 が作業 tree と一致 |

**全体: PASS（診断専用の経路として）。ただし次の 3 点は未解決で、製品へ入れる判断の前に必要である。**

1. **利益が Project 全体では小さい。** 合成器は不透明な下地だけを対象にするので、従来の経路の layer
   （拡縮する layer）の上に重なる数式は group にできない。v4 の Project では、申告した 99 clip のうち
   group にできたのは 25、配置が対象外なのは 2 だけで、残り 72 は「下に従来の layer がある」ために
   残った（3.2）。
2. data race は code の構造と実測（worker 1・4・8 本で全 frame 一致）で確かめただけで、検出器は
   使っていない（この環境で使えない）。
3. v4 の Project 全体（6840 frame）の書き出しは実行していない。

Task A（`math_equation_sequence_product_ui` の SegFault）は **未解決のまま**である。再現せず、原因は
分かっていない（1）。この判定とは別に扱う。

## 1. `math_equation_sequence_product_ui` の SegFault（Task A）

合成器の画素一致の判定とは別の調査である。結果を混ぜない。

**結論: 再現せず、原因は分かっていない。未解決のまま残す。** ただし、落ちた binary は合成器の code を
1 つも含んでおらず、P3-1.2 の変更がこの失敗の code 上の原因になる経路は無い。

### 元の失敗

`task-a/original-failure.txt`。P3-1.2 の gate で 49 件を続けて実行したとき（2026-10-12 13:58 ごろ）、
25 番目の `math_equation_sequence_product_ui` が開始 0.72 秒で `SegFault` になった。

- 試験の command は `mvm_test_text_ui_input.exe --equation-sequence-ui build/p351-ui --scratch`
  （通常は約 170 秒かかる製品 UI の試験）。
- [事実] 残っている記録は、ctest の 1 行（`***Exception: SegFault 0.72 sec`）だけである。
  試験の出力は画面への絞り込みで捨てており、`LastTest.log` は直後の再実行で上書きされた。
  crash dump（`%LOCALAPPDATA%\CrashDumps`）にも Windows の Application event log にも、この時刻の記録は無い。
  したがって、元の失敗の stack・例外の address・落ちた module は分からない。
- [事実] 元の失敗のとき、同じ機械で、会話の作業が起動した disk 全体の検索（`find /`、13:10 ごろ開始）が
  動いていた。この検索は 14:47 ごろまで続いた。

### 事前に決めた再現の試行

`pwsh scripts/math-transition-p33a-crash-cohort.ps1`（`task-a/20261012-141631-cohort/`）。回数は実行の前に
固定した。build は元の失敗の時から変えていない（試験の binary の SHA-256 は `13660872…5c4be7`、
link は 13:40。元の失敗は 13:58 ごろ）。

| 試行 | 内容 | 回数 | 結果 |
| --- | --- | --- | --- |
| R1 | 元と同じ 49 件（同じ正規表現・同じ順序） | 3 | 3 回とも 49/49 通過 |
| R2 | 直前の試験との組（`math_inspector_product_ui` → 対象） | 5 | 5 回とも通過 |
| R3 | 対象の command を procdump の下で直接実行（未処理の例外で full dump） | 5 | 5 回とも終了コード 0、dump 0 件 |

- 元の失敗の直後の単独の再実行 3 回も通過している。合わせて 16 回、再現していない。
- R1 の 3 回と R2 の 1〜2 回目は、上の disk 全体の検索が動いている間に実行された（この条件でも
  再現していない）。
- R3 では、処理される例外（first-chance の `E06D7363`、`80010108`）が終了時に記録されている。
  どれも process の中で処理されており、未処理の例外は無い。

### 今回の変更との関係

- [事実] 落ちた binary（`mvm_test_text_ui_input.exe`、上の SHA-256）の symbol を調べた
  （`task-a/…-cohort/exe-symbols.txt`）。symbol 75,301 個のうち、合成器（`composeMathGroup`・
  `PreparedMathRaster`・`mathGroupDestinationEligible`・`mathGroupAlphaRoundTripIsIdentity`）は 0 個。
  対照として、同じ library の別の関数（`composeMathClip…`）は 2 個ある。P3-1.2 の時点では、合成器は
  製品のどの code からも呼ばれておらず、linker がこの binary へ取り込んでいない。
- [事実] P3-1.2 のそれ以外の変更は、別の実行 file（`mvm_test_math_group_compose`、
  `mvm_math_score_export_parity`）の source と、script・文書だけである。
- したがって、P3-1.2 の変更がこの binary の code を変えた経路は無い。
- [未検証] P3-1.2 の前の commit で build した binary と byte 単位で比べてはいない（link の配置だけが
  変わった可能性までは否定していない）。
- [推測] 残る候補は、この試験か製品 UI の起動に前からある不安定さ、または当時の機械の状態
  （disk の負荷など）である。2026-10-10 に同じ実行 file の crash dump
  （`mvm_test_text_ui_input.exe.36884.dmp`）が残っており、この実行 file が以前にも落ちたことは分かるが、
  同じ原因かどうかは調べていない（この環境に minidump を読む debugger が無い）。

### 扱い

- 元の失敗は「解決」としない。再現したら dump を採れるよう、R3 の手順（procdump）を script に残した。
- P3-3A からは、製品の code（`timeline_export.cpp`）が診断用の経路で合成器を参照するので、
  この binary にも合成器の code が入る。今後この試験が落ちた場合は、上の「含まれていない」という根拠は
  使えない。

## 2. MLT 7.36.1 の transition と frame buffer の所有（Task B）

MLT 7.36.1 の source（tag `v7.36.1`、commit `0af529c4`）を読んで確かめた。実行時の binary は MSYS2 の
package で、source と同じ版である。行番号は `src/framework/` と `src/modules/` のもの。

### 2.1 下の frame（destination）の得方

- [事実] tractor は frame ごとに全 track の frame を集め、隠されていない（`hide` の bit 0 が 0）・test card で
  ない track のうち **一番上** のものを出力にする（`mlt_tractor.c` の `producer_get_frame`）。
  下の track と重ねる処理は tractor には無く、transition が行う。
- [事実] transition は `a_track`〜`b_track` の frame を取り出し、a と b の frame を `process` へ渡す
  （`mlt_transition.c` の `transition_get_frame`）。`process` が a の frame を返すと、b の frame の `hide` に
  bit を立てる。これで b の track は tractor の出力の候補から外れる。
- [事実] この段階では画素は無い。`process` は a の frame の画像の stack へ自分の callback を積むだけで、
  画素は render worker が出力 frame の画像を要求したときに、積んだ順の逆に呼ばれて作られる。
- [事実] affine は callback の中で `mlt_frame_get_image(a_frame, …, writable = 1)` で下の画像を得て、
  その buffer へ **直接** 書く（`transition_affine.c` 478 行以降）。

### 2.2 buffer は書いてよいか、その frame だけのものか

- [事実] `mlt_frame_get_image` の `writable` は、下流の callback へそのまま渡されるだけである
  （`mlt_frame.c` 499〜539 行）。framework は複製も検査もしない。copy-on-write の仕組みは無い。
- [事実] 今の書き出しで下の frame を作る producer は、frame ごとに複製を渡している。
  - `qimage`（静止画・数式の PNG）: cache した画像を `mlt_pool_alloc` で複製して frame に持たせる
    （`producer_qimage.c`。「Cloning is necessary with this producer」と注記がある）。
  - `color`（V1 の隙間の黒）: frame ごとに確保する（`producer_colour.c`）。
  - `avformat`（動画）: cache から得るときは `mlt_cache_get_frame` が frame を深く複製する
    （`mlt_cache.c` の `mlt_frame_clone(*hit, 1)`）。
- [推測] したがって今の graph では、affine が直接書いても他の frame の画素は壊れない。ただしこれは
  producer と filter の実装に依存する性質で、framework の契約ではない。
- **試作の方針**: 渡された buffer へは書かない。`mlt_pool_alloc` で確保した複製へ合成し、成功したときだけ
  `mlt_frame_set_image` で frame の画像を複製へ差し替える（前の buffer は、持ち主の destructor が解放する）。
  失敗したら複製を捨て、渡された buffer をそのまま返す。これで「検査に失敗したとき、合成先を途中まで
  書き換えない」も満たす。代わりに 1 frame・1 group につき全画面 1 枚の複製の時間がかかる（5 で計測）。

### 2.3 既存の transition との順序

- [事実] `mlt_field_plant_transition` は、新しい transition を「今までの最後の service」の後ろへつなぐ
  （`mlt_field.c`）。frame の要求は最後に植えたものから先頭へ遡り、戻りながら `process` が呼ばれるので、
  **先に植えた transition が先に（下に）合成される**。
- [事実] mvm の書き出しは、`clips` の並びの順に、上位 track の clip ごとに affine を 1 つ植える
  （`mvm_mlt_export.c`）。合成の順はこの並びで決まる。
- **試作の方針**: group は 1 つだけ作り、最初の member の位置へ 1 つの transition として植える。
  member にするのは、配置が対象で、かつ **並びで自分より前にある従来の経路の clip のどれとも時間が
  重ならない** clip である。group の中では、並びの順（下から）に合成する。
  - member と時間が重なる従来の clip は、必ず並びで member より後ろにある。その transition は group より
    後に植わるので、member の上に合成される。したがって、どの frame でも合成の順は従来と同じになる。
  - 同じ条件から、member の下にあるのは V1 と、同じ group の前の member だけになる（3.2 の下地の条件）。
- [事実] group の member を playlist に置いたままにすると、transition が隠すのは `b_track` の frame だけ
  なので、残りの member の frame が tractor の出力に選ばれてしまう（2.1）。試作では member を playlist に
  **置かない**（その区間は blank になる）。blank の frame は test card なので出力の候補にならない。
  transition は `accepts_blanks` を立てて、上の track が blank でも呼ばれるようにする。
- [事実] transition は `b_track` の frame を必ず隠す。従来の経路の clip が載っている track を `b_track` に
  すると、その clip の frame まで隠してしまう。試作では、映像 track の一番上に **何も載せない track を
  1 本足し**、それを `b_track` にする（audio の track はその上に続く）。

### 2.4 render worker の並行

- [事実] 書き出しは consumer の `real_time` を負の値（worker の数）にしている。worker は queue の frame を
  1 枚ずつ取り、`mlt_frame_get_image` を呼ぶ（`mlt_consumer.c` の `consumer_worker_thread`）。
  別々の frame の画像の callback が、同じ transition に対して並行に呼ばれる。
- [事実] `process`（frame を組み立てる段階）は、transition ごとの mutex の中で直列に呼ばれる。
- [事実] affine は callback の中で transition の service を lock して animation を評価する。
- **試作の方針**: transition の状態（member の一覧、frame ごとの位置と不透明度、画像への pointer）は
  作った後に書き換えない。準備済みの画像も変更できない型である。callback は lock を取らず、counter だけを
  atomic に足す。frame ごとの作業用の memory は callback の中で確保して解放する。
- [未検証] data race の検出器（ThreadSanitizer）は、この環境（MinGW）で使えないので実行していない。
  上は code の構造からの判断で、4 worker での全 frame 一致と、2 回の baseline の一致が実測の裏づけである。

### 2.5 transition と画像の寿命

- [事実] 植えた transition は、次に植えた service（最後は tractor）が参照を持つ
  （`mlt_service_connect_producer`）。tractor を閉じると鎖が順に閉じる。
- [事実] frame は、transition の pointer を **参照数を増やさずに** 画像の stack へ積む。
  frame が transition より長く生きると、解放済みの transition を参照する。
- [事実] consumer を止めると worker を join し、queue に残った frame を閉じる（`mlt_consumer.c`）。
  mvm の書き出しは consumer を閉じてから tractor を閉じるので、通常は frame が先に無くなる。
- **試作の方針**:
  - `process` で、frame に transition の参照を 1 つ持たせる（frame が閉じるときに返す）。
    閉じる順が変わっても、frame が生きている間は transition の状態が残る。
  - 準備済みの画像は、書き出しを呼ぶ側（`exportTimeline`）が、書き出しの関数から戻るまで保持する。
    transition は pointer だけを持つ。
  - 書き出しの関数は、graph を閉じた後に「生きている group の transition」と「実行中の callback」が
    0 であることを確かめ、0 でなければ成功を返さない。0 でない状態で戻ると、呼び出し側が画像を解放した
    後に参照されうるからである。

### 2.6 取消・終了・失敗の伝わり方

- [事実] 画像の callback が error を返すと、`mlt_frame_get_image` は test 画像を作って続行する
  （`mlt_frame.c` 522〜523 行）。**MLT は transition の失敗で書き出しを止めない。**
- **試作の方針**: callback は失敗を observer に記録する（最初の 1 件の理由と frame 番号）。書き出しの待ち
  loop がそれを見て consumer を止め、書き出しを失敗として返す。失敗した frame は元の画像のまま下流へ
  流れるが、書き出し全体が失敗になるので出力 file は残らない（上位の層が一時 file を消す）。
- 取消は従来と同じ経路（待ち loop が consumer を止める）で、group に固有の処理は無い。

### 2.7 画素の形式と alpha

- [事実] affine は a の画像を `mlt_image_rgba`（8 bit、straight alpha）で要求する。試作も同じ形式で要求し、
  戻った形式・幅・高さが出力と違えば失敗にする（別の形式へ黙って変換しない）。
- [事実] 下の画像の alpha が 255 だという保証は MLT に無い。試作は合成器の検査（全画素の alpha が 255）に
  通らなければ拒否する。
- 見えている member が 1 つも無い frame では、試作は形式を要求し直さず、下の要求をそのまま通す。

## 3. 試作の実装（Task C）

| file | 役割 |
| --- | --- |
| `src/media/mlt/mvm_mlt_math_group.c`・`.h` | group の transition（MLT の callback、buffer の複製、失敗の記録、寿命） |
| `src/media/mlt/mvm_mlt_export.c` | member の決定、member を playlist に置かない、transition を植える、失敗で書き出しを止める、解放の確認 |
| `src/media/mlt/mvm_mlt_export.h` | observer の入力・出力、clip の `math_group_raster`、合成の callback の型 |
| `src/app/timeline_export.cpp` | 準備済みの画像を作る（stage する PNG と同じ画素から。motion のある clip は qtcrop と同じ描き直しの後）、合成の callback |

`mvm_mlt` は合成器（`mvm_math`）に依存しない。合成は上位の層が渡す callback で行う。
新しい worker の queue も、frame をまたいで残る cache も作っていない。

### 3.1 既定では動かない

- 入口は `MvmExportObserver::diagnostic_math_group` だけである。製品の書き出しは observer に NULL を渡すので、
  この値を立てる手段が無い。Project の設定・書き出しの設定・UI からは有効にできない。
- observer が NULL、または値が 0 のとき、`timeline_export.cpp` は画像を準備せず、`mvm_mlt_export.c` は
  member の配列を作らない。graph は従来と同じになる。

### 3.2 group にする clip

上位の層は、静止の数式 clip（Write・変形・数式の連続・graph・字幕でないもの）だけを申告する。
MLT の層が、申告された clip を frame ごとの値で調べる。

- 配置が対象: 上位 track の映像、全 frame の矩形が出力と同じ大きさ（等倍）、回転・shear・crop が全 frame で 0、
  frame ごとの位置と不透明度がある。1 frame でも外れれば clip 全体を対象外にする（理由を残す:
  `scale`・`rotation-or-shear`・`crop` など）。
- 下に従来の経路の layer が重ならない（2.3）。重なる clip は `above-retained-layer` として残す。
- 対象外の clip は、従来の affine・qtcrop のまま書き出す。合成器へは渡さない。数と内訳を report に残す。

2 つ目の条件は、P3-1.2 の「合成を始める前の下地は不透明」という契約から来る。従来の affine が重ねた画素は
alpha が 254 になりうるので、その上の group は拒否される。最初の実装（合成の順で連続する分ごとに group を
作る）は、`cmp-region-scale` で実際にこれに当たった（`20261012-152522-parity/`）: 従来の layer の後の group が
「数式の合成先に不透明でない画素があります」で拒否され、書き出しが失敗した。黙って合成せず失敗したのは
意図どおりだが、設計としては使えないので、上の条件へ直した。

### 3.3 失敗のとき

- 合成は複製の上で行うので、検査に失敗しても渡された画像は変わらない（2.2）。
- 合成器は、対象外の入力（不透明でない下地、準備していない画像、大きさの違い）を、何も書かずに拒否する。
- 失敗は書き出し全体の失敗になる（2.6）。対象外の画素を別の計算で埋めて成功にはしない。

## 4. 寿命と対照（Task D）

`pwsh scripts/math-transition-p33a-verify.ps1`（`20261012-162458-final1/`）。対照は `cmp-layers-16`
（16 層、位置と不透明度が補間の途中）へ故障を入れる。

| 対照 | 期待 | 結果 |
| --- | --- | --- |
| 合成を 1 frame 飛ばす | 画素の不一致 | 検出（119/120 一致、9,770 画素） |
| 1 つ後の frame の値を使う（対応の誤り） | 画素の不一致 | 検出（91/120、40,131 画素） |
| 不透明度を半分にする | 画素の不一致 | 検出（0/120、1,152,754 画素） |
| 別の member の画像を使う（取り違え） | 画素の不一致 | 検出（0/120、1,433,281 画素） |
| 一番上の layer を落とす | 画素の不一致 | 検出（0/120、76,200 画素） |
| 下地に不透明でない画素を作る | 合成器が拒否し、書き出しが失敗 | 検出（「合成器が拒否しました…」で失敗、transition の残り 0） |
| 書き出しの途中で取消 | 取消で終わり、transition が残らない | 検出（25 frame を合成した後に取消、残り 0、実行中の callback 0） |

- 画素の対照 5 つは、比較そのものは成立している（全 frame を比べ、2 回目の baseline は 1 回目と一致）。
- [事実] 下地の対照では、待ち loop が失敗に気づくまでに 25 frame が元の画像のまま下流へ流れた。
  書き出しは失敗を返し、出力 file は残らない。
- 解放の確認: 全部の run（fixture 9 つ × 3 回、Project 全体の graph 4 区間、対照 7 つ、worker 1・8 本）で、
  書き出しの後に「生きている transition」と「実行中の callback」が 0 だった。
- 植える順序: 従来の layer と group が混ざる `cmp-region-scale`（group 2 clip、従来 34 clip）と Project 全体の
  graph（affine の transition は製品の経路で 120、candidate で 95）で全 frame が一致した。順序が 1 か所でも入れ替われば、重なる字形の
  画素が変わる。
- 並行: 製品と同じ worker 4 本に加えて、1 本と 8 本でも `cmp-layers-48` が 120/120 frame 一致した
  （`…-threads1/`・`…-threads8/`）。

[未検証]

- data race の検出器（2.4）。
- 「遅れて届いた完了を古いものとして捨てる」処理は、この設計に無い（合成は callback の中で同期に終わり、
  非同期の完了が無い）。近い対照は、取消の後の解放と、対応・画像の取り違えの 2 つである。
- MLT の memory pool（`mlt_pool`）に残る buffer の量は計っていない。確かめたのは transition の状態と
  callback の数だけである。

## 5. 画素の一致と時間（Task E）

基準は、同じ process で先に書き出した製品の経路の pre-YUV である。candidate の全 frame の全 byte を比べ、
最後にもう一度製品の経路で書き出して、1 回目と同じであることを確かめる。

### 5.1 画素

[事実] 最終の source で 3 回（`20261012-162458-final1/`・`20261012-165402-final2/`・`20261012-170653-final3/`）。

| fixture | frame | 3 回の結果 | group にした clip | 従来の経路へ残した clip（配置 / 下に従来の layer） |
| --- | --- | --- | --- | --- |
| `cmp-integer` | 120 | 3 回とも 120/120 一致 | 1 | 0 |
| `cmp-fraction-fade` | 120 | 3 回とも一致 | 1 | 0 |
| `cmp-layers-16` | 120 | 3 回とも一致 | 16 | 0 |
| `cmp-layers-48` | 120 | 3 回とも一致 | 48 | 0 |
| `cmp-region-motion` | 120 | 3 回とも一致 | 28 | 0 |
| `cmp-region-scale` | 180 | 3 回とも 180/180 一致 | 2 | 34（2 / 32） |
| `p2-window-early` | 60 | 3 回とも一致 | 24 | 0 |
| `p2-window-middle` | 60 | 3 回とも一致 | 40 | 0 |
| `p2-window-late` | 60 | 3 回とも一致 | 24 | 0 |
| Project 全体の graph、frame 1200〜（60） | 60 | 一致（1 回） | 25 | 74（2 / 72） |
| 同、3400〜 | 60 | 一致（1 回） | 25 | 74（2 / 72） |
| 同、5280〜 | 60 | 一致（1 回） | 25 | 74（2 / 72） |
| 同、6500〜 | 60 | 一致（1 回） | 25 | 74（2 / 72） |

- 比べなかった frame は無い。対象外の clip は従来の経路で書き出され、その frame も比較に含まれている。
  P3-1.2 で「対象外」だった `cmp-region-scale` の 119 frame（拡縮する layer を含む）も、ここでは全画素を
  比べて一致している（拡縮する layer は MLT が合成している）。
- 残した clip の数は、申告した clip（静止の数式）の中での数である。申告しない clip（Write・変形など）は
  数えていない。

### 5.2 時間

[事実] 1920×1080、worker 4 本（製品と同じ）。

単体の計測（`mvm_test_math_group_compose bench`、MLT の外）:

| 項目 | 時間 |
| --- | --- |
| 下地の alpha の検査（全画素） | 0.38 ms / 回 |
| 合成先の複製（全画面 1 枚、`memcpy` だけ） | 0.20 ms / 回 |
| 画像の準備（複製と範囲の走査、1 枚 7.9 MB） | 2.3 ms / 枚 |
| 合成器だけ（40 層、字形 532×20、端数位置） | 9.5 ms / frame |

書き出しの中（3 回の中央値。render は consumer の開始から終了まで）:

| fixture | group の callback（1 frame）: 複製 / 合成 / 全体 | 画像の memory | 画像の準備 | render 秒: candidate / 製品（2 回目） | 比（3 回） |
| --- | --- | --- | --- | --- | --- |
| `cmp-integer`（1 層） | 1.2 / 1.9 / 3.0 ms | 7.9 MB | 6 ms | 1.5 / 1.8 | 1.18・1.19・1.30 |
| `cmp-fraction-fade`（1 層） | 1.1 / 1.5 / 2.6 ms | 7.9 MB | 3 ms | 1.5 / 1.6 | 1.12・1.10・1.08 |
| `cmp-layers-16` | 1.1 / 7.5 / 8.7 ms | 127 MB | 189 ms | 2.2 / 18.6 | 8.62・8.37・9.07 |
| `cmp-layers-48` | 1.1 / 24.7 / 25.9 ms | 380 MB | 437 ms | 3.5 / 55.6 | 15.4・16.1・16.3 |
| `cmp-region-motion` | 1.5 / 13.6 / 15.1 ms | 222 MB | 266 ms | 3.8 / 30.4 | 7.84・7.84・8.04 |
| `cmp-region-scale` | 1.3 / 1.2 / 2.5 ms | 285 MB | 368 ms | 59.5 / 63.0 | 1.07・1.06・1.04 |
| `p2-window-early` | 1.6 / 13.2 / 15.0 ms | 190 MB | 236 ms | 2.2 / 17.3 | 8.12・7.60・7.93 |
| `p2-window-middle` | 1.3 / 27.8 / 29.1 ms | 316 MB | 394 ms | 2.1 / 26.0 | 12.2・12.0・14.0 |
| `p2-window-late` | 1.3 / 12.7 / 14.0 ms | 190 MB | 281 ms | 2.4 / 20.3 | 8.56・8.22・8.51 |
| Project 全体の graph、1200〜 | 1.2 / 3.1 / 4.3 ms | 783 MB | 930 ms | 21.5 / 30.0 | 1.39（1 回） |
| 同、3400〜 | 1.1 / 5.8 / 6.9 ms | 783 MB | 941 ms | 41.2 / 48.3 | 1.17（1 回） |
| 同、5280〜 | 1.2 / 1.2 / 2.4 ms | 783 MB | 984 ms | 39.6 / 46.2 | 1.17（1 回） |
| 同、6500〜 | 1.2 / 1.1 / 2.3 ms | 783 MB | 932 ms | 38.6 / 45.0 | 1.17（1 回） |

読み方と限界:

- 「製品（2 回目）」は、candidate の後にもう一度書き出した製品の経路である。1 回目の製品の経路は、2 回目より
  常に 3〜5 割遅い（process の起動直後の影響）。比は candidate に不利な 2 回目との比で出した。
  書き出しの順は毎回「製品 → candidate → 製品」で、順を入れ替えた計測はしていない。
- 数式が 16 層以上重なる fixture では、利益は 3 回とも同じ大きさで出た（7.6〜16 倍）。
- 1 層の fixture の差（1.1〜1.3 倍）は、0.1〜0.3 秒の差で、書き出しの順の影響と区別できない。
  利益があるとは言えない。
- `cmp-region-scale` と Project 全体の graph では、group にできた clip が少なく、利益は小さい
  （1.04〜1.39 倍）。残した clip の affine と、transition 1 つごとの固定費がそのまま残る。
  Project 全体の graph の値は各 1 回である。
- 「合成」には下地の検査（上の 0.38 ms）が含まれる。書き出しの中の「複製」（1.1〜1.6 ms）は、単体の
  `memcpy`（0.20 ms）より大きい。buffer の確保と、cache に乗っていない画面の読み書きを含む。
- 画像の memory は、clip 1 つにつき全画面 1 枚（7.9 MB）を持つためで、字形の範囲だけを持つようにはしていない。
  Project 全体では 783 MB になる。**このままでは製品に入れられない大きさである。**
- render の時間は合成と encode の両方を含む。candidate の書き出しは、どの run でも成功を返し、全 frame の
  pre-YUV が検査の callback を通っている。
- [未検証] encoder・muxer の進み（encode した frame・packet の数）は、candidate の書き出しでは別に数えて
  いない（harness が数えるのは最初の製品の経路の書き出しだけ）。

[未検証] v4 の Project 全体（6840 frame）の書き出しは実行していない。Project 全体の graph での確認は、
上の 60 frame の 4 区間だけである。

## 6. gate と provenance

| gate | 結果 |
| --- | --- |
| 合成器の単体試験（`math_group_compose_oracle`） | 通過（oracle と 1,064,872 byte 一致） |
| 関連の CTest（`math`・`mlt_export`・`m7b_3_product_export`・`timeline_export` に合う 49 件） | 49/49 通過（`ctest-final.log`）。`math_equation_sequence_product_ui` も通過 |
| lint | 通過 |
| 全 CTest | 実行していない |

- 最終の 5 run（`final1`〜`final3`、`threads1`、`threads8`）の `identity.json` にある 15 file（harness の binary、
  MLT・Qt の DLL、Project、関係する source、script）の SHA-256 は、gate の後の作業 tree・binary と一致している。
  git の HEAD は `4f83f2b`（P3-1.2）で、P3-3A の変更は commit していない。
- 使わない run（消さずに残した）:
  - `20261012-152404-smoke/`・`20261012-152522-parity/`: 最初の実装（連続する分ごとの group）。
    `…-parity/` は `cmp-region-scale` の失敗（3.2）の記録である。
  - `20261012-154520-smoke2/`・`20261012-155215-run1/`: 設計を直した後、内訳の counter を足す前の build。
    結果は最終と同じ（全 frame 一致、対照 7 つ検出）だが、最終の source ではない。
  - `20261012-162119-run2/`: 途中で止めた run（source を書き換えた後に始まったので使わない）。
- P1〜P3-1.2 の成果物と判定は変更していない。

## 7. 次に必要なこと

1. **下地の条件をどうするか決める。** Project 全体で利益を出すには、従来の layer（alpha 254 の画素を作る）
   の上でも group を使える必要がある。候補は 2 つある。
   - 合成器の対象を「下地の alpha が 254 以上」へ広げる。P3-1.2 は、合成の途中で 254 になった画素の上への
     合成が MLT と一致することを確かめているが、「合成を始める前から 254 がある下地」を対象として
     実際の MLT と比べてはいない。広げるなら、その比較が要る。
   - 拡縮する layer を合成器が扱えるようにする（下の layer ごと group に入る）。MLT の拡縮の画素の
     振る舞いは確かめていない。
2. 画像の memory を、字形の範囲だけを持つ形にする。
3. data race を検出器で確かめる手段を用意する（別の toolchain での build など）。
4. その後で、v4 の Project 全体の書き出しで時間を測る。
5. Task A の SegFault は、再現したときに dump を採る（procdump の手順は script にある）。
