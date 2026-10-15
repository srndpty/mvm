# P3-6C.2: 実際の mvm.exe の検収（自動の操作による）

2026-10-14。「数式の書き出しを高速化（試験的）」を、実際の `mvm.exe` の画面から使えることを確かめた。

## 判定: PASS（自動の検収）/ CLOSED

2026-10-14 に利用者が受け入れ、閉じた。数式 group の書き出しの milestone の閉鎖と製品の方針は
[roadmap](roadmap.md) に記録した。

**人の操作による検収ではない。** 利用者の指示（2026-10-14）で、検収を自動の検査だけで完結させる方針に
変えた。人が行うはずだった操作は、driver（`scripts/math-group-product-p36c2-drive.ps1`）が、実際の
`mvm.exe` の画面の部品に対して行った（UI Automation と Windows の message。mouse と keyboard は使わない）。
手順は [手順書](math-group-product-p36c2-procedure.md)、経過は 2.3・2.4。

| # | 項目 | 結果 |
| --- | --- | --- |
| 1 | 実際の `mvm.exe` の GUI の操作 | **成立（自動）。** 場面 A・B・C の 47 段が全部成立（2.4） |
| 2 | 全長の完走と時間 | **成立。** 6840 frame、**180.1 秒**（`mvm` が出す開始と結果の行を pipe で受け取った時刻の差。基準 600 秒） |
| 3 | 出力の形式と MP4 の SHA-256 | **成立。** H.264 High、1920x1080、60 fps、6840 frame を復号して error 0、時刻、x264 の設定、**SHA-256 は過去の全長の出力（P3-4・P3-5A・P3-6B）と同じ** |
| 4 | 高速化の選択と、選ばれた graph | **成立。** 判定 ELIGIBLE、数式 group の graph、group 2、まとめた clip 97、affine 23、合成 13,491、実行時の失敗 0 |
| 5 | 取消と失敗の後始末 | **成立。** 取消・掴まれた file への書き出しの失敗とも、最終 file・一時 file が残らず、元の file は変わらない |
| 6 | 利用者に見える報告 | **成立。** 「使いました（数式 clip 16 本・97 本）」「使いませんでした（理由）。従来の方法で書き出しました」が状態の表示の行頭に出る。checkbox は開くたび・起動し直すたびに off |
| 7 | 回帰 | **成立。** 全 CTest 1524/1524（今の `mvm.exe` と同じ binary。2.2）。検収の道具の自己試験 58/58。lint 通過 |
| 8 | 残る制約と方針 | 下（2.4 の最後、3、4） |

既定は off のまま。実行時の失敗のときの自動のやり直しは入れていない。commit・push はしていない。数式
group の合成の式、下地の alpha の条件、MLT の affine、V1 の背景の保護、timeout、encoder 直前の画素の
authority は変えていない。

証拠は `build/math-group-product-p36c2-20261014/automated/`。

## 1. この phase で変えたこと

### 1.1 表示の修正（Task F）

P3-6C.1 の画面を調べた結果、修正が要ると判断した（`screens-before-presentation-fix/`）。

- 状態の表示は 1 行で、右側が省略される。P3-6C.1 は、数式の高速化の結果を **path の後ろ** に付けていた。
- 対象外のときの文は、幅 1440 の window で、行の右端まで 145 px しか余っていなかった
  （`status-fallback-unknown.png`）。理由を内部の名前（`base-clip-transformed`、21 字）から人が読める文
  （30 字前後）に直すと、同じ window で入り切らない。window が狭いとき、path が長いときも同じである。

これは、試験の実行 file で描画した製品の画面での確認である。**実際の `mvm.exe` を人が見た結果では
ない。** 人の検収の前に直したのは、検収の後に source を変えると、検収をやり直すことになるためである。
人の検収で、この表示が読みやすいかどうかを確かめる（手順書の A5・B3・C5）。

直したこと（`apps/mvm/mvm_controller_export.cpp`。QML は変えていない）:

| 項目 | 前 | 後 |
| --- | --- | --- |
| 結果の位置 | `書き出しました: <path> (…) / <高速化の結果>` | `<高速化の結果>。書き出しました: <path> (…)`（対象外・上限の超過のときは「…。従来の方法で書き出しました: <path>」） |
| 対象外の理由 | 内部の名前のまま | 人が読める文（下の表）。**記録の値（`reason`）は変えていない。** 読める文は `reasonText` として別に持つ |
| 高速化を選んでいない書き出し | `書き出しました: <path> (…)` | 変えていない |

