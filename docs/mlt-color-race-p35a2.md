# P3-5A.2: MLT の color producer の cache の公開の race への対処

2026-10-13。[P3-5A](math-export-p35a.md) で見つかった、MLT 7.36.1 の color producer（V1 の隙間の黒）の
data race を、製品の書き出しの graph で起きない形にした。

MLT の code（vendor）は変えていない。数式 group の合成・書き出しの timeout・画素の authority・MLT の affine は
変えていない。数式 group は、製品の書き出しでは無効のままである。commit・push はしていない。
P3-5A の race の記録と全 CTest の結果（1513/1514）は、履歴としてそのままである。
`math_equation_sequence_product_ui` の SegFault は UNKNOWN のままである。

証拠は `build/mlt-color-race-p35a2-20261013/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| cache の契約（何で作り直されるか） | source で確かめた。作り直しは「色の文字列・幅・高さ・形式のどれかが前の要求と違う」ときだけ（1） |
| prewarm が成り立つか | **成り立つ。** ただし形式を固定しないと成り立たない（製品の書き出しでは、要求の形式が frame で変わっていた） |
| 修正 | 背景の形式を rgba に固定し、consumer を始める前に 1 thread で cache を作り、以後は画像の要求ごとに cache が変わっていないことを確かめる。変わっていたら書き出しを失敗にする |
| 検出器（保護あり） | ThreadSanitizer・AddressSanitizer + UBSan とも報告 0（worker 1・4・8 本） |
| 検出器（保護なしの対照） | ThreadSanitizer が `producer_colour.c` の中の data race を報告。AddressSanitizer の build で、黒でない背景を 10 回中 10 回受け取った |
| Windows の出力 | 修正の前後で MP4 と encoder 直前の RGBA が同じ（36/36） |
| 全 CTest・lint | lint 通過。**release の全 CTest は PASS にしていない**: 1517/1522。落ちた 5 件は window の描画が必要な試験で、実行中に monitor が無くなっていた（4.2） |

**全体: race への対処そのものは成立（保護した graph で検出器の報告 0、出力は不変、作り直しは検査が見つける）。全 CTest の gate だけが未成立で、monitor のある状態での取り直しが残っている。****

## 1. cache の仕組み（Task A）

MLT 7.36.1 の `src/modules/core/producer_colour.c`（SHA-256 `3c8601eb…`、`mlt-7.36.1-source/` に保存）。
実行時の binary は MSYS2 の配布物で、source との対応は version の一致による [推測]。

### 1.1 作り方と、作り直す条件

`producer_get_image` は次の順に動く。

1. producer の lock を取る。
2. cache（producer の property `image`）と、作ったときの幅・高さ・形式（`_width`・`_height`・`_format`）、
   色の文字列（`_resource`）を読む。
3. 要求の形式を決める。producer に `mlt_image_format` が設定されていれば、**要求側の形式に関わらず**それを使う。
   ただし、設定が rgba で要求が rgba64（またはその逆）のときは、要求の形式を使う。
4. **色の文字列・幅・高さ・形式のどれかが cache と違えば、作り直す**: 新しい buffer を確保し、`image` と
   幅・高さ・形式・色を書き換え（古い buffer はここで解放される）、**lock を外してから**、画素を埋める。
5. 違わなければ、lock を外す。
6. cache を複製して返す（lock の外）。

[事実] race は 2 つある。

- 作り直している thread が画素を埋め終える前に、別の thread が 5→6 で「埋まる前の cache」を複製する。
- 6 で複製している途中の thread がいても、別の thread の作り直しが古い buffer を解放する。

### 1.2 6 つの問いへの答え

| 問い | 答え | 根拠 |
| --- | --- | --- |
| 1. 作る・作り直す条件 | 色の文字列が違う、幅か高さが違う、形式が違う、のどれか。最初の要求は必ず作る | [事実] source の条件式 |
| 2. cache の key | 色の文字列（`resource`）、要求の幅・高さ、形式。frame の位置は入らない | [事実] |
| 3. 書き出しの途中で、要求の形式は変わるか | **変わる。** overlay が重なる frame は affine が rgba を要求する。overlay の無い frame は consumer の形式（yuv422）で要求される。encoder 直前の RGBA の検査を付けた書き出し（試験）は、全 frame が rgba になる。**製品の書き出しは検査を付けないので、隙間に overlay のある frame と無い frame が混ざると、cache が作り直され続けていた** | [事実] 形式を固定しない対照で、最初の frame の要求が yuv422（形式 3）で来た（3.3） |
| 4. 位置・色・service の property・大きさ・graph の再開で作り直されるか | 位置: されない。色: 文字列が変われば、同じ色でもされる。大きさ: 要求の幅・高さが変わればされる。書き出しでは、幅・高さは consumer の値（profile と同じ）で、transition はそのまま下へ渡す。graph の再開: producer を作り直せば cache も新しくなる | [事実] source。書き出しが consumer に幅・高さ・scale を設定していないことは `configure_mp4_consumer` |
| 5. 1 つの producer を複数の worker が使うか | **使う。** V1 の隙間は、全部が同じ producer の cut である。frame を作るのは consumer の 1 thread だが、画像の要求は render worker が並行に行う | [事実] `mvm_mlt_export.c` の `append_gap`、`mlt_consumer.c` の worker |
| 6. prewarm が終われば、後の worker は埋まった画素を見るか | **key が変わらない限り、見る。** 作り直さない道では、lock の中で cache の pointer を読み、埋め終わった buffer を複製するだけである。buffer が解放されるのは、作り直しと producer を閉じるときだけ | [事実] source。key が変わらないことは、形式の固定と、要求ごとの検査で保証する（2） |

ほかに color producer を使う場所:

- affine の **filter** は、背景用の color producer を filter ごとに 1 つ持つ。filter は、自分の lock を持ったまま
  その producer の画像を取り終える（`filter_affine.c`）ので、同じ producer へ 2 つの thread が同時に入らない。
  対象外とした [事実]。
- qtcrop の `color` は色の値で、producer ではない。

## 2. 修正（Task C）

新しい module `src/media/mlt/mvm_mlt_v1_background.c`（MLT の code は変えない）。書き出し
（`mvm_mlt_export_two_track`）は、V1 の背景をこの module で作る。従来の graph でも数式 group の graph でも同じである。

1. **形式を固定する。** color producer に `mlt_image_format = rgba` を設定する。要求が yuv でも、producer は rgba で
   作る（rgba64 の要求だけは MLT が rgba64 で作るが、書き出しは 8 bit の yuv420p で、rgba64 を要求しない）。
2. **prewarm。** consumer を始める前に、同じ producer へ 1 thread で、出力と同じ大きさの rgba を 1 回要求する。
   返った画像と cache の **全画素** が (0, 0, 0, 255) であること、cache の幅・高さ・形式が期待どおりであることを
   確かめる。だめなら、consumer を始める前に書き出しを失敗にする。
3. **要求ごとの検査。** この producer が作る frame の画像の要求ごとに、cache の buffer・幅・高さ・形式が
   prewarm のときのままであること、返った画像が出力と同じ大きさの rgba であること、3 か所の画素が黒であることを
   確かめる。違っていれば記録し、書き出しの待ち loop がそれを見て **書き出しを失敗にする**（observer が無い
   製品の書き出しでも同じ）。

3 がある理由: 「作り直しは起きないはず」は、MLT の中の要求の出し方についての読解に依存している。読解が
外れたとき（将来、別の大きさや形式で要求する filter を足したときなど）に、気づかずに誤った画素を出さないため
である。**この producer への画像の要求は、どれも検査を通る。cache を作り直す要求は、それ自身が検査で
見つかる**: 形式か大きさが違う要求は、返る画像の形が違う。色の文字列だけが違う要求は、古い buffer を持った
まま新しい buffer を確保するので、その要求の検査では buffer が必ず違う。

形式を固定した影響: overlay の無い隙間の frame は、以前は color producer が yuv422 で作っていた。今は rgba で
作り、consumer（avformat）が encode の前に yuv420p へ変換する。黒はどちらも同じ値になり、出力は変わらなかった
（3.2）。

変えていないもの: 背景の色（`#000000`）、producer の尺、playlist への置き方、後始末の順（consumer → tractor →
playlist → clip → 背景）。

