# P3-7C.1: 再現の cohort を繰り返さずに crash の証拠を残す

2026-10-15。P3-7C（`docs/math-equation-sequence-crash-p37c.md`）の後続。試験の native の crash を、
人の操作・machine 全体の debugger の設定・際限の無い disk の消費なしに、後から解析できるようにした。
仕組みの規則は `docs/crash-evidence.md`。

製品の code は変えていない。数式 group、encoder、timeout、画素の authority、preview の描画、書き出しには
触れていない。commit・push はしていない。

## 判定

| 項目 | 判定 |
| --- | --- |
| 診断の仕組み | **DIAGNOSTIC_INFRASTRUCTURE_PASS**（6） |
| 過去の SegFault の根本原因 | **SEGFAULT_ROOT_CAUSE_UNKNOWN**（5） |
| 製品の修正 | **PRODUCT_FIX_NOT_STARTED**（証拠が無い） |

過去の SegFault は、cohort が通ったことを理由に閉じない。

## 1. 記録の訂正（Task A）

`build/` の削除（2026-10-15 19:24〜19:48）は、利用者が容量の最適化のために意図して行ったものだった。
P3-7C の文書の 0 を訂正した。結果（過去の binary と build の中の証拠が無いこと、build し直した binary が
別の identity であること）と、P3-7C の probe が WER の回転で別の試験の dump を 1 件消したという記録は、
そのまま残した。P3-7C の判定と run の分類は変えていない。

## 2. 証拠の置き場所（Task B）

`scripts/lib/crash-archive.ps1`・`scripts/crash-archive.ps1`。既定 `%LOCALAPPDATA%\mvm\crash-archive`。

- repo の中（build/ を含む）を拒否する。repo と build/ は消してよい。
- dump は内容の SHA-256 で `objects/` に置き、元の path・名前・時刻を `index.jsonl` に追記する。同じ名前の
  別の dump を上書きしない。複製は hash を確かめてから置く。
- 容量の上限（64 GiB）と volume の空きの下限（32 GiB）を、複製の前に確かめる。超えるなら 1 件も書かない。
- 自動では何も消さない。消すのは SHA-256・理由・`-Apply` を明示したときだけで、index に記録する。
- 保全に失敗したら、crash しうる処理を起動しない（`Invoke-MvmAfterCrashArchive`）。
- [事実] 初回の保全: `CrashDumps` の 9 件（約 630 MB）。P3-7C が build の中に複製した 9 件と SHA-256 が
  すべて一致した。動画の書き出しや build の tree は入れていない。

## 3. crash を採る方式（Task C）

実際に probe を落として比べた（`docs/crash-evidence.md` の 1）。

- [事実] **pwsh から起動した ctest の下でも dump は残らない**（P3-7C で確かめたのは Git Bash から起動した
  ctest だけだった）。CTest 自身が子の WER を抑える。
- [事実] HKCU の `LocalDumps\<exe>` は無視された（dump は既定の場所に minidump で書かれた）。key は作る前の
  状態（`LocalDumps` 自体が無い）へ戻し、戻ったことを確かめた。
- [事実] HKLM は管理者でない session からは書けない（`SecurityException`）。使うには試験のたびに UAC の操作が
  要り、しかも CTest の下では WER が呼ばれないので、それだけでは足りない。
- **採用: CTest の `TEST_LAUNCHER` で `mvm_wer_launch` を挟む。** error mode を戻し、子の終了コード・
  入出力・作業 directory・環境をそのまま渡し、Job Object で子を所有し、落ちたら子の dump を run の
  staging へ複製する。registry は変えない。`main()` も変えない。
- 範囲: COMMAND が実行ファイルの target である試験すべて（1544 件中 299 件。apps/ の
  `mvm_test_text_ui_input` などを含む）。pwsh から起動する 1245 件には掛からない。
- 解析: `scripts/minidump-stack.py`。`.pdata`・`.xdata` に従う本物の unwind と、stack の値を拾うだけの
  heuristic な scan を出力で分ける。
- [事実] 制御した probe（既知の呼び出しの連鎖）の dump を、CTest の下の起動器の経路で採り、unwind が
  `probe_leaf` → `probe_alloca` → `probe_compare` → UCRT の `qsort` → `probe_big_frame` → `main` →
  CRT → kernel32 → ntdll と、thread の始まりまで正しく辿った（関数と行は addr2line）。同じ dump の scan には、
  本当の戻り先でない値（`qsort+0x462` など）が混ざる。exception の code（`0xC0000005`）と書き込み先（`0x10`）、
  module の identity（TimeDateStamp・SizeOfImage の照合）も読めた。