| 内部の名前 | 利用者に見せる文 |
| --- | --- |
| `base-alpha-zero` | 一番下の映像 track に透明な部分があるため |
| `base-alpha-unproven` | 一番下の映像 track の透明度を事前に確かめられないため |
| `base-clip-transformed` | 一番下の映像 track の clip に拡大・回転などの変形があるため |
| `no-eligible-math-clip` | まとめて描画できる数式 clip が無いため |
| `prepared-raster-budget-exceeded` | 準備する画像が上限を超えたため |
| 上に無い名前 | 対象外（名前）。黙って別の理由に読み替えない |

修正の後の画面は `screens-after-presentation-fix/`。

### 1.2 書き出しの記録を console へ出す

実際の `mvm.exe` には、検証 harness のような記録の口が無い。人の操作で「どの graph が選ばれたか」
「何回合成したか」「失敗が 0 か」を確かめるために、書き出しの開始と結果を、起動した console（stderr）へ
1 行ずつ出すようにした（`main.cpp` の既存の出し方と同じ）。

```
書き出しを始めます: <path> (数式の高速化: 選択あり)
書き出しの結果: 成功 frame=6840 error='' | 数式の高速化: outcome=used decision=ELIGIBLE reason=… graph=math-group groups=… grouped_clips=… affine=… composed=… callbacks=… errors=0 …
```

値は、書き出しの結果の記録（`MathGroupExportReport`）の写しである。診断用の observer は使っていない。
書き出しの要求も変えていない。

### 1.3 検収の道具

| もの | 内容 |
| --- | --- |
| [手順書](math-group-product-p36c2-procedure.md) | 人が行う操作と、確かめること。Session A（対象外の Project: 既定 off、on、fallback の文、開き直し、取消、失敗）、Session B（数式だけの Project: 起動し直し、成功の文、「入力映像: 仕様を取得できません」が書き出しを止めないこと）、Session C（v4 全体） |
| `scripts/math-group-product-p36c2-accept.ps1` | `freeze`（対象の固定）、`reference`（短い fixture の参照の出力）、`session`（固定した `mvm.exe` を起動して記録する。**GUI は操作しない**）、`verify`（事後の検査） |
| 短い確認用の Project（`fixtures/`） | `short-fallback-unknown.mvm`（V1 の画像を拡大。対象外）、`short-math-only-eligible.mvm`（V1 に何も無い、数式だけ。対象）。どちらも 2 秒・数式 16 本。v4 の Project から検証 harness が作り、数式の成果物の cache を写した |

`session` が記録するもの:

- 起動した実行 file の path と SHA-256（固定した `mvm.exe` と同じか）。
- 読み込まれた DLL の全部と、その SHA-256（固定した値と同じか）。repository と MSYS2 の外から差し込まれる
  DLL は、別に記録する（この PC では、他の software の IME の hook が 1 つ入る）。
- 「書き出しを始めます」「書き出しの結果」の行を、script の単調な時計で読んだ時刻。**開始から結果までの
  経過が、検収の時間である**（基準 600 秒）。行を出すのは `mvm.exe` だが、時間を測るのは script である。
  読む間隔は約 40 ms。
- 起動の前後の、アプリの設定（registry）と、Project の file。
- `mvm` の window の画面（2 秒ごと。書き出しの間は 20 秒ごと）。
- watchdog（1500 秒）は、超えたら console へ出して記録するだけである。止めない。600 秒の基準とは別に持つ。

`verify` が確かめるもの（v4）: 固定した `mvm.exe` と DLL だったこと、高速化を選んだ書き出しが成功した
こと、`outcome=used`・ELIGIBLE・数式 group の graph・合成が 0 でないこと・失敗 0、経過が 600 秒未満、
H.264 High・1920x1080・60 fps・6840 frame・時刻・復号の error 0、一時 file が無いこと、x264 の設定、
**MP4 の SHA-256 が、過去の全長の出力（P3-4・P3-5A・P3-6B）と同じこと**、書き出しの選択が設定に
保存されていないこと。

