# P3-7C: `math_equation_sequence_product_ui` の過去の SegFault の調査

2026-10-15。P3-1.2 の gate（2026-10-12）で 1 回だけ起きた SegFault を、dump を採れる形で再現させる仕組みを
作り、事前に決めた回数だけ実行した。

製品の code は変えていない。数式 group の画素の計算、alpha の条件、MLT の affine、V1 の背景の保護、
書き出しの timeout、出力の authority には触れていない。数式 group は既定で off のまま。commit・push は
していない。過去の失敗・無効の run の分類は変えていない。

**判定: ROOT_CAUSE_UNKNOWN。** 129 回の有効な run（起動直後の区間だけの 119 回を含む）で一度も落ちなかった。
落ちなかったことは、直ったことの証拠ではない。

証拠は `build/math-equation-sequence-crash-p37c-20261015/`。再現は
`pwsh scripts/math-equation-sequence-crash-p37c.ps1`。

**P3-7C.1 での訂正と追記**: `build/` の削除は利用者が意図して行った容量の整理だった（0）。dump の正本は
`build/` の外の archive に移した（`docs/crash-evidence.md`）。49 件の選定と順序での実行は
`docs/math-equation-sequence-crash-p37c1.md`。この文書の判定（ROOT_CAUSE_UNKNOWN）と各 run の分類は変えていない。

## 0. 調査の前に起きたこと（build/ の削除と証拠の消失）

- [事実] 2026-10-15 19:24〜19:48 に、`build/` の大部分が削除された。**利用者が容量の最適化（100 GB を
  超えていた）のために意図して行ったもので、事故ではない**（P3-7C.1 で利用者が訂正。調査の時点では
  原因を知らず、「誰が消したか分かっていない」と書いていた）。この調査の開始（20:1x）より前である。
  結果として失われたもの（訂正しても変わらない）:
  - `build/ucrt64-release` の binary・`CMakeCache.txt`・`Testing/`（P3-7B.2 で 1527/1527 だった binary）
  - `build/ucrt64-debug` 全体
  - P3-3A〜P3-7B.2 の文書が参照する証拠の directory（`math-transition-p33a-20261012/task-a/` を含む）
  - `build/p351-ui` の過去の run（3 つだけ残っている）
  - `build/tools`（固定版の clang-format。`scripts/install-clang-format.ps1` で入れ直した）
- [事実] 利用者の了承を得て、HEAD `c09fb2b` から release を build し直した。したがって、今回の binary は
  過去に落ちた binary とも、P3-7B.2 の binary とも別の identity である。
- [事実] この調査の WER の probe（3 で述べる）が `%LOCALAPPDATA%\CrashDumps` に dump を書いたため、WER の
  既定の上限（10 件）を超え、最古の `mvm_test_transition_preview.exe.69960.dmp`（2026-10-01 09:09、
  83,961,421 byte）が WER によって消された。**この作業が起こした消失で、取り戻せない。** この調査の対象とは
  別の試験の dump である。残りの 9 件は SHA-256 付きで
  `build/math-equation-sequence-crash-p37c-20261015/historical-crashdumps/` に複製した（元は残してある）。
  cohort の script は、実行の前に未保全の dump を同じ場所へ複製する。
  （P3-7C.1: build/ も消しうるので、正本は build/ の外の archive に移した。同じ 9 件の SHA-256 が一致する。
  cohort の script も archive へ保全するよう変えた。`docs/crash-evidence.md`）

## 1. 過去の SegFault の証拠と、その信頼性（Task A）

元の証拠の directory が消えたので、以下は文書（`docs/math-transition-p33a.md` の 1、
`docs/math-compositor-p312.md` の 7）に書き写された内容だけに基づく。

### 確かめられている事実（文書による）

| 項目 | 内容 |
| --- | --- |
| 日時 | 2026-10-12 13:58 ごろ |
| CTest の呼び出し | P3-1.2 の gate。正規表現 `math\|mlt_export\|m7b_3_product_export\|timeline_export` に合う 49 件を順に実行した 25 番目（直前は `math_inspector_product_ui`） |
| 試験の command | `mvm_test_text_ui_input.exe --equation-sequence-ui build/p351-ui --scratch` |
| binary | `mvm_test_text_ui_input.exe`、SHA-256 `13660872…5c4be7`（文書には先頭と末尾だけ）、link 13:40。合成器の symbol を含まない |
| 結果 | `***Exception: SegFault 0.72 sec`（ctest の 1 行だけ） |
| 出力 | 画面への絞り込みで捨てた。`LastTest.log` は直後の再実行で上書きされた |
| dump・event log | `%LOCALAPPDATA%\CrashDumps` にも Application の event log にも、この時刻の記録は無い |
| 並行の負荷 | 同じ機械で、disk 全体の検索（`find /`、13:10〜14:47 ごろ）が動いていた |
| GUI の環境 | 記録が無い（`gui-environment-preflight.ps1` は P3-5A.2 から。当時は無い） |

