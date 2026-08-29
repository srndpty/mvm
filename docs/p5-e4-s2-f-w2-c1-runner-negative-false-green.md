# P5-E4 S2-f: W2-C1 runner negative の false green 帰属

## 1. 結論

`p2_d5_2_w2c1_runner_negativepresentedbeforepredecessornotdropped` と
`p2_d5_2_w2c1_runner_negativepresentedaftersuccessornotdropped` の2件は
2026-08-25 以降 vacuous PASS だった。release preset の
`1332/1332 PASS` はこの2件を含むため closure authority に使えない。

真の ordinary 件数は次である。

```text
ucrt64-release   1330/1332
ucrt64-debug     1329/1332
```

## 2. 二重の空振り経路

false green は単一の欠陥ではなく2段で成立していた。

```text
[1] fixture schema drift
    → runner が parameter binding error で異常終了
    → negative の「非0 exit = 違反検出」が vacuous に成立

[2] stale mapping.json (2026-08-25 20:00) が残存
    → artifact existence check を突破
    → 全 assertion が古い artifact に対して評価される
```

`[1]` だけなら artifact 不在で FAIL していた。`[2]` だけなら runner が
正しい artifact を生成していた。両方が揃った時のみ緑になる。

debug preset は build directory が新規だったため `[2]` を継承できず、
結果として debug だけがこの欠陥を露出させた。release が緑で debug が赤
という非対称は、build config の差ではなく **artifact 継承の有無**である。

## 3. 時系列帰属

```text
2026-08-24  84a98c2  test 作成。expected blocker = PHYSICAL_MAPPING_MISSING
2026-08-25  47b5dac  formal_transport_eligible を B2 terminal record に要求
                     → fixture が schema を満たさなくなる
                     → runner crash 開始 = [1] 成立
2026-08-25 20:00     最後に正常生成された mapping.json = 以後 [2] の汚染源
2026-08-26  2fda618  RequireAllCandidatesInsideSupport 導入
                     → 本 case の intended blocker が意味的に変わる
```

`[1]` を作った commit と intended violation を変えた commit は別である。
したがって fixture の field 追加だけでは閉じない。

## 4. intended violation の再導出

推測ではなく現在の source から導出した。

`p2-d5-2-w2-c1-mapping-core.ps1`:

```powershell
$mappingRequired = $relation -eq 'INSIDE_SUPPORT'
if($mappingRequired -and $solutionCount -eq 0){++$missing}
if($missing -ne 0){$blockers += 'PHYSICAL_MAPPING_MISSING'}
```

本 case の `display_relation` は `BEFORE_PREDECESSOR` / `AFTER_SUCCESSOR`
であり `INSIDE_SUPPORT` ではない。よって `mappingRequired=False`、
`missing` は増加し得ず、**`PHYSICAL_MAPPING_MISSING` は構造的に到達不能**
である。test が凍結していた expectation は現契約では死んでいる。

fixture drift のみを修復して実測した現在の実際の出力は次である。

```text
exit                       1
blockers                   FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT
presented_candidate_count  1
candidate_count_identity   True
missing_mapping_count      0
mapping_support_relation   BEFORE_PREDECESSOR / AFTER_SUCCESSOR
physical_vblank_mapping_required  False
```

case 名が要求する不変条件は「candidate が silent drop されないこと」であり
`presented_candidate_count=1` と `candidate_count_identity=True` で成立している。
reject 理由は `PHYSICAL_MAPPING_MISSING` から
`FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT` へ移動した。後者は
support 外の formal candidate を名指しするため、本 case の違反に対して
より precise である。

したがって frozen expected blocker は
`FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT` に再固定する。

## 5. harness に必要な二層化

```text
runner が PowerShell exception / usage error で死んだ
  → RUNNER_EXECUTION_FAILURE として test FAIL
  → negative contract violation と同一視しない

runner が intended phase まで到達し違反を報告した
  → actual blocker == frozen expected blocker で PASS 判定
  → $LASTEXITCODE -ne 0 だけでは PASS にしない
```

stale artifact 対策は mtime 比較ではなく構造的に行う。
case ごとの output directory を invocation 単位で一意化し、
stale inheritance を不可能にする。

## 6. 横断確認

同一 harness 形状(`*> $null` + 非0 exit + `Test-Path`)を持つ test:

```text
test-p2-d5-2-w2-c1-runner-candidate-closure.ps1    2件 OPEN (本件)
test-p2-d5-2-w2-c11-support-envelope-contract.ps1  現時点で vacuous ではない
test-p2-d5-2-w2-c11-support-gap-contract.ps1       現時点で vacuous ではない
```

後2者は fresh debug tree で artifact を実際に生成しており、現時点の
false green ではない。ただし stale artifact 継承に対する構造的耐性は
持たないため、S2-f2 の hardening 対象に含める。
