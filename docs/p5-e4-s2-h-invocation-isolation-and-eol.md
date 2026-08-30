# P5-E4 S2-h: invocation isolation と line-ending 非依存の mutation matching

2件は同じ failure class (test が環境・履歴に依存して本来の contract を検証
できない) だが原因は独立している。closure evidence も分けて持つ。

## S2-h1: PID-only isolation は invocation identity ではない

### 症状

S2-g4 の diagnostic cohort run-2 が `p2_d5_2_w2c2_checker_integration` で停止した。

```text
既存C2 artifactを上書きしません:
  build\ucrt64-release\tests\p2-d5-2-w2c2-checker-integration\process-55804\c2-good.json
```

### 原因

isolation key が `process-$PID` だった。Windows は PID を再利用するため、過去 run
が残した同名 directory と衝突する。build directory には `process-*` が
**5940 件**蓄積しており、Aug 25 のものまで残っていた。

`process-$PID` は invocation identity ではない。

### 修正

S2-f2 で確立した pattern をそのまま適用する。

```powershell
$dir = Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
```

対象 12 file。蓄積していた stale directory も削除した。

### evidence

同一 build directory に対する連続 3 run が 11/11 PASS。生成される key は
`process-11084-d83f550c8783` のように invocation ごとに一意である。

## S2-h2: CRLF here-string と LF source は一致しない

### 症状

`b3_i6b` / `b3_i6c` の guard 10 件が `変異対象がありません` で失敗した。

```text
変異対象がありません: tests/gpu_preview/test_i6b_publication_atomicity.cpp / ...
```

source 側に該当テキストは存在する (line 142-143)。

### 原因

`.gitattributes` は次を定めている。

```text
*        text=auto eol=lf     -> .cpp / .h は LF
*.ps1    text eol=crlf        -> guard script は CRLF
```

mutation pattern は guard script 内の here-string なので **CRLF** を含む。
target source は `Get-Content -Raw` で読むので **LF** である。したがって
`[regex]::Escape($From)` は決して一致しない。

実測で確定した。

```text
cpp contains CR              False
ps1 contains CR              True
cpp.Contains(LF pattern)     True
cpp.Contains(CRLF pattern)   False
```

guard を LF へ変換すると PASS、CRLF へ戻すと FAIL することも確認した。

### これは fresh checkout で必ず落ちる defect である

これらの guard は、working tree の `.ps1` が LF のままだったときにだけ通って
いた。git が materialize すれば `.gitattributes` により CRLF になるため、
**clone 直後は必ず失敗する。** つまりこれまでの suite green は、
正規化されていない working tree に依存していた。

S2-f2 と同じ false-green class が別 layer で起きていた。

### 修正

pattern の保存形式や `.gitattributes` は変更しない。guard 内で source と
pattern の**両方**を LF domain へ射影してから match する。

```powershell
function Normalize-Lf([string]$Text) {
    if ($null -eq $Text) { return $Text }
    return $Text -replace "`r`n", "`n"
}

$sources[$path] = Normalize-Lf (Get-Content -Raw -Encoding utf8 -LiteralPath ...)
# Edit-Source 冒頭
$From = Normalize-Lf $From
$To   = Normalize-Lf $To
```

source だけを正規化しても CRLF の `$From` が残れば同じ失敗になる。両方を
canonicalize することが要点である。

`Edit-Source` を持つ guard 7 件すべてに適用した。失敗していた 3 件だけでなく、
同じ潜在依存を持つ残り 4 件 (単一行 pattern のため偶然通っていた) も含む。

### 意味づけ

```text
before:  physical line ending が偶然一致する必要があった
after:   logical source text が一致すればよい

contract semantics              unchanged
checkout representation 依存    removed
```

### evidence

guard script が `w/crlf` の状態 (= fresh checkout と同じ) で 109/109 PASS
(release / debug 両方)。mutation applicability は構造的に維持されている
(不一致なら `変異対象がありません` で FAIL する)。

## 実装上の誤りの記録

Normalize-Lf helper を最初は `Edit-Source` の直前に置いたが、source 読み込みは
それより前に実行される。PowerShell は関数定義を文として実行するため、初回呼び出し
より前に定義が必要である。`Set-StrictMode` 直後へ移動した。

## S2-h closure

```text
S2-h1 PID/GUID isolation
  affected files                 12/12
  stale collision                structurally impossible
  同一build dir 連続3 run        11/11 PASS

S2-h2 EOL-normalized matching
  affected guards                7/7  (失敗していたのは3 file / 10 test)
  CRLF guard + LF target         109/109 PASS (両preset)
  fresh-checkout 相当状態        PASS

ordinary regression             release 1348/1348 / debug 1348/1348
assertion semantics             unchanged
threshold changes               0
skip / delete                   0
```

S2-g4 acquisition は `INCONCLUSIVE_OTHER_TEST_FAILED` のまま immutable に保持し、
続きからは再開しない。新 checkpoint で新しい diagnostic cohort を取り直す。
