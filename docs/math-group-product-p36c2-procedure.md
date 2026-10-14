# P3-6C.2 手順書: 実際の mvm.exe を人が操作する検収

「数式の書き出しを高速化（試験的）」を、実際の `mvm.exe` の画面から使えることを、人の操作で確かめる。
所要は約 25 分（全長の書き出し 5〜6 分を含む）。

**GUI の操作は、全部、人が行う。** script は、決めた `mvm.exe` を起動して、記録を取るだけである
（console へ出る行の時刻、読み込まれた DLL、画面の保存、設定と Project の前後）。script は window を
操作しない。

## 0. 始める前に

- monitor が点いていて、lock されていない。
- 他の重い処理（build、test、動画の再生、他の書き出し）を動かしていない。
- 検収の間は、この PC で他の作業をしない（全長の書き出しは時間を測る）。
- PowerShell 7 を開き、`C:\dev\soft\mvm` へ移る。
- `mvm.exe` を他に起動していない。

各 session は、`mvm` の window を閉じると終わり、記録の場所と、次に打つ `verify` の command を表示する。
session の console は閉じない。

確認の結果は、session ごとの directory の `operator-checklist.md` に書く（下の表を写し、`[ ]` を `[x]` に
する。違ったことは、そのまま文で書く）。画面は script が 2 秒ごとに保存するので、自分で撮らなくてよい。
撮りたい場面で 3 秒ほど手を止めると、確実に残る。

