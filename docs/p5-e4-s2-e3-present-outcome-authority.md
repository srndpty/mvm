# P5-E4 S2-e3: Present outcome authority と visibility precondition

## 1. 確定した defect

probe は Present の戻り値を `FAILED(presentHr)` だけで判定していた。

```cpp
const HRESULT presentHr = swapChain->Present(1, 0);
if (FAILED(presentHr)) { ++presentFailureCount; break; }
```

`DXGI_STATUS_OCCLUDED` は success severity の DXGI status であり `FAILED()` は
false を返す。したがって **success-severity の Present status が未分類のまま
submission 成功として数えられていた**。

画面へ到達していない Present を成功として数えると、`GetFrameStatistics` の
PresentCount が進まない。その結果は sampler ack timeout として現れる。つまり

```text
display側のPresent outcome
→ 未分類のまま成功扱い
→ sampler_ack_timeout_count = 1
```

という誤帰属が起きる。S2-e で導入した event-based publication path の欠陥に
見えるが、実際には Present outcome の分類欠落である。

## 2. 今回の failed cohort との関係

`build/p5-e4-ordinary-stability-cohort-20260829` の
`run-1-ucrt64-release-1` は immutable な cohort failure として保持する。

この run の切り分けは次である。

```text
UNCLASSIFIED_SUCCESS_STATUS_DEFECT   CONFIRMED
OCCLUSION_AS_ROOT_CAUSE              HYPOTHESIS (未確定)
```

`DXGI_STATUS_OCCLUDED` が未分類だったことは source から確定できる。しかし
DXGI のドキュメントには flip model swap chain では `DXGI_STATUS_OCCLUDED` を
受け取らないという記述もあり、本 probe は `FLIP_DISCARD` である。したがって
**当該 run で実際にその HRESULT が返ったとは証明されていない**。

S2-e3 の目的は「今回の run は occlusion だった」と断定することではなく、
未分類の success status を成功扱いする defect を除去し、今後 direct evidence を
取得できるようにすることである。

## 3. A: detection authority (必須)

```text
presentHr == S_OK                  successful submission
presentHr == DXGI_STATUS_OCCLUDED  OCCLUDED_NOT_AUTHORITY
                                   submission成功として数えない
                                   sampler ack timeoutへ流さない
FAILED(presentHr)                  PRESENT_FAILURE
上記以外のsuccess status           UNCLASSIFIED_PRESENT_STATUS
```

warmup loop の Present も同じ分類を適用する。

artifact には次を記録する。

```text
present_occluded_count
present_unclassified_status_count
present_outcome_authority_exact
present_outcome_code
window_visibility_precondition
```

## 4. B: acquisition stabilization (mitigation)

probe window を topmost / foreground へ持っていき、可視性を確認する。

```cpp
SetWindowPos(hwnd, HWND_TOPMOST, ...);
SetForegroundWindow(hwnd);
const bool windowVisibilityPrecondition = IsWindowVisible(hwnd) && !IsIconic(hwnd);
```

**これは occlusion を不可能にする authority ではない。** session 状態、最小化、
Secure Desktop、表示切替などは topmost だけでは排除できない。発生確率を下げる
acquisition precondition として扱う。実際に occlusion が起きた場合は A 側が
fail-close する。

precondition を確立できない場合は fail-close する (exit 5)。

## 5. retry しない

`OCCLUDED_NOT_AUTHORITY` が出た run は cohort failure のまま保持する。

```text
OCCLUDED_NOT_AUTHORITY
  != sampler failure
  != performance failure
  != retryable PASS
```

環境理由でその run を捨てて再試行することはしない。B を実装した新しい
checkpoint で **新しい cohort を最初から開始**する。旧 failed cohort は
immutable evidence として残す。

## 6. negative authority

```text
NegativeOccludedPresent            occluded count 1 => OCCLUDED_NOT_AUTHORITY
NegativeUnclassifiedPresentStatus  未分類 status    => UNCLASSIFIED_PRESENT_STATUS
NegativePresentOutcomeAuthority    countは0だがflagのみfalse => 自己申告不整合
NegativeWindowVisibility           visibility false => fail-close
```

`NegativePresentOutcomeAuthority` は count が 0 なのに authority flag だけ
false という不整合を捕まえる。producer の自己申告と再計算の一致を要求する。

## 7. 結果

```text
checker contract   25/25 PASS (release / debug 両方)
live release       PRESENT_OUTCOME_EXACT / occluded 0 / visibility true / 900/900
live debug         PRESENT_OUTCOME_EXACT / occluded 0 / visibility true / 900/900
threshold relaxation  0
skip / delete         0
```
