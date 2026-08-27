# P2-D5-2 W2-E Canonical Authority Cutover / Legacy Authority Retirement

## 位置づけ

W2-E は性能評価の段ではない。**authority selector を切り替える段**である。

```text
Before W2-E:
  canonical presentation authority = historical frameSwapped / DWM path
  formal-v2                        = validated shadow only

After W2-E:
  canonical presentation authority = formal-v2 exact chain
  historical frameSwapped / DWM    = diagnostic / non-authoritative only
```

切替と performance verdict 生成は同じ段階にしない。W2-E 後、canonical performance
verdict (fps / drop / threshold / PASS/FAIL) は**どこにも存在しない**。W3 の
formal-v2 fresh acquisition まで保留する。

今回の範囲は **checker-level のみ**である。producer / runtime は変更していない。
`recordFrameSwapped -> commitSwap` も `formalOpportunityIgnoreNextSwap` もそのまま残る。
binary が変わらないため、fresh-7 は W2-E 検証の証拠として有効なままである。

## W2-E.1 canonical cutover contract

canonical artifact は W2-D shadow artifact の boolean を反転させたものではない。
runner / checker は共通手順 [`p2-d5-2-w2-e-shared-replay.ps1`](../scripts/p2-d5-2-w2-e-shared-replay.ps1)
で次を行う。

1. W2-D checker を再実行する (W2-A/A.1、B1、B2、C1.4、C2.1、C2、C2.4 もここで再実行される)
2. legacy authority retirement inventory を再実行する
3. 同じ sealed authority から formal-v2 integration を**独立再構築**する
4. canonical statement を組む

artifact には次を固定している。

```text
presentation_authority_schema         = FORMAL_V2
canonical_authority                   = true
canonical_source                      = intent -> composition_token -> native_present
                                        -> exact_present_event -> final_state
                                        -> displayed_qpc -> physical_vblank_ordinal
frame_swapped_authority               = false
dwm_frame_statistics_authority        = false
legacy_presentation_authority_retired = true
legacy_diagnostics_retained           = true
retirement_means_deletion             = false
performance_threshold_evaluated       = false
canonical_verdict_evaluated           = false
canonical_performance_verdict_deferred_to = W3
historical_verdicts_rewritten         = false
source_frame_identity_used            = false
nearest_qpc_or_tolerance_used         = false
```

checker はこれらを再構築比較より**先**に検査する。`drop_rate` / `effective_fps` /
`performance_pass` / `canonical_pass` などの field が artifact・run・record の
どこかに現れた場合も reject する。

provenance は `source_w2d_proof_sha256` で束縛し、canonical が別の W2-D shadow proof を
指していれば reject する。W2-D artifact が主張した verdict は `source_w2d_verdict` として
そのまま運び、書き換えない。

## W2-E.2 retirement completeness

ここが本体である。証明したいのは 1 点だけである。

```text
legacy presentation authority が canonical verdict の入力として使われていない
```

**retirement = deletion ではない。** `frameSwapped` / `DwmGetCompositionTimingInfo` /
legacy presentation counters は diagnostic として残してよい。

### 分類の定義

[`inventory-p2-d5-2-w2-e-legacy-authority.ps1`](../scripts/inventory-p2-d5-2-w2-e-legacy-authority.ps1)
は legacy performance metric (`effective_fps` / `drop_rate` / `effective_video_fps`) を
参照する失敗地点を次へ分類する。

```text
CANONICAL_PERFORMANCE  run の presentation / performance verdict を決める判定。
                       fps / drop / threshold の PASS/FAIL がこれにあたる。
                       W2-E 後は 0 件でなければならない。

DIAGNOSTIC_INTEGRITY   diagnostic ledger が自己整合であることの検査。
                       「記録値が再計算値と一致するか」しか主張しないため、
                       presentation authority を主張していない。残してよい。
```

### 走査対象は discovery する

走査対象を列挙で固定すると、登録されていない checker に threshold を足すだけで
false-PASS できてしまう。そのため対象は `scripts/check-*.ps1` から **discovery** する
(現在 45 件、うち legacy metric consumer は 10 件)。

```text
legacy metric を参照し、かつ失敗しうる checker
  ├─ disposition 宣言あり → failure site を分類
  └─ disposition 宣言なし → LEGACY_METRIC_CHECKER_AUTHORITY_UNDECLARED / fail-close
```

### failure site は AST + taint で追う

site の検出を「metric 名と FAIL が同一物理行にある」に頼ると、行が分かれた瞬間に
すり抜ける。

```powershell
$fps = Require-Property $raw 'effective_fps'   # ここに FAIL は無い
$minimumFps = 55
if ($fps -lt $minimumFps) {
    Fail 'fps too low'                         # ここに metric 名は無い
}
```

そのため行 regex を増やすのではなく PowerShell AST を使う。