## 1. 短い確認（Session A: 対象外の Project）

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project short-fallback-unknown -Label usability
```

V1 の画像を拡大してある、2 秒の Project である（高速化の対象外。従来の方法で約 25 秒かかる）。
書き出し先は、どれも `build\math-group-product-p36c2-20261014\manual-out\` に置く（無ければ作る）。

| # | 操作 | 確かめること |
| --- | --- | --- |
| A1 | 「ファイル」→「メディアを書き出し」。書き出し先に `a1-fallback.mp4`。 | 書き出し設定が開く |
| A2 | 何も触らずに見る（3 秒止まる） | 「数式の書き出しを高速化（試験的）」が **off**。説明の文が読める。欄からはみ出していない |
| A3 | checkbox を click する（3 秒止まる） | check が付き、「書き出す」を押すまで付いたままである |
| A4 | 「書き出す」 | 進行の dialog が出て、終わる |
| A5 | 画面の上の状態の表示を読む | **「数式の高速化は使いませんでした（一番下の映像 track の clip に拡大・回転などの変形があるため）。従来の方法で書き出しました: …」** と読める。文の意味が分かる。右で切れていても、理由までは読める |
| A6 | もう一度「メディアを書き出し」。書き出し先に `a6-cancel.mp4`。設定を見る | checkbox が **off に戻っている** |
| A7 | checkbox を on にして「書き出す」。進行の dialog で **「キャンセル」** を押す | 「書き出しをキャンセルしました」と出る。`manual-out` に `a6-cancel.mp4` も `a6-cancel.mp4.mvmtmp` も無い |
| A8 | **別の** PowerShell 7 を `C:\dev\soft\mvm` で開き、表の下の「A8 の command」を実行して `a1-fallback.mp4` を掴む。`mvm` で「メディアを書き出し」、書き出し先に同じ `a1-fallback.mp4` を選ぶ（上書きの確認は「はい」）。checkbox は off のまま「書き出す」 | 約 25 秒後に **「書き出しに失敗しました」** の dialog が出る（最終の名前へ置けない）。`a1-fallback.mp4.mvmtmp` が残っていない。元の `a1-fallback.mp4` は消えていない |
| A9 | 別の PowerShell で、表の下の「A9 の command」を実行して file を放す。失敗の dialog を「OK」で閉じ、`mvm` の window を閉じる（保存を聞かれたら **保存しない**） | — |

A8 の command（`C:\dev\soft\mvm` で実行する。file を、他からは読むことも書くこともできない形で開く）:

```powershell
$locked = Join-Path (Get-Location) 'build' | Join-Path -ChildPath 'math-group-product-p36c2-20261014' | Join-Path -ChildPath 'manual-out' | Join-Path -ChildPath 'a1-fallback.mp4'
if (-not (Test-Path -LiteralPath $locked)) { throw "A1 の出力がありません: $locked" }
$h = [System.IO.File]::Open($locked, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
"掴みました: $locked"
```

A9 の command（A8 と同じ PowerShell で実行する）:

```powershell
$h.Dispose()
"放しました: $locked"
```

## 2. 短い確認（Session B: 数式だけの Project、起動し直し）

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project short-math-only-eligible -Label restart
```

V1 に何も無い、数式だけの 2 秒の Project である（高速化の対象）。

| # | 操作 | 確かめること |
| --- | --- | --- |
| B1 | 「メディアを書き出し」。書き出し先に `b1-math-only.mp4`。設定を見る | 起動し直した後なので、checkbox は **off**。「入力映像: 仕様を取得できません」「1件の入力ファイルは仕様を確認できませんでした。」という表示が出ていても、「書き出す」は押せる |
| B2 | checkbox を on にして「書き出す」 | 数秒で終わる。失敗の dialog は出ない |
| B3 | 状態の表示を読む | **「数式の高速化を使いました（数式 clip 16 本）。書き出しました: …」** と読める |
| B4 | `mvm` の window を閉じる（**保存しない**） | — |

## 3. 全長の書き出し（Session C: v4 の Project 全体）

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project v4 -Label full
```

| # | 操作 | 確かめること |
| --- | --- | --- |
| C1 | 数式の描画が全部終わるのを待つ（timeline の数式 clip に「描画中」の表示が無い） | — |
| C2 | 「メディアを書き出し」。書き出し先に `c-v4-full.mp4`。設定を見る（3 秒止まる） | checkbox は **off**。出力は 1920×1080 / 60 fps。品質は「標準（CRF 23）」 |
| C3 | checkbox を on にする（3 秒止まる）。品質は変えない | check が付いている |
| C4 | 「書き出す」。**終わるまで PC に触らない**（5〜6 分） | 進行の dialog に frame の数と残り時間が出る |
| C5 | 終わったら、状態の表示を読む（3 秒止まる） | **「数式の高速化を使いました（数式 clip 97 本）。書き出しました: …c-v4-full.mp4 (6840 frame / 114.00 秒)」** |
| C6 | もう一度「メディアを書き出し」を開き、設定を見て「キャンセル」で閉じる | checkbox が **off に戻っている** |
| C7 | `mvm` の window を閉じる（**保存しない**。保存を聞かれなければ、Project は変わっていない） | — |

書き出しの時間は、script が測る（「書き出しを始めます」の行から「書き出しの結果」の行まで）。
基準は 600 秒である。25 分を超えても終わらなければ、script が console に watchdog の行を出す
（止めはしない）。その場合は「キャンセル」を押して、起きたことを書く。

## 4. 事後の検査

各 session が最後に表示した command を、そのまま実行する。例:

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage verify -Session 20261014-120000-session-v4-full
```

全長の検査は、6840 frame を復号するので 1〜2 分かかる。結果は session の directory の `verify.json`。

## 5. operator-checklist.md の形

```markdown
# operator-checklist（Session A / B / C のどれか）
操作した人: （名前）　日時: （開始〜終了）
- [ ] A1 …（上の表の番号ごとに 1 行。違ったことは文で書く）
気付いたこと:
```

## してはいけないこと

- 検収の間に build・test を動かさない（`mvm.exe` が link し直されると、固定した対象と変わる）。
- 試験の実行 file（`mvm_test_*`）や検証 harness の書き出しで、人の操作の代わりにしない。
- Project を保存しない（v4 の Project と短い fixture は、固定した対象である）。
- うまくいかなかった session の directory を消さない（やり直すときは、もう一度 session を起動する。
  新しい directory ができる）。
