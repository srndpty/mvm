# P5-E4 ordinary regression debt — attribution (slice 1)

`docs/p2-d5-2-b3-counter-free-required-intent-completion.md` §25.4 が
`ordinary regression debt 既知failure 7件 / 未整理` として残した債務を帰属する。

本 slice は **分類だけ**を行う。production semantics / threshold /
W3 / P3-C-2 / P4 の contract は一切変更していない。test の削除・skip・
threshold 変更・assertion 緩和も行っていない。修正は §6 の後続 slice で扱う。

`pre-existing` は exclusion 理由に使わない。formal exclusion は
superseding authority が存在するか、nonhermetic 性が証明された場合だけ許す。

## 1. 採取条件

```text
checkpoint        ff516e06307fe36920d77ebf5557246c8d685346
worktree          clean (git status --porcelain 空)
build             build/ucrt64-release (CMAKE_HOME_DIRECTORY=C:/dev/soft/mvm)
pwsh              C:/Program Files/PowerShell/7/pwsh.exe
```

診断のために改変した script は scratchpad 上の複製だけであり、
repo 内の test / production source は無変更である。

## 2. 分類結果

| # | test | 分類 | 処遇 |
| --- | --- | --- | --- |
| 1 | `p2_c3_a3_t2_startup_order_negativeflaggatedcontext` | TEST_CONTRACT_DRIFT | fix |
| 2 | `p2_c3_a3_t2_startup_order_negativemissingnullguard` | TEST_CONTRACT_DRIFT | fix |
| 3 | `p2_c3_a3_t2_startup_order_negativeshowbeforeattach` | TEST_CONTRACT_DRIFT | fix |
| 4 | `p2_d5_2_w2c21_required_intent_domain_architecture` | TEST_CONTRACT_DRIFT | fix |
| 5 | `p2_d5_2_w4c3_stop_arbitration_architecture_good` | TEST_CONTRACT_DRIFT | fix |
| 6 | `p2_d5_2_w4c0_static_control_flow_goodstaticinventory` | OBSOLETE_CONTRACT | 契約の再導出 |
| 7 | `p2_present_id_oracle_live` | NONHERMETIC_LIVE_TEST | fix（exclusion 不可） |

**`REAL_PRODUCT_REGRESSION` は 0 件である。** 7 件それぞれについて、対応する
production invariant が現在の source 上で成立していることを §3〜§5 で個別に確認した。

**formal exclusion は 7 件とも不許可**である。#7 は nonhermetic だが、
superseding authority が存在せず（§5.4）、かつ nonhermetic 性の近接原因が
repo 内の probe 実装にあるため修正可能である（§5.3）。

## 3. #1〜#3 — startup_order negative 3 件

### 3.1 観測

```text
ctest -R p2_c3_a3_t2_startup_order   →  2/5 PASS, 3/5 FAIL
  584 good                      Passed
  585 negativeflaggatedcontext  Failed
  586 negativemissingnullguard  Failed
  587 negativevisibleatload     Passed
  588 negativeshowbeforeattach  Failed
```

3 件とも同一箇所で throw する。

```text
tests/gpu_preview/test-p2-c3-a3-t2-startup-order-contract.ps1:81
  if($text.IndexOf($Old)-lt0){throw "mutation対象が見つかりません: $Relative"}

585 / 586 : src/app/preview/compositor_rhi_item.cpp
588       : apps/compositor_spike/main.cpp
```

### 3.2 exact root cause — 改行 domain の不一致

```text
$ git ls-files --eol <対象>
i/lf  w/crlf  attr/text eol=crlf     tests/gpu_preview/test-p2-c3-a3-t2-startup-order-contract.ps1
i/lf  w/lf    attr/text=auto eol=lf  src/app/preview/compositor_rhi_item.cpp
i/lf  w/lf    attr/text=auto eol=lf  apps/compositor_spike/main.cpp
i/lf  w/lf    attr/text=auto eol=lf  apps/compositor_spike/Main.qml
```

