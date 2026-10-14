# P3-6C.2: 実際の mvm.exe を人が操作する検収

2026-10-14。「数式の書き出しを高速化（試験的）」を、実際の `mvm.exe` の画面から人が操作して確かめる
phase である。

## 判定: MANUAL_ACCEPTANCE_PENDING

**人による `mvm.exe` の操作は、まだ行われていない。** この文書の時点で済んでいるのは、検収の準備
（表示の修正、手順書、起動と記録と事後検査の script、短い確認用の Project）と、その準備に対する自動の
試験である。PASS ではない。

残っているのは、順に次の 3 つである（1 は monitor が消えていて実行できず、2 は 1 の後でないと意味が無く、3 は人にしかできない）。

1. release の全 CTest（monitor が点いた、静かな環境が要る）。
2. 対象の固定（`mvm.exe` を含む）と、参照の出力の作成。
3. 人による操作（[手順書](math-group-product-p36c2-procedure.md)。約 25 分）と、事後の検査。

数式 group の合成の式、下地の alpha の条件、MLT の affine、V1 の背景の保護、timeout、encoder 直前の
画素の authority は変えていない。既定は off のまま。実行時の失敗のときの自動のやり直しは入れていない。
commit・push はしていない。v4 全体の書き出しは、この準備の中では行っていない。

証拠は `build/math-group-product-p36c2-20261014/`。

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
| **release の全 CTest** | **未実行（ENVIRONMENT_INVALID）。** 下 |

### 全 CTest が未実行である理由

`ctest-run-1/`。実行の前の環境の検査が ENVIRONMENT_INVALID を返したので、規則どおり、実行していない。

- 接続中の monitor が無い（PnP・WMI の両方）。描画先の電源の状態を確かめられない。
- 無操作 1,548 秒（約 26 分）。monitor が消えた（または切られた）状態である。
- 環境を直す操作（入力を送って画面を起こす、など）はしていない。

P3-6C.1 の全 CTest（1524/1524）は、今回の表示の修正の前の binary のものである。**今回の binary の
全 CTest の結果は、まだ無い。**

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
