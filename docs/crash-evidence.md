# crash の証拠を残す仕組み

試験の native の crash を、再現の cohort を繰り返さなくても後から解析できるようにする。
P3-7C.1（2026-10-15）で作った。経緯は `docs/math-equation-sequence-crash-p37c.md` と
`docs/math-equation-sequence-crash-p37c1.md`。

## 1. 実測した前提（2026-10-15）

いずれも、この開発機で crash する probe を実際に動かして確かめた。

| 起動の仕方・設定 | WER の dump | Application の event 1000 |
| --- | --- | --- |
| pwsh から直接 | 残る（`%LOCALAPPDATA%\CrashDumps`、minidump） | 残る |
| Git Bash（MSYS）から | 残らない | 残らない |
| ctest の下（Git Bash から起動） | 残らない | 残らない |
| ctest の下（pwsh から起動） | 残らない | 残らない |
| HKCU の `LocalDumps\<exe>`（`DumpFolder`・`DumpType=2`） | 設定は無視され、既定の場所に minidump | 残る |
| HKLM の `LocalDumps\<exe>` | 書き込めない（管理者でない session。`SecurityException`） | — |

- CTest と MSYS は、子 process の error mode に `SEM_NOGPFAULTERRORBOX` を渡す。WER が呼ばれないので、
  設定に関わらず dump も event log も残らない。**過去の CTest の SegFault に dump が無いのはこのため**で、
  記録が失われたのではない。
- この機械の WER の `LocalDumps` は、値の無い root key だけがある（既定の設定）。dump は
  `%LOCALAPPDATA%\CrashDumps` に minidump で書かれ、**10 件を超えると最古の dump を WER が消す**。
  P3-7C で 1 件（`mvm_test_transition_preview.exe.69960.dmp`）を、P3-7C.1 の試験の試作で 2 件
  （archive に保全済みのものを、archive から書き戻した）をこの回転で失った。
- CTest の `***Exception: SegFault` は `0xC0000005`・`0xC0000006`・`0xC0000008`・`0xC00000FD` の
  どれか。heap の破損（`0xC0000374`）や abort（`0xC0000409`）は `Exit code 0x…` と表示される。

## 2. 採用した方式: 試験の起動器（TEST_LAUNCHER）

`cmake/mvm_test_launcher.cmake` が `CMAKE_TEST_LAUNCHER` を `mvm_wer_launch --test` にする。CTest は
試験の実行ファイルを起動器を通して起動する。起動器（`tests/harness/wer_launch.c`）は:

- error mode を戻して（`SEM_FAILCRITICALERRORS` だけ残す）、試験を起動する。debugger は付けない
  （heap も時間も変えない）。
- 標準入出力・作業 directory・環境はそのまま受け継ぎ、**子の終了コードをそのまま返す**
  （CTest の `SegFault` などの表示は変わらない）。
- 子を Job Object（`KILL_ON_JOB_CLOSE`）で所有する。CTest の TIMEOUT で起動器が止められると、子と孫も止まる。
- 子が例外で終わったら、WER が書いた `CrashDumps\<exe>.<pid>.dmp` を、`MVM_CRASH_STAGING` があれば
  そこへ複製する（10 件の回転で消える前に）。結果を標準エラーに 1 行書く
  （`wer_launch: 例外 0xC0000005 で終了。dump: … (複製: …)`）。dump が無い・複製できないときも、その旨を書く。
- 起動器自身の失敗（起動できない、など）は `0xE0C0FF02` で終わる。

範囲:

- 対象は、COMMAND が実行ファイルの target である試験すべて（2026-10-15 で 1544 件中 299 件。
  `math_equation_sequence_product_ui` を含む）。apps/ の試験用の実行ファイルにも掛かるよう、top-level で
  `add_subdirectory(src)` の前に読む。
- **pwsh から起動する試験（1245 件）には掛からない。** その script が起動した実行ファイルが落ちても、
  CTest の error mode を受け継ぐので dump は残らない。必要になったら、その script が起動器を通して起動する。

registry・machine 全体の設定（AeDebug など）は変えない。したがって元に戻す手順は無い。

### 採らなかった方式

