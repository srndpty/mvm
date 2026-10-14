# P3-7A: 書き出しの後の「Explorer で表示」が失敗していた

2026-10-14。P3-6C.2 の自動の検収で、書き出しは成功したのに、状態の表示に
「Explorerで表示できません: Explorerでファイルを表示できません (HRESULT=0x80070057)」が付いていた。
原因を特定し、Explorer 表示の helper の中だけを直した。

書き出しの MP4・timeout・encoder・数式 group の経路は変えていない。数式 group の既定（off）も変えていない。
commit・push はしていない。

証拠は `build/explorer-reveal-p37a-20261014/`。

## 判定

| # | 項目 | 結果 |
| --- | --- | --- |
| 1 | 原因 | **特定した。** QML の FileDialog の URL から作った出力の path は区切りが `/`。`SHParseDisplayName` は `/` 区切りの path を E_INVALIDARG（0x80070057）で拒否する |
| 2 | 修正の前の再現 | **再現した。** Explorer を開かない小さな program で、同じ file を `/` 区切りなら 0x80070057、`\` 区切りなら成功（0x00000000） |
| 3 | 修正と対照 | Shell へ渡す前に `GetFullPathNameW` で Windows の絶対 path（`\` 区切り）にする。修正を外した対照で、意図した検査が 10 件落ちる |
| 4 | 自動の回帰 | 通過（C の helper、controller の実際の書き出し、実際の `mvm.exe` の GUI smoke） |
| 5 | 全 CTest | **PASS: 1524/1524**（実行の前後とも GUI の環境は VALID）。lint 通過 |

## 1. 原因

### 実装（`src/util/mvm_reveal_in_explorer.c`）

1. `GetFileAttributesW(path)` で、file があり、directory でないことを確かめる。
2. `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`（既に別の mode なら、その状態のまま使う）。
3. `SHParseDisplayName(path, …)` で PIDL を作る。
4. `SHOpenFolderAndSelectItems(pidl, 0, NULL, 0)` で、file を選んだ状態の folder を開く。

controller（`apps/mvm/mvm_controller.cpp` の `revealFileInExplorer`）は、書き出しの結果の `outputPath` を
そのまま渡す。`outputPath` は、書き出しの要求の出力先で、UI の FileDialog の URL を
`QUrl::toLocalFile()` で path にしたもの（`apps/mvm/mvm_controller_export.cpp`）。

### 渡していた path

`QUrl::toLocalFile()` は区切りを `/` で返す。P3-6C.2 の場面 C で渡した path:

```
C:/dev/soft/mvm/build/math-group-product-p36c2-20261014/automated/manual-out/c-v4-full.mp4
```

### なぜ失敗するか

- `GetFileAttributesW` は `/` を受け付けるので、存在の確認は通る。
- `SHParseDisplayName` は `/` 区切りを解釈できず、**E_INVALIDARG（0x80070057）** を返す。その時点で
  失敗し、`SHOpenFolderAndSelectItems` は呼ばれていなかった。
- COM の初期化、apartment・thread、引数（PIDL、親と item の関係）は原因ではない。`\` 区切りにするだけで、
  同じ thread・同じ手順で成功する（2）。

### 範囲

- **数式 group とは関係が無い。** P3-6C.2 の自動の検収で、成功した 5 回の書き出し（従来の方法の
  `a1-fallback.mp4` を含む）が全部、この失敗を出していた（`automated/*/drive.json`）。
- 製品の UI から書き出した全部の場合に起きていたと考えられる。helper は 2026-09-27（`59d8e0c`）から、
  出力の path を URL から作る形は 2026-08-30（`0d03b9e`）からある。
- これまで見つからなかった理由: helper の試験（`reveal_in_explorer_negative`）は、Explorer を開いてしまう
  ので成功の経路を通していなかった。controller の試験は、記録するだけの偽の revealer を使っていた。
- 1 回目の人の操作（P3-6C.2 の 2.3）で、この文が出ていたかは記録が無い（状態の表示を記録していない）。
- 書き出しの成功は変わらない（失敗の文を状態の表示に足すだけの、既存の動き）。

## 2. 修正の前の再現

`build/explorer-reveal-p37a-20261014/repro/`。`parse_repro.c` は、`SHParseDisplayName` だけを呼び、
Explorer は開かない（人の操作も要らない）。

| path | `GetFileAttributesW` | `SHParseDisplayName` |
| --- | --- | --- |
| `C:/…/automated/manual-out/c-v4-full.mp4`（`/` 区切り） | 成功 | **0x80070057** |
| 同じ file（`\` 区切り） | — | **0x00000000** |

（`repro-run-v4.txt`。日本語の path での試行 `repro-run-1.txt` は、試験の program が引数を UTF-8 として
読めず、file を見つけていない。日本語・空白の path は 4 の試験で確かめた。）

## 3. 修正（`src/util/mvm_reveal_in_explorer.c`・`.h`）

- Shell へ渡す前に、`GetFullPathNameW` で Windows の絶対 path（区切りは `\`）にする。相対 path も、
  現在の directory から解決される。存在の確認も、その path で行う。
- 失敗の文を 2 つに分けた: path の解釈の失敗（「Explorerに渡すファイルのパスを解釈できません」）と、
  Explorer を開く処理の失敗（「Explorerでファイルを表示できません」）。どちらも HRESULT をそのまま出す。
  失敗を隠したり、Explorer の処理を外したりはしていない。
- 試験用の Shell の境界 `mvm_reveal_set_shell_for_test` を足した。`SHParseDisplayName`・
  `SHOpenFolderAndSelectItems` を差し替えられ、Shell へ渡す直前の path を受け取れる。製品は使わない。

### 対照

`build/explorer-reveal-p37a-20261014/negative-control/`。修正の後の code で、Shell へ渡す path だけを
元の path に戻す（修正の前と同じ動き）と、意図した検査が落ちた:

- helper の試験: 7 件（「/ 区切りの日本語・空白の path を表示できる」「Shell へ渡す path は \ 区切りの
  絶対 path」「相対 path を現在の directory から解決する」ほか）。
- controller の試験: 3 件。状態の表示が「… / Explorerで表示できません: Explorerに渡すファイルのパスを
  解釈できません (HRESULT=0x80070057)」になり、修正の前の現象と同じ HRESULT が出た。

元に戻し、build し直してから、通ることを確かめた。

## 4. 自動の回帰

### C の helper（CTest `reveal_in_explorer_negative`）

`SHOpenFolderAndSelectItems` だけを差し替え、`SHParseDisplayName` は実際の API を使う（Explorer は開かない）。

| 場合 | 確かめたこと |
| --- | --- |
| 前提 | 実際の `SHParseDisplayName` が、`/` 区切りを E_INVALIDARG で拒否し、`\` 区切りの日本語・空白の path を受け付ける |
| `/` 区切りの、日本語・空白・丸数字を含む path（`日本語 フォルダ/出力 先/書き出し 結果 ①.mp4`） | 表示できる。Shell へ渡す path は `\` 区切りの絶対 path。解釈した item で開く |
| `\` 区切りの同じ path | 表示できる |
| 相対 path（親の directory が現在の directory） | 絶対 path に直して表示できる |
| 存在しない file（親の directory はある） | Shell を呼ばずに失敗（0x80070002） |
| Explorer を開けない（差し替えで 0x80070005） | 失敗の文と HRESULT をそのまま返す |
| path を解釈できない（差し替えで 0x80070057） | Explorer を開かずに、解釈の失敗として返す |
| 50 回くり返す | 毎回表示できる。呼び出しの後に COM の初期化が残らない |
| 呼び出し側が MTA で COM を初期化している | 表示でき、呼び出し側の mode を変えない |
| 既存の負例（空、NULL、directory、存在しない file） | 変わらず失敗する |

### controller と実際の書き出し（CTest `m7b_4_controller_export_lifecycle` に追加）

数式を使わない、画像 1 枚の Project を、製品の既定の書き出しの関数で本当に書き出し、製品の既定の
revealer を通す。書き出し先の URL は UI と同じ `QUrl::fromLocalFile` で作る（`書き出し 先 日本語/結果 ①.mp4`）。

- 成功: 状態の表示が「書き出しました: …」で、Explorer の失敗の文が付かない。Shell へ渡す path は `\` 区切り
  の絶対 path。
- Explorer を開けない（差し替えで 0x80070005）: 書き出しは成功のまま。状態に「Explorerで表示できません:
  Explorerでファイルを表示できません (HRESULT=0x80070005)」が付く。書き出しの失敗の通知は出ない。

### 実際の `mvm.exe`（製品 GUI の smoke）

`scripts/math-group-gui-smoke.ps1`（P3-6C.2 の driver を使う。人の操作なし）を、直した binary で実行した
（`build/math-group-gui-smoke/20261014-195837/`）。driver に「書き出しの後の状態に、Explorer の失敗の文が
無い」の確認を足した。

| 場面 | driver の段 | verify |
| --- | --- | --- |
| A（対象外の Project） | 25/25（Explorer の確認を含む） | 全項目成立 |
| B（数式だけの Project） | 10/10（同） | 全項目成立 |

この smoke では、書き出しが成功するたびに Explorer の window が実際に開いた（製品の動き）。

## 5. 製品 GUI の smoke（Task E）

`scripts/math-group-gui-smoke.ps1`:

- 既定は短い場面 A・B だけ（約 4 分）。`-Full` で v4 全体の場面 C も行う。全長は、最終の binary が変わった
  とき、または書き出しの経路・数式 group に関わる変更のときにだけ使う。
- 毎回、新しい出力先（`build/math-group-gui-smoke/<日時>/`）に、対象の固定（`mvm.exe`・DLL・Project・
  fixture・script）、参照の出力、session と verify の記録、`smoke.json` を作る。
- P3-6C.2 の検収の記録と判定には触れない（別の出力先）。
- 検収の道具の自己試験（`scripts/math-group-product-p36c2-selftest.ps1`）は 59 件が期待どおり。

## 6. 回帰

| 試験 | 結果 |
| --- | --- |
| `reveal_in_explorer_negative`・`m7b_4_controller_export_lifecycle` | 通過 |
| 製品 GUI の smoke（A・B） | 通過 |
| 検収の道具の自己試験 | 59/59 |
| lint | 通過 |
| **release の全 CTest** | **1524/1524 通過**（1,180 秒。`build/explorer-reveal-p37a-20261014/ctest-run-1/`）。GUI の環境は前後とも VALID。登録数は変わらない（既存の 2 試験に検査を足した） |

## 7. 残っていること（関係の無いもの）

- 数式だけの Project の書き出し設定に「入力映像: 仕様を取得できません」と出る（P3-6C.2 の 3。書き出しは
  止めない）。直していない。
- `math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま。
- 全 CTest は、負荷や利用者の操作で 1 件落ちることがある（P3-6A・P3-6B）。
- Explorer の window が開くこと自体は、製品の動きとして変えていない。製品 GUI の smoke を走らせると、
  書き出しのたびに Explorer の window が開く。