## 3. 確認（Task B・D）

### 3.1 検出器（MLT 7.36.1 を検出器付きで build）

`scripts/mlt-color-race-p35a2-sanitizers.sh`、`tests/sanitizer/v1_background_mlt_race.cpp`、証拠は `sanitizers/`
（`identity.txt` に source・MLT の library の SHA-256）。**実物の `mvm_mlt_v1_background.c`** を、P3-5A と同じ
方法（MLT の framework と core module を `-fsanitize=thread`、`-fsanitize=address,undefined` で source から
build、g++ 11.4、WSL）で動かす。graph は harness が書き直したもので、V1 は 背景（0〜79）・別の clip（80〜159）・
背景（160〜239）である。`mvm_mlt_export.c` そのものは、今回も検出器の対象外である。

保護あり（どれも ThreadSanitizer と AddressSanitizer + UBSan の両方で、報告 0）:

| scenario | worker | 内容 |
| --- | --- | --- |
| `protected` | 1・4・8 | consumer は yuv422 を要求（検査の無い製品の書き出しと同じ）。同じ process で 3 回（背景は毎回作り直す）。全 frame を観測し、隙間の全 frame が rgba の黒（alpha 255）。隙間ごとの最初の frame を含む |
| `protected-rgba` | 1・4・8 | consumer が rgba を要求 |
| `seek` | 1・4・8 | consumer を使わず、worker が順不同の位置へ seek して画像を要求する。形式は位置ごとに rgba と yuv422 が混ざる |
| `cancel` | 1・4・8 | 0・1・8・100 枚の時点で consumer を止める |
| `fault-prewarm` | — | prewarm の検査の失敗 → 背景を作らない |
| `fault-rebuild` | 8 | prewarm の後で cache を作り直させる → 検査が見つけて止まる |

