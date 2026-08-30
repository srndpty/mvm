# P5-E4 S2-a — startup order contract の改行 domain 修復

## 1. 変更

`tests/gpu_preview/test-p2-c3-a3-t2-startup-order-contract.ps1` の
source 読み込みと mutation の比較対象を LF domain に正規化した。
production source、startup order assertion、threshold は変更していない。

修正前は、CRLF の契約 script にある複数行 here-string と LF の source を
`IndexOf` / `Replace` で直接比較していた。このため3件の mutation が適用前に
失敗していた。正規化は source、mutation の旧文字列、新文字列のすべてに適用し、
比較 domain を一つにした。

## 2. closure evidence

採取条件:

```text
checkpoint  a734d1a9a44595ec5d683305394bac5dc6e66032 + 本 slice の未コミット差分
build       build/ucrt64-release
date        2026-08-29
```

各ケースを個別実行し、Good の成功だけでなく、4件の negative がそれぞれ
対応する assertion で契約違反を検出したことを確認した。

```text
Good
  PASS

NegativeFlagGatedContext
  expected violation: initialize()が後から変更されるdiagnostic flagに依存しています

NegativeMissingNullGuard
  expected violation: issueTargetPixelToggle()にnativeContext1_のnull guardがありません

NegativeVisibleAtLoad
  expected violation: Main.qmlがengine.load()時点でwindowを可視にしています

NegativeShowBeforeAttach
  expected violation: main()がattach()より前にwindowを可視化しています
```

CTest 登録経路でも確認した。

```powershell
ctest --test-dir build\ucrt64-release `
  -R p2_c3_a3_t2_startup_order `
  --output-on-failure --timeout 60
```

```text
5/5 PASS
timeout 0
```

## 3. 判定

```text
S2-a                                  CLOSED
startup_order ordinary failures      3 -> 0
observed false-green negatives       0
production semantics changes         0
assertion / threshold relaxation     0
```

S2-a は閉じた。P5-E4 ordinary regression gate は、残る S2-b〜S2-e が
未完了のため OPEN のままである。
