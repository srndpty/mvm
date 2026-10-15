# P3-6C.2 手順書: 実際の mvm.exe を自動で操作する検収

「数式の書き出しを高速化（試験的）」を、実際の `mvm.exe` の画面から使えることを確かめる。

**人の操作は要らない。** 2026-10-14 に方針を変えた（人が数十分 GUI を操作する検収は、負担が重く、操作の
誤りも入る。1 回目の人の操作では、保存先の取り違え・書き出し中の PC の使用・checklist の未記入が起きた）。
人が行っていた操作は、driver（`scripts/math-group-product-p36c2-drive.ps1`）が行う。

- driver は、固定した `mvm.exe` を普通に起動し、製品の画面の部品を操作する。menu・checkbox・button は
  UI Automation（Invoke・Toggle）で、Windows 標準の保存 dialog は Windows の message（file 名の欄への
  WM_SETTEXT、「保存」への BM_CLICK、上書きの確認への TDM_CLICK_BUTTON）で操作する。
- **mouse と keyboard は使わない**（SendInput を使わない）。driver が動いている間も、PC は使える。ただし
  書き出しの時間は、他の作業の負荷で延びうる（遅くなる方向なので、600 秒の基準に対しては厳しい側）。
- 書き出しの時間は、`mvm` が stderr へ出す「書き出しを始めます」「書き出しの結果」の行を、検収の script が
  pipe から受け取った時刻（単調な時計）の差で測る。
- 人の checklist の代わりに、driver が各段で画面の部品の値（checkbox の状態、状態の表示の文、dialog の文）
  を読んで確かめ、`drive.json` に残す。

## 前提

- 全 CTest は、今の source と binary で通っている（[状況](math-group-product-p36c2.md) の 2.2）。
- 検収の間に build・test を動かさない（`mvm.exe` が link し直されると、固定した対象と変わる）。
- `mvm.exe` を他に起動していない。
- monitor は点いていなくてよい（書き出しと UI Automation は画面の表示を要しない）。環境の検査の結果は
  記録するが、検収の判定には入れない。

## 実行（全部で約 8 分。人の操作なし）

`C:\dev\soft\mvm` で順に実行する。session は、終わると次に打つ verify の command を表示する。

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage freeze
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage reference
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project short-fallback-unknown -Label auto-a -Drive
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project short-math-only-eligible -Label auto-b -Drive
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project v4 -Label auto-c -Drive
```

記録と出力は `build\math-group-product-p36c2-20261014\automated\` に入る（書き出しの出力は `manual-out\`）。
session の名前は起動した日時で決まる。一覧は次で出す:

```powershell
Get-ChildItem build\math-group-product-p36c2-20261014\automated -Directory -Filter '*-session-*' | Select-Object Name
```

verify の command の形（`<session の名前>` を一覧の名前に置き換える）:

```powershell
pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage verify -Session "<session の名前>"
```

## driver が行う操作と、確かめること

### 場面 A（`short-fallback-unknown`: V1 の画像を拡大した 2 秒の Project。高速化の対象外）

| # | 操作 | 確かめること |
| --- | --- | --- |
| A1 | 「ファイル」→「メディアを書き出し」、保存 dialog に `a1-fallback.mp4` | 書き出し設定が開く |
| A2 | — | checkbox「数式の書き出しを高速化（試験的）」が off。説明の文（「従来の方法で書き出します」「自動ではやり直しません」）がある |
| A3〜A4 | checkbox を on、「書き出す」 | on と表示される |
| A5 | 終わるのを待つ | 状態の表示が「数式の高速化は使いませんでした（一番下の映像 track の clip に拡大・回転などの変形があるため）。従来の方法で書き出しました: 」で始まる |
| A6 | もう一度、`a6-cancel.mp4` へ | checkbox が off に戻っている |
| A7 | on にして「書き出す」、進行の dialog の「キャンセル」 | 「書き出しをキャンセルしました」。最終 file も一時 file も無い |
| A8 | `a1-fallback.mp4` を、他から読み書きできない形で開いたまま、同じ名前へ off で書き出す（上書きの確認は「はい」） | 失敗の dialog が出る（最終の名前へ置けない） |
| A9 | 失敗の dialog を「OK」で閉じ、file を放す | 元の file は変わらず、一時 file が残らない |

### 場面 B（`short-math-only-eligible`: V1 に何も無い、数式だけの 2 秒の Project。高速化の対象）

| # | 操作 | 確かめること |
| --- | --- | --- |
| B1 | 起動し直した `mvm` で、`b1-math-only.mp4` へ書き出し設定を開く | checkbox が off（前の起動の選択が残らない）。入力映像が「なし（数式 …件を出力の仕様で描きます）」で、「仕様を取得できません」「…件の入力ファイルは仕様を確認できませんでした。」が無い（P3-7B で修正。以前はこの 2 つが出ていた） |
| B2〜B3 | on にして書き出す | 状態の表示が「数式の高速化を使いました（数式 clip 16 本）。書き出しました: 」で始まる |

### 場面 C（v4 の Project 全体。6840 frame）

| # | 操作 | 確かめること |
| --- | --- | --- |
| C2 | `c-v4-full.mp4` へ書き出し設定を開く | checkbox が off。出力が「1920×1080 / 60 fps」。品質が「標準（CRF 23）」 |
| C3〜C4 | on にして書き出す | — |
| C5 | 終わるのを待つ | 状態の表示が「数式の高速化を使いました（数式 clip 97 本）。書き出しました: 」で始まり、「c-v4-full.mp4 (6840 frame / 114.00 秒)」を含む |
| C6 | もう一度書き出し設定を開き、「キャンセル」で閉じる | checkbox が off に戻っている |

どの場面も、最後に `mvm` の window を閉じる（保存を聞かれずに終わること、を確かめる）。

## verify が確かめること

- 起動したのが固定した `mvm.exe` で、読み込まれた DLL が全部固定と同じ。対象の固定が、前後で変わらない。
- driver の各段が全部成立した（`automated_gui_operations`）。
- 数式の高速化の選択が、アプリの設定に保存されていない。
- 書き出しの後始末（一時 file が無い、失敗・取消で file が残らない）。
- 短い場面: 出力の SHA-256 が参照（検証 harness の書き出し）と同じ。
- 場面 C: 高速化を選んだ書き出しが成功、判定 ELIGIBLE、数式 group の graph、合成が 0 でない、失敗 0、
  経過が 600 秒未満、H.264 High・1920x1080・60 fps・6840 frame を復号して error 0、時刻、x264 の設定、
  **MP4 の SHA-256 が過去の全長の出力と同じ**、transition・callback の残り 0。
- 参考として残すもの（判定にしない）: 環境の検査、書き出しの間の利用者の入力と前面の window。

verify が確かめないもの: encoder 直前の RGBA（この書き出しには計測を付けない。MP4 の一致は間接の確認）。
