# P3-5A.2 の closure: 表示の環境つきの全 CTest と、背景の cache の保護の点検

2026-10-13。[P3-5A.2](mlt-color-race-p35a2.md) で未成立だった全 CTest の gate を、GUI の環境を確かめた上で
取り直した。あわせて、背景（V1 の隙間の color producer）へ届く要求の形を洗い出し、検査を 1 段足した。

数式 group の合成・書き出しの timeout・MLT の affine・画素の authority は変えていない。commit・push はしていない。
P3-5A・P3-5A.2 の過去の結果（1513/1514、1517/1522）は、履歴としてそのままである。
`math_equation_sequence_product_ui` の SegFault は UNKNOWN のままである。

証拠は `build/mlt-color-race-p35a2-closure-20261013/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| 表示の環境 | 判定する script を作った。**今日の環境は安定していない**: 有効 → 途中で monitor が無くなる、をくり返している（1） |
| 全 CTest（commit `ee3fbbd` = P3-5A.2 の内容） | **PASS: 1522/1522。** 実行の前後とも環境は VALID（2.1） |
| 全 CTest（「渡す前の検査」を含む最終の内容。commit `c3b683f`） | **PASS: 1522/1522。** 実行の前後とも環境は VALID（2.3）。1 回目は 1518/1522 で、実行中に monitor が無くなった（2.2。履歴として残す） |
| color producer の race への対処 | 成立。検出器・Windows の出力・対照とも、足した検査を含めて確かめ直した（4） |
| cache の key を変える要求 | 対応している書き出しの条件では、起きない。起きた場合は、MLT へ渡す前に断る（3） |

**全体: PASS / CLOSED。** 最終の内容（渡す前の検査を含む）で、環境が有効なまま終わる全 CTest が通った。残る不確かさは 6 にある。次は P3-5B（書き出しの timeout を実時間で数える修正）を勧める。

## 1. 表示の環境（Task A）

`scripts/gui-environment-preflight.ps1`。次を全部確かめ、JSON に記録する。1 つでも欠ければ
`ENVIRONMENT_INVALID`（終了コード 2）。環境を直すことはしない（入力を送って画面を起こす、などはしない）。

| 確かめること | 方法 |
| --- | --- |
| 対話の session で、console の session である | process の session と console の session の番号 |
| lock されていない | lock 画面の process が無い、入力を受ける desktop を開ける |
| 接続中の monitor がある | PnP（`Get-PnpDevice -Class Monitor` の Present）と WMI（`WmiMonitorBasicDisplayParams` の Active）の両方 |
| 解像度・DPI | screen の一覧、system の DPI（記録） |
| Windows が報告する描画先の電源が on | 試験の script と同じ仕組み（`test-display-lease`） |
| **実際に window を描画できる** | `mvm_test_fixed_test_window`（試験用の window の描画） |
| **実際に preview が frame を提示できる** | `mvm_p5c_preview_smoke`（D3D11 の提示まで） |

「monitor の電源が入っている」「Windows が電源 on と報告している」だけでは足りない。今日の失敗は、
Windows の報告が電源 on のまま、接続中の monitor が 0 になっている状態で起きた。

今日の記録:

| 時刻 | 環境 | 記録 |
| --- | --- | --- |
| 09:34（P3-5A.2 の全 CTest の後） | monitor 0（PnP 3 件とも Present = False） | `build/mlt-color-race-p35a2-20261013/monitor-state-at-failure.txt` |
| 12:02 | monitor が戻った直後。描画先の電源が off で、試験の script が実行を断った（試験は走っていない） | 同 `ctest-full-2.log` |
| 12:13 | **VALID**（monitor 1、1920×1200、DPI 96、電源 on、描画と提示の probe が成功） | `committed-ee3fbbd/gui-environment-1.json` |
| 12:34 | **VALID** | `committed-ee3fbbd/gui-environment-after.json` |
| 12:44 | **VALID** | `final/gui-environment-before.json` |
| 13:04 | **INVALID**（monitor 0。電源の報告は on、無操作 99 秒） | `final/gui-environment-after.json` |
| 13:08 | **INVALID**（同じ） | `gui-environment-recheck-1.json` |

monitor の出入りは、利用者の操作（monitor の電源）によるものと考えられる [推測]。こちらからは変えていない。

## 2. 全 CTest（Task B）

`pwsh scripts/test.ps1 -Preset ucrt64-release`（通常の試験 1522 件、並列 8）。実行の前後に 1 の確認を行い、
出力の全部（`ctest-full.log`）、`LastTest.log`、binary（157 個）・runtime・変更した source の SHA-256
（`identity-before.json`・`identity-after.json`）を残した。前の結果は上書きしていない。

### 2.1 commit `ee3fbbd`（P3-5A.2 の内容。作業 tree の変更は、環境を確かめる script だけ）

`committed-ee3fbbd/`。

- 実行の前: VALID。実行の後: VALID。
- **1522/1522 通過**（1,110 秒）。
- P3-5A.2 で落ちた 5 件（`audio_mixer_product_ui`・`text_ui_direct_input`・`transition_preview`・
  `fixed_test_window_independent_of_screen`・`preview_engine_p5c_engine_lifetime_detach`）は、全部通った。
- `test.ps1` の build は、毎回 `mvm.exe` と `mvm_test_text_ui_input.exe` を link し直す（実行の前に採った
  SHA-256 と、この 2 つだけが違う）。ほかの 155 個は同じである。

これにより、P3-5A.2 の 5 件の失敗は、monitor が無かったことによるものだと確かめられた。同じ source・
同じ試験が、環境が有効なら通る。

### 2.2 作業 tree（3 で足した「渡す前の検査」を含む）

`final/`。

- 実行の前: VALID。**実行の後: INVALID**（monitor 0）。この run は、環境が有効なまま終わっていないので、
  合否の根拠にしない。
- 結果は 1518/1522。落ちた 4 件:

| 試験 | 時刻・形 | 調べたこと |
| --- | --- | --- |
| `fixed_test_window_independent_of_screen` | 終盤（1517 番目）。「window が描画されません」 | monitor が無くなった後。P3-5A.2 と同じ形 |
| `preview_engine_p5c_engine_lifetime_detach` | 終盤（1518 番目）。0xc0000409 | 同上。下の注 |
| `math_equation_sequence_preview_controller` | 序盤（12:46）。24.8 秒かかり、196 検査中 2 件失敗（壊れた frame の検出） | monitor はあった。単独では 8 回中 8 回通る（9.7〜10.8 秒） |
| `math_raster_cache_focused` | 序盤（12:46）。90.6 秒かかり、344 検査中 2 件失敗 | monitor はあった。単独では 8 回中 8 回通る（16.7〜18.2 秒）。**binary は 07:34 に作られたもので、今回の変更は入っていない**（08:00・12:15 の全 CTest で通ったものと同じ） |

- 序盤の 2 件は、全 CTest の中では普段の 2.5〜5 倍の時間がかかって落ちている。並列の負荷で時間が延びると
  落ちる試験だと考えられる [推測]。原因は切り分けていない。背景の code は、この 2 つの試験では動かない
  （書き出しをしない）。**ただし、落ちたことは事実なので、この run を PASS にはしない。**
- 0xc0000409 について: 試験の出力は「P5-C smoke が 30 秒以内に完了しませんでした」「smoke 失敗: exit code 10
  （stage 5）」「terminate called without an active exception」の順である。frame の提示を待って時間切れに
  なり、その失敗の道で `std::terminate` が呼ばれて、終了コードが 0xc0000409 になる（以前から記録のある動き）。
  **dump と stack は採れていない。** 再現には「提示できない環境」が要り、有効な環境では通るためである。
  原因を monitor と断定する根拠は、(a) 同じ binary の同じ試験が、有効な環境の全 CTest で 2 回通っている（08:00・12:15）、(b) 時間切れの文が
  出ている、の 2 つで、crash の場所そのものは確かめていない [未検証]。

取り直し: 環境が VALID になったら全 CTest を実行し、終わった後にもう一度環境を確かめる仕組みを動かして
ある（`final-2-stage.log`。結果は `final-2/`）。

## 3. 背景の cache の保護の点検（Task C）

### 3.1 背景へ届く要求

背景の producer が作る frame の画像を要求するのは、次の 4 つだけである（`mvm_mlt_export.c` と MLT 7.36.1 の
source）。背景の cut には filter を付けていない。

| 要求する所 | 形式 | 大きさ |
| --- | --- | --- |
| consumer の render worker（overlay の無い隙間の frame） | consumer の形式。書き出しは yuv420p 8 bit なので yuv422。直前の frame が rgba で返ると、次の要求は rgba になる | consumer の幅・高さ = profile（書き出しは consumer に幅・高さ・scale を設定しない） |
| affine の transition（overlay が隙間に重なる frame） | rgba（要求が rgba64 のときだけ rgba64） | 上から渡された値をそのまま。位置のずれ（ox・oy）があるときは profile の値 |
| 数式 group の transition（診断用） | rgba | 上から渡された値 |
| encoder 直前の RGBA の検査（試験・診断のときだけ） | rgba | 上から渡された値 |

[事実] Windows の実物の書き出しで、背景への要求の形式を数えた（試験 `mlt_export_v1_gap_background`、
3 つの scenario）。

| 書き出し | 要求された形式 |
| --- | --- |
| RGBA の検査なし（製品と同じ） | rgba と yuv422 |
| RGBA の検査あり | rgba だけ |

大きさは、どの要求も出力と同じだった（違えば断られて、試験が落ちる）。

producer の property（cache の key に関わるもの）: `resource`（色の文字列。`#000000` のまま）、
`mlt_image_format`（`rgba` に固定）。書き出しは、作った後にこの producer の property を書き換えない。