`.gitattributes` は `* text=auto eol=lf` に加えて `*.ps1 text eol=crlf` を持つ。
その結果 working tree では contract script だけが CRLF になる。

```text
contract ps1   CR=130 bytes / LF=130 bytes   (全行 CRLF)
対象 3 source  CR=0   bytes                  (全行 LF)
```

`Edit-Copy` の `$Old` は PowerShell here-string であり、script 自身の CRLF を
そのまま含む。対象 source は LF なので `String.IndexOf` が -1 を返す。

**この機構は、失敗する 3 件と成功する 2 件を過不足なく説明する。**

```text
Good                      mutation 無し                       → 影響無し
NegativeVisibleAtLoad     単一行 anchor '    visible: false'   → 改行を含まない
NegativeFlagGatedContext  複数行 here-string anchor            → CRLF/LF 不一致
NegativeMissingNullGuard  複数行 here-string anchor            → CRLF/LF 不一致
NegativeShowBeforeAttach  複数行 here-string anchor            → CRLF/LF 不一致
```

### 3.3 production 側は無変更であることの確認

anchor 対象の literal はいずれも現在の source に存在する。

```text
src/app/preview/compositor_rhi_item.cpp:255-256
    const HRESULT contextResult = static_cast<ID3D11DeviceContext*>(h->context)
                                      ->QueryInterface(IID_PPV_ARGS(&nativeContext1_));
src/app/preview/compositor_rhi_item.cpp:1007
    if (!nativeContext1_) {
apps/compositor_spike/main.cpp:149 / 152 / 153
    controller.attach(surface);  →  window->setVisible(true);  →  QObject::connect(&controller
```

さらに `Good` case が PASS しているため、positive contract
（initialize() の capability 取得が flag 非依存、null guard が使用より前、
Main.qml が load 時 `visible: false`、main() が attach → setVisible の順）は
実 source に対して現在も強制されている。

### 3.4 空振り検査

3 件は `Edit-Copy` で throw して **FAIL する**。誤って緑にはなっていない。
mutation 検出能力が失われているだけで、fail-closed である。

### 3.5 修正可能性の証明（scratchpad、repo 無変更）

`Edit-Copy` の比較を LF 正規化した複製で 5 case を実行した。

```text
Good                      PASS
NegativeFlagGatedContext  PASS  expected violation: initialize()が後から変更されるdiagnostic flagに依存しています
NegativeMissingNullGuard  PASS  expected violation: issueTargetPixelToggle()にnativeContext1_のnull guardがありません
NegativeVisibleAtLoad     PASS  expected violation: Main.qmlがengine.load()時点でwindowを可視にしています
NegativeShowBeforeAttach  PASS  expected violation: main()がattach()より前にwindowを可視化しています
```

各 negative が **自分の意図した violation message** で落ちている。
「throw さえすれば PASS」という空振り復旧ではない。

なお同一 repo 内の `test-p2-d5-2-w4-c3-stop-arbitration-architecture.ps1` は
既に同じ問題を `Lf` helper で解決済みであり、前例がある。

```text
tests/gpu_preview/test-p2-d5-2-w4-c3-stop-arbitration-architecture.ps1:23-25
  # source treeもこのscript自身も改行が混ざり得るので、mutationは常にLFで比較する。
  function Lf([string]$Text){ ... }
```

**分類: TEST_CONTRACT_DRIFT。** production semantics の変更ではなく、
test harness の文字列照合が改行 domain に対して脆弱であることによる。

## 4. #4〜#6 — guard drift 3 件

### 4.1 #4 `p2_d5_2_w2c21_required_intent_domain_architecture` — TEST_CONTRACT_DRIFT

観測（`test-p2-d5-2-w2-c21-required-intent-domain-architecture.ps1:11`）。

```text
throw: required intent setがscheduler start時点で生成されていません
```

