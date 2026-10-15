# P3-5A: 全長の書き出しの再現と、MLT の統合部分の寿命・並行の確認

2026-10-13。[P3-4](math-export-p34.md) で残っていた次の 3 つを扱った。

1. 全長（6840 frame）の書き出しは 1 回だけだった。
2. 全長の画素の比較は、frame ごとの 64 bit の digest だった。baseline は 12 区間に分けて採った。
3. MLT の統合部分を、race の検出器の下で動かしていなかった。

製品の書き出しの経路・timeout・MLT の affine・画素 oracle・合成の式は変えていない。数式 group は、製品の
書き出しからは動かない。commit・push はしていない。P1〜P3-4 の成果物と判定は変更していない
（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。

証拠: P3-4 と同じ identity の run は `build/math-export-p34-20261013/`、harness の改訂と検出器は
`build/math-export-p35a-20261013/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| 全長のくり返し | **PASS。** 追加の 2 回とも完走。P3-4 と合わせて 3 回、どれも同じ identity |
| 600 秒の要件 | **PASS（診断用の経路として）。** 272.4・294.1・281.5 秒 |
| frame の被覆と、encoder 直前の RGBA の一致 | **PASS。** 3 回とも 6840/6840 frame の digest が P3-4 の baseline と一致。今回、SHA-256 と byte の比較を足した（2） |
| H.264 の復号・mux | **PASS。** 3 回とも 6840 frame、時刻は 1 frame ずつ。出力の MP4 は 3 回とも同じ SHA-256 |
| 数式 group の code の寿命・並行 | **検出器で確認（隔離 harness）。** MLT 7.36.1 の framework ごと TSan・ASan で build し、worker 1・4・8 本で報告 0。対照 3 つを検出 |
| **MLT の color producer の data race** | **新しく見つかった問題（HOLD）。** MLT 本体の不具合で、数式 group の code の外。製品の経路も同じ producer を使う（3.3） |
| 全 CTest・lint | lint 通過。release の全 CTest は 1514 件中 1513 件通過、**1 件 SegFault（`transition_preview`）**。原因は preview の audio の後始末の解放後参照で、今回の変更とは無関係（4） |
| 製品での有効化 | **HOLD。** 理由は 5 |

## 1. 全長のくり返し（Task A）

P3-4 の `freeze.json`（755 file）と照合してから実行した。source・binary・runtime・Project は P3-4 と同じで、
build も変異もしていない。git の HEAD は、利用者の commit で `7277de5` から `58a43d4` に変わったが、
file の内容は同じである（755 file の SHA-256 が一致）。

| | P3-4 | 追加 1 | 追加 2 |
| --- | --- | --- | --- |
| directory | `20261013-011412-full` | `20261013-021747-full-repeat1` | `20261013-023241-full-repeat2` |
| 書き出しの呼び出しから戻りまで（秒、単調な時計） | 272.4 | 294.1 | 281.5 |
| 外部: 起動から最終 file が現れるまで（秒） | 276.2 | 297.9 | 285.2 |
| render・encode・mux（秒） | 260.8 | 281.7 | 268.8 |
| CPU 時間（秒） | 1,064 | 1,089 | 1,083 |
| working set の peak（MB） | 1,656 | 1,659 | 1,664 |
| commit の peak（MB） | 1,956 | 1,956 | 1,963 |
| 判定・graph | ELIGIBLE・数式 group | 同じ | 同じ |
| group の callback・合成した回数・layer の延べ | 13,730・13,491・184,773 | 同じ | 同じ |
| group の失敗 | 0 | 0 | 0 |
| 書き出し後の transition・callback・画素 | 0・0・0 | 0・0・0 | 0・0・0 |
| encoder へ渡した frame・mux した packet | 6840・6840 | 6840・6840 | 6840・6840 |
| RGBA を観測した frame 番号 | 0〜6839 を 1 つずつ | 同じ | 同じ |
| P3-4 の baseline と digest が一致 | 6840/6840 | 6840/6840 | 6840/6840 |
| 復号（ffmpeg） | 6840 frame、error 0 | 同じ | 同じ |
| 出力の MP4 の SHA-256 | `FF8EB4D5…9A8BF6C0` | **同じ** | **同じ** |
| watchdog による打ち切り・dump | なし・0 | なし・0 | なし・0 |

- [事実] 3 回とも 600 秒以内。最も遅い回は 294.1 秒で、余裕は約 2.0 倍。
- [事実] 出力の MP4 は 3 回とも byte 単位で同じである（2,062,352 byte）。
- frame 番号は 0〜6839 が 1 行ずつで、欠けも重複も無い。最後の frame（6839）だけは consumer が 26 回
  要求するが、どの回も画素は同じで（くり返しの不一致 0）、encoder へ渡るのは 6840 frame である。
- **追加 1 の時間についての注意**: この run の間、別の作業（WSL での MLT の source の取得と展開、source の
  閲覧）をしていた。294.1 秒はその影響を含みうる。捨てずに記録する。
- **追加 2 についての経過**: 最初の起動は、実行の前の照合で拒否された（書き出しは始まっていない。
  `p35a-repeats.log`）。私が `tests/sanitizer/` に新しい file を足したため、固定した集合に「増えた」file が
  あったからである。新しい file を repository の外へ移し、755 file が一致する状態に戻してから実行した。
  失敗した書き出しを捨てたのではない。

## 2. 画素の証拠（Task B）

### 2.1 64 bit の digest の点検

`tests/harness/math_score_export_parity.cpp` の `digestFrame`。

- **frame 番号との対応**: MLT の frame の位置（`mlt_frame_get_position`）を key にして記録する。区間を
  指定した書き出しでは、file の番号は区間の先頭からの相対で、比較の script が区間の先頭を足す。
  この足し算が 1 つずれていれば、絵が変わる frame（全長で 1,860 か所）で不一致になる。全 frame が一致
  しているので、対応は合っている。
- **初期値と畳み方**: frame ごとに同じ定数から始め、8 byte ずつ「xor して掛け、上位を下位へ混ぜる」。
  1920×1080×4 byte は 8 の倍数で、端数の処理は使われない。行の間の詰め物は無い（幅 × 4 byte）。
- **必ず検出できる違い**: 1 step は、状態についても入力の 8 byte についても 1 対 1 の写像である。
  したがって、**違いが 1 つの 8 byte（横に並ぶ 2 画素）の中だけなら、digest は必ず変わる。**
- **検出を保証できない違い**: 2 つ以上の 8 byte が違う場合は、偶然 digest が同じになる可能性が残る。
  暗号学的な hash ではない。比較の script は alpha の最小値も比べているが、補助にすぎない。
- **含んでいないもの**: frame 番号と大きさは digest に入っていない。同じ絵の frame は同じ digest になる
  （全長で digest は 1,861 種類）。同じ絵の frame どうしの入れ替わりは digest では分からないが、出力の
  画素は変わらない。
- 同じ frame が 2 回以上要求されたとき、画素が違えば記録する仕組みがあり、3 回とも 0 件である。

### 2.2 足した証拠

harness を改訂した（`--strong-digest`: frame ごとの全 byte の SHA-256、`--dump-frames`: 指定した frame の
RGBA をそのまま file へ書く）。描く画素には触れない。**この改訂は harness の binary を変える**ので、
P3-4 の固定とは別の cohort として固定し直した（`build/math-export-p35a-20261013/freeze.json`、757 file、
harness `307AE651…287FDB28`）。1 の 3 回は、この改訂の前の binary である。

`20261013-024935-strong/`。全長の baseline は取り直していない。

| 比べたもの | 範囲 | 結果 |
| --- | --- | --- |
| (a) candidate（全長 1 回）の 64 bit の digest 対 P3-4 の baseline | 全 6840 frame | 6840/6840 |
| (b) candidate（全長）の SHA-256 対 製品の経路の SHA-256 | 20 区間 × 6 frame = 120 frame | 120/120 |
| (c) candidate（全長）の RGBA 対 製品の経路の RGBA、**全 byte** | 40 frame（各区間の 2 枚） | 40/40 |
| (d) 今回の製品の経路の区間の digest 対 P3-4 の baseline（書き出し始めの位置が違う同じ frame） | 120 frame | 120/120 |
| (e) candidate の中で、digest が同じなのに SHA-256 が違う frame | 全 6840 frame | 0 |

- 区間は実行の前に決めた: 先頭、P3-4 の 11 か所の区間の境目（前 3 frame・後 3 frame）、既存の代表 frame
  （600・5370・6720）、P3-3D の区間の先頭（1200・3400・5280・6500）、末尾。
- (c) は hash を介さず、`fc /b` で file どうしを比べた。dump した 40 枚の SHA-256 は、digest の file に
  記録した値と同じである（dump と hash が同じ画素を見ている）。dump は比べた後に消した（1 枚 8.3 MB）。
- (d) は「区間の途中から書き出した frame は、別の位置から書き出した同じ frame と同じ」ことの確認である。
  P3-4 では区間の先頭だった frame（570 の倍数）を、今回は区間の 4 枚目として書き出し、同じ digest になった。
  確かめたのは境目の前後だけで、全 frame ではない。
- (e) により、この出力の中では、64 bit の digest の違いと SHA-256 の違いが 1 対 1 に対応している
  （どちらも 1,861 種類）。
- この全長の run（SHA-256 と dump の負荷つき）は 306.8 秒で、出力の MP4 は 1 の 3 回と同じ SHA-256 である。

### 2.3 何が言えて、何が言えないか

1. **byte 単位の比較が済んでいるもの**: 短い fixture と全体 graph の 4 区間（P3-3D まで、2,640 frame）、
   P3-4 の判定の対照（1,200 frame）、今回の 40 frame。
2. **全長で言えること**: 6840 frame すべての 64 bit の digest が、製品の経路と一致する（4 回の書き出しで
   同じ結果）。これは、全画素の byte を比べたことと同じではない。
3. **今回足したこと**: 120 frame の SHA-256 の一致と、40 frame の全 byte の一致。全長の baseline の
   SHA-256 は採っていないので、残りの 6,720 frame は 64 bit の digest のままである。
4. 全長の製品の経路の baseline は、今も 12 区間に分けて採ったものである。区間をまたぐ整合は (d) の
   120 frame で確かめた。

## 3. MLT の統合部分の寿命・並行（Task C）

### 3.1 検出器の下で動かせるか

| 手段 | 結果 |
| --- | --- |
| Windows の製品の toolchain（MSYS2 UCRT64 の gcc） | ThreadSanitizer・AddressSanitizer とも無い |
| UCRT64 の clang（22.1.8） | 検出器の runtime が入っていない（`lib/clang/*/lib/windows` に asan・tsan が無い） |
| MSYS2 CLANG64 の環境 | 入っていない。入れると利用者の環境を変えるので、行っていない。ThreadSanitizer は Windows に無い |
| Linux（WSL）で `mvm_mlt_export.c` をそのまま build | **できない。** Windows の API（`Sleep`・`psapi`・file の API）と、Qt・FFmpeg の MLT module（`qimage`・`qtcrop`・`avformat`）を必須にしている |
| Linux で、MLT 7.36.1 の framework と core module を検出器付きで build し、実物の `mvm_mlt_math_group.c` を動かす | **できた。** 今回の方法 |

採った方法（`scripts/math-group-p35a-sanitizers.sh`、`tests/sanitizer/`）:

- MLT は tag `v7.36.1` の source（tarball の SHA-256 を照合）から、framework と core module だけを
  `-fsanitize=thread`、`-fsanitize=address,undefined` で build した。MLT の中の code も検出器が追う。
- `mvm_mlt_math_group.c` と `math_group_compose.cpp` は、製品と同じ source をそのまま build する。
  `<windows.h>` だけは、`Interlocked` 系を gcc の atomic へ置き換える代わりの header を使う。
- graph の組み立て・待ち loop・閉じる順は、harness が `mvm_mlt_export.c` と同じ順で書き直している
  （V1 は color producer、空の track、数式 group の transition、`null` consumer、出力直前の画素の検査）。
- **対象外のまま**: `mvm_mlt_export.c` そのもの（graph の構築、判定、後始末の実物）、`timeline_export.cpp`、
  `avformat` consumer、`qimage`、affine。これらは code の読解と、Windows での実行（1・P3-3D）による。
- compiler は g++ 11.4（WSL の Ubuntu 22.04）。Windows の binary ではない。

### 3.2 数式 group の code の結果

`build/math-export-p35a-20261013/sanitizers/`（最終の source。`identity.txt` に SHA-256）。

| scenario | worker | ThreadSanitizer | AddressSanitizer + UBSan |
| --- | --- | --- | --- |
| 書き出しを同じ process で 3 回。全 240 frame の画素を 1 thread の参照と比べる | 1・4・8 | 報告 0、画素一致 | 報告 0、画素一致 |
| 途中で consumer を止める（0・1・8・100 枚の時点） | 1・4・8 | 報告 0 | 報告 0 |
| 下地が対象外（全 worker が同時に失敗を記録）× 5 回 | 1・4・8 | 報告 0 | 報告 0 |
| transition を作る途中の失敗、使わずに閉じる | — | 報告 0 | 報告 0（解放漏れ 0） |
| frame が transition より長く生きる | — | 報告 0 | 報告 0、画素一致 |

確かめた点:

- **frame と transition の参照**: transition を閉じた後で、その transition を通した frame の画像を要求しても、
  状態は生きていて（frame が参照を 1 つ持つ）、画素は参照と同じ。frame を閉じると状態が解放される。
- **取消・consumer を閉じた後の callback**: consumer を閉じた後、callback の数は増えない。graph を閉じた後、
  生きている状態・実行中の callback は 0。検出器は、解放後の参照を報告しない。
- **準備済みの画像の寿命**: 画像は書き出しの関数から戻るまで呼び出し側が持つ。合成の間に解放すると
  検出される（下の対照）。
- **observer の共有の状態と最初の失敗の公開**: 8 本の worker が同時に失敗しても、race の報告は無い。
  最初の失敗の文は完全な形で読める（公開の flag が立ってから読む）。
- **初期化の途中の失敗**: 3 つの member を確保した後で失敗しても、解放漏れは無い。
- **閉じる順**: consumer → tractor → playlist → producer の順（`mvm_mlt_export.c` と同じ）で、報告は無い。

対照（検証の方法が働いていること）:

| 対照 | 期待 | 結果 |
| --- | --- | --- |
| frame が持つ参照を外してから transition を閉じ、画像を要求する | 解放後の参照 | AddressSanitizer が `math_group_get_image`（`mvm_mlt_math_group.c:147`）で報告 |
| 画像を解放してから合成する | 解放後の参照 | AddressSanitizer が合成器の中（`math_group_compose.cpp:313`）で報告 |
| 合成の callback が共有の変数へ同期なしで書く（worker 8 本） | data race | ThreadSanitizer が callback の中で報告 |

**書き出し後の counter が 0 であることは、解放後の参照が無いことの証明としては使っていない。** 上の結果は、
検出器が実際の読み書きを追った結果である。ただし、追ったのは隔離 harness の実行であって、Windows の
製品の binary ではない。

### 3.3 見つかった問題: MLT 7.36.1 の color producer の data race

**数式 group の code ではなく、MLT 本体（`src/modules/core/producer_colour.c`）の不具合である。**
製品の書き出しは、V1 の隙間をこの producer で埋めている（`mvm_mlt_export.c` の `v1_background`）。

- [事実] 最初の実行（`sanitizers-first-run/`）で、worker が 4 本・8 本のとき、ThreadSanitizer が
  `producer_colour.c` の中の data race を報告した（worker 1 本では報告なし）。
- [事実] 原因は source で確かめられる。color producer は、作った画像を producer に cache する。
  cache を作り直すとき、「作った」という記録（大きさ・形式・色）を書いて **lock を外してから**、画素を
  埋める（124〜133 行で確保・記録して unlock、その後で塗る）。同時に来た別の worker は、lock を取り、cache が
  あると判断し、**埋まる前の buffer を複製して返す**（233 行）。
- [事実] AddressSanitizer の build では、これが実際に誤った画素になった。worker 4 本・8 本の最初の
  書き出しで、下地が未初期化（alpha 0）になり、合成器が拒否して書き出しが止まった（数式 group の経路は
  失敗として止まる）。
- [事実] harness で、consumer を始める前に 1 thread で cache を作っておくと（prewarm）、race の報告も
  画素の誤りも消えた（3.2 はこの状態での結果）。prewarm を外した scenario（`mlt-color-race`）は、
  記録として残してあり、ThreadSanitizer が毎回報告する。
- [事実] Windows の書き出しでは、現れていない。V1 が隙間だけ・前半だけの fixture を、同じ process で
  製品の経路 → 数式 group → 製品の経路 の順に書き出すことを 10 回くり返し（30 回の書き出し）、
  全 frame が一致し、失敗は 0 だった（`20261013-030712-color/`）。P3-4 の 4 回（12 回の書き出し）も同じ。
- [推測] 現れる条件は、cache を作る最初の frame（と、要求の形式・大きさが変わった直後の frame）を、
  2 本以上の worker が同時に要求することである。窓は 8 MB を塗る数 ms。Windows で現れていないのは
  timing によるもので、起きないことの保証ではない。
- **影響の範囲**:
  - v4 の Project は、V1 が全区間 1 枚の静止画で、color producer を使わない。1・2 の結果には関わらない。
  - V1 に隙間がある Project では、**製品の経路でも** 隙間の最初の frame の下地が誤る可能性がある。
    製品の経路は下地の alpha を検査しないので、誤った画素がそのまま出る [推測。Windows では未観測]。
  - 数式 group の経路では、下地が alpha 0 なら合成器が拒否して書き出しが失敗する（黙って出ることはない）。
    alpha が 0 でない誤り（塗りかけ）は検出されない。

提案する最小の修正（**この phase では行っていない**。製品の書き出しの経路を変えるため）:

1. `mvm_mlt_export.c` で、V1 の背景の color producer に形式（`mlt_image_format = rgba`）を固定し、
   consumer を始める前に 1 thread で画像を 1 回要求して cache を作っておく。以後は cache が作り直されない。
2. 回帰の試験: `tests/sanitizer/` の harness に、同じ処置をした scenario を足し、ThreadSanitizer の報告が
   0 になることを確かめる。処置を外した `mlt-color-race` が報告することを、対照として残す。
3. Windows では、V1 に隙間のある fixture の最初の frame の下地が黒（alpha 255）であることを、既存の
   RGBA の検査で確かめる。

形式を固定すれば作り直しが起きないことは、製品の graph では確かめていない [未検証]。

## 4. 回帰の gate（Task D）

### 4.1 実行したもの

| gate | 結果 |
| --- | --- |
| lint | 通過 |
| 数式 group の oracle（`math_group_compose_oracle`・`math_group_alpha_floor_lemma`・`math_group_preflight_decisions`） | 通過（全 CTest の中で実行） |
| release の全 CTest（`pwsh scripts/test.ps1 -Preset ucrt64-release`。通常の試験 1514 件、並列 8） | **1513 通過、1 失敗**（`ctest-full.log`、1,027 秒） |
| debug の全 CTest | 実行していない |

- 実行した binary は、`build/math-export-p35a-20261013/freeze.json` の cohort（harness の改訂の後）である。
  `test.ps1` は build を伴うが、実行の後も 757 file の SHA-256 は固定と一致している（作り直されていない）。
- 開始の時点で利用者の無操作は 696 秒だった。`test.ps1` は描画の試験のために自動消灯を止める。

### 4.2 失敗した 1 件: `transition_preview` の SegFault

[事実] 全 CTest の中で、`transition_preview` が開始 0.53 秒で SegFault になった。

- **今回の変更との関係**: この試験の binary（`mvm_test_transition_preview.exe`）は 2026-10-13 00:46 に
  作られたもので、P3-5A の変更（harness の source 1 つと、build に入らない file）より前である。
  数式 group の code は、observer の診断用の入力が無ければ動かない。
- **再現**: 単独で 5 回実行すると、4 回通過、1 回は別の形（39.7 秒で Failed）で失敗した。
  **procdump の下で起動すると、12 回中 12 回、開始直後に ACCESS_VIOLATION になった**
  （`transition-preview-cohort/`。dump は 2 つを残し、同じ内容の 10 個は消した）。gdb の下で起動しても
  同じ場所で止まる。debugger の下で起動した process は debug heap を使い、解放した memory を
  `0xFEEEFEEE` で埋めるので、解放後の参照が毎回表に出る。
- **場所**（`gdb-run-1.txt`）: `AudioMasterClock::stop()`（`audio_clock.cpp:141`）が mutex を lock する
  ところ。register に `0xFEEEFEEEFEEEFEF1` があり、解放済みの memory を読んでいる。呼び出しは
  `~WasapiAudioSink` → `WasapiAudioSink::stop` → `clock_.stop()`、その上は
  `PreviewEngine::removeSource`（`preview_engine.cpp:2907`、関数の終わり）←
  `MvmController::applyAudioSourceFor` ← `seekTimelineFrame`。
- **原因**（source で確かめた）: `removeSource` は、sink の参照を局所変数 `audioSink` に写してから、
  `impl_->audioSink.reset()`・`impl_->audioClock.reset()` を行う。sink は clock を参照（`AudioMasterClock&`）
  で持っている。局所変数が残っているので、sink は関数の終わりまで生きていて、そのときには clock は
  解放されている。sink の destructor が `stop()` を呼び、解放済みの clock を触る。
  この局所変数は 2026-08-20 の commit（`9db677fc`）からある。
- **これは製品の preview の code の不具合で、数式の書き出しとは無関係である。** 通常の起動では、解放した
  memory がまだ読めることが多く、たまにしか落ちない [推測]。
- **過去の UI の SegFault との関係**: `math_equation_sequence_product_ui` を同じ方法（gdb の下で起動）で
  1 回動かしたが、落ちずに 545 検査が通過した（`gdb-equation-sequence-ui-1.txt`）。同じ原因だという
  証拠は得られていない。**UNKNOWN のまま**である。

提案する最小の修正（**この phase では行っていない**。preview の製品の code を変えるため）:

1. `PreviewEngine::removeSource` で、`impl_->audioClock.reset()` の前に、局所の `audioSink`（と
   `audioWorker`）を手放す。または、sink を止めた後は clock を触らないようにする。
2. 回帰の試験: `transition_preview` を debugger の下で起動する形（debug heap）を試験に足す。修正の前は
   毎回落ち、修正の後は通ることを確かめる。

## 5. 製品で有効にできるか（Task E）

**P3-5B（実時間の timeout）と P3-6（製品での有効化）へは、まだ進まない。** 3.3 の問題が見つかったので、
先にそれを扱う。

| 項目 | 状態 |
| --- | --- |
| 全長のくり返し | PASS（3 回、同じ identity、同じ出力） |
| 600 秒 | PASS（診断用の経路。272〜294 秒） |
| frame の被覆と RGBA の一致 | PASS。全長は 64 bit の digest、120 frame は SHA-256、40 frame は全 byte |
| H.264 の復号・mux | PASS |
| 数式 group の code の寿命・並行 | 隔離 harness で検出器の報告 0。`mvm_mlt_export.c` の実物は検出器の対象外のまま |
| MLT の color producer の race | **HOLD。** 製品の経路にも関わる。Windows では未観測 |
| 回帰 | 1513/1514。残る 1 件は、既存の preview の code の解放後参照（4.2） |

残っているもの:

1. **MLT の color producer の race の処置**（3.3 の提案）。V1 に隙間のある Project の正しさに関わるので、
   数式 group を製品で有効にするかどうかとは別に、先に扱うことを勧める。
2. `mvm_mlt_export.c` の実物を検出器の下で動かす手段が無い（Windows の API と Qt・FFmpeg の module への
   依存）。隔離 harness は同じ順を書き直したもので、実物ではない。
3. 製品の timeout は `Sleep` の回数で数える（変えていない）。
4. 製品から数式 group を使う入口が無い。実行時に拒否されたときに従来の graph でやり直す仕組みも無い
   （今回も、自動のやり直しは入れていない）。
5. process を外から止めると一時 file が残る（P3-4）。
6. V1 に動画がある Project は UNKNOWN になり、従来の経路のまま。
7. UI の SegFault（UNKNOWN）。

勧める順: **color producer の race の処置（小さい phase）→ P3-5B → P3-6。**
