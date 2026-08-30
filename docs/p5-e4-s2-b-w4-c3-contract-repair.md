# P5-E4 S2-b — W4-C3 mutation-detection capability の修復

## 1. 変更

`tests/gpu_preview/test-p2-d5-2-w4-c3-stop-arbitration-architecture.ps1` の
次の test harness drift を修復した。

- `stopPublicationRecord = StopPublicationRecord{true` の固定空白を、
  `stopPublicationRecord\s*=\s*StopPublicationRecord\{true` へ変更した
- 現在の C++ 改行位置へ追随していなかった2件の mutation anchor
  (`NegativeUnclaimedExplicitStopWriter` / `NegativeInlineArbitrationCas`) を
  再アンカした
- 各 negative について、少なくとも1つの source text が実際に変化したことを
  assertion 前に検査するようにした
- 各 negative の期待 violation message を固定し、無関係な assertion の例外を
  PASS として扱わないようにした

空白許容化は C++ formatting を非semantic として扱う正規化である。
lock 下の coalescing と最初の publication record 保持という assertion の意味、
production source、threshold は変更していない。

## 2. 修正前

attribution 採取時点では次の状態だった。

```text
Good                         FAIL 1
registered negatives         23
false-green negatives         6
```

偽greenだった6件:

```text
NegativeExplicitStopClaimDefaulted
NegativeExplicitStopClaimReconstructedFromWinner
NegativeInlineArbitrationCas
NegativeMissingSchedulerConfigEmit
NegativeStopPublicationRecordOverwrittenBeforeConsume
NegativeUnclaimedExplicitStopWriter
```

6件はいずれも自身の mutation に対応する assertion より前に、Good と同じ
空白依存 assertion で throw していた。修復中に mutation 適用そのものを検査すると、
このうち `NegativeUnclaimedExplicitStopWriter` と
`NegativeInlineArbitrationCas` は C++ の改行位置変更により mutation anchor も
空振りしていたことが判明した。

## 3. closure evidence

採取条件:

```text
checkpoint  e4f52a5c9b6a362497d18ec13a56181c503b0bb1 + S2-b の未コミット差分
build       build/ucrt64-release
date        2026-08-29
```

Good と全23 negative を個別実行した。結果は24/24 PASSであり、各 negative は
mutation 適用検査を通過したうえで、`expectedViolations` に定義した自身の
violation message と完全一致した。修正前に偽greenだった6件の到達先は次のとおり。

```text
NegativeExplicitStopClaimDefaulted
  expected violation: explicit stop consumptionがconsumeしたrecordを引き継いでいません

NegativeExplicitStopClaimReconstructedFromWinner
  expected violation: explicit stop consumptionがconsumeしたrecordを引き継いでいません

NegativeInlineArbitrationCas
  expected violation: helper外のinline CASがあります

NegativeMissingSchedulerConfigEmit
  expected violation: scheduler_configがemitされません

NegativeStopPublicationRecordOverwrittenBeforeConsume
  expected violation: pending requestのrecordが後着publicationで上書きされ得ます

NegativeUnclaimedExplicitStopWriter
  expected violation: explicit stop writerがclaimを通りません
```

CTest 登録経路でも確認した。

```powershell
ctest --test-dir build\ucrt64-release `
  -R '^p2_d5_2_w4c3_stop_arbitration_architecture_' `
  --output-on-failure --timeout 60
```

```text
24/24 PASS (Good 1 + negative 23)
timeout 0
```

補助検査では PowerShell parser と `git diff --check` が PASS した。
`pwsh scripts/lint.ps1` は、今回変更していない C++ 6ファイルの clang-format差分と
既存 script 6件の PSScriptAnalyzer 警告により FAIL した。architecture、層隔離、
GPU preview禁止事項、producer service の各検査は PASS した。

変更した test script を同じ設定で単独解析すると、変更前から存在する
`SourceRoot` の closure 経由参照に対する `PSReviewUnusedParameter` だけが報告され、
本 slice で追加した変数・処理への警告はなかった。したがって repository-wide
lint は clean gate としては未成立だが、S2-b の変更に起因する新規 lint failure はない。

## 4. 判定

```text
S2-b                                  CLOSED
W4-C3 ordinary failures               1 -> 0
observed W4-C3 false-green negatives  6 -> 0
negative mutation application         23/23 verified
negative intended violation           23/23 verified
production semantics changes          0
assertion / threshold relaxation      0
```

S2-b は閉じた。P5-E4 ordinary regression gate は、残る S2-c〜S2-e が
未完了のため OPEN のままである。