全 assertion を評価した結果、**drift は 18 件中 1 件だけ**である
（他 17 件はすべて成立）。

該当 assertion。

```powershell
Require -Text $scheduler -Pattern 'requiredIntentOrdinals_\.push_back\(ordinal\)' ...
# $scheduler = src/media/gpu_preview/presentation_opportunity_scheduler.cpp
```

要求している literal は現存するが、**別 component にある**。

```text
src/media/gpu_preview/required_intent_queue.cpp:7-16
    bool RequiredIntentQueue::start(long long requiredCount) {
        ...
        for (long long ordinal = 0; ordinal < requiredCount; ++ordinal)
            requiredIntentOrdinals_.push_back(ordinal);
        started_ = true;
```

B3-I1 (`19a5146`) で immutable required set が
`presentation_opportunity_scheduler.cpp` から `RequiredIntentQueue` へ抽出された。
invariant「required set は start 時点の `[0,N)` で immutable」は成立したままで、
guard の file inventory だけが古い。

**分類: TEST_CONTRACT_DRIFT。** 修正は assertion の対象 file 差し替えのみ。

### 4.2 #5 `p2_d5_2_w4c3_stop_arbitration_architecture_good` — TEST_CONTRACT_DRIFT

観測（`test-p2-d5-2-w4-c3-stop-arbitration-architecture.ps1:25`）。

```text
throw: pending requestのrecordが後着publicationで上書きされ得ます
```

全 assertion を評価した結果、**drift は 1 件だけ**である。

該当 assertion（同 script:262）は末尾で literal
` = StopPublicationRecord{true` を要求するが、source は行折り返し済みである。

```text
src/app/preview/compositor_rhi_item.h:681-692
    inline void publishStopRequest(CompositorSpikeState& state, StopArbitration cause,
                                   const StopClaimResult& claim) {
        std::lock_guard<std::mutex> lock(state.stopPublicationMutex);
        if (state.measurementStopRequested.load(std::memory_order_acquire)) {
            // 既にpending requestがある。recordは最初のpublicationのまま保持する。
            state.coalescedStopPublicationCount.fetch_add(1, std::memory_order_seq_cst);
            return;
        }
        state.stopPublicationRecord =
            StopPublicationRecord{true, cause, claim.previous, claim.succeeded, claim.publishSerial};
        state.measurementStopRequested.store(true, std::memory_order_release);
    }
```

`=` と `StopPublicationRecord{true` の間に改行と継続 indent が入っている。

```text
$ git blame -L 689,691 src/app/preview/compositor_rhi_item.h
8167bb28 (2026-08-28)     state.stopPublicationRecord =
8167bb28 (2026-08-28)         StopPublicationRecord{true, cause, claim.previous, ...};
dc099237 (2026-08-27)     state.measurementStopRequested.store(true, ...);
```

`8167bb2` (B3-I2) の整形で折り返された。**coalescing semantics は無変更**であり、
pending 時に early return して最初の record を保持する構造はそのまま成立している。

**分類: TEST_CONTRACT_DRIFT。** 修正は regex の空白許容化のみ。

### 4.3 #6 `p2_d5_2_w4c0_static_control_flow_goodstaticinventory` — OBSOLETE_CONTRACT

観測（`test-p2-d5-2-w4-c0-static-control-flow-contract.ps1:24`）。

```text
throw: anchored ordinal advancement式が変更されています (actual=0 expected=1)
```

全 assertion を評価すると、**drift は 7 件**ある。

```text
FAILS: anchored ordinal advancement式が変更されています         (actual=0 expected=1)
FAILS: ordinalがrefresh authority以外から作られています          (actual=0 expected=1)
FAILS: last finalized writerがfinalize pathから外れています       (actual=0 expected=1)
FAILS: source-domain resultが変更されています                    (actual=0 expected=1)
FAILS: AUTHORITY_DISCONTINUITY fatal returnが分類と一致しません   (actual=1 expected=2)
FAILS: ARITHMETIC_OVERFLOW fatal returnが分類と一致しません       (actual=1 expected=2)
FAILS: domain terminal branchの分類数が変更されています           (actual=0 expected=2)
```