### 3.2 通常の書き出しで key は変わるか

**変わらない。** key は 色の文字列・幅・高さ・形式である。

- 形式: producer に固定してあるので、要求が yuv422 でも rgba で作る。rgba64 の要求だけは MLT が rgba64 で
  作るが、書き出しの出力は 8 bit の yuv420p で、rgba64 を要求する所が無い。
- 幅・高さ: 上の表のとおり、どれも profile の値である。
- 色の文字列: 誰も書き換えない。

対応している入力（Project の内容）で、この 3 つを変える手段は無い。出力の大きさは profile と一緒に決まり、
10 bit の出力や、consumer の scale は、書き出しに入口が無い。

### 3.3 「渡した後の検査」は同期の壁ではない

P3-5A.2 の検査は、**MLT が要求を処理した後**に、cache が変わっていないかを見るものだった。cache を作り直す
要求は、その要求自身の検査で見つかるが、見つけた時点で、MLT はもう cache を作り直している。その間に来た
別の worker は、埋まる前の画素を受け取っているかもしれない。書き出しは失敗になるので誤った出力は残らないが、
**race そのものを防ぐ壁ではない。**

### 3.4 足した検査（MLT へ渡す前に断る）

通常の書き出しでは key は変わらない（3.2）ので、必須ではない。ただ、読解が外れた場合の備えが「起きた後に
見つける」だけなのは弱いので、**key を変える要求を、MLT の producer へ渡す前に断る**検査を足した
（`mvm_mlt_v1_background.c`）。断る条件:

- 形式の固定（`mlt_image_format = rgba`）が外れている。
- 要求の形式が rgba64。
- 要求の幅・高さが出力と違う（0 以下は、MLT が profile の値を使うので通す）。
- 色の文字列が期待と違う。

断った要求は producer へ届かないので、cache は作り直されない。断ったことは違反として数え、書き出しは
失敗になる（黙って続けない）。「渡した後の検査」は弱めずに残してある（2 段目。渡す前の検査が見ていない所で
cache が変わった場合を見つける）。

残る限界:

- 断られた frame には、MLT が試験用の画像を入れる。書き出しは失敗するので出力には残らないが、
  やり直しの仕組みは無い。
- 色の文字列の確認は、MLT 自身と同じく、lock なしで property を読む。worker が動いている間に誰かが
  property を書き換えると、読む側（MLT も、この検査も）と競合する。書き出しは書き換えない。
- affine の filter が持つ背景の producer は対象外のまま（filter の lock で直列。source の読解だけ）。
- `mvm_mlt_export.c` そのものは、検出器の下で動かしていない。

## 4. 確かめ直し（Task D）

3.4 で source を変えたので、次を実行し直した。

| 確認 | 結果 | 証拠 |
| --- | --- | --- |
| `mlt_export_v1_gap_background`（Windows、実物の書き出し） | 通過。隙間だけの frame は全画素 (0, 0, 0, 255)。形式を固定しない対照は「MLT へ渡す前に断った」として失敗し、cache を作り直させる対照は「渡した後の検査」で失敗する | 2.2 の全 CTest の中でも通過 |
| Windows の出力（修正の前との比較。3 つの scenario × 検査あり・なし × worker 1・8 本） | MP4 と RGBA の digest が 36/36 一致 | `…-after-pre-request/` |
| 上位の層を通した書き出し（V1 に隙間のある fixture 2 つ × 3 回、故障 2 つ） | 全 frame 一致、違反 0、修正の前と同じ digest。故障では書き出しが失敗し、file が残らない | `…-app/` |
| 検出器（背景の harness。保護あり、worker 1・4・8 本） | ThreadSanitizer・AddressSanitizer + UBSan とも報告 0 | `sanitizers/` |
| 検出器（key を変える要求 4 つ: 形式の固定なし・別の大きさ・rgba64・色の文字列） | 全部「MLT へ渡す前に断った」。**cache が作り直されないので、検出器の報告も 0**（P3-5A.2 では、ここで MLT の中の race が報告されていた） | 同上 |
| 検出器（保護なしの対照。そのまま残してある） | ThreadSanitizer が `producer_colour.c` の data race を報告。AddressSanitizer の build で、黒でない背景を 10 回中 10 回受け取った | 同上 |
| 検出器（数式 group の harness） | 不成立 0 | `sanitizers-math-group/` |
| lint | 通過 | — |