```text
1. scripts/check-*.ps1 を Parser::ParseFile で AST 化する (parse error は fail-close)
2. legacy metric literal を参照する代入から変数 taint を固定点まで伝播させる
   $value = ... 'drop_rate' ...  →  $bad = $value -gt $limit  →  if ($bad) { Fail }
3. failure emitter (*Fail* / Close / Require-* / Assert-* / throw / 非0 exit) を集める
4. emitter 自身、または emitter を囲むすべての if 条件が legacy を参照していれば
   failure site とする (入れ子の外側が legacy 依存なら内側が clean でも site)
5. 注記が無ければ UNCLASSIFIED / fail-close
```

file-level fallback も併用する。legacy metric を参照し失敗しうる checker が
disposition を宣言していなければ、site 検出の結果に関係なく reject する。
未登録 checker のすり抜けはここで止まる。

canonical checker は disposition を machine-readable に宣言し、legacy metric を参照する
失敗地点には注記を置く。注記の無い地点は `UNCLASSIFIED` として fail-close する。
これにより「新しい fps threshold FAIL をうっかり足す」ことを防げる。

```powershell
$MvmPresentationAuthorityDisposition = [ordered]@{
    presentation_authority        = 'FORMAL_V2'
    legacy_presentation_metrics   = 'DIAGNOSTIC'
    canonical_performance_verdict = 'DEFERRED_TO_W3'
}
```

### 実際に降格した canonical decision

discovery を入れた結果、当初列挙していた 2 checker の外にもう 1 件見つかった。
`check-p3-c-contract.ps1` は同じ display ledger 由来の fps / drop で canonical FAIL を
出しており、列挙固定のままなら false-PASS していた。

| 場所 | 変更前 | 変更後 |
| --- | --- | --- |
| [check-p2-contract.ps1](../scripts/check-p2-contract.ps1) | `effective_fps>=55` / `drop_rate<=0.02` を `Add-Failure` | 値を diagnostic として報告するのみ |
| [check-p3-c-contract.ps1](../scripts/check-p3-c-contract.ps1) | `effective_video_fps<55.0` / `drop_rate>0.02` を `Fail` | 同上 |
| [check-p4-formal-contract.ps1](../scripts/check-p4-formal-contract.ps1) | `fps<55.0` / `drop>0.02` を `Fail` | 同上 |

残した `DIAGNOSTIC_INTEGRITY` は 2 件である。

```text
check-p2-contract.ps1        drop_rate が ledger 再計算と一致するか
check-p4-formal-contract.ps1 effective_video_fps / drop_rate が記録値と再計算値で一致するか
```

どちらも「diagnostic が嘘をついていないか」しか主張していない。threshold 判定ではない。

### inventory 結果 (repository 実測)

```text
checker_discovery                        scripts/check-*.ps1
failure_site_analysis                    POWERSHELL_AST_WITH_LEGACY_METRIC_TAINT
discovered_checker_count                 45
legacy_metric_consumer_count             10
legacy_metric_canonical_decision_count   0
legacy_metric_diagnostic_integrity_count 7
legacy_metric_unclassified_count         0
authority_undeclared_checkers            (なし)
legacy_diagnostics_retained              true
verdict  LEGACY_PRESENTATION_AUTHORITY_RETIRED
```

AST 化により、同一行 regex では見えていなかった site も検出された。いずれも
再計算一致検査か field 存在検査であり `DIAGNOSTIC_INTEGRITY` である。

```text
check-p1-contract.ps1        effective_fps が displayed/elapsed と一致するか
check-p2-contract.ps1        effective_fps / drop_rate の field 存在、drop_rate の再計算一致
check-p4-formal-contract.ps1 effective_video_fps / drop_rate の記録値と再計算値の一致
check-p4-smoke-contract.ps1  producer fps/drop が ledger 再計算値と一致するか
```

`check-p1-contract.ps1` は自身の doc に「fps / seek の閾値判定は含まない」と
明記しており、threshold gate ではない。

legacy metric を参照する 10 checker すべてに disposition 宣言を入れた。

legacy diagnostic source は消していない。次を positive に記録している。

```text
src/app/preview/compositor_rhi_item.cpp               recordFrameSwapped   present=true
src/media/gpu_preview/presentation_opportunity_scheduler.h  commitSwap     present=true
  いずれも authoritative=false / used_for_canonical_verdict=false
```

DWM 側は W2-E 以前から `dwm_diagnostic_*` 命名であり、
`window_output_vblank_observer.h` は `DwmGetCompositionTimingInfo` を参照しない。

## fresh-7 offline cutover 結果

fresh capture は取得していない。checker-level cutover であり binary が変わらないため、
fresh-7 sealed authority の offline 検証で足りる。

入力 / 出力:

```text
build/p2-d5-2-w2-d-formal-v2-shadow-fresh-7-20260825.json
build/p2-d5-2-w2-e-canonical-authority-fresh-7-20260825.json
```

[事実] W2-D checker と retirement inventory を再実行したうえで canonical statement を
独立構築し、次を得た。

```text
canonical required intents                    900 (300 x 3 run)
canonical satisfied intents                   438 (146 x 3 run)
canonical unsatisfied intents                 462 (154 x 3 run)
canonical formal Presented                    438
canonical in-domain Presented                 438
canonical in-domain foreign Presented           0
canonical physical VBlank opportunities       897 (299 x 3 run)
canonical filled physical opportunities       438
legacy_metric_canonical_decision_count          0
```