7 件すべてが **B3 の意図した production correction** に由来する。

```text
$ git diff 19a5146^ 19a5146 -- src/media/gpu_preview/presentation_opportunity_scheduler.cpp
-        if (!presentationOpportunityOrdinal(originRefreshCount_, preRenderAuthority, completed)) {
-            captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, ...
-            fail(PresentationOpportunityError::AuthorityDiscontinuity);
-            fail(PresentationOpportunityError::ArithmeticOverflow);
-        ordinal = completed + 1;
-                                PresentationSchedulerInvocationResult::OutsideSourceDomainDecision,

$ git log -S "formalOpportunityDomainReached.store(true," -- src/app/preview/compositor_rhi_item.cpp
19a5146 Implement Required Intent Queue with lifecycle management and testing
```

つまり W4-C0 guard が凍結しているのは **B3 以前の ordinal 導出**である。

```text
旧: completed refresh ordinal (NULL DWM counter) から ordinal = completed + 1
新: immutable required set の queue head を reserveHead() で先に確定
```

現在の `selectForRender` では、ordinal は DWM counter ではなく
`requiredIntentQueue_.reserveHead()` の reservation から来る。

```cpp
// src/media/gpu_preview/presentation_opportunity_scheduler.cpp
const auto queueDecision = requiredIntentQueue_.reserveHead();
...
const long long ordinal = queueDecision.reservation.intentOrdinal;
```

この置換こそ W4-C3 で確定した causal chain に対する correction であり、
W3 `CANONICAL_PERFORMANCE_PASS` はこの後の checkpoint `6f23aaf19f68` で取得されている。
したがって guard が要求する旧式は **superseded** である。

残り 3 件も同様に整合する。

```text
last finalized writer   finalizePendingOpportunity() が prepare / apply に分割され
                        (48b77d7 / de93045)、単一 writer は
                        applyPendingOpportunityFinalization() 内へ移動した。
                        「scheduler 全体で writer は 1 件」の assertion は現在も成立する。
source-domain result    past source domain は InvalidFatal へ変更された (19a5146)。
                        OutsideSourceDomainDecision は enum と switch には現存する。
domain terminal branch  formalOpportunityDomainReached.store(true, ...) は
                        全 source から消滅し store(false, ...) だけが残る。
                        B3 の「DOMAIN_TERMINAL を successful completion にしない /
                        normal completion owner は PLANNED_WINDOW_END だけ」と一致する。
```

**分類: OBSOLETE_CONTRACT。ただし exclusion ではない。** guard の目的
（ordinal producer が単一、未分類 writer / return が無い、intent ordinal を
target / source field から間接再構築しない）は現在も生きている。
superseded なのは *式の literal* であって *不変条件* ではない。
新しい queue ベース producer に対して契約を再導出する必要がある。

### 4.4 guard drift の二次被害 — negative の空振り

これらの guard は Good と Negative が同一 script を共有し、
mutation の有無にかかわらず同じ try block を通る。Good が drift 由来の
assertion で throw するようになった結果、**その assertion より後を対象とする
negative は、自分の mutation と無関係に throw して PASS する**。

各 negative が実際に投げた message を採取した（scratchpad の複製で実行）。

W4-C0（5 negative 中 **4 件が空振り**）。

```text
NegativeUnclassifiedOrdinalWriter        未分類のopportunity ordinal writerがあります (actual=2 expected=1)  ← 真
NegativeUnclassifiedLastFinalizedWriter  anchored ordinal advancement式が変更されています                    ← 空振り
NegativeUnclassifiedNoDecisionReturn     anchored ordinal advancement式が変更されています                    ← 空振り
NegativeSecondIntentProducer             anchored ordinal advancement式が変更されています                    ← 空振り
NegativeIndirectOrdinalReconstruction    anchored ordinal advancement式が変更されています                    ← 空振り
```