v4 全体（6840 frame）の書き出しは、やり直していない。v4 は V1 が全区間 1 枚の静止画で、背景の producer へ
要求が届かない（隙間が無い）ので、今回の変更は全長の書き出しの画素に関わらない。背景を作るときの prewarm
（1 回の画像の要求と全画素の確認）は、v4 でも実行される。時間は測っていない [未検証]。

## 5. source・binary・runtime

- git の HEAD は `ee3fbbd`。作業 tree の変更（未 commit）: `mvm_mlt_v1_background.c`・`.h`、`mvm_mlt_export.c`・`.h`、
  試験 2 つ、検出器の script と shim、環境を確かめる script。差分は `final/git-diff.patch`。
- 2.1 の run の binary・runtime は `committed-ee3fbbd/identity-before.json`、2.2 は `final/identity-before.json`・
  `identity-after.json`（binary 157 個、MLT の module、Qt・FFmpeg・x264 の DLL、変更した source）。
- 検出器の run の source と MLT の library は `sanitizers/identity.txt`（g++ 11.4、WSL。Windows の binary ではない）。

### 2.3 最終の内容（commit `c3b683f`。作業 tree の変更なし）

2.2 の作業 tree は、その後 commit された（`c3b683f`。内容は 2.2 で試験したものと同じ）。16:00 までの
自動の取り直しは、環境が VALID にならなかったので実行されなかった（`final-2/`。試験は走っていない）。
18:32 に環境が VALID になっていたので、全 CTest を 1 回だけ実行した（ほかの全 CTest と重ねていない）。`final-3/`。

| | |
| --- | --- |
| 実行の前の環境 | VALID（monitor 1、1920×1200、DPI 96、電源 on、描画と提示の probe が成功） |
| 実行の後の環境 | VALID（同じ。無操作 890 秒。試験の script が自動消灯を止めていた） |
| 結果 | **1522/1522 通過**（1,088 秒）。登録数は 1522 のまま |
| git | HEAD `c3b683f`、作業 tree の変更なし（実行の前後とも） |
| binary | 157 個の SHA-256 を前後で記録。`test.ps1` の build が link し直す 2 つ（`mvm.exe`・`mvm_test_text_ui_input.exe`）以外は前後で同じ。背景の試験の binary（`mvm_test_mlt_export_v1_gap.exe`、`36E1ED48…`）と検証 harness（`C4C71CB6…`）は、4 の確かめ直しに使ったものと同じ |
| runtime・source | 前後で同じ（`identity-before.json`・`identity-after.json`） |

- 2.2 で落ちた 4 件は、全部通った: `math_equation_sequence_preview_controller`（14.9 秒）、
  `math_raster_cache_focused`（24.0 秒）、`fixed_test_window_independent_of_screen`、
  `preview_engine_p5c_engine_lifetime_detach`。
- 2.2 の結果（1518/1522）と、P3-5A・P3-5A.2 の結果（1513/1514、1517/1522）は、履歴としてそのまま残してある。


## 6. 残っていること

1. 全 CTest は、最終の内容では 1 回だけ通っている（2.3）。くり返しの安定は見ていない。
2. `math_equation_sequence_preview_controller`・`math_raster_cache_focused` が、全 CTest の負荷の中で 1 回落ちた。
   単独では 8 回中 8 回、最終の全 CTest（2.3）でも通った。今回の変更と無関係な binary を含む。並列の負荷で
   時間が延びると落ちると考えられるが、原因は切り分けていない。もう一度落ちたら、負荷を決めた cohort で
   別に調べる（sleep を伸ばしたり、検査を弱めたりして通すことはしない）。
3. `preview_engine_p5c_engine_lifetime_detach` の 0xc0000409 の dump は採れていない。有効な環境では再現しない
   （2.3 でも通った）。
4. 全 CTest は、monitor が無いと GUI の試験が落ちる。gate の前後に `scripts/gui-environment-preflight.ps1` で環境を確かめないと、結果を取り違える。`test.ps1` 自身は、monitor が無いことを検出しない。