### 今回、実験で確かめたこと

- [事実] **CTest の `SegFault` は access violation に限らない。** exit code を変えて終わる probe を CTest で
  動かした（`scratchpad/ctest-label`）。`SegFault` と表示されたのは `0xC0000005`（access violation）、
  `0xC0000006`（in-page error）、`0xC0000008`（invalid handle）、`0xC00000FD`（stack overflow）。
  `0xC0000374`（heap corruption）、`0xC0000409`（fast fail / abort）、`0xC0000602` は
  `Exit code 0x…***Exception:` と表示され、`SegFault` にならない。したがって元の失敗は、この 4 つのどれかで
  あり、heap の破損の検出や abort ではない。どれだったかは分からない。
- [事実] **CTest の下で落ちた試験は、WER の dump も event log も残さない。** 同じ crash する probe を
  起動の仕方だけ変えて比べた。PowerShell から起動すると dump と Application の event 1000・1001 が残り、
  CTest と Git Bash（MSYS）から起動すると、どちらも残らなかった。CTest と MSYS は
  `SEM_NOGPFAULTERRORBOX` を子へ受け継がせる。元の失敗に dump と event log が無いことは、これで説明できる
  （記録が失われたのではなく、もともと作られない）。
- [事実] **落ちた時点は、試験の最初の window を作る区間である。** 今回の binary で、証拠の directory の
  file の時刻を測った。`recovery-guard.mvm` が 0.14〜0.18 秒、`quadratic.mvm` が 0.16 秒、最初の画像
  `01-fresh-one-state.png` が 5.9〜6.8 秒。0.72 秒は `quadraticWorkflow` の `open()`（MvmController・
  preview・QML の window の作成）の中にあたる。[推測] 過去の binary でも各段の時間は大きく違わないと
  考えているが、確かめていない。

### 2026-10-10 の dump（`mvm_test_text_ui_input.exe.36884.dmp`）

P3-3A で「同じ原因かどうかは調べていない」とした dump を読んだ（`minidump` の Python package で解析）。

- [事実] **debug の build**（`build/ucrt64-debug/bin/`）で、例外は `0xC0000409`、引数 7
  （`FAST_FAIL_FATAL_APP_EXIT`、つまり `abort()`）。address は `ucrtbase.dll+0xa527e`。stack の上部に
  `libstdc++-6.dll` がある。access violation ではない。
- [事実] 起動から約 13 秒（12:45:38 に起動、dump は 12:45:51）。
- [事実] dump の memory に `--equation-sequence-ui`・`--scratch`・`p351-ui` は一度も現れない。どの mode で
  動いていたかは分からない。
- 結論: **元の SegFault とは別の失敗である**（例外の種類が違い、CTest の `SegFault` にならない）。debug の
  binary が消えたので、frame の関数名は分からない。

### 信頼性

- 元の失敗は ctest の 1 行（文書への転記）しか残っていない。stack・address・module・stdout はもともと
  採られていない。
- 過去の再現の試み: P3-3A で 16 回（49 件 × 3、組 × 5、procdump の下 × 5、直後の単独 × 3）、P3-5A で gdb の下
  1 回。すべて通過。

## 2. dump を採る準備（Task B）

- 起動器 `scripts/lib/wer-launch.c`（script が run ごとに UCRT64 の gcc で build する）。error mode を 0 に
  してから、CTest に登録された command を **そのまま**（同じ引数・同じ証拠の親 directory）起動する。
  標準出力・標準エラーは file へ書き、process の木は Job Object（`KILL_ON_JOB_CLOSE`）で所有する。
- 環境は `ctest --show-only=json-v1` から取り、`ENVIRONMENT` と `ENVIRONMENT_MODIFICATION`（`set:` だけ
  を解釈し、それ以外は fail-closed）を起動の間だけ当てる。