W4-C3（22 negative 中 **6 件が空振り**。いずれも
「pending requestのrecordが後着publicationで上書きされ得ます」を投げている）。

```text
NegativeExplicitStopClaimDefaulted
NegativeExplicitStopClaimReconstructedFromWinner
NegativeInlineArbitrationCas
NegativeMissingSchedulerConfigEmit
NegativeStopPublicationRecordOverwrittenBeforeConsume
NegativeUnclaimedExplicitStopWriter
```

すなわち ordinary debt の実体は **FAIL 7 件ではなく、FAIL 7 件 + 偽 green 10 件**である。
AGENTS.md「テストが通ったら、空振りで通っていないかを確認する」に該当する。

`p2_d5_2_w2c21_required_intent_domain_negative*` (#276-282) は別 script
（`test-p2-d5-2-w2-c21-required-intent-domain-contract.ps1`、JSON data contract）
であり、この空振りの影響を受けない。

## 5. #7 — `p2_present_id_oracle_live`

### 5.1 観測

```text
ctest -R '^p2_present_id_oracle_live$'   →  Failed (21.38 sec)
scripts/check-p2-present-id-oracle.ps1:8
  ORACLE_SAMPLING_GAP: transition数が不足しています
tests/gpu_preview/run-p2-present-id-oracle.ps1:25
  完全なPresent-ID oracleを取得できませんでした: probe=4 checker=1
```

生成 artifact `build/ucrt64-release/tests/p2-present-id-oracle-live.json`。

```text
oracle_status                 INVALID
sampling_gap_code             ORACLE_SAMPLING_GAP
configured_present_count      900
present_submissions           900        submitted_ids_consecutive = True
statistics_transitions        406        うち [1,900] 範囲内 = 405
未観測 PresentCount           495 件     (最初の欠落 = 6, 9, 10, 21, 25, ...)
observed_ids_complete         False
final_drain_complete          False
final_submitted_present_id    900        final_observed_present_count = 898
sampler_vblank_gap_count      51
max_poll_interval_qpc         495483     nominal_period_qpc = 166805   (2.97 倍)
poll_interval_valid           False      contract は max_poll * 2 < nominal を要求
```

substrate 側はすべて健全である。

```text
present_failure_count                 0
get_last_present_count_failure_count  0
statistics_failure_count              0
statistics_disjoint_count             0
vblank_ring_overflow_count            0
vblank_wait_failure_count             0
window_output_stable                  True
statistics_output_matches_window      True
sampler_high_priority                 True
sampler_baseline_ready                True
```

API 失敗でも observer 失敗でもなく、**consumer thread が VBlank publish を
読み落としている**。

### 5.2 nonhermetic 性の証拠 — 16 run の分散

保持されている全 artifact（2026-08-22 〜 2026-08-29、16 件）を再集計した。

```text
run 日時              status   vblank_gap  max_poll_qpc  transitions/900
2026-08-22 18:12      INVALID     126        1000255         227
2026-08-23 09:13      INVALID     199        1000902         700
2026-08-23 09:14      INVALID     144         669549         757
2026-08-23 16:33      INVALID     435        1835943         463
2026-08-23 16:37      INVALID     295        2168884         605
2026-08-24 01:46      INVALID     132         670941         140
2026-08-25 23:40      INVALID     129         835050         191
2026-08-25 23:45      INVALID     129         666921         312
2026-08-26 00:23      INVALID     227        1001356         553
2026-08-28 04:06      INVALID     159        1000265         742
2026-08-28 04:12      INVALID     170        1002824         729
2026-08-28 04:13      INVALID     164        1331383         737
2026-08-28 04:37      INVALID     133         833693         421
2026-08-28 20:11      INVALID     134         667087         319
2026-08-29 02:09      INVALID      65         660880         406
2026-08-29 09:03      INVALID      51         495483         406   ← 本 slice の実測
```

verdict は 16/16 で INVALID だが、gap は 51〜435（8.5 倍）、
観測 transition は 140〜757（5.4 倍）と大きく分散する。
これは code の決定的な誤りではなく、**実行時の thread scheduling 負荷に
outcome が支配されている**ことを示す。live display（59.95 Hz）と
OS scheduler に依存する測定であり、hermetic ではない。

### 5.3 nonhermetic 性の近接原因は repo 内にある

```cpp
// apps/p2_present_identity_probe/main.cpp:202-231
std::thread statisticsThread([&] {
    const bool priorityOk = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    ...
    while (!statisticsStop.load(std::memory_order_acquire)) {
        const std::size_t currentVBlankCount = observer.ring().publishedCount();
        if (currentVBlankCount == observedVBlankCount) {
            Sleep(0);          // ← yield busy-wait
            continue;
        }
```

`THREAD_PRIORITY_HIGHEST` + `Sleep(0)` の yield は、同 priority 以上の
runnable thread が居る場合しか譲らず、負荷下で 1 refresh 期間内の
再スケジュールを保証しない。実測の `max_poll_interval / nominal_period` は
2.97〜13.00 であり、poller が 3〜13 refresh 期間 descheduled されている。

したがって nonhermetic 性は **外部環境の不可避な性質ではなく、
自前の acquisition 設計に起因する修正可能な欠陥**である。

これは `docs/p2-d5-2-w3-p0-acquisition-preflight.md` W3-P0-A の帰属
（`attribution = EXACT (probe-local sampler consumer starvation)`、
`regression = NO`、`W3 acquisition validityへの影響 = NO`）と一致する。

### 5.4 formal exclusion は許可されない

nonhermetic と分類したが、exclusion の条件は満たさない。

第一に、**superseding authority が存在しない**。formal-v2 canonical chain は
`native present hook -> ETW PresentEvent -> FinalState/DisplayedQPC ->
WindowOutputVBlankObserver` で構成され、`GetFrameStatistics` を使わない。
これは「独立」であって「上位互換」ではない。この oracle 以上の保証を持つ
canonical test は現時点で存在しない。

第二に、**既存の凍結済み authority が exclusion を明示的に禁止している**。

```text
docs/p2-d5-2-w0-formal-authority-inventory.md §3
  緩めない・PASS 扱いしない・skip しない・削除しない。
  suite migration を行うなら、先に新 canonical authority test が
  この oracle 以上の保証を持つことを示してから、別作業として行う。
  ORACLE_SAMPLING_GAP はサンプリング観測に内在する取りこぼしであり、
  「gap を許容する」修正は authority を弱めるので行わない。

docs/p2-d5-2-w1-formal-accounting-contract-v2.md §9
  legacy oracle / D5-2 non-blocker のまま変更しない。
  ORACLE_SAMPLING_GAP は緩めない・skip しない・削除しない・PASS 扱いしない。
```

第三に、§5.3 のとおり修正経路が repo 内に存在する。

**分類: NONHERMETIC_LIVE_TEST。処遇は exclusion ではなく fix。**
ただし他 6 件と違い production adjacent な probe の acquisition 設計変更を伴うため、
独立した slice として扱う。

## 6. 修正 slice 提案

risk 昇順に分ける。S2-a 〜 S2-c は test harness だけを触り、
production source・threshold・W3 / P3-C-2 / P4 contract に触れない。

### S2-a — startup_order の改行 domain 正規化（production risk 無し）

- `test-p2-c3-a3-t2-startup-order-contract.ps1` に、w4c3 が既に持つ
  `Lf` helper と同じ正規化を導入し、`Read-Source` と `Edit-Copy` の
  比較を LF domain に揃える
- assertion の内容・強度は変更しない
- 期待結果: 5/5 PASS、かつ各 negative が自分の violation message で落ちる
  （§3.5 で実証済み）
- 回復: FAIL 3 件

### S2-b — w4c3 assertion の空白許容化

- `stopPublicationRecord = StopPublicationRecord\{true` を
  `stopPublicationRecord\s*=\s*StopPublicationRecord\{true` 相当へ直す
- 検査対象の不変条件（lock 下での coalescing と最初の record の保持）は変更しない
- 回復: FAIL 1 件 + 空振り negative 6 件

### S2-c — w2c21 assertion の対象 file 再アンカ

- `requiredIntentOrdinals_.push_back(ordinal)` の検査対象を
  `src/media/gpu_preview/presentation_opportunity_scheduler.cpp` から
  `src/media/gpu_preview/required_intent_queue.cpp` へ移す
- 「required set は scheduler start 時点で `[0,N)` として生成され immutable」
  という不変条件は変更しない
- 回復: FAIL 1 件

### S2-d — W4-C0 static control-flow contract の再導出（設計作業）

literal の付け替えでは閉じない。旧 contract は DWM counter 由来の
ordinal 導出を凍結しており、B3 がそれを production から除去したためである。
再導出は次を満たす必要がある。

- ordinal producer が `requiredIntentQueue_.reserveHead()` の
  reservation 単一であること
- `selectForRender` の全 return が `finishInvocation` を通ること
  （この assertion は現在も成立しており、維持する）
- `lastFinalizedOrdinal_` の writer が単一で、finalize path
  （`finalizePendingOpportunity` → `applyPendingOpportunityFinalization`）
  上にあること
- intent ordinal を target / source field から間接再構築しないこと
  （この assertion も現在成立しており、維持する）
- 削除された fail branch（旧 ordinal 導出の AUTHORITY_DISCONTINUITY /
  ARITHMETIC_OVERFLOW、OutsideSourceDomainDecision、DOMAIN_TERMINAL の
  `store(true, ...)`）を「消えたから期待値を下げる」のではなく、
  **B3 の新しい fail-close 経路を明示的に数え直す**

threshold・production semantics は変更しない。W3 / P3-C-2 / P4 の
contract にも触れない。

### S2-e — Present-ID oracle probe の consumer starvation 修正（独立 slice）

- `apps/p2_present_identity_probe/main.cpp` の `statisticsThread` を
  `Sleep(0)` yield busy-wait から観測待ち event ベースへ変更する、
  あるいは `THREAD_PRIORITY_TIME_CRITICAL` へ上げる
- **checker の threshold（`max_poll_interval_qpc * 2 < nominal_period_qpc`）や
  gap 許容は一切緩めない。** W0 §3 が明示的に禁止している
- 判定は単発 PASS ではなく、複数 run で `sampler_vblank_gap_count = 0` と
  `poll_interval_valid = true` が安定することを要求する
  （§5.2 のとおり単発は分散が大きい）
- P5-E4 closure の gate には載せず、独立して閉じる

### S2-f — 空振り防止の横断 gate（提案。範囲外なので判断を仰ぐ）

今回の 10 件の偽 green は、Good と Negative が同一 assertion 列を共有し、
Good 側の drift が Negative を無効化することで生じた。同型の guard が
repo 内に多数あるため、次の meta 検査を提案する。

- Good + Negative 構成の architecture guard について、各 Negative が
  **自分の mutation に対応する message** で落ちることを assert する
- これにより Good の drift が Negative を静かに無効化する経路を塞ぐ

本 slice の範囲外なので実施していない。採否は判断を仰ぐ。

## 7. 本 slice で行わなかったこと

```text
production semantics 変更           無し
threshold 変更                      無し
W3 / P3-C-2 / P4 contract 変更      無し
test の削除 / skip / 無効化         無し
assertion の緩和                    無し
formal exclusion の適用             無し（7 件とも不許可と判定）
repo 内 file の変更                 本文書の追加のみ
```

診断のための script 改変はすべて scratchpad 上の複製で行った。