cache の作り直しを実際に起こす対照（検査が見つけること。作り直しそのものは MLT の race を含むので、
ThreadSanitizer はここでは報告する）:

| scenario | 起こすこと | 検査の結果 |
| --- | --- | --- |
| `invalidate-format` | 形式を固定しない（consumer が yuv422 を要求） | 「出力と違う形の画像が返りました」（frame 0、形式 3） |
| `invalidate-size` | consumer が 320×180 を要求 | 「出力と違う形の画像が返りました」（320×180） |
| `invalidate-resource` | 書き出しの途中で、色の文字列を同じ色の別の書き方へ変える | 「cache が prewarm の後で作り直されました」 |

保護なしの対照（修正の前の状態の再現）:

| 対照 | 結果 |
| --- | --- |
| `unprotected`（worker 4・8。順不同・形式が混ざる要求） | ThreadSanitizer が `producer_colour.c` の中の data race を報告（各 6 件。232・233 行の確保・複製と、画素を埋める書き込み） |
| `unprotected-pixels`（最初の画像を 8 本が同時に要求、40 round） | AddressSanitizer の build で、**10 回中 10 回**、黒でない画像を受け取った（1 回あたり 320 枚中 4〜7 枚） |

- 検出器の報告として数えたのは、`producer_colour.c` の行を含む data race だけである。起動の失敗や、関係の無い
  報告は「検出」として数えていない。
- P3-5A の最初の記録（`build/math-export-p35a-20261013/sanitizers-first-run/`）は、そのまま残してある。
- P3-5A の数式 group の harness（`scripts/math-group-p35a-sanitizers.sh`）も最終の source で実行し直し、
  不成立 0 だった（`sanitizers-math-group/`）。

### 3.2 Windows の書き出し（実物の `mvm_mlt_export_two_track`）

**修正の前後の比較**（`scripts/mlt-color-race-p35a2-windows.ps1`、`…-before/` と `…-after/`）。
scenario は 3 つ: `gap`（clip なし）、`mixed`（V2 の静止画が 30〜89）、`multi`（V1 の静止画が 40〜79 で隙間が 2 つ、
V2 の静止画が 20〜99）。640×360、120 frame。encoder 直前の RGBA の検査あり・なし、worker 1 本と 8 本（8 本は 5 回）。

| | 修正の前（36 run） | 修正の後（36 run） |
| --- | --- | --- |
| 書き出しの成功 | 36 | 36 |
| scenario ごとの MP4 の種類（検査あり・なし、worker 1・8 本、くり返しを通して） | 1 | 1 |
| scenario ごとの RGBA の digest の種類 | 1 | 1 |
| 隙間だけの frame で黒でなかったもの・観測できなかった frame | 0・0 | 0・0 |
| **同じ条件どうしの MP4 と RGBA の digest が、前後で一致** | — | **36/36** |

- 出力の MP4 は、検査あり・なしでも同じである（overlay の無い隙間の frame を、color producer が yuv で作っても、
  rgba で作って consumer が変換しても、黒は同じ値になる）。