## 4. 通常の試験の記録（Task D）

`scripts/test.ps1` が既定で 1 回の実行を archive の `runs/` に記録する（`scripts/lib/test-run-record.ps1`）。
ctest の全出力、LastTest.log・LastTestsFailed.log、junit、試験ごとの分類、identity（git・package・GPU・
OS・実行ファイルの SHA-256）、GUI の preflight の前後、落ちた試験の dump。開始の前に既存の dump を保全し、
失敗したら試験を始めない。

全 CTest で書式を試す前に、小さな fixture（pass・fail・crash・timeout の 4 件。
`tests/fixtures/test-run-record`）で確かめた（`test_run_record_contract`）。分類は PASS・FAILED・
NATIVE_CRASH・TIMEOUT で、crash の試験は dump の SHA-256 と結び付いた。

## 5. 元の 49 件の選定と順序での実行（Task E）

### 選定と順序の復元

- [事実] 元の失敗（2026-10-12 13:58）の binary は 13:40 に link された。その source に当たる commit
  `4f83f2b`（14:11）を worktree に取り出し、configure だけ行って（build はしない）、P3-1.2 の gate の
  正規表現 `math|mlt_export|m7b_3_product_export|timeline_export` で `ctest -N` した。
  `tests/CMakeLists.txt` は直前の commit `685ba23`（12:59）から変わっていないので、13:40 の時点の試験の登録も
  同じである。
- [事実] 結果は **49 件、対象が 25 番目、直前が `math_inspector_product_ui`**。文書の記録（49 件の 25 番目。
  P3-3A の R2 が直前の試験として組にしたもの）と一致する。復元は信頼できると判断した。
- [事実] 今の tree でも 49 件すべてが登録され、相対順序も同じ。今の正規表現は 5 件増えて 54 件に当たるので、
  名前の一覧（`scripts/math-equation-sequence-crash-p37c1-sequence.ps1` に固定）で選んだ。
- [不一致] 当時の登録の総数は 1485 件で、対象は #1441 だった。P3-3A の cohort の script は対象を `#1485` と
  書いており、当時の登録と食い違う（総数と同じ値。古い script の誤記と考えるが、確かめられない）。
- [不一致] binary・runtime・環境の identity は元と同じではない。今回の source は HEAD `e8fb392`（利用者が
  P3-7C の成果を commit したもの）に P3-7C.1 の未 commit の変更（起動器・記録の仕組み。製品の code は無い）を
  加えたもの（`git diff HEAD` の SHA-256 `CEC047C1…AA133016F`）、runtime は qt6-base 6.11.1-1・mlt 7.36.1-1・
  ffmpeg 8.1.2-2。元の binary は無い。**これは同じ選定と順序であって、元の実行の正確な再現ではない。**

### 実行

`pwsh scripts/math-equation-sequence-crash-p37c1-sequence.ps1`。回数は事前に固定して 2 回（P3-3A が dump を
採れない形で 3 回流しているので、新しいのは dump を採れることだけ。増やさない）。crash が起きたらその時点で
止める。`ctest -j 1 --tests-from-file`。native の試験は TEST_LAUNCHER の起動器を通る。

記録: archive の `runs/20261015-221312-p37c1-sequence49/`（identity、binary の SHA-256、preflight の前後、
ctest の全出力、junit、試験ごとの分類、実行の順序、2 秒ごとの process と GPU、終了後に残った process）。

| 回 | 結果 | 対象 | native の crash | 実行の順序 | 同時に動いた試験の process | 残った process | preflight |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 49/49 通過 | PASS（171 秒） | 0 | 元と同じ | 0 回（172 標本） | 0 | 前後とも VALID |
| 2 | 49/49 通過 | PASS | 0 | 元と同じ | 0 回（162 標本） | 0 | 前後とも VALID |

- GPU の使用率は最大 18%（1 回目）・15%（2 回目）。WerFault の process は標本に一度も現れなかった。
- [注意] この run の `rep1.LastTestsFailed.log`・`rep2.LastTestsFailed.log` は、この実行のものではない。
  ctest は失敗が無いとこの file を書かないので、前の試験（`test_run_record_contract` の試作の失敗）の file が
  写された。archive は上書きしないので、そのまま残してある。記録の lib は、group の開始より前の file を
  写さず `stale_skipped` に記録するよう直した（契約試験に追加）。