[事実] `900 != 897` は W2-D と同じく verdict へ接続していない。異なる母集団である。

[事実] canonical accounting は W2-D artifact の aggregate をコピーせず、sealed source
からの再構築結果から取っている。checker は artifact 全体を再構築結果と JSON 比較する。

[exit] fresh-7 canonical cutover は `CANONICAL_PRESENTATION_AUTHORITY_FORMAL_V2`。
performance threshold、fps、drop、canonical PASS/FAIL は評価していない。
frameSwapped / DWM の code / field / log も削除していない。

## negative

`p2_d5_2_w2e_canonical_authority_*` (core 契約):

```text
NegativeShadowProofNotExact             shadow が exact でないまま canonical へ昇格
NegativeShadowVerdictInvalid            shadow verdict が INVALID
NegativeHistoricalVerdictMutation       upstream verdict の書き換え
NegativeLegacyAuthorityStillEnabled     retirement 未成立のまま cutover
NegativeLegacyMetricFeedsCanonicalVerdict  legacy metric が canonical verdict へ到達
NegativeLegacyMetricUnclassified        未分類の legacy metric 失敗地点
NegativeLegacyDiagnosticsDeleted        retirement を deletion として実装
NegativeCanonicalSourceFrameIdentity    canonical chain に source frame identity
NegativeCanonicalNearestQpcFallback     canonical chain に nearest QPC / tolerance
NegativeCanonicalRecordSourceFrameField record に source frame field
```

`p2_d5_2_w2e_retirement_*` (合成 source root に対する retirement contract):

```text
NegativeCanonicalPerformanceAnnotated   CANONICAL_PERFORMANCE と注記された地点
NegativeUnclassifiedSite                注記の無い legacy metric 失敗地点
NegativeThresholdReintroduced           fps<55 の FAIL を再導入
NegativeDispositionMissing              disposition 宣言なし
NegativeDispositionWrong                legacy metric を CANONICAL と宣言
NegativeLegacyDiagnosticDeleted         legacy diagnostic source を削除
NegativeUnregisteredCheckerWithLegacyThreshold
                                        未登録checkerにfps thresholdを追加
NegativeUnregisteredCheckerWithMultilineLegacyThreshold
                                        metric参照とFAILが別行の未登録checker
NegativeDeclaredCheckerWithMultilineLegacyThreshold
                                        disposition宣言済みで複数行threshold
NegativeDeclaredCheckerWithAliasedLegacyThreshold
                                        disposition宣言済みでalias 2段経由のthreshold
```

後半 4 件が discovery と site 検出の穴を直接固定する。とくに
`NegativeDeclaredChecker*` は disposition を宣言しているため file-level fallback では
止まらない。test は artifact の blocker を読み、
`LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED` で捕まっていること、かつ
`LEGACY_METRIC_CHECKER_AUTHORITY_UNDECLARED` で**は**捕まっていないことを確認する。
これで AST + taint の site 検出が fallback とは独立に効いていることを証明する。

positive として `GoodLegacyDiagnosticsRemainPresent` を両方に置いている。
旧 frameSwapped / DWM 値が残っていても `authoritative=false` /
`used_for_canonical_verdict=false` なら PASS させる。これが
「retirement = deletion」という誤った実装を防ぐ。

cross-cohort canonical splice は W2-D の再構築がそのまま fail-close する
(C1 / C2.1 / C2 の SHA binding と sealed source hash)。

`p2_d5_2_w2e_repository_retirement` は合成ではなく **repository 実体**に対して
inventory を走らせ、`legacy_metric_canonical_decision_count = 0` を CI で維持する。

## 再現

```powershell
pwsh scripts/build-p2-d5-2-w2-e-canonical-authority.ps1 `
  -W2DProof build/p2-d5-2-w2-d-formal-v2-shadow-fresh-7-20260825.json `
  -Output build/p2-d5-2-w2-e-canonical-authority-<new-name>.json

pwsh scripts/check-p2-d5-2-w2-e-canonical-authority.ps1 `
  -Proof build/p2-d5-2-w2-e-canonical-authority-fresh-7-20260825.json

pwsh scripts/inventory-p2-d5-2-w2-e-legacy-authority.ps1 `
  -Output build/p2-d5-2-w2-e-legacy-authority-retirement.json

ctest --test-dir build/ucrt64-release `
  -R 'p2_d5_2_w2e_' --output-on-failure --timeout 300
```

runner は既存 artifact を上書きしない。

## W2-E で行っていないこと

```text
canonical performance verdict の生成 (fps / drop / threshold / PASS/FAIL)
producer / runtime の変更
recordFrameSwapped -> commitSwap の除去 (PreW2 baseline は維持)
frameSwapped / DWM の field / log / code の削除
historical FAIL/INVALID の書き換え
W3 acquisition
```

canonical performance authority には W3 の fresh acquisition が必須である。
W2-E は authority wiring を切り替えただけであり、切替後 binary による計測は
まだ 1 度も行っていない。