- HKLM の `LocalDumps\<exe>`（exe ごとの専用 folder・件数・full dump）: 管理者の権限が要り、試験のたびに
  UAC の操作が要る。さらに CTest の下では WER が呼ばれないので、これだけでは dump が残らない。
  使うなら、管理者の PowerShell で一度だけ次を行い、不要になったら key を消す（この repo の試験は前提にしない）:

  ```powershell
  $k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\mvm_test_text_ui_input.exe'
  New-Item $k -Force | Out-Null
  New-ItemProperty $k -Name DumpFolder -PropertyType ExpandString -Value 'D:\mvm-dumps' | Out-Null
  New-ItemProperty $k -Name DumpType -PropertyType DWord -Value 2 | Out-Null
  New-ItemProperty $k -Name DumpCount -PropertyType DWord -Value 50 | Out-Null
  # 元に戻す: Remove-Item $k
  ```
- HKCU の `LocalDumps`: 無視された（1 の表）。
- 試験の `main()` で `SetErrorMode(0)`: 試験ごとに code を変える必要があり、起動器で足りる。
- debugger の下で起動（procdump `-x`・gdb）: debug heap に替わり、heap の配置と時間が変わる。

## 3. archive（build/ の外の正本）

`scripts/lib/crash-archive.ps1`。既定は `%LOCALAPPDATA%\mvm\crash-archive`（環境変数 `MVM_CRASH_ARCHIVE`
で変更）。repo の中（`build/` を含む）は拒否する。repo も `build/` も消してよく、archive は残る。

```
crash-archive/
  config.json     max_bytes (既定 64 GiB)、min_free_bytes (既定 32 GiB)
  index.jsonl     追記だけの記録 (dump の元の path・名前・時刻・SHA-256、run、削除)
  objects/        <SHA-256>.dmp。内容で名前を決めるので、同じ名前の別の dump も上書きしない
  runs/           <日時>-<名前>/ (試験の実行 1 回分の記録。4)
  incoming/       複製の途中 (hash を確かめてから objects/ へ移す)
```

- **保全**: `Save-MvmCrashDumps` は、指定した directory の `*.dmp` の SHA-256 を取り、まだ無い内容だけを
  複製し、複製の hash が元と一致したら `objects/` に置き、`index.jsonl` に元の path と一緒に記録する。
  同じ内容が既にあれば index だけ足す。同じ元の path と内容の組は二度記録しない。
- **容量**: 複製の前に、追加する byte 数で `max_bytes` と volume の空きの `min_free_bytes` を確かめる。
  超えるなら 1 件も書かずに失敗する。`pwsh scripts/crash-archive.ps1` で使用量を出す。
  CI（GitHub hosted runner）は空きが既定の下限より小さいので、`.github/workflows/ci.yml` が
  `MVM_CRASH_ARCHIVE` を `$RUNNER_TEMP\crash-archive` に向け、`config.json` を runner 向けの値
  （`max_bytes` 8 GiB、`min_free_bytes` 4 GiB）で先に置く。既定値は開発機向けのまま変えない。
- **失敗したら probe を起動しない**: `Invoke-MvmAfterCrashArchive` は、保全に成功したときだけ処理を実行する。
  WER が書いている途中の dump（開けない）も保全の失敗にする。cohort・試験の記録・契約試験は、crash
  させる前にこれを通る。
- **書き手は 1 つ**: `.lock` を排他で開く。lock は操作ごとに取り、長い試験の間は持たない。
- **削除の方針**: 自動では何も消さない。消すのは、解析が済んで不要になった dump を、人が SHA-256 と理由を
  明示したときだけ。

  ```powershell
  pwsh scripts/crash-archive.ps1 -Remove -Sha256 <64 桁> -Reason '<理由>'          # 対象を表示するだけ
  pwsh scripts/crash-archive.ps1 -Remove -Sha256 <64 桁> -Reason '<理由>' -Apply   # 消して index に記録
  ```

  上限に近づいたら（`pwsh scripts/crash-archive.ps1` の報告）、`config.json` を上げるか、上の手順で消す。run の記録（runs/）は小さい
  （log・JSON）ので、消す手順は用意していない。
- 動画の書き出しの成果物や build の tree は archive に入れない。入れるのは dump と試験の記録だけである。