- `…-after-trial/` は、最終の build の前に同じ比較を行った run（36/36 一致）。

**新しい CTest `mlt_export_v1_gap_background`**（`tests/harness/test_mlt_export_v1_gap.cpp`）:

- 3 つの scenario × 検査あり・なし × worker 1・8 本: 書き出しが完了し、背景の違反が 0。検査ありでは、
  全 120 frame を観測し、**隙間だけの frame は全画素が (0, 0, 0, 255)**。隙間の最初と最後の frame
  （境目）を含む。
- 同じ process で 3 回続けて書き出す。途中で取消しても、背景の違反にならない。
- 対照（書き出しが失敗すること）:

| 故障 | 結果 |
| --- | --- |
| 形式を固定しない（検査なし = 製品と同じ要求の形） | 失敗。「出力と違う形の画像が返りました（frame 0、形式 3）」。**修正の前の製品では、ここで cache が作り直されていた** |
| prewarm の後で cache を作り直させる（形も画素も同じ。buffer だけが違う） | 失敗。「cache が prewarm の後で作り直されました」 |
| prewarm の検査の失敗 | consumer を始める前に失敗。出力を作らない |

**上位の層を通した書き出し**（検証 harness、1920×1080。`scripts/mlt-color-race-p35a2-app.ps1`、`…-app/`）:

| 場合 | 結果 |
| --- | --- |
| `base-gap`（V1 に clip が無い）× 3 回 | 製品の経路 → 判定つきの数式 group → 製品の経路 の順で、全 120 frame が一致。背景の違反 0。製品の経路の frame ごとの digest は、修正の前（P3-5A の run）と同じ |
| `base-half-gap`（前半だけ clip）× 3 回 | 同じ（全 frame 一致、違反 0、修正の前と同じ digest） |
| cache を作り直させる故障（従来の graph） | 書き出しが失敗。最終 file・一時 file とも残らない |
| cache を作り直させる故障（数式 group の graph） | 同じ |

- 数式の layer の順は変えていない。数式 group の経路と製品の経路の全 frame の一致が、その確認である。
- frame の欠け・重複: 検査ありの run で、frame 0〜119 を 1 回以上観測している（最後の frame は consumer が
  くり返し要求する。画素は同じ）。

### 3.3 修正の前の Windows では現れていたか

[事実] 修正の前の binary でも、Windows の出力は 36 run とも同じで、隙間の frame は黒だった（`…-before/`）。
P3-5A の 30 回の書き出しでも現れていない。race の窓は、cache を作り直す数 ms である。

[事実] ただし、修正の前の製品の書き出し（検査なし）では、overlay の無い隙間の frame が yuv422 で要求されて
いたことを、形式を固定しない対照で確かめた。隙間に overlay のある frame と無い frame が混ざる Project では、
境目のたびに cache が作り直され、そのたびに race の窓があった。

**Windows で誤った画素が実際に出たことは、確かめていない。** 確かめたのは、(1) MLT の source に race が
あること、(2) 検出器付きの build で誤った画素になること、(3) 製品の書き出しがその条件（複数の worker、
作り直し）を満たしていたこと、である。

## 4. 回帰の gate と provenance（Task E）

### 4.1 実行したもの

| gate | 結果 |
| --- | --- |
| lint | 通過 |
| `mlt_export_v1_gap_background`（新しい CTest） | 通過（全 CTest の中でも通過） |
| 数式の書き出しと parity の試験、数式 group の oracle | 全 CTest の中で通過 |
| 検出器（背景の harness、数式 group の harness） | 不成立 0（3.1） |
| release の全 CTest（`pwsh scripts/test.ps1 -Preset ucrt64-release`） | **1517/1522。PASS にしない**（`ctest-full.log`） |
| debug の全 CTest | 実行していない |

### 4.2 全 CTest で落ちた 5 件

`audio_mixer_product_ui`、`text_ui_direct_input`、`transition_preview`、`fixed_test_window_independent_of_screen`、
`preview_engine_p5c_engine_lifetime_detach`（最後の 1 つは 0xc0000409）。

- [事実] どれも window の描画が必要な試験で、失敗の文は「固定 window が描画されません」「背面 QML window が
  描画されません」「cut を通して再生が続きません」「P5-C smoke が…秒以内に完了しませんでした」である。