`verify` が確かめないもの: **encoder 直前の RGBA**（この書き出しには計測を付けない。画素の authority は
P3-4・P3-5A のままで、MP4 の一致は間接の確認である）。人が目で見た項目（checkbox の見え方、文の読み
やすさ）は、`operator-checklist.md` と保存した画面に残す。

## 2. 準備に対する自動の試験

| 試験 | 結果 |
| --- | --- |
| 製品の UI からの書き出し（CTest `math_group_export_product_ui`） | 64 検査中 0 件失敗（表示の修正の後。結果の位置と、理由の文を検査に足した） |
| controller の単体試験（`m7b_4_controller_export_lifecycle`） | 通過（理由の言い換えの全部と、知らない名前を足した） |
| 対照（製品を 1 か所ずつ壊す） | **9/9 が意図した検査で検出**（P3-6C.1 の 7 通りに、「結果を path の後ろへ戻す」「理由を内部の名前のまま出す」を足した。`mutations/`）。終わった後、source が元と同じことを確かめ、build し直した |
| 製品の入口の統合（P3-6B の script、最終の harness） | 不成立 0（`focused/20261014-093911-focused/`）。36 通りが期待どおり、encoder 直前の RGBA が 2,520 frame 一致、MP4 が 8 組一致。対象は前後で一致（909 file） |
| 検収の script の動作 | `freeze`・`reference` は実行できた（短い fixture 2 本は製品の controller で開け、on と off の出力が同じ）。`session` は、書き出しをせずに起動して閉じるだけの確認をした（`…-script-smoke-not-acceptance/`。**検収ではない**）。console の行の解析は、実際の行の形で確かめた。`verify` は、人の session の記録が無いので、まだ実行していない |
| lint | 通過 |
| **release の全 CTest** | 1 回目は未実行（ENVIRONMENT_INVALID。下）。**2 回目は 1524/1524 通過**（2.2） |

### 全 CTest が未実行である理由

`ctest-run-1/`。実行の前の環境の検査が ENVIRONMENT_INVALID を返したので、規則どおり、実行していない。

- 接続中の monitor が無い（PnP・WMI の両方）。描画先の電源の状態を確かめられない。
- 無操作 1,548 秒（約 26 分）。monitor が消えた（または切られた）状態である。
- 環境を直す操作（入力を送って画面を起こす、など）はしていない。

P3-6C.1 の全 CTest（1524/1524）は、今回の表示の修正の前の binary のものである。今回の binary の
全 CTest は、monitor が戻り、進めてよいと言われた後に実行した（2.2）。

## 2.1 手順書と道具の修正（2026-10-14、人の検収の前）

人の検収はまだ行われていない。その前に、手順書と検収の script に誤りが見つかり、直した。製品の
書き出しの動きは変えていない。

| 誤り | 直したこと |
| --- | --- |
| **手順書の A8 の command に、制御文字（U+0008・U+0007）が入っていた。** Windows の path の `\b`・`\a` が、文書を作る途中で制御文字に化けた。そのまま写しても動かない | path を文字列で書かず、`Join-Path` で組み立て、`[System.IO.FileMode]::Open`・`[System.IO.FileAccess]::Read`・`[System.IO.FileShare]::None` を明示する command に替えた。表の外の code block に置いた |
| **設定の判定が広すぎた。** `no_export_setting_persisted` は、名前に `export` を含む設定が 1 つでもあれば不成立にしていた。数式の高速化と関係のない書き出しの設定（今は無いが、あってよい）まで拒否する | 判定を `math_group_selection_not_persisted` に替えた。session の前後の設定の写しを記録し、(1) 数式 group・高速化を指す名前の key が後に無いこと、(2) session の間に増えた・変わった・消えた key に、その名前が無いこと、を見る。他の `export` の key は、記録するだけで不成立にしない。写しが無い記録は不成立にする |
| verify が、壊れた出力や欠けた項目で、判定を作らずに落ちることがあった | 出力を調べられないときは `output_valid = false` にする。記録の必須の項目が無ければ、判定を作らずに失敗する |

`scripts/math-group-product-p36c2-selftest.ps1` を足した（道具の自己試験。**人による検収ではない。**
GUI を起動しない）。49 件が期待どおりだった（`selftest/20261014-122114/`）。

