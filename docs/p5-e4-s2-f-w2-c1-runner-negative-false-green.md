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

## 7. S2-f2 実装

### 7.1 expectation の再固定

旧 expectation は「昔から誤っていた」のではなく、2fda618 で superseded された。
両者を別々に記録する。

```text
84a98c2 時点 (2026-08-24)
  expected blocker  PHYSICAL_MAPPING_MISSING
  当時の contract 上は有効

2fda618 以降 (2026-08-26) = current
  expected blocker  FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT
  旧 expectation は mappingRequired=false により到達不能
```

`RequireAllCandidatesInsideSupport` の導入により、formal candidate が support 外に
ある場合はそれ自体を violation として名指しする。support 外に mapping を要求する
こと自体が support 外の authority 主張になるため、mapping の欠落
(`PHYSICAL_MAPPING_MISSING`) として数えない。これは relaxation ではない。

### 7.2 harness の二層化

```text
層1  runner execution
     mapping artifact 不在 / 必須 field 不在
       => RUNNER_EXECUTION_FAILURE => test FAIL
     非0 exit を semantic success にしない

層2  semantic expectation
     actual blocker == frozen expected blocker
     + candidate retention assertions
```

### 7.3 stale artifact の構造的排除

invocation ごとに `inv-$PID-<guid>` directory を作り、その中だけへ書く。
mtime 比較や事前削除には依存しない。旧固定 output path に stale artifact が
あっても参照経路が存在しない。

### 7.4 case 構成

```text
Good                            baseline。runner が mapping phase を完走し
                                blockers=[] / mapping_exact / INSIDE_SUPPORT
NegativePresentedBefore...      BEFORE_PREDECESSOR
NegativePresentedAfter...       AFTER_SUCCESSOR
NegativeStaleArtifactNotReused  旧固定 path に stale artifact を置き、
                                新 harness が参照しないことを検査
```

negative は次を同時に固定する。

```text
blocker exact              FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT
mapping_support_relation   BEFORE_PREDECESSOR / AFTER_SUCCESSOR
mapping_required           false
missing_mapping_count      0
presented_candidate_count  1 (top / run 両方)
candidate_count_identity   true
```

### 7.5 mutation applicability

harness 自体が violation を検出できることを実証した。

```text
M1  fixture schema drift 再導入 (2026-08-25 の事故そのもの)
    => DETECTED
    => RUNNER_EXECUTION_FAILURE: mapping artifactが生成されませんでした
    => 非0 exit を negative 成功として扱わない

M2  intended violation 除去 (displayed を support 内へ戻す)
    => DETECTED
    => がsubset PASSしました

M3  invocation 隔離除去 (旧固定 output path へ戻す)
    => DETECTED
    => invocation directoryに既存artifactがあります
```

3/3 で mutation は適用され、意図した経路で検出された。

## 8. c11 の扱い

`test-p2-d5-2-w2-c11-support-envelope-contract.ps1` と
`test-p2-d5-2-w2-c11-support-gap-contract.ps1` には invocation 隔離だけを
適用した。expectation semantics は変更していない。

ただし hardening 中に次を確認した。

```text
c11 support envelope の negative 4件
  checker は fail-close 時に proof artifact を書かない
  => proof.json は生成されない
  => artifact による「semantic fail-close か usage crash か」の区別が不可能
```

したがって c11 negative は現在も

```text
$LASTEXITCODE -ne 0  => PASS
```

という形状のままであり、W2-C1 と同型の vacuous PASS 余地が残る。今回は
stale artifact 継承だけを塞いだ。exact blocker 検査へ移行するには checker 側が
fail-close 時にも diagnostic artifact を出す必要があり、これは expectation
semantics の変更にあたるため S2-f2 の範囲外とする。

**これは「c11 は健全」という意味ではない。未閉鎖項目として残す。**