- [事実] 落ちた後に調べると、**接続中の monitor が 1 つも無かった**（`Get-PnpDevice -Class Monitor` の 3 件とも
  Present = False、`WmiMonitorBasicDisplayParams` が 0 件。`monitor-state-at-failure.txt`）。利用者の最後の操作は
  9:12 ごろである（全 CTest は 9:06〜9:25。5 件は終盤で落ちた）。試験の script が取る「自動消灯の防止」は有効だった（Windows が
  消灯したのではない）。monitor の電源が切られたと考えられる [推測]。
- [事実] 5 件を、自動消灯を止めた状態で 2 回実行し直したが、2 回とも 5 件とも同じ形で落ちた
  （`ctest-rerun-failed-1.log`・`-2.log`）。monitor は無いままである。
- [事実] `fixed_test_window_independent_of_screen` と `preview_engine_p5c_engine_lifetime_detach` の binary
  （`mvm_test_fixed_test_window.exe`・`mvm_p5c_preview_smoke.exe`）は 07:33 に作られたもので、
  08:00 の全 CTest（P3-5A.1、1521/1521）で通ったものと同じである。今回の変更は入っていない。
- 残りの 3 件の binary は今回の変更の後に link し直されているが、落ち方は同じ（window が描画されない）で、
  書き出しの背景の code は preview では動かない。**ただし、monitor のある状態で通ることは、今回の binary では
  確かめられていない。**
- 2026-10-05 にも、画面が消えている間に同じ 5 件が落ちた記録がある。

**この結果では、全 CTest の gate を PASS にしない。** monitor が戻ったら全 CTest を取り直す仕組みを動かして
ある（`wait-monitor.log`。戻れば `ctest-full-2.log` ができる）。

### 4.3 provenance

Windows の書き出しの比較に使った file（SHA-256 の先頭 16 桁。全体は各 run の `identity.json`）:

| file | 修正の前 | 修正の後 |
| --- | --- | --- |
| `mvm_test_mlt_export_v1_gap.exe` | `29E50257C3B9BA1E…` | `B48013AE7E863DD9…` |
| `src/media/mlt/mvm_mlt_export.c` | `532404928975B5FC…` | `CEC0645EF16F3304…` |
| `src/media/mlt/mvm_mlt_v1_background.c` | `9563676303F9E0EC…` | `9563676303F9E0EC…` |
| `libmlt-7.dll` | `13D5EFA4536D3225…` | `13D5EFA4536D3225…` |
| `libmltcore.dll` | `0A4F93FA10E1DAF7…` | `0A4F93FA10E1DAF7…` |
| `libmltavformat.dll` | `0FA0E99C0D06E2C3…` | `0FA0E99C0D06E2C3…` |
| `libx264-165.dll` | `ACF3D8F926F42ADA…` | `ACF3D8F926F42ADA…` |

- 修正の前の run は、背景の module と試験を足しただけで、書き出し（`mvm_mlt_export.c`）がまだ module を使って
  いない build である（背景は従来どおり、その場で color producer を作る）。
- 検出器の run の source・MLT の library の SHA-256 は `sanitizers/identity.txt`。compiler は g++ 11.4（WSL）で、
  Windows の binary ではない。
- git の HEAD は `cc6aece` で、今回の変更は commit していない（差分は各 run の `git-diff.patch`）。
- 全 CTest は、Windows の比較（`…-after/`）の直後に同じ build で実行した。`test.ps1` の build で
  `mvm.exe` などが link し直されたが、比較に使った試験の binary の SHA-256 は実行の後も同じである。

## 5. 残っていること

- `mvm_mlt_export.c` そのものは、検出器の下で動かしていない（Windows の API と Qt・FFmpeg の MLT module に
  依存する）。検出器で見たのは、背景の module と MLT の framework・core module である。
- 保護は「cache が作り直されないこと」に依存する。作り直しが起きたら書き出しは失敗する（黙って続けない）が、
  そのときに従来の方法でやり直す仕組みは無い。書き出しの graph に、背景へ別の大きさ・形式で要求する
  filter を足すと、書き出しが失敗するようになる。
- rgba64 を要求する書き出し（10 bit の出力）を足す場合は、形式の固定の値を見直す必要がある。
- affine の filter の背景は、filter の lock で直列になっていることを source で確かめただけで、検出器では
  動かしていない（core module だけを build しているため）[未検証]。
- 製品の timeout、数式 group の製品での有効化、UI の SegFault（UNKNOWN）は、この phase の対象外である。