| 確かめたこと | 数 |
| --- | --- |
| 検収に使う script 5 本を PowerShell の parser に通す（誤り 0）。**lint の通過は、script が実行できることの証明にならない**（構文の誤りを lint が通した例が、この phase にある） | 5 |
| 手順書・script・短い fixture に、tab・改行以外の制御文字が無い | 6 |
| 手順書の A8・A9 の command を、手順書から取り出して実行する。掴んでいる間は最終の名前へ置けない。放した後は置ける | 5 |
| 引数と終了コード（不正な段階、freeze なし、freeze の上書き、参照なしの session、session の指定なし、存在しない session） | 7 |
| verify に、作りものの session の記録を与える。正しい記録 4 通りは通り、1 か所ずつ壊した 21 通りは、**決めておいた判定だけ**が false になる（時間の超過、内容の違い、frame の欠け、動画でない file、従来の graph、合成 0、実行時の失敗、後始末の残り、一時 file、選択なし、失敗、書き出しなし、別の実行 file、DLL の違い、環境の不成立、対象の変化、設定への保存 4 通り、短い fixture の内容の違い）。必須の項目の欠けは、判定を作らずに失敗する | 26 |

- 作りものの記録には `synthetic_selftest = true` を入れてあり、verify は `human_acceptance = false` と
  記録する。出力先は `selftest/` で、検収の証拠とは別である。
- 自己試験が確かめていないもの: `session` の段階が、実際の書き出しの行を読むところ（行の解析は、実際の
  行の形で別に確かめた。起動と記録は、書き出しをしない確認だけ）。画面の保存の内容。
- 前の commit にある手順書（制御文字入り）は、履歴に残る。使えなかった freeze・参照
  （`trial-before-ctest/`）、ENVIRONMENT_INVALID で止まった全 CTest（`ctest-run-1/`）も残してある。

## 2.2 最終の回帰・対象の固定・参照（2026-10-14、人の検収の前）

進めてよいと言われた後に、次の順で実行した。build はしていない（全 CTest の中の build を除く）。
GUI の操作も、全長の書き出しもしていない。

| 段 | 結果 |
| --- | --- |
| 環境の検査（全 CTest の直前。`scripts/gui-environment-preflight.ps1`） | **VALID**。monitor が接続・点灯、lock なし、試験用の window の描画と preview の提示の probe が成功 |
| **release の全 CTest**（`pwsh scripts/test.ps1 -Preset ucrt64-release`、`ctest-run-2/`） | **1524/1524 通過**（1,183 秒）。登録数は 1524 で、P3-6C.1 から変わらない。実行の前に、他の CTest・検証 harness・build・`mvm.exe` が動いていないことを確かめた（CPU の負荷 4%） |
| 環境の検査（全 CTest の直後） | **VALID** |
| 対象の固定（`freeze.json`） | 1,524 file の SHA-256（数が全 CTest の登録数と同じなのは偶然である）。git の HEAD `5abcc44`、作業 tree に変更なし |
| 参照の出力（`reference.json`、`20261014-125449-reference/`） | 不成立 0。下の表 |

固定した実行 file: `build/ucrt64-release/bin/mvm.exe`、SHA-256
`43B11F5D22EEE82DF0DA0F69825C39BE3C12E825A3CB0FC3B38EAB4FA250DE62`（全 CTest の中の build が link し直した
後のもの。全 CTest の後は build していない）。

固定に含めたもの: `mvm.exe` と、その隣の DLL。MSYS2 UCRT64 の `bin` の DLL の全部、MLT の module、Qt の
plugin と QML の plugin、ffprobe・ffmpeg。`src`・`apps`（QML を含む）・`tests`・CMake。v4 の Project と
短い fixture、それぞれの数式の成果物、参照する素材。検収の script 2 本・自己試験・手順書。
主なもの: `libmlt-7.dll` `13D5EFA4536D3225…`、`libx264-165.dll` `ACF3D8F926F42ADA…`、`Qt6Quick.dll`
`B7869486543010B5…`、`avcodec-62.dll` `FE0190F188FB066E…`、検証 harness `059E3CC770F818C9…`
（製品の入口の統合試験を回したものと同じ）、v4 の Project `D6991972807458A7…`。

参照の出力（検証 harness の `--product-pure`。製品の要求を書き換えない書き出し）:

| fixture | 選択 | 判定 → graph | まとめた clip・合成 | frame | MP4 の SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `short-math-only-eligible` | on | ELIGIBLE → 数式 group | 16・145 | 120 | `8c6ef830b8330ecc…` |
| 同 | off | 判定しない → 従来 | 0・0 | 120 | 同じ |
| `short-fallback-unknown` | on | UNKNOWN → 従来 | 0・0 | 120 | `dd0aa3dc20a2d790…` |
| 同 | off | 判定しない → 従来 | 0・0 | 120 | 同じ |

v4 全体の参照は、過去の全長の出力 3 本（P3-4・P3-5A・P3-6B）で、今回計算し直した SHA-256 は 3 本とも
`FF8EB4D5D80B3021…A8BF6C0` である。

**起動される実行 file と DLL の確認について。** 起動する path と SHA-256 は、上のとおり固定と一致して
いる。実際に読み込まれる DLL は、起動しないと分からないので、各 session が起動のたびに全部の SHA-256 を
固定と照合し、verify が不一致を不成立にする（この仕組みは、書き出しをしない起動で 1 度確かめてある。
`…-script-smoke-not-acceptance/`。その後、人の検収の前に、私は `mvm.exe` を起動していない）。

**人による検収の判定について。** verify の記録の `human_acceptance = true` は、「作りものの記録ではない」
という意味でしかない。人が操作したことの証拠にはならない。PASS には、`operator-checklist.md`（人が書く）
と、session が保存した画面を、別に確かめることが要る。

## 2.3 1 回目の人の操作（2026-10-14 15:55〜16:11）と、記録の不具合

人が Session A・B・C を操作した。`mvm` 自身が console へ出した行（`mvm-stderr.log`）と、出力の file から
分かることは次のとおりである。