- dump は、この機械に既にある WER の LocalDumps の既定（`%LOCALAPPDATA%\CrashDumps`、minidump）を使う。
  **registry は変えていない**（変えるには管理者の承認が要り、操作を求めることになる）。machine 全体の
  debugger の設定（AeDebug）にも触れていない。
- debugger は付けない。debugger の下で起動すると debug heap に替わり、heap の配置と時間が変わる
  （P3-5A で、gdb の下の起動が解放後の参照を表に出した記録がある。`docs/math-export-p35a.md`）。
- dump が採れたら run の directory へ複製し、SHA-256 と Application の event 1000 を残す。
- 解析: release は RelWithDebInfo なので、`addr2line`（image base `0x140000000` に RVA を足す）で試験の
  binary の frame に関数名と行を付けられる。対応の計算は probe で確かめた。Qt・MLT・FFmpeg の DLL は
  symbol が無く、`module+offset` までである。stack は正確な unwind ではなく、stack 上の値の走査（heuristic）
  になる。**今回は dump が 1 件も無いので、stack は採れていない。**
- 検証: probe（null への書き込み）で、`wer` mode は dump を残し（`0xC0000005`、address と書き込み先 `0x10`
  が一致）、`ctest` mode（CTest と同じ error mode）は残さないことを確かめた。window と timeout の打ち切りも
  確かめた。

## 3. 再現の手順（Task C）

```powershell
pwsh scripts/math-equation-sequence-crash-p37c.ps1            # 全段
pwsh scripts/math-equation-sequence-crash-p37c.ps1 -Trial 1   # harness の試走 (-trial へ書き、数えない)
```

回数は実行の前に固定した（結果を見て増減していない）。build はしない。

| 段 | 内容 | 回数 |
| --- | --- | --- |
| Control | 本物の `ctest -R ^math_equation_sequence_product_ui$`（dump は採れない） | 1 |
| QuietFull | 起動器で最後まで（timeout 300 秒、CTest と同じ） | 5 |
| QuietWindow | 起動器で最初の 10 秒だけ。生き延びたら job ごと止める（合格ではない） | 60 |
| StressWindow | QuietWindow と同じ。disk 全体の metadata の走査（元の失敗の時の負荷）の下で | 60 |
| StressFull | QuietFull と同じ。同じ負荷の下で | 3 |

- 段ごとに前後で `gui-environment-preflight.ps1` を実行し、段の間は display-power lease を持つ。
- GPU を使う試験を並行に動かしていない。負荷は disk の走査だけで、変数は 1 つにした。
- 試験の binary と主要な DLL（Qt・MLT・FFmpeg・libstdc++ など 14 個）の SHA-256 を前後で比べた。
- 見えない desktop は使っていない。この試験は実際に window を描画して画像を比べ、見えない desktop では
  同じ描画の環境にならない（`docs/gui-smoke-hidden-desktop-p37b1.md`）。試験の window は従来どおり
  背面・入力透過で、利用者の操作を止めない。

### identity（`20261015-203938/identity.json`）

| 項目 | 値 |
| --- | --- |
| source | HEAD `c09fb2b43b2d…`。未追跡は今回の script 2 つだけ |
| 試験の binary | `mvm_test_text_ui_input.exe` SHA-256 `081FE54F…5C6347D2D`（2026-10-15 に build） |
| Qt / MLT / FFmpeg / gcc | qt6-base・qt6-declarative 6.11.1-1、mlt 7.36.1-1、ffmpeg 8.1.2-2、gcc 16.1.0-6 |
| GPU | NVIDIA GeForce RTX 4090、driver 32.0.16.1088 |
| OS | Windows 11 Home 10.0.26300 |
| CTest | 登録 1544 件、対象は #1496 |
| 起動器の source | SHA-256 `16A33B60…1969EEAAA`（実行したものを `tools/wer-launch.as-run.c` に残した。その後 clang-format で整形） |

## 4. 回数と分類（Task D）

`20261015-203938/summary.json`。全段で前後の環境は VALID、binary の identity は前後で一致。

| 段 | 回数 | 分類 |
| --- | --- | --- |
| Control | 1 | PASS（218.7 秒） |
| QuietFull | 5 | PASS 5 |
| QuietWindow | 60 | SURVIVED_WINDOW 59、EXTERNAL_INTERFERENCE 1 |
| StressWindow | 60 | SURVIVED_WINDOW 60 |
| StressFull | 3 | PASS 3 |