**結論: 元の選定と順序でも再現しなかった。crash が起きなかったので、根本原因の解析には進んでいない。**
元の失敗は 1 回きりで、この後に同じ 49 件を合計 5 回（P3-3A の 3 回と今回の 2 回）流して 0 回である。

## 6. 試験（Task F）

### 対象を絞った試験（先に実行）

| 試験 | 結果 | 確かめたこと（要件との対応） |
| --- | --- | --- |
| `crash_archive_contract` | 通過 | archive の失敗（場所が file・repo の中）、容量の上限・空き不足（1 件も書かない）、同じ名前の別の dump、再実行、開けない dump、保全の失敗で probe を起動しない、上書きの拒否、明示の削除 |
| `crash_capture_contract` | 通過（CTest の下） | crash の dump、abort の dump、正常終了と終了コード、起動器の失敗（`0xE0C0FF02`）、起動器の停止（CTest の timeout）で子と孫が止まる、cohort mode の timeout と window、unwind、`CrashDumps` が元どおり |
| `test_run_record_contract` | 通過 | fixture の pass・fail・crash・timeout の記録、前の実行の file を写さない、`MVM_CRASH_STAGING` の復元、archive の失敗で始めない |
| lint | 通過 | clang-format（C の 2 file を含む）、PSScriptAnalyzer |

設定の復元: 採用した方式は registry を変えない。調査で作った HKCU の `LocalDumps` の key は削除し、作る前の
状態（`LocalDumps` が無い）に戻ったことを確かめた。`CrashDumps` は、試験のあとも 9 件で元どおりである。

### 全 CTest（release）

起動器が 299 件の試験の起動の仕方を変え、`scripts/test.ps1` も変えたので、全 CTest を 1 回実行した。

`pwsh scripts/test.ps1 -Preset ucrt64-release`: **通常 1530/1530 通過**（P3-7B.2 の 1527 件に、今回の契約試験
3 件を加えた数）。preflight は前後とも VALID。crash 0 件、新しい dump 0 件。記録は archive の
`runs/20261015-222913-test-ucrt64-release-All/`（ctest の全出力・LastTest.log・junit・試験ごとの分類
〈PASS 1530〉・identity・binary の SHA-256・preflight。1 回で約 5 MB）。

### 判定

**DIAGNOSTIC_INFRASTRUCTURE_PASS。** 制御した probe で、CTest の下の dump の採取から、archive への保全、
試験との結び付け、本物の unwind まで、通しで確かめた。ただし、実際の GUI 試験の crash で通したことはまだ
無い（起きていないため）。

## 7. 試作の途中で起きたこと

- [事実] `crash_capture_contract` の最初の版は、probe の dump 3 件を最後にまとめて `CrashDumps` から出していた。
  その間に件数が 10 件を超え、WER が利用者の dump 2 件（`mvm_test_controller_export.exe.23240.dmp`・
  `.24808.dmp`、2026-10-02）を消した。2 件は試験の前に archive へ保全してあった（SHA-256 一致）ので、
  archive から元の名前で書き戻し、hash を照合した。試験は「`CrashDumps` が元どおりでない」として失敗し、
  この変化を検出していた。直した版は、probe の dump を落ちるたびに出し、件数を「試験の前 + 1」以下に保つ。
- 起動器の message を wide 文字で書いていたため、console・pipe の code page で日本語が化けた。他の試験と
  同じく UTF-8 で書くように直した。

## 8. 残る危険と次の手順

- pwsh から起動する試験（1245 件）の中で落ちた実行ファイルは、dump を残さない。
- WER の既定の `CrashDumps` は 10 件で回転する。起動器を通った試験の dump は、落ちた直後に staging へ複製
  されるので回転では失われない。CTest の外で同じ時間に落ちた process（利用者が使っている別の app など）の
  dump は、1 回の CTest の間に 10 件を超えると、終了時の保全の前に消えうる。開始の前にあった dump は archive に
  ある。（pwsh の試験の中の実行ファイルは、CTest の error mode を受け継ぐので、そもそも dump を残さない。）
- dump は既定の minidump で、heap の内容は無い。object の中身が要る crash では、HKLM の exe 別の full dump を
  管理者が一度だけ設定する（`docs/crash-evidence.md` の 2）。
- build を消すと、その build の dump は試験の実行ファイルの frame で unwind が止まる（image の identity が
  一致しない）。解析は build を消す前に行う。