## 4. 通常の CTest の記録

`pwsh scripts/test.ps1`（`dev.ps1 test` も同じ）は、既定で 1 回の実行を archive の `runs/` に記録する
（`scripts/lib/test-run-record.ps1`）。`-NoCrashRecord` で止められる（その旨を表示する）。

1. 開始の前に `CrashDumps` の既存の dump を保全する。**失敗したら試験を始めない。**
2. `MVM_CRASH_STAGING` を run の `staging/` にする（起動器が落ちた試験の dump をここへ複製する）。
3. `identity.json`: git の HEAD・status・`git diff HEAD` の SHA-256、UCRT64 の package の版、GPU と driver、OS。
   `binaries-<preset>.json`: build の `bin/` の exe・dll の大きさと SHA-256。
4. GUI の試験を含む実行では、preset ごとに前後で `gui-environment-preflight.ps1` を実行して記録する
   （記録だけで、試験を止めない）。
5. CTest の group ごとに: ctest の全出力（`<label>.ctest.log`）、`LastTest.log`・`LastTestsFailed.log`、
   junit（`<label>.junit.xml`。試験ごとの出力を含む）、試験ごとの名前と分類（`<label>.tests.json`。
   PASS・FAILED・NATIVE_CRASH・TIMEOUT・NOT_RUN・UNKNOWN）。
6. 終わったら（失敗しても）、staging と `CrashDumps` の dump を保全し、`summary.json` に crash した試験と
   その dump の SHA-256、preflight、group の結果、archive の使用量を書く。

## 5. dump の解析

```powershell
C:\msys64\ucrt64\bin\python.exe -I scripts/minidump-stack.py <dump> --addr2line C:\msys64\ucrt64\bin\addr2line.exe [--json out.json]
```

この機械には minidump を読む debugger（cdb・WinDbg・lldb）が無いので、標準 library だけで書いた。

- **unwind**: PE の `.pdata`・`.xdata`（RUNTIME_FUNCTION・UNWIND_INFO）に従ってレジスタを 1 frame ずつ
  戻す。disk の image の TimeDateStamp と SizeOfImage が dump の記録と一致する module だけを使い、一致しない・
  image が無い module に入ったらそこで止める（**build を消した後の dump は、試験の実行ファイルの frame で
  止まる**。解析は build を消す前に行うか、同じ build を残す）。
- **scan**: stack の値のうち module の範囲を指すものを並べただけ（heuristic）。本当の戻り先でない値を
  含む。unwind と出力を分ける。
- 関数名: Windows の DLL 以外は addr2line（release は RelWithDebInfo なので関数と行が付く）。名前が出ない
  ものは export 表の直前の名前 + offset で、推定と明記する。
- 既知の制限: 例外の frame が epilog の途中だと unwind を誤ることがある。minidump は既定の型なので heap の
  内容は無い（object の中身は見えない）。

## 6. 契約試験

| 試験 | 確かめること |
| --- | --- |
| `crash_archive_contract` | 置き場所の拒否、同じ名前の別の dump、再実行の冪等、容量・空き不足で 1 件も書かない、開けない dump、保全の失敗で probe を起動しない、上書きの拒否、明示の削除と記録、lock |
| `crash_capture_contract`（workstation） | CTest の下で起動器が dump を残す、終了コードの受け渡し、入出力・作業 directory・環境、起動の失敗、abort、staging が無いとき、起動器の停止で子と孫が止まる、cohort mode の timeout と window、unwind が既知の呼び出しの連鎖を出す（`probe_leaf` → `probe_alloca` → `probe_compare` → UCRT の `qsort` → `probe_big_frame` → `main`）、試験の前の dump を失わない |
| `test_run_record_contract`（workstation） | fixture（pass・fail・crash・timeout）の記録: 全出力・LastTest.log・分類・dump の SHA-256 との結び付け・identity・preflight・上書きの拒否・`MVM_CRASH_STAGING` の復元、archive の失敗で始めない |

`crash_capture_contract` と `test_run_record_contract` は probe を実際に落とす。先に実際の archive へ
既存の dump を保全し、probe の dump は落ちるたびに `CrashDumps` から出す（件数を増やさず、回転を起こさない）。