- **native の crash: 0。timeout・hang: 0。assertion の失敗: 0。dump: 0。**
- 129 回すべてが 0.72 秒の時点を通過した（QuietWindow の 15 番も、6.2 秒で試験が自分で「利用者の操作と
  干渉した」と判定して終わるまで動いていた）。window の
  run 119 回のうち 118 回は最初の画像まで進み、1 回（負荷の下）は `quadratic.mvm` の後、画像の前で 10 秒に
  達した。
- QuietWindow の 15 番（exit 4、7 検査中 0 件失敗）は crash ではなく、合格とも数えない。
- 利用者は実行中も PC を使っていた（GetLastInputInfo で、window の run の 65 回、full の run の大部分）。
  試験の自己検査が干渉を検出したのは上の 1 回だけである。
- 数えていない試走（`build/math-equation-sequence-crash-p37c-20261015-trial/`）:
  - `20261015-202702`: script の不具合（試験の環境の PATH で script の `pwsh` が見えなくなった）で途中終了。
  - `20261015-202740`: **HARNESS_PROTOCOL_INVALID。** 証拠の親を長い path に替えたため、数式の作業
    directory を作れず 166 検査中 11 件失敗した（`数式の作業 directory を作成できません`）。製品の問題として
    扱わない。これを受けて、CTest と同じ親 directory をそのまま使う形に直した。
  - `20261015-203153`: Control・QuietWindow・QuietFull 各 1 回、すべて通過（直した後の確認）。

過去の分を合わせると、元の失敗の後に 146 回（P3-3A 16、P3-5A 1、今回 129）実行し、一度も再現していない。
ただし、今回の 129 回は元と別の binary である。

## 5. native の stack（Task E）

**採れていない。** crash が起きなかったので dump が無い。元の失敗は dump を作らない経路（CTest）で起きた。
2026-10-10 の dump は別の失敗（debug build の abort）で、symbol の元の binary も消えている。

code を読んで 1 つの仮説を外した: `recoveryFixtureGuard` では `FakeMathBackend` が controller より先に
破棄されるが、`preflight()` が返す関数は `shared_ptr` を値で持つだけで、backend を参照しない。この順序が
解放後の参照を生むことはない。それ以上の原因の推測は、crash の証拠が無いので行わない。

## 6. 判定

**ROOT_CAUSE_UNKNOWN。** 確かめられたのは、元の失敗が CTest の `SegFault` の 4 種のどれかで、起動から
0.72 秒（今回の binary では最初の window を作る区間）に起きた、ということだけである。preview の音声の
解放後の参照（P3-5A.1 で修正）、見えない desktop、画面の消灯、書き出しの status の所有のどれとも、
結び付ける証拠は無い。

## 7. 修正の提案

**製品の修正は提案しない**（証拠が無い）。観測を増やす提案だけをする（いずれも行っていない）:

1. CTest の下でも dump が残るようにする。案 A: GUI の試験の binary の `main` の最初で `SetErrorMode(0)` を
   呼ぶ（試験の code だけの変更。CTest が渡した error mode を戻す）。案 B: 管理者の承認を得て、
   `LocalDumps\mvm_test_text_ui_input.exe` に `DumpType=2`（full）と専用の `DumpFolder` を設定する
   （試験の binary に限った設定で、10 件の回転で他の dump を消さない）。案 A は全 CTest の通常の実行で
   次の発生を捕まえられる。
2. 全 CTest の結果を残す script で、`ctest --output-on-failure` の log を run ごとに残す（元の失敗は出力を
   捨てていた）。

## 8. 残る危険と次の手順

- 再現の確率が低い（146 回で 0）。数百回の追加でも捕まらない可能性が高い。上の 1 の案で、通常の CTest の
  実行そのものを観測にするほうが効率がよい。
- 今回の負荷は disk の走査だけである。元の失敗は 49 件を続けて実行した中で起きた。その順序（前の試験の
  GPU の後片付けと重なる、など）は今回の cohort で試していない。次に試すなら、49 件の cohort を起動器の形で
  実行する（CTest の順序と同じにし、各試験を `wer` mode で起動する）。
- 冷えた起動（file cache が空）は、管理者の権限なしに作れないので試していない。
- 起動器は stdout を pipe ではなく file に書く。CTest との違いはこの点と error mode だけである。
- 証拠の directory は、利用者が容量のために `build/` を削除したことで失われた（0。P3-7C.1 で訂正）。
  過去の文書の「証拠: `build/…`」は、多くが今は辿れない。`build/` は今後も消しうるので、crash の証拠は
  `build/` の外の archive に置く（`docs/crash-evidence.md`）。