| session | `mvm` が出した結果 |
| --- | --- |
| A（対象外の Project） | 高速化を選んで書き出し → 成功、`outcome=conventional`、判定 UNKNOWN（`base-clip-transformed`）。取消 → 取消。掴んだ file への書き出し → 失敗（「正規名へ rename できません: Permission denied」）。**最初の 2 回は、手順書の folder ではない場所（`C:\dev\soft\mvm\tmp\trash\`）へ書き出された**（操作した人の申告どおり。その folder は今は無い） |
| B（数式だけの Project） | 高速化を選んで書き出し → 成功、`outcome=used`、ELIGIBLE、まとめた clip 16、合成 145、失敗 0 |
| C（v4 全体） | 高速化を選んで書き出し → **成功、6840 frame、`outcome=used`、ELIGIBLE、数式 group の graph、group 2、まとめた clip 97、affine 23、合成 13,491、callback 13,730、失敗 0、画像 2,947,112 byte、残り 0** |

出力の file を、session の記録を使わずに直接調べた（`outputs-direct-check-20261014-161532/`）:

| file | 結果 |
| --- | --- |
| `manual-out/c-v4-full.mp4` | H.264 High、1920x1080、60 fps、6840 frame を復号、error 0、時刻は 1 frame ずつ、x264 の設定は過去の出力と同じ、**SHA-256 は参照（`FF8EB4D5…`）と同じ**、一時 file なし |
| `manual-out/b1-math-only.mp4` | 120 frame、error 0、SHA-256 は参照と同じ |
| `manual-out/a1-fallback.mp4` | 120 frame、error 0、SHA-256 は参照と同じ。掴まれていた間の書き出しの失敗の後も、元のまま |

**それでも検収に使えない理由:**

1. **session の記録が、書き出しの行を読み取れなかった**（B・C は 0 回、A は 4 回のうち 2 回）。script が
   `Start-Process` の出力の転送に頼り、転送先の file を後から読んでいたため、行が file に届かないまま
   だった。`mvm` が出した行そのものは、session の終わりに file に揃っていた。
   その結果、**全長の書き出しの時間を、決めた方法（行を受け取った時刻の差）で測れていない。**
   参考までに、出力の file の作成時刻（`.mvmtmp` を作った時刻）から最後に書いた時刻までは 246.8 秒で
   ある。画面の記録からは、書き出しを始めたのは 16:04:34 以降と読める（その時点の画面に、まだ進行の
   dialog が無い）。合わせると、書き出しの全体は 247〜270 秒程度で、600 秒より十分短い。ただし、これは
   壁の時計と file の時刻を組み合わせた推定で、検収の測り方ではない。
2. **全長の書き出しの間に、PC が使われていた。** script の画面の記録（画面のその位置を写す方式だった）に、
   書き出しの間、VS Code と browser が写っている。手順書の「終わるまで PC に触らない」に合わない。
   時間の基準に対しては不利な方向（遅くなる方向）の条件だが、静かな環境という前提は満たしていない。
3. `operator-checklist.md` が、どの session にも無い。
4. Session A の最初の書き出しと取消は、決めた folder の外だった（出力は消えている）。

固定した `mvm.exe` と DLL（読み込まれたもの全部）、環境の検査、設定（数式の高速化の選択は保存されて
いない）、対象の固定は、3 つの session とも記録上は問題が無かった（`verify.json`。不成立は、書き出しを
検出できなかったことによるものだけ）。

**直したこと（script。製品は変えていない）:**

- `mvm` の stderr を pipe で直接受け取り、行を受け取った時刻（script の単調な時計）を付ける。log の file
  にも、受け取ったそのときに書く。
- 書き出しの間の keyboard・mouse の入力と、`mvm` 以外の window が前面になった回数を記録する。全長の
  書き出しでは、どちらかが 1 回でもあれば verify が不成立にする（`quiet_during_export`）。
- 画面は、`mvm` の window の中身を描かせる方式（PrintWindow）にした。上に他の window が重なっていても
  `mvm` を写し、他の software の画面を写さない。
- 手順書: 2 回目であること、書き出し先（`attempt-2\manual-out\`）を毎回選び直すこと、書き出しの間に
  触らないこと、verify の command の例を、そのまま実行すると失敗する名前から、置き換えの形に直した
  （1 回目は、例の名前をそのまま実行して「session の記録がありません」になった）。
- 自己試験に、偽の app（pwsh）を使った pipe の記録の試験（受け取った時刻の差が約 3 秒）、書き出しの間の
  操作の記録、固定した `mvm.exe` を書き出しなしで起動して強制終了する確認（起動の行、DLL 205 個の一致、
  PrintWindow の画面）を足した。62 件が期待どおりだった（`selftest/20261014-162302/`。強制終了は、
  window を閉じる操作の代わりではなく、process を止めただけである）。

2 回目の対象の固定と参照は `attempt-2/` に作った（`mvm.exe` は全 CTest の後のものから変わっていない:
`43B11F5D…`。script と手順書の変更は、build を伴わない）。参照の出力は 1 回目と同じ SHA-256 だった。

1 回目の記録と出力は、消さずに残してある（`20261014-155529-…`・`20261014-160116-…`・`20261014-160204-…`・
`manual-out/`）。全長の書き出しの間の画面には、操作した人の他の software（browser など）が写っている。
共有する前に、必要なら消してほしい（検収の判定には使っていない）。

## 2.4 自動の検収（2026-10-14 19:16〜19:22）

方針の変更（人の操作をやめる）を受けて、次を作った。製品の code は変えていない。

- `scripts/math-group-product-p36c2-drive.ps1`: 人の代わりに `mvm.exe` を操作する driver。menu・checkbox・
  button は UI Automation（Qt が公開する部品を名前で探し、Invoke・Toggle）、Windows 標準の保存 dialog は
  file 名の欄への WM_SETTEXT と「保存」への BM_CLICK、上書きの確認（task dialog）は TDM_CLICK_BUTTON で
  操作する。各段で、部品の値（checkbox の状態、状態の表示の文、dialog の文）を読んで確かめる。
- 検収の script の `-Drive`: session を起動すると driver も起動し、driver の結果を記録に入れる。verify は
  driver の各段の成立を判定に入れる（`automated_gui_operations`）。環境の検査と、書き出しの間の利用者の
  操作は、判定ではなく参考として残す（driver は画面の表示も mouse・keyboard も使わない）。
- 手順書を、人の操作を求めない形に書き換えた。

開発の途中で直した driver の誤り（`drive-dev/` に残してある）: 保存 dialog の file 名の欄の探し方、menu が
開かないときの開き直し、上書きの確認が task dialog であること（UI Automation から押せないので
TDM_CLICK_BUTTON にした）、dialog が開く瞬間の UI Automation の一時的な error。

本番の実行（`automated/`）。対象の固定（1,525 file。`mvm.exe` は全 CTest の後のもの `43B11F5D…` のまま）→
参照の出力 → 場面 A・B・C → 各 verify。全部、人の操作なしで通った。

| 場面 | driver の段 | 書き出し | verify |
| --- | --- | --- | --- |
| A（対象外の Project） | 24/24 | 高速化を選んで成功（従来の方法、15.9 秒、参照と同じ SHA-256）、取消、掴まれた file への書き出しの失敗 | 全項目成立 |
| B（数式だけの Project） | 9/9 | 高速化を選んで成功（数式 group、16 本、合成 145、2.5 秒、参照と同じ SHA-256） | 全項目成立 |
| C（v4 全体） | 14/14 | 高速化を選んで成功（数式 group、97 本、affine 23、合成 13,491、失敗 0、**180.1 秒**、6840 frame、参照と同じ SHA-256） | 全項目成立 |

- 3 つの session とも、起動したのは固定した `mvm.exe` で、読み込まれた DLL 212 個のうち、repository と
  MSYS2 のものは全部固定と同じだった（他の 8 個は、他の software が差し込む IME などの hook で、別に記録）。
- 数式の高速化の選択は、アプリの設定に保存されていない（設定の変化 0 件）。
- 参考: 環境の検査は 3 つとも VALID。場面 C の書き出しの間、利用者の入力 0 回、他の window の前面 0 回。
- 全長の時間（180.1 秒）は、P3-6B の検証 harness の 291.0 秒、1 回目の人の操作の推定 247〜270 秒より
  短い。出力は 3 つとも byte 単位で同じなので、差は実行のときの負荷の違いと見ている（調べていない）。
- （原因は P3-7A で特定して直した: [結果](explorer-reveal-p37a.md)。この判定は変えていない）
  状態の表示に「Explorerで表示できません: …(HRESULT=0x80070057)」が付いた（書き出しの成功の後、出力の
  file を Explorer で表示する処理の失敗。書き出しの成功は変わらない、という既存の動き）。1 回目の人の
  操作でも出ていたかは記録が無い。driver が起動した `mvm` だけの現象かどうかは調べていない。

**自動の検収で確かめていないこと:**

1. 画面の見た目（文字が欄に収まっているか、読みやすいか）。部品の値は読んだが、描画は見ていない
   （保存した画面は残してある。P3-6C.1 の画面の確認は、試験の実行 file での描画）。
2. 人が mouse や keyboard で操作したときの反応（focus、click の当たり判定など）。driver は部品を直接
   操作する。
3. encoder 直前の RGBA（この書き出しには計測を付けない。MP4 の一致は間接の確認）。

## 3. 分かっていること・気付いたこと

- **「入力映像: 仕様を取得できません」の表示は、この phase より前からある。** 書き出し設定の要約
  （`exportSettingsSummary`）が、音声以外の clip を全部「入力の映像」として数え、素材の file を調べる。
  数式・文字の clip は file を持たないので、調べられない、という表示になる。この処理は 2026-09-27 の
  commit `559991a` からあり、数式の高速化とは関係が無い。書き出しを止める条件ではない（製品の UI の
  試験と、参照の出力の作成で、この表示が出る Project を書き出せている）。人の検収でも確かめる
  （手順書の B1）。直していない。
- **lint は、PowerShell の script の構文の誤りを検出しなかった。** 検収の script に構文の誤り（文字列の中の
  `$name:`）があったが、`scripts/lint.ps1` は通過した。実行して気付き、直した。lint の側は調べていない。
- `scripts/build.ps1` だけでは `mvm.exe` は link し直されなかった（SHA-256 が変わらない）。link し直すのは
  `scripts/test.ps1` である。そのため、対象の固定は全 CTest の後に行う。

## 4. 方針（変えていない）

- 既定は off。利用者が、書き出しごとに選ぶ。保存しない。
- on のとき: ELIGIBLE → 数式 group、UNKNOWN・INELIGIBLE・画像の上限の超過 → 従来の方法。
- 実行時の失敗 → 書き出しを止める。自動ではやり直さない。
- 既定を on にする移行は、入れていない。

## 5. これまでの証拠の扱い

P3-4・P3-5A・P3-6A・P3-6B・P3-6C.1 の証拠と判定は変えていない
（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま。全 CTest が負荷や利用者の操作で 1 件
落ちることがある、という既知の事項もそのまま）。今回の途中の記録も残してある: 表示の修正の前後の画面、
script の試走で作った freeze と参照（`trial-before-ctest/`）、ENVIRONMENT_INVALID で止まった全 CTest。
