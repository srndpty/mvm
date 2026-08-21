# P5-E 実装プラン — Product composition (二source / 二layer)

- 状態: **P5-E1 / P5-E2 / P5-E3 完了 / P5-E4 実装済み・closure未確定 (P3-C-2再監査FAIL)**
- 親計画: [Phase 5 計画](phase5-plan.md) §10
- 製品契約: [PreviewEngine 製品契約](preview-engine-contract.md)
- 起点commit: `2de8e2d` (P5-D closure merge)

## 1. 背景

P5-D は `docs/phase5-plan.md` §9.4 の D1〜D4 がすべて「済」となり、`feature/p5-d4` が
`main` へ merge 済みである。Phase 5 の slice 順 (§6) に従い、次は **P5-E — Product composition**
(`docs/phase5-plan.md` §10) である。

現状の `PreviewEngine` は「video source 1 件 / composition layer 1 層」の product wiring で止まっている。
型 (`CompositionSnapshot`) と acceptance algorithm (`CompositionAcceptanceState::submit`) は既に N 層へ
一般化済みだが、その外側 — source 所有、render 経路、capability — が単数固定である。

P5-E で閉じるのは次である。

- `PreviewSourceId` → `gpu::SourceId` / `audio::SourceId` mapping の複数化
- `removeSource()` の実装 (active/pending composition 参照中は `InvalidState` で拒否)
- `CompositorCoordinator` / `ExactFramePairer` の product boundary への配線
- `maxQualifiedActiveVideoSources == 2` / `maxQualifiedCompositionLayers == 2` の独立報告
- 多層 render 経路と `PresentedFrameInfo` への actual accepted token / actual layer count 固定

### 1.1 決定事項

1. **4 sub-slice へ分割する** (E1〜E4)。各 slice が単独で §14 gate を満たす。
2. **`ExactFramePairer` を N 対応へ一般化する**。既存 2 引数コンストラクタと `PairResult` /
   `ExactPairingCounters` のセマンティクスは温存し、frozen P2/P4 テストを不変に保つ。
   engine は 1 層でも 2 層でも同じ実装を通る (AGENTS.md の DRY)。
3. **`CompositorCoordinator` を composition epoch の権威にする**。engine は accepted token が
   変わるたびに `adoptCompositionRuntimeSnapshot()` で atomic 採用し、`validateForDisplay()` で
   stale epoch 提示阻止を製品経路にも効かせる。public には従来どおり `AcceptedComposition`
   だけを出す (`ResourceEpoch` / `SourceGeneration` / `CompositionEpoch` の owner を混ぜない
   — phase5-plan §4)。
   なお E1 の実装では、当初案の `adoptCompositionSnapshot()` + `configure()` では
   参照 source 集合が変わるたびに coordinator instance を作り直すことになり epoch lineage が
   切れるため、`adoptCompositionRuntimeSnapshot(state, layout, generations)` を新設した
   (§3「E1 実装結果」4 を参照)。

---

## 2. 現状の要点 (調査結果)

| 対象 | 場所 | 状態 |
| --- | --- | --- |
| acceptance algorithm | `src/preview_engine/preview_engine.cpp:352-421` | 契約 §7.5 の 12 段を実装済み。**N 層に一般化済みで capability 値だけで挙動が決まる** |
| 1 層 hard-code | `preview_engine.cpp:1378-1382` | `layers.size() > 1` を `UnsupportedCapability` で拒否 |
| capability 初期化 | `preview_engine.cpp:483-490` | video/layer は既定値 1 のまま |
| video source 1 件制限 | `preview_engine.cpp:1218-1222` | `publicVideoSource` が既にあれば拒否 |
| video source 所有 | `preview_engine.cpp:499-504` | `publicVideoSource` / `internalVideoSource` / `videoWorker` が単数 optional。参照 53 箇所 |
| `removeSource()` | `preview_engine.cpp:1350-1363` | 未実装。常に `UnsupportedCapability` |
| render 経路 | `preview_engine.cpp:2041-2200` | `takeExact` 単数 → `layers.size() != 1` ガード → `layers.front()` → `composeSingleLayerToTarget` → `activeLayerCount` を 1 固定 |
| seek | `preview_engine.cpp:610, 875, 2096` | `pendingSeek.expectedVideoGeneration` が単数 |
| `ExactFramePairer` | `src/media/gpu_preview/exact_frame_pairer.{h,cpp}` | 2 source 固定。**engine から未使用** |
| `CompositorCoordinator` | `src/media/gpu_preview/compositor_coordinator.{h,cpp}` | `LayerLayout` は N 対応。`adoptCompositionSnapshot` / `validateForDisplay` 実装済み。**E1 開始時点では engine から未使用** (E1 で `adoptCompositionRuntimeSnapshot` を足して配線済み) |
| `GpuCompositor` | `src/media/gpu_preview/gpu_compositor.cpp:194-217` | 内部は N 層。入口 `composeProductToTarget(frame, target, expectedLayerCount, ...)` に層数を渡すだけ |
| `PresentedFrameInfo` | `src/preview_engine/preview_types.h:74-80` | 既に `AcceptedComposition composition` と `activeLayerCount` を保持 |
| fixture | `tests/assets/p3_audio/` | A=`p3_av_h264_aac.mp4` (1080p60 h264 + 48k stereo aac)、B=`p3_video_hevc_b.mp4` (1080p60 hevc, audio 無し)。**両方 60/1 で二source に使える** |

---

## 3. P5-E1 — internal multi-source ownership + 1-layer product wiring

capability は `1/1` のまま。**外形的な挙動を変えない refactor slice**であり、P5-C / P5-D の
既存 regression がそのまま通ることが主要な gate である。

### 変更内容

1. `preview_engine.cpp` の `Impl` (499-504) に video source table を導入する。

   ```cpp
   struct VideoSourceEntry {
       gpu::SourceId internal{};
       std::unique_ptr<gpu::SourceDecodeWorker> worker;
   };
   std::map<std::uint64_t, VideoSourceEntry> videoSources; // key = PreviewSourceId.value
   ```

   挿入順ではなく `PreviewSourceId` 昇順で決定論的に走査する (`std::map`)。
   `publicVideoSource` / `internalVideoSource` / `videoWorker` を撤去し、53 箇所の参照を
   table 経由へ置き換える。特に:
   - `addSource` commit (`1320-1346`) — entry の emplace
   - `play()` (`1402`) / `seek()` (`1584`) の前提チェック — 「video source が 1 件以上」へ
   - fatal 時の source 付与 (`853`, `2054`) — frame の `sourceId` から public ID を逆引き
   - identity 検査 (`2080-2081`) — `videoSources` に存在する internal ID か
   - shutdown (`2481-2482`) — 全 entry を stop / unregister。順序は `PreviewSourceId` 昇順で固定

2. `internal::PreviewRenderPort` / `P5CRuntimeDiagnostics` の `registeredVideoSourceCount` を
   table の size から出す (現状も同名 field があるので報告経路は不変)。

3. `ExactFramePairer` を N 対応へ一般化する (`src/media/gpu_preview/exact_frame_pairer.{h,cpp}`)。
   - 新コンストラクタ `ExactFramePairer(std::vector<SourceFrameBuffer*> sources, CompositorCoordinator&)`
   - 既存の `(SourceFrameBuffer& a, SourceFrameBuffer& b, CompositorCoordinator&)` は
     新コンストラクタへ委譲するだけにし、frozen テストの include/呼び出しを変えない
   - `tryPair` は全 buffer に対し `discardBefore` → `peekFrontIdentity` → missing 判定 →
     exact take → `coordinator_.compose()` を行う。**`missingACount` / `missingBCount` は
     index 0 / 1 に対応させ、既存 2 source 時のカウント結果を bit 単位で保つ**
   - `SourceFrameBuffer::takeExactPair` (2 固定) を N 化する必要がある。
     `takeExactAll(std::vector<SourceFrameBuffer*>&, long long, std::vector<DecodedGpuFrame>&)` を
     `source_frame_buffer.{h,cpp}` へ追加し、`takeExactPair` はそれへ委譲する。
     **全 buffer の mutex を決定論的順序 (アドレス順ではなく `SourceId` 昇順) で一括 lock し、
     全部が requested と一致した場合だけ commit する** (現行 `takeExactPair` の
     「一方でも違えばどちらも残す」不変条件を N へ拡張する)

4. engine の render 経路 (`preview_engine.cpp:2041-2150`) を coordinator 経由へ差し替える。
   capability は 1/1 なので実際に通るのは 1 layer だが、経路そのものは N 用に書く。
   - `Impl` に `std::unique_ptr<gpu::CompositorCoordinator> coordinator` と
     `std::unique_ptr<gpu::ExactFramePairer> pairer` を持たせる
   - accepted token が前回 compose 時と変わったら
     `coordinator->adoptCompositionRuntimeSnapshot(CompositionStateId{token->id.value}, layout,
     generations)` を呼ぶ (当初案の `adoptCompositionSnapshot()` + `configure()` から差し替えた。
     理由は §3「E1 実装結果」4)。
     `layout[i].zOrder = static_cast<int>(i)` とし、**`CompositionSnapshot::layers` の vector 順が
     背面→前面の z 順である**ことを契約として明記する (public API に zOrder field は増やさない)
   - source generation は同じ `adoptCompositionRuntimeSnapshot()` へ渡して同期する。
     generation だけが動いた場合 epoch は進まない
   - `composed.compositionEpoch` / `compositionState` の直書き (`2144-2145`) を撤去し、
     `coordinator->compose()` が入れた値をそのまま使う
   - 提示直前に `coordinator->validateForDisplay(composed)` を通し、`StaleEpoch` なら提示しない
   - `GpuCompositor` へは新設の `composeLayersToTarget(frame, target, expectedLayerCount, err)`
     (= `composeProductToTarget` を public 化した product API) を呼ぶ。
     `composeToTarget` (2 固定・診断契約) は流用しない
   - `result.frame` の `activeLayerCount` を `composed.layers.size()` から取る

### テスト (E1)

- 新 positive: `tests/gpu_preview/test_gpu_pure.cpp` の `testExactPairingNSource()` に
  N 化 `ExactFramePairer` / `takeExactAll` の 1 source / 3 source ケース。
  既存 2 source ケース (`testP2SourceAndComposition`) は**変更していない**
- 新 negative: `takeExactAll` が一つでも不一致なら**どの buffer も消費しない** (depth 不変を assert)。
  この検査が無ければ partial consume がそのまま通る
- 新 negative: `takeExactAll` が同一 buffer の重複、null、空集合を拒否する
- 新 positive: buffer を渡す順序を変えても exact take が成立し、出力順は引数順である
  (lock 順は `SourceId` 昇順で固定されるため呼び出し順に依存しない)
- 新 positive/negative: `testCompositionRuntimeSnapshotAdoption()` が
  `adoptCompositionRuntimeSnapshot()` の epoch lineage を固定する。
  source 集合が循環しても epoch が単調増加すること、generation の追随では epoch を
  進めないこと、supersede された `ComposedFrame` が `StaleEpoch` になること、
  invalid state / 空 layout / 同一 state id の別 layout / 追跡中 source の generation
  巻き戻しを reject すること、および regression 拒否が追跡中 source に限る
  (追跡対象から外れた source は歴史的 floor を持たない) こと
- 新 positive: `advanceCompositionEpochForTest()` が epoch だけをちょうど 1 進め、
  state / generation を変えないこと
- 新 negative: `ExactFramePairer` の construction preflight (empty / null /
  同一 buffer 重複 / 同一 `SourceId` の別 buffer) と、invalid pairer の
  `tryPair()` が buffer を触らず `Rejected` を返すこと
- 新 negative: `composeLayersToTarget()` の layer 数不一致 (expected=1 / 2 layer、
  expected=2 / 1 layer、expected=0)
- 新 product negative: `preview_engine_p5e_stale_composition_epoch`
  (`apps/p5e_preview_smoke --fault-stale-composition-epoch`)
- 新 product positive: `preview_engine_p5e_product_smoke`
- targeted: `mvm_test_gpu_pure`, `mvm_test_preview_engine`, `p2_gpu_compositor_offscreen`
- regression: 既存 P5-C 9 本 / P5-D 13 本の product test が**無変更で PASS** すること

### E1 実装結果 (実測)

- ordinary CTest **456/456 PASS** (ucrt64-release)。P5-C 9 本 / P5-D 13 本を含む
- 既存 P5-C / P5-D の product test は**一行も変更していない**。capability が `1/1` のままで
  外形的な受理能力を変えていないことの証拠である

計画から変えた点と、その理由を記録する。

1. **`takeExactPair` を拡張せず `takeExactAll` を新設した。**
   `takeExactPair` は frozen な P1〜P4 呼び出し側が使う 2 source API である。
   signature を変えずに `takeExactAll` へ委譲させることで、既存の呼び出しと
   「一方でも変化していればどちらも残す」不変条件をそのまま保った。
   lock 順は引数順ではなく `SourceId` 昇順に固定してある。同じ buffer 集合を
   別順で渡したときに deadlock しないことが N source では必須になる。

2. **`ExactFramePairer` の counter は 2 source の意味を保ったまま index 版を足した。**
   `missingACount` / `staleADiscardCount` は index 0、`missingBCount` /
   `staleBDiscardCount` は index 1 を指し続ける。frozen test の期待値を動かさない。
   source が 1 本のときの欠落は `MissingBoth` を表現できないので `MissingA` を返す。

3. **`validateForDisplay()` は製品経路の negative test 付きで配線した。**
   render path は compose -> validate -> draw を同じ engine lock 内で行うため、
   通常実行だけではこの branch を踏めない。
   そこで `CompositorCoordinator::advanceCompositionEpochForTest()` (state / layout /
   generation を一切変えず `CompositionEpoch` だけを 1 進める test 専用 API) と、
   それを compose 成立後・validate 前に一度だけ呼ぶ
   `PreviewRenderPort::injectCompositionEpochAdvanceForTest()` を足した。
   **完成した error を注入するのではなく、supersede だけを再現して通常の
   validate 経路を通す。**

   固定するのは「reject branch を踏んだこと」だけではない。exit criterion の本体は
   「stale epoch の frame を**提示しない**」なので、拒否した output frame の identity を
   `P5CRuntimeDiagnostics::lastStaleCompositionRejectedFrame` に残し、
   `apps/p5e_preview_smoke --fault-stale-composition-epoch` が次を assert する。

   ```text
   staleCompositionEpochRejectCount == 1
   lifecycleViolationCount == 1
   lastStaleCompositionRejectedFrame == N (>= 0)
   sink の PresentedFrameInfo に position == N が存在しない
   position > N の提示が存在する (reject は fatal ではなく skip)
   最終 state は Shutdown、error 無し
   ```

   この負例が空振りでないことは 2 通りの mutation で確認した。

   | mutation | 結果 |
   | --- | --- |
   | `validateForDisplay()` の分岐自体を無効化 | FAIL (reject が 0 のまま提示が進み続ける) |
   | counter は数えるが `if (rejected) return` を削除 | FAIL (`rejected_frame_presented: true`) |

   後者は counter / lifecycle violation / 最終 state / last accepted token がすべて
   正常値のまま stale frame を描画する mutation であり、frame identity を見ていなければ
   PASS してしまう。

4. **coordinator を session 中に作り直さない。**
   `CompositorCoordinator::configure()` は layout と generations を 1:1 で要求し
   一度きりなので、参照 source 集合が変わるたびに instance を作り直すと
   epoch が instance ごとの別 namespace になり、lineage が切れる
   (source 集合が循環すると古い `ComposedFrame` と新 owner の epoch が衝突し得る)。
   engine 側に `nextCompositionEpoch` を持たせると owner が二重化するので、
   coordinator へ `adoptCompositionRuntimeSnapshot(state, layout, generations)` を足した。
   これは source 集合ごと atomic に置換しつつ、**resolved composition state が
   実際に変わったときだけ epoch をちょうど 1 進める**。generation だけが動いた場合は
   NoOp として追随のみ行い、同一 `CompositionStateId` が別 layout を指す要求は
   fail-closed で拒否する (通すと state id が composition identity を表さなくなる)。

   generation の巻き戻し拒否の範囲は **「現在追跡中の source」に限る**。
   layout から外れた source の generation は保持しないので、
   一度外れてから戻ってきた source に対する歴史的な floor は持たない
   (`A gen 10 -> B -> A gen 9` は受理される)。
   `SourceGeneration` の owner は source 側であり、coordinator へ寄せないための
   意図的な線引きである。header / docs の表現もこの範囲に合わせてあり、
   `testCompositionRuntimeSnapshotAdoption()` がこの境界を期待値として固定している。
   engine は coordinator を lazily 一度だけ生成し、`addSource()` / detach では
   pairer だけを作り直す。

5. **`expectedLayerCount` の authority を accepted snapshot に置いた。**
   `composed.layers.size()` を渡すと `size() == size()` の tautology になる。
   product caller は `snapshot->layers.size()` を渡す。
   `tests/gpu_preview/test_p2_gpu_compositor.cpp` に expected=1 / 2 layer、
   expected=2 / 1 layer、expected=0 の negative を足して、この検査の空洞化を防いだ。

6. **`ExactFramePairer` の N constructor に preflight を入れた。**
   `tryPair()` は `takeExactAll()` へ到達する前に `discardBefore()` /
   `peekFrontIdentity()` で dereference するので、`takeExactAll()` 側の防御だけでは
   効かない。construction 時に empty / null / 同一 buffer の重複 /
   同一 `SourceId` の別 buffer を弾き、invalid な pairer の `tryPair()` は
   buffer を一切触らずに `Rejected` を返す。engine は `valid()` を確認できなければ
   `CompositionFailure` で fail-closed にする。
   `takeExactAll()` の lock 順 tie-break も、無関係な pointer の `<` (未規定) から
   `std::less<SourceFrameBuffer*>` へ変えた。

7. **`seekStaleGenerationRejectCount` の authority を seek 経路に限定した。**
   pairer 由来の `StaleGeneration` / `FutureGeneration` を無条件にこの counter へ
   足すと、seek 以外で起きた reject が seek の証拠に混ざる。
   generation が動くのは seek のときだけなので、それ以外は単なる drop として数える。

8. **`GpuCompositor::composeToTarget` (2 層固定・診断契約) は流用していない。**
   product 用に `composeLayersToTarget(frame, target, expectedLayerCount, err)` を新設し、
   呼び出し側が capability で検証済みの層数を明示する形にした。
   層数を暗黙に受け入れないので、composition acceptance と GPU 描画の食い違いが
   黙って通らない。

---

## 4. P5-E2 — removeSource()

capability は `1/1` のまま。

### 変更内容

1. `internal::CompositionAcceptanceState` (`preview_engine_internal.h:174-192`) に参照集合の
   照会 API を足す。

   ```cpp
   bool referencesSource(PreviewSourceId source) const; // latestAcceptedSnapshot_ と
                                                        // lastPresented 対応 snapshot の両方を見る
   ```

   `lastPresentedToken_` に対応する snapshot を保持していないため、`markPresented()` で
   `lastPresentedSnapshot_` も併せて保持するよう変更する。

2. `PreviewEngine::removeSource` (`preview_engine.cpp:1350-1363`) を実装する。
   契約 `docs/preview-engine-contract.md:158-161` の順で検査する。
   1. control thread affinity (既存)
   2. `ReadyPaused` であること (既存)
   3. 未知の `PreviewSourceId` は `InvalidSource`
   4. active (last presented) または pending (latest accepted) composition が参照していれば
      **`InvalidState`** で拒否
   5. seek 進行中 (`pendingSeek.active`) なら `InvalidState`
   6. commit: video entry を `stop()` → `sourceRegistry.unregisterSource()` →
      `videoSources.erase()`、`eligibleSources.erase()`、coordinator の source generation を落とす
   7. authoritative audio source を削除した場合は audio sink → worker → clock の順で停止し、
      **audio authority を空へ戻す** (`audioMasterActive = false`、`publicAudioSource` を空に)。
      停止順序は shutdown ordering (`contract §12`) と同じ helper へ委譲し、二重実装しない
   8. 削除で video source が 0 になった場合は wall-clock master へ戻る。これは P5-C の
      qualified master であり `videoMasterQpcFallbackCount` には数えない

### テスト (E2)

unit (`tests/preview_engine/test_preview_engine.cpp`):

- `p5eCompositionSourceReferences()` — acceptance state の参照判定
  - composition 無しで参照なし
  - pending (accepted 未提示) が参照を保持する
  - active (last presented) が参照を保持する
  - **A を提示中に B を accept しただけでは A の参照は外れない**
  - B を実際に提示して初めて A の参照が外れる
  - opacity 0 の layer も参照である (描画に寄与しないことと解放可能性は別)
- `p5eRemoveSourceNegatives()` — engine 経路のうち native device を要さない検査
  - `ReadyPaused` 以外 / `ShuttingDown` 中は `InvalidState`
  - 未登録 ID・0 番 ID は `InvalidSource`
  - control thread 以外からの呼び出しは `InvalidState`

product (`apps/p5e_preview_smoke`、`-L p5e`、8 本):

- `--remove-unreferenced-source` — composition 提出前の source を remove 成功、
  `registeredVideoSourceCount` が 1 → 0、二重削除は `InvalidSource`、再登録して再生継続
- `--remove-referenced-source` — `Playing` 中の remove が `InvalidState`、
  pause 後も active/pending composition が参照中なら `InvalidState`、
  未登録 ID は `InvalidSource`
- `--remove-audio-source` — authoritative audio source を安全に解放すると
  `registeredAudioSourceCount == 0` / `audioMasterActive == false` になり、
  wall-clock master で再生を継続できる。解放は failure ではないので
  `audioTransportFailureCount` / `audioSinkDeviceFailureCount` /
  `audioDomainRejectCount` / `videoMasterQpcFallbackCount` はいずれも 0 のまま
- `--fault-remove-audio-stop` — audio transport の pause/stop sequence のうち、
  fallible な `pause()` を sink 自身に失敗させ、通常の removal 経路を通す
  (`stop()` は後続でそのまま呼ぶ)。
  `AudioFailure` / `RemoveSource` / `FatalToSession` を返し、removal は commit されず
  (`registeredAudioSourceCount == 1`)、`ShuttingDown -> Error` へ落ちる。
  `audioTransportFailureCount` はちょうど 1
- `--remove-shutdown-race` — audio 停止フェーズ (engine lock を持たない窓) を barrier で
  止め、その間に別 thread から fatal を入れる。`removeSource()` は `InvalidState` を返し、
  removal は commit されない。`lifecycleViolationCount == 0` (二重 stop や ordering 違反なし)
- `--remove-fatal-event-order` — removal fatal を commit して unlock した直後・flush 前で
  止め、その間に teardown を終端まで進める。mailbox insertion 順を**二段で**観測する。
  - stage 1 (barrier 侵入直後): removal 由来の `ErrorOccurred` -> `StateChanged(ShuttingDown)`
    が既に入っていること
  - stage 2 (terminal 到達後・barrier release 前): terminal `Error` が入ってもなお
    `ErrorOccurred(RemoveSource)` < `ShuttingDown` < `Error` であること

  `ErrorOccurred` は「最初の 1 件」ではなく `operation == RemoveSource` /
  `category == AudioFailure` / `source == audioSource` で識別する。
  stage 1 だけでは `[Error, ErrorOccurred, ShuttingDown]`
  (teardown が先に terminal を commit した元バグの順序) が通ってしまうため、
  stage 2 がこの negative の本体である。

### E2 実装結果 (実測)

- ordinary CTest **全件 PASS**。P5-E product test は E1 の 2 本 → E2 初版 5 本 → 現在 8 本
- P5-C 9 本 / P5-D 13 本は無変更で PASS

計画から変えた点と、その理由を記録する。

1. **capability 1/1 では「参照が外れた video source の削除」に到達できない。**
   当初計画の
   「A を参照する composition を submit → B を submit → 提示 → A を remove 成功」は
   video source が 2 件必要である。`maxQualifiedActiveVideoSources == 1` のうちは、
   composition を submit した時点でその source の参照は二度と外れない
   (差し替え先の source が存在せず、empty snapshot は `CompositionFailure` で拒否される)。
   したがって **E2 で到達できる削除成功は次の 2 経路だけ**である。
   - composition から一度も参照されていない video source の削除
   - composition へ参加しない audio source の削除

   A → B 差し替え後の削除は capability 2 を前提とするので **P5-E3 の product test へ回す**。
   参照判定そのもの (A 提示中に B を accept しただけでは外れない) は unit で固定してある。

2. **`markPresented()` に snapshot を渡す形へ変えた。**
   removal guard は active composition の参照集合を必要とするが、
   `CompositionAcceptanceState` は last presented の **token** しか持っていなかった。
   `latestAcceptedSnapshot_` から暗黙に拾うと、A 提示中に B を accept した時点で
   A の参照が外れたと誤判定する。呼び出し側が token と snapshot を対で渡す形にした。

3. **removal は「検査 → lock 解放して停止 → 再取得して commit」の 3 段にした。**
   audio transport の停止は engine mutex を保持したまま行えない
   (`audioTransportMutex` との取得順が固定されているため)。
   停止中に fatal が入った場合は removal を commit せず `InvalidState` を返し、
   実体の teardown は shutdown 経路へ委ねる。
   停止段階では ownership を移さないので、この窓で shutdown が走っても
   同じ実体を stop/join できる。
   audio sink を停止できなかった場合は `pause()` と同じく
   `AudioFailure` / `FatalToSession` で `ShuttingDown -> Error` へ落とす
   (鳴り続けている endpoint の owner を居なくしない)。

4. **negative が空振りでないことを mutation で確認した。**

   | mutation | 結果 |
   | --- | --- |
   | `referencesSource()` の呼び出しを削除 | `remove_referenced_source` / `remove_audio_source` が FAIL |
   | `referencesSource()` から active (last presented) 側の判定を削除 | `preview_engine_p5b_unit` が FAIL |
   | audio stop 失敗を無視して commit する | `remove_audio_stop_fail_closed` が FAIL |
   | 停止中の state 変化を無視して commit する | `remove_shutdown_race` が FAIL |
   | state commit と mailbox insertion を分離する | `remove_fatal_event_order` stage 1 が FAIL (3/3) |
   | 同上 + stage 1 を無効化して stage 2 だけを残す | `remove_fatal_event_order` stage 2 が FAIL (3/3) |

   2 行目は stage 2 が独立した authority を持つことの確認である。
   stage 1 は「commit 時点で挿入済みか」しか見ないので、
   `[Error, ErrorOccurred, ShuttingDown]` のように terminal が先頭へ来た順序は
   stage 2 でしか捕まえられない。

5. **fatal event の順序は「commit と同じ critical section で mailbox へ入れる」ことで閉じた。**
   当初は `recordFatal()` まで commit してから unlock し、その後 `notify()` していた。
   これは P5-D3 で一度潰したものと同型で、次の逆転が可能だった。

   ```text
   remove:   lock / recordFatal -> ShuttingDown / startWorkerShutdown / unlock
   teardown: terminal transition -> Error / enqueue Error
   remove:   notify ErrorOccurred / notify ShuttingDown   // stale
   ```

   `pendingDispatch` を使い、`recordFatal()` と同じ critical section 内で
   `notifyLocked()` まで行い、unlock 後は `flushDispatch()` だけにした。

   **この性質は sink への delivery では観測できない。** terminal 到達後の pending event は
   contract どおり破棄されるため、正しい実装でも配信されない (実測で確認した)。
   そこで `mailboxEventsForTest()` を足し、**commit 直後・flush 前の mailbox insertion 順**
   そのものを観測する形にした。これが linearizability の authority である。

6. **video worker の raw pointer を lock gap 越しに持ち出さない。**
   phase 1 で `worker.get()` を取り出して phase 2 の後に使うと、その窓で shutdown が勝った
   場合に ownership が `detachedWorkers` へ移り、raw pointer が dangling になり得る
   (audio は `shared_ptr` を local に持つので lifetime が安定していた)。
   phase 3 で table から引き直し、engine lock を保持したまま stop まで行う。
   `ReadyPaused` の worker は既に pause 済みなので join は短く、この窓の render path は
   早期 return するだけである。

7. **削除した public ID を再登録で使い回さないことを assert した。**
   コメントだけで主張していたので、`--remove-unreferenced-source` に
   `first.value() != source.value()` の検査を足し、contract §6 にも明記した。

---

## 5. P5-E3 — capability 2/2 + 多層 render + per-source seek generation

### 変更内容

1. capability を引き上げる (`preview_engine.cpp:483-490`)。

   ```cpp
   value.maxQualifiedActiveVideoSources = 2;
   value.maxQualifiedCompositionLayers = 2;   // 別 field として独立に報告する
   value.duplicateSourceLayersSupported = false; // 初期 capability では拒否のまま
   ```

2. `submitComposition` の 1 層ガード (`1378-1382`) を削除し、capability 検査に一本化する。

3. `addSource` の video 1 件制限 (`1218-1222`) を
   `videoSources.size() >= capability.maxQualifiedActiveVideoSources` の検査へ置き換える。
   エラーメッセージは capability 値を含めた日本語にし、slice 名を埋め込まない。

4. 多層 render 経路 (E1 で N 用に書いた経路をそのまま使う)。
   - pairer に渡す buffer 集合は **accepted snapshot が参照する distinct source 集合**から作る。
     snapshot が変わったら pairer を作り直す
   - 提示成功時に全 buffer へ `noteDisplayed(target)`
   - `currentSourceQueueDepth` は「参照中 source の最大 depth」とし、意味を doc comment に書く
   - `activeLayerCount` は `composed.layers.size()`

5. seek の per-source generation 化 (`Impl::PendingSeek`, `preview_engine.cpp:610, 875, 2096`)。
   - `gpu::SourceGeneration expectedVideoGeneration` → `std::map<std::uint64_t, gpu::SourceGeneration>`
   - seek request 発行 (`1651`) を全 video worker へ。**一つでも request が受理されなければ
     `SeekFailure` で fail-closed** にする (P5-D3 の「片側だけ受理された状態を許さない」を N へ拡張)
   - seek completion は**全 source が期待 generation の exact frame を出し、
     それを実際に提示できた時点**でのみ成立する。いずれかが未達なら
     `seekAwaitingPresentationCount` を増やして待つ
   - `seekStaleGenerationRejectCount` の意味は不変 (substitution ではなく reject)

6. 検証アプリ `apps/p5e_preview_smoke` を拡張する (E1 で新設済み)。
   fixture A + B を二 source 登録し、二層 composition を提示する経路を足す。fault 引数:
   - `(なし)` — 二source / 二層 smoke
   - `--single-layer` — 1 層 regression が同じ経路で通ること
   - `--exceed-source-count` — 3 source 目を `UnsupportedCapability` で拒否
   - `--exceed-layer-count` — 3 層を `UnsupportedCapability` で拒否
   - `--duplicate-source-layer` — 同一 source を 2 層へ配置して `UnsupportedCapability`
   - `--fault-missing-pair` — 片側 source の供給を止め、**old/latest frame で代用せず提示しない**
   - `--remove-released-source` — A を参照する composition を B へ差し替えて提示し、
     参照が外れた A を remove できること (capability 2 が要るため E2 から繰り越し)
   - `--seek-two-source` — 二 source exact seek の completion
   - `--fault-seek-partial-generation` — 片側だけ generation が揃った状態で complete にしない

7. `tests/CMakeLists.txt` の `mvm_add_p5e_product_test` マクロ (E1 で追加済み) へ
   二 source 用の test を足す。fixture wrapper は
   `scripts/run-with-required-fixture.ps1` を再利用し、A と B の**両方**を必須にする
   (wrapper が単一 `-RequiredFile` なので、複数対応へ拡張するか wrapper を 2 段掛けにする。
   拡張する場合は `tests/preview_engine/check-required-fixture-wrapper.ps1` の契約テストも更新する)。
   末尾の**登録数の literal 検査**を実数へ更新し、対象 0 件を成功にしない。

### テスト (E3)

`docs/phase5-plan.md` §10.2 の各項目に対応させる。既存 unit テストの更新:

- `tests/preview_engine/test_preview_engine.cpp:870-872` — 「two-layer submission を拒否」を
  **受理**へ反転する
- 同 `951-956` — `maxQualifiedActiveVideoSources == 1` / `maxQualifiedCompositionLayers == 1` の
  literal を `2` へ更新
- `compositionIdentityAndCapabilities()` (`396-478`) は既に手動 capability 2/2 で
  token 採番・no-op 再利用・supersede・reject 時 revision 不変・duplicate source・
  count 超過を網羅しているため、**engine 公開経路でも同じ期待値が出ることを確認する
  テストを追加**する (validator を呼ばず独立 literal から検査 — plan §10.2 末尾の要求)
- 新規: `PresentedFrameInfo` が actual accepted token と actual layer count を持つこと
- 新規 negative: exact pair 不足時に old/latest frame を使わない
- 新規 negative: stale composition epoch を提示しない
- 新規 negative: unknown / removed source を参照する snapshot を拒否

### E3 実装結果 (実測)

- ordinary CTest は ucrt64-release **473/473 PASS**、ucrt64-debug **473/473 PASS**
- P5-E product test は **17/17 PASS**。二source / 二layer、1-layer regression、
  source/layer上限、duplicate source layer拒否、exact pair不足、参照解放後のremove、
  二source exact seek、片側generation不一致、N-way seek requestの部分受理を個別のtestとして登録した
- P5-C regression **11/11 PASS**、P5-D regression **13/13 PASS**
- full build (`pwsh scripts/build.ps1`) PASS

計画から変えた点と、その理由を記録する。

1. **P5-C smokeの「2 source目は拒否」をE3 capabilityへ追随させた。**
   §8では`apps/p5c_preview_smoke`を変更しないとしていたが、実リポジトリのP5-C smokeは
   2 source目が`UnsupportedCapability`になることを直接assertしていた。この期待値は
   `maxQualifiedActiveVideoSources == 2`への引き上げと両立しない。P5-Cの1-layer render検査は
   変えず、2 source目の受理を確認後、composition提出前に`removeSource()`で解放してから
   従来の1-source経路を続ける形へ最小限追随させた。

2. **複数fixtureのpreflightはwrapper自身を配列対応にした。**
   `run-with-required-fixture.ps1`へ`AdditionalRequiredFile`を追加し、A/Bを一度に検査してから
   子processへ同じ順序で渡す。contract testには「先頭が存在しても後続が欠如すればexit 2」
   となるnegativeを追加した。wrapperを二段にして外側だけ通る状態を作らないためである。

3. **pair不足と片側generation不一致は完成済みerrorを注入していない。**
   pair不足は対象workerをpauseしてbufferをclearし、停止直前のin-flight decodeが収束した後の
   提示数が増えないことを検査する。generation負例は片側sourceの期待generationだけをずらし、
   通常のper-source照合とdeadline経路を通して`seekCompletedCount == 0`を固定する。

4. **seek request/completionのidentityをpublic source IDごとのmapへ移した。**
   全video workerへrequestを発行し、一つでも拒否された場合はsession-fatalとする。
   completionは全workerのexact frameとgenerationが揃うまでdecode-readyにせず、提示時にも
   composed layerのinternal IDをpublic IDへ逆引きしてsourceごとの期待generationと照合する。

5. **implementation reviewで指摘されたmulti-source seekの観測穴を閉じた。**
   `--seek-two-source`はA-only paused seekでAのgenerationだけを先に進め、B登録後に
   `A generation != B generation`を診断値からassertする。その状態で二source seekを行い、
   requested frame提示だけでなく`Playing`復帰、`seekCancelledByShutdownCount == 0`、復帰後8 frameの
   継続提示まで待ってからshutdownする。これによりAのcompletion generationをBへコピーする実装や、
   resume前shutdownを正常完了扱いする実装は通らない。

6. **N-way seek requestの部分受理を独立したnegativeで固定した。**
   `--fault-seek-partial-request`はBのseek mailboxだけを事前にbusyにし、public ID順でAが
   Acceptedになった後にBを`RejectedBusy`へする。`SeekFailure / Seek / FatalToSession`、error source B、
   engine loopのaccepted数1、seek completion 0、全worker join、lifecycle violation 0を要求する。

7. **layer count negativeをduplicate policyから独立させた。**
   test seamでsource上限だけを3へ広げ、layer上限2とduplicate=falseは維持する。
   A/B/Cの3 distinct sourceによる3-layer snapshotをproduct APIへ提出するため、layer-count checkを
   削除してもduplicate拒否へ流れて同じcategoryでPASSすることはない。

8. **partial-generation fatalのroot causeを固定した。**
   terminal `Error`だけでなく、`SeekFailure / Seek / FatalToSession`と
   `seekRequestCount == 1`、`seekDecodeReadyCount == 1`、`seekCompletedCount == 0`、
   generation reject 1件以上を要求する。無関係なfatalによるterminal到達ではPASSしない。

---

## 6. P5-E4 — closure

`docs/phase5-plan.md` §9.4 の P5-D4 と同じ形で閉じる。

1. §10.2 の全項目に対応するテストが存在し、**対象 0 件の group が無い**ことを突き合わせる。
   突き合わせ表を `docs/phase5-plan.md` §10 へ追記する。
2. frozen regression の再走。P5-E は GPU composition hot path
   (`GpuCompositor` 入口 / `SourceFrameBuffer` / `ExactFramePairer` / `CompositorCoordinator`) を
   触るため、§15 の「共有 hot path を変更する場合は closure gate を明示する」に該当する。
   - `p2-matrix.ps1` — 二source exact pairingを複数runし、各runで
     `check-p2-contract.ps1` (P2-D5-1) を実行する
   - `run-p4-formal-matrix.ps1` — composition catalog / referenceを複数runし、各runで
     `check-p4-formal-contract.ps1`を実行する
   - `p3-c2-matrix.ps1` — audio-master seekを複数runし、各runで
     `check-p3-c2-contract.ps1`を実行する (E3 のseek変更に対する帰属確認)
   FAIL が出た場合は **P5-D4 と同じ手順**で、変更後と未変更の親 commit の双方を複数回実行して
   帰属を判定し、後続 PASS で歴史的 FAIL を上書きしない。
3. docs 更新。
   - `docs/phase5-plan.md` §10 に sub-slice 表と各 exit criteria を追記し、P5-E を「済」にする
   - `docs/preview-engine-contract.md` §7.4 の「P5-D closure 時点の現在値は
     `maxQualifiedActiveVideoSources == 1` …」を P5-E closure 時点の `2 / 2 / 1` へ更新し、
     末尾の「二source/二layer は P5-E で … capability を引き上げる」を実績記述へ書き換える
   - 同 §7 に **layers の vector 順 = 背面→前面の z 順** を明記する
   - 同 §6 に removeSource の実装済み範囲 (audio authority の返却を含む) を追記する

### E4 実装結果 (実測)

- `docs/phase5-plan.md` §10.2の全要求をunit/product testへ突き合わせ、対応表を§10へ追加した。
  CTest configure時のliteral件数検査により、P5-E groupは0件ではなく**17件**であることも固定した
- ordinary CTestはucrt64-release **473/473 PASS**、ucrt64-debug **473/473 PASS**
- product regressionはP5-E **17/17 PASS**、P5-C **11/11 PASS**、P5-D **13/13 PASS**
- clean detached worktree (`bb65ea5`) でformal matrixを再走し、P2 **6/6 PASS**、P4 **3/3 PASS**
- P3-C-2再監査では、`bb65ea5`のformal matrix 2回がそれぞれ**8/9 PASS、1/9 FAIL**、
  未変更の第一親`06182a2`のformal matrix 2回がともに**9/9 PASS**だった。変更後のFAILはattempt 1の
  seek run 1におけるaudio underflow 1件と、attempt 2のpause-resume run 3におけるclock regression
  1件である。rawを別directoryに保持して後続PASSで上書きしない。この帰属を解消するまで
  P5-E closureは未確定とする
- 全raw/summaryとSHA-256 manifestは
  [`bench/results/p5-e4-closure-bb65ea5/`](../bench/results/p5-e4-closure-bb65ea5/README.md)
  に保存した
- `--seek-two-source`はseek完了後にもA/Bのper-source generation divergenceをassertする。
  Aのcompletion generationをBへコピーするmutationを、fixtureのseek時generation増分に依存せず落とす
- 製品契約§6/§7/§7.4を、実装済みremove範囲、layersの背面→前面順、closure capability `2/2/1`
  へ更新した

### ATTR-Q1 — first-failure runtime attribution

P3-C-2 regressionの修正候補を試す前に、既存の判定と実行条件を変えず、最初のfailureだけを
固定PODへ記録する診断を追加する。これは原因帰属のための計装であり、formal PASS authorityではない。

- audio underflowは発生QPC、mode/engine state/seek identity、要求sample範囲、同じqueue lock内の
  consume前・実消費・consume後sample数、source generation、audio master sample位置を記録する
- clock regressionは最初の発生箇所を`SchedulerProjectionInvalid`、`SchedulerDecision`、
  `DisplayProjectionInvalid`の3値で区別し、直前frame、候補frame、raw audio sample、scheduler target、
  current displayed、generation、seek/pause-resume contextを記録する
- どちらも0→writer→publishedの一方向stateで最初の1件だけを公開する。2件目で上書きせず、
  未発生時はJSONのsnapshotを`null`にする
- buffer/preroll、counter、failure条件、retry、clamp、threshold、run数、warmup/measurement、seed、
  checkerは変更しない

正式matrixのprefix順序を再現する診断runnerは次の2 profileだけを持つ。

```powershell
pwsh scripts/p5-e4-attribution-prefix.ps1 -Profile SEEK-PREFIX `
  -OutputDirectory <new-evidence-directory>
pwsh scripts/p5-e4-attribution-prefix.ps1 -Profile PAUSE-PREFIX `
  -OutputDirectory <new-evidence-directory>
```

- `SEEK-PREFIX`: playback×3の後にseek×1
- `PAUSE-PREFIX`: playback×3、seek×3の後にpause-resume×3
- 全processはwarmup 5秒、measurement 60秒、seed `20260808`、seek 1000回、display timeout
  3000ms、`--formal-contract-c2`を固定し、各rawへ既存checkerを実行する
- runner summaryは`authority = DIAGNOSTIC_ONLY`、`formal_pass_authority = false`、
  `formal_verdict = NOT_RUN`を常に記録する。prefixの全PASSをP3-C-2 formal PASSへ読み替えない
- output directoryが既に存在する場合は上書きせず失敗する。historical FAIL cohort
  `bench/results/p5-e4-closure-bb65ea5/`はimmutableのまま保持する

比較は同一計装を載せた`bb65ea5`由来と`06182a2`由来のclean exact SHAで行う。まずsnapshotから
failure siteと直前状態を特定し、その後にだけproduction fix候補を決める。診断中のprefix runや
failing mode反復は帰属材料であり、fix後のclosureには元のfull matrixを無変更で再走する。

### ATTR-Q2 — paired prefix reproduction

ATTR-Q1を変更せず、`bb65ea5`由来`b5e4c12`と`06182a2`由来`9793c13`へ同一patch
(`f301d8bb5fbb030845a480e2d9f982fcb943dd68`)を適用した。head→parentの順で
`SEEK-PREFIX`と`PAUSE-PREFIX`を各3 paired attempts実行し、12/12 prefix、78/78 processを完了した。

- head: underflow 0、clock regression 0。seek AV abs max 59.146msのFAILが1件
- parent: underflow 3、clock regression 0。3件ともseek ordinal 523、target 3892、generation 524、
  `WaitDisplay`、decode-ready / seek-pending / not-presented、queue 288/consumed 288/requested 480で一致
- parentには別にGPU teardown timeout 1件とpause中video advance 1件があった
- hardware/display provenanceは全processで不変

historical headのunderflow/clock regressionは再現せず、underflowはparent側だけで決定的に再現した。
したがってproduction selective revert候補は現時点ではなしとする。必要な次段はfixではなく、
seek 523付近のqueue supply margin、seek AV first-threshold、pause video first-advanceの追加診断である。
artifactは
[`bench/results/p5-e4-attr-q2-b5e4c12-9793c13/`](../bench/results/p5-e4-attr-q2-b5e4c12-9793c13/README.md)
に保存した。prefix結果はformal PASS authorityではなく、P5-E closureはBLOCKEDのままである。

### ATTR-Q2B — counterbalanced order

Q2と同じdiagnostic SHA / executableを再利用し、parent→headへ順序反転した最初のcampaignは、
attempt 1のparent全4 processとhead playback 3本の後にdisplayがlandscape 1920x1200から
portrait 1200x1920へ変化した。以後71 processはdisplay preflightでworkload開始前に拒否されたため、
このcampaignはorder attributionに使用しない。invalid evidenceは
[`bench/results/p5-e4-attr-q2b-invalid-display-9793c13-b5e4c12/`](../bench/results/p5-e4-attr-q2b-invalid-display-9793c13-b5e4c12/README.md)
へ分離し、将来の有効runで上書きしない。landscape 1920x1200復帰後に新しいartifact rootで再実行する。

operatorはこのcampaign中に外部displayを物理的に再接続していた。display provenance変化の有力な
環境要因ではあるが、historical campaignのINVALID判定は変更せず、production attributionにも使わない。

landscape復帰後の最初の再実行は、parent playback run 2が約12分終了せず、当該repo processだけを
停止した時点でrawが欠落したため中止した。不完全artifactは
[`bench/results/p5-e4-attr-q2b-invalid-teardown-hang-9793c13-b5e4c12/`](../bench/results/p5-e4-attr-q2b-invalid-teardown-hang-9793c13-b5e4c12/README.md)
へ分離した。

次の再実行は12/12 prefix、78/78 processを完了した。hardware、display geometry / orientation / DPR /
RHI targetは不変だったが、`screen_name`だけが2 processで`\\.\DISPLAY1`から`DELL U2412M`へ一時的に
変化し、直後に戻った。既存contractが`screen_name`も不変条件とするため、このcampaignもQ2B
attributionには使用しない。head / parentともordinal 523を含むaudio underflowは0、clock regressionも0
だった。別にparent pause-resume 1件で`pause_video_advance_zero=false`とAV projection failure 1件を
観測したが、invalid campaign内の症状として扱う。artifactは
[`bench/results/p5-e4-attr-q2b-invalid-screen-name-9793c13-b5e4c12/`](../bench/results/p5-e4-attr-q2b-invalid-screen-name-9793c13-b5e4c12/README.md)
へ分離した。

したがってcounterbalanced orderの4分類はまだ未確定であり、production selective revertやQ3-Aへは
進めない。次回も同じdiagnostic SHA / executable / fixture / P→H順序を使い、新しいartifact rootで
display provenance不変のcampaignを採取する。

#### ATTR-Q2B-R3 — counterbalanced retry

Q2Bの再採取はR3の1 campaignだけとする。formal workload開始前にdisplay-only probeを5回、2秒間隔で
実行し、約10秒にわたり次の現行P3-C-2 display signatureが連続一致した場合だけP→H campaignへ進む。

- `screen_name`、orientation、screen geometry、available geometry、DPR
- QQuickWindow / CompositorSurface logical size
- actual RHI target pixel size

probeは`CompositorRhiItem`だけを初期化し、media decode、WASAPI endpoint/session、formal checkerを
起動しない。probe executable / checkerのSHA-256、git commit / dirty state、各sample JSONをartifactへ
保存する。gate実装を含むmain worktreeと両diagnostic worktreeがclean exact SHAでなければ、probeも
formal workloadも開始せずfail-closedする。`screen_name`をcanonicalizeしたり不変条件から外したりしない。

```powershell
pwsh scripts/p5-e4-attribution-paired.ps1 `
  -HeadWorktree tmp/attr-q2-bb65 `
  -ParentWorktree tmp/attr-q2-parent `
  -OutputDirectory <new-r3-artifact-root> `
  -HeadBaseSha bb65ea5 -ParentBaseSha 06182a2 `
  -HeadDiagnosticSha b5e4c12d19009da98bcdbc58cd745b974d6515ea `
  -ParentDiagnosticSha 9793c13a5699752e25975102da44b11a8e999e0b `
  -PatchIdentity f301d8bb5fbb030845a480e2d9f982fcb943dd68 `
  -CohortOrder ParentHead `
  -DisplayPreflightExecutable build/ucrt64-release/bin/mvm_display_preflight_probe.exe
```

R3がprovenance-validならQ2とのorder comparisonを確定する。R3も`screen_name`だけでINVALIDなら
Q2B retryを終了して`ATTR-Q2C — Display Provenance Instability`へ移る。teardown hang等の別protocol
failureならidentityを保存してretryを終了する。R3確定前にQ3-A、selective revert、link/timing ablationへ
進まない。

R3実測ではdisplay-only preflightが5/5 PASSし、`DELL U2412M`、landscape、screen 1920x1200、
available 1920x1152、DPR 1、RHI target 1920x1080で連続一致した。formal P→H campaign開始後、parent
playback run 1 / 2は既存checker PASSだったが、run 3がrawを生成せず12.71分残存した。command lineで
campaign所属を確認したprocess treeだけを停止し、campaignをprotocol failureでINVALIDとした。final rawが
無いため内部の正確な停止stageは未確定であり、未観測のteardown fieldを補完しない。artifactは
[`bench/results/p5-e4-attr-q2b-r3-invalid-teardown-hang-9793c13-b5e4c12/`](../bench/results/p5-e4-attr-q2b-r3-invalid-teardown-hang-9793c13-b5e4c12/README.md)
へ保存した。

事前に定めたstop ruleにより、Q2B retryはR3で終了する。R3はorder attribution authorityを持たず、
Q2/Q2Bの4分類は未確定のままである。次はparent teardown/protocol hangの停止stageを切り分ける。
Q3-A、selective revert、link/timing ablationへはまだ進まない。

### ATTR-Q3-T0 — shutdown / protocol hang stage attribution

Q2B retryは終了し、parent formal-playback process hangの内部停止stageを診断する。optional
`--shutdown-stage-journal <jsonl>`指定時だけ、shutdown開始からfinished emission / event-loop returnまでの
blocking boundaryと`writeMetrics()`のsnapshot / mutex / `QSaveFile`境界をJSONLへ追記する。各entryは
sequence、QPC、process/thread ID、stageを持ち、Windows write-throughと`FlushFileBuffers`でdurableにする。
option未指定時のproduction behavior、timeout、buffer / pre-roll、threshold、counter、checkerは変更しない。

外部watchdogはparent clean exact diagnostic SHAでformalと同じplayback workloadを最大10回反復し、
180秒を超えたprocessについてjournal最終entry、thread snapshot、可能ならProcDump full dumpを保存する。
停止対象はwatchdog自身が起動したPID配下だけとする。hangを再現できなければT0未確定、journalが無ければ
fail-closedとし、少なくとも1つのblocking call / event-loop / metrics-write boundaryへ帰属できた場合だけ
T0の出口を満たす。

#### ATTR-Q3-T0 実測結果

parent diagnostic SHA `b0175dd781dffd741f238c5ee623f3bd81284c32` の1回目で180秒watchdogが
発火した。durable journalは`main.event_loop.exec.before`までの3件で、`shutdown.enter`、metrics生成、
finished emission、event-loop returnには到達しなかった。main threadは`Wait / UserRequest`だったため、
hangはshutdown内のworker stop / mutex / GPU teardown / metrics-writeではなく、shutdown開始前の
event-loop / `tick()`領域へ帰属した。T0の出口は満たすが、この証拠だけではtimer未発火、clock stall、
event-loop starvationを区別しない。

full dumpは保存できたが、この環境にWindows dump debuggerはなく、MSYS2 GDBは
`file format not recognized`としてWindows full dumpを認識しなかった。T0 artifactは
[`bench/results/p5-e4-attr-q3-t0-parent-b0175dd-r1/`](../bench/results/p5-e4-attr-q3-t0-parent-b0175dd-r1/summary.json)
に保存し、manifest 10件を再検証して不一致0件だった。

### ATTR-Q3-T1 — phase / audio-clock liveness attribution

T0 artifactを変更せず、optional `--liveness-sidecar <jsonl>`指定時だけ次をdurable JSONLへ記録する。

- timer開始と`tick()` first-entry / exit
- 全phase transition
- `openPipelines()`と`startAtFrame()`のenter / exit
- phase、audio clock、audio queue、decoder、WASAPI sinkの周期heartbeat

外部watchdogは、timer未発火、pipeline blocking、initial-start blocking、Warmup clock stall、
Playback clock stall、event-loop starvationの6分類を一本化したclassifierでfail-closedに判定する。
classifierのsynthetic testは6/6 PASS、target build、clang-format / architecture lint、短縮smokeを通過した。
option未指定時のproduction behavior、formal threshold / timeout、buffer / pre-roll、counter、checkerは
変更しない。

1秒heartbeatのclean diagnostic SHA `c85492440bd86607674dc1c13c4b980914ed0e04` は、独立した2 campaignで
各10/10 process exit 0かつP3-C-2 checker PASSだった。durable flushのtiming介入を減らすため、許可範囲内の
5秒heartbeatへ変更したclean diagnostic SHA `736c7510acd0c8b74d491c767d0ad4cdc4ef5fc5` も10/10 process exit 0かつ
checker PASSだった。3 campaign合計30/30でhangは再現せず、全manifestは各52件、不一致0件だった。

- [`T1 R1`](../bench/results/p5-e4-attr-q3-t1-parent-c854924-r1/summary.json)
- [`T1 R2`](../bench/results/p5-e4-attr-q3-t1-parent-c854924-r2/summary.json)
- [`T1 R3`](../bench/results/p5-e4-attr-q3-t1-parent-736c751-r3/summary.json)

したがってT1の6分類は**未確定**であり、T1の出口は未達である。30回の正常診断runはT0のhistorical hangを
無効化せず、formal closure evidenceにも使用しない。P5-E closureはBLOCKEDのままとする。

### ATTR-Q3-T2 — T0 full dumpの低摂動解析

T1反復は1秒heartbeat 20/20、5秒heartbeat 10/10の正常終了で打ち切った。これはhang解消やformal
closureの証拠ではなく、in-process durable sidecarによるprobe effectの可能性も残す。新しい計装を足す前に、
T0の既存full dumpをWindows Debugger 10.0.29617.1000でread-only解析した。

`~* k`ではmain threadが`NtUserMsgWaitForMultipleObjectsEx`からQt event dispatcher / event loop、
`QCoreApplication::exec()`へ至る通常待機だった。video decode worker 2本とaudio decode worker 1本は
condition variable待ち、QSG render threadは`QRhi::beginFrame()`配下のwaitで、CPU spinは無かった。
一方、全68 threadに`WasapiAudioSink::renderLoop()`は存在しなかった。

dump内objectをread-onlyで確認すると、controllerはphase 5 (`Playback`)、warmup reset済みだった。
audio master clockはrunning=trueのまま951,850 samples (19.830208秒)で停止し、formalの終了条件
2,880,000 samples (60秒)へ未到達だった。WASAPI sinkはopen=true、running=false、joined=false、
device failure count 1で、次のerrorを保持していた。

```text
WASAPI padding を取得できません: HRESULT 0x88890004
```

Windows SDK / MSYS2 UCRT64 headerでは`0x88890004`は`AUDCLNT_E_DEVICE_INVALIDATED`である。
`renderAvailable()`は`GetCurrentPadding()`失敗時に`recordFailure()`を呼んでfalseを返し、render loopは
終了する。`recordFailure()`はsinkのplaying / runningをfalseにするがcontrollerのfatal stateへ伝播しない。
formal Playbackの`tick()`はsink failureを検査せず、停止したclockがrequired samplesへ到達するのを待つ。

したがってT0 hangは**Playback clock stall**へ帰属し、causal chainは
`endpoint invalidation → render thread終了 → clock停止 → controllerがPlaybackで無期限待機`と確定した。
shutdown、GPU teardown、metrics writeのblocking hangではない。endpoint invalidationの外因やE3 changeへの
causal attributionは未成立だが、非同期WASAPI failureをtermination pathへ伝播しないshared-path liveness
defectは成立する。P5-E closureは引き続きBLOCKEDとする。

元のT0 executableは保存されていない。同じsource SHAからの参照rebuildはImageSizeが一致したが、
SHA-256は元の記録値と不一致なのでexact binaryとは扱わない。RVA / layoutはformal config列、controller
vptr、pointer先objectの値でも相互確認した。この制約を含む解析ログとsummaryは
[`bench/results/p5-e4-attr-q3-t2-t0-dump-b0175dd/`](../bench/results/p5-e4-attr-q3-t2-t0-dump-b0175dd/ATTRIBUTION.md)
へ保存した。959 MBのdump本体はlocal preservationとしGitから除外するが、T0 manifestのSHA-256は保持する。
既存dumpだけでT2出口を満たしたため、追加workload、live attach、atomic probeは実施しない。

T2後のscope correctionとして、これはPreviewEngine product runtime全体の欠陥ではなく、P3 frozen-gate
controller固有のfailure propagation欠落とする。PreviewEngine本体は既に
`WasapiSnapshot.deviceFailureCount != 0`を`audioFailure(...)`へ伝播している。historical bb65ea5 FAILは
保存するが、E3 causal regression attributionは成立していない。

### QUAL-F1 — P3 async audio failure fail-close

`P3AvSyncController`はactive phaseのphase switch前に既存sink snapshotの`deviceFailureCount`と
`lastError`をpollし、非同期WASAPI failureをfailure shutdownへ伝播する。sinkの`recordFailure()`、
counter、retry / reopen、clock、buffer / pre-roll、formal threshold / checkerのsemanticsは変更しない。
`ShutdownWait` / `Done`では再検出を行わず、terminal teardown pollingを継続する。

negative integrationはsinkがplayingになった後に既存`injectRenderFaultForTest()`を発火し、render thread、
`recordFailure()`、sink snapshot、controller tick、failure shutdownというT0と同じcausal pathを通す。
bounded process exit、device failure identity、formal pass=false、Playbackからのshutdown enter、metrics生成、
terminal teardownを固定する。controller側pollを削除するmutationではtimeoutすることを確認する。

実測ではnegativeは1.79秒でprocess exit 4、`deviceFailureCount=1`、injected failure identity、
Playbackからのshutdown enter、metrics生成、terminal teardownを満たした。controller側pollだけを削除した
mutationは同一UCRT64環境で15秒timeoutとなり、negativeがpoll欠落を検出することを確認した。poll復元後の
既存playback / seek / pause-resumeを含むtargeted integrationは4/4 PASSだった。

clean production candidate `b0103ba54dbc5da355279e8e4db712c451661886`でcanonical P3-C-2を
独立2 campaignとして再qualificationした。campaign Aは9/9 PASSで、git / executable / fixture /
hardware / display provenanceも不変だった。stop ruleに従ってcampaign Bへ進み、playback 3/3 PASS後の
seek run 1がprocess exit 4、producer pass=false、measurement audio underflow 1でFAILしたため、4/9で
停止した。final closure suiteは実行しない。

failureはseek ordinal 523、target frame 3892、requested sample start 3,119,840、request 480、
queue before 288、consumed 288、shortage 192 samplesだった。これはATTR-Q2 parent cohortと同一identityで、
device failure countは0、1000/1000 seeks、teardown、provenanceは正常だった。したがってQUAL-F1のasync
WASAPI fail-close regressionとは分類しないが、canonical formal FAILとしてP5-E closureをBLOCKする。
A PASSとB FAILは
[`bench/results/p5-e4-qual-f1-b0103ba/`](../bench/results/p5-e4-qual-f1-b0103ba/README.md)
へ分離保存し、再実行で上書きしない。

### ATTR-Q3-A1 — seek 523 EOF-tail attribution

QUAL-F1を変更せず、diagnostic SHA `abed99cff80844a26ac1858b8df30f0c6fd045e7`でcanonical
seek domain末尾のtarget 3890 / 3891 / 3892を各3回固定指定した。固定target seamは診断専用であり、
canonical seed、1000-seek workload、threshold、pre-roll、buffer、counter、checkerは変更しない。

target 3890と3891は各3/3でunderflowなし、target 3892は3/3で同一underflowを再現した。3892では
decoder EOF=true、actual decoded audio end=3,120,128 samples exclusive、requested start=3,119,840、
request=480、queue末尾=3,120,128、consumed=288だった。よって
`actualAudioEndExclusive - requestedStart == 288 == actuallyConsumed`が成立する。直前consume traceも
480 samplesずつ消費してqueue残量が1,728、1,248、768、288へ減る過程を記録した。これはproducer
starvationやqueue accounting discontinuityではなく、供給可能PCM自体がendpoint requestより192
samples短い**EOF-tail不足**である。

repo指定のUCRT64 ffprobeによる独立走査でも、最終audio frameのPTS 3,119,104 + 1,024 samplesから
同じ3,120,128 samples exclusiveを確認した。diagnostic campaignはformal PASS authorityではなく、
formal seek-domain、natural EOFのunderflow分類、audio-start/display marginのどれを修正するかは未決定とする。
したがって`QUAL-F1 implementation: PASS / requalification: FAIL / P5-E closure: BLOCKED`を維持する。
raw、summary、provenance、SHA-256 manifestは
[`bench/results/p5-e4-attr-q3-a1-abed99c/`](../bench/results/p5-e4-attr-q3-a1-abed99c/README.md)
へ保存し、後続runで上書きしない。

### QUAL-F2 — generation-scoped audio EOF classification

QUAL-F2では次をproduct contractとする。

- **Audio underflow**: current generationの有効なaudio PCM domain内で、endpointが要求したsampleを
  供給できなかったこと。
- **Terminal EOF silence**: current generationについてdecoderがdrain後に確定したauthoritative
  decoded endへ連続して到達し、その後の要求suffixをsilenceで満たすこと。underflowには数えない。
- EOS未確定、generation不一致、stale generationのEOS、EOSより前のgapはterminal silenceとせず、
  starvationとしてfail-closedに扱う。

`AudioFrameQueue`がgenerationとterminal sample exclusiveを一体で所有する。seekによるgeneration前進、
stop、restartは旧EOS authorityを破棄し、decoderがcurrent generationの実EOFを観測した場合だけ再確定する。
target 3892はcanonical seek domainに残し、seed、target generator、WASAPI buffer、100 ms pre-roll、
threshold、formal checkerは変更しない。

---

## 7. 検証手順

```powershell
pwsh scripts/build.ps1
pwsh scripts/test.ps1                      # ordinary CTest (release/debug)
ctest --test-dir build/ucrt64-release -L p5c # P5-C regression (11 本)
ctest --test-dir build/ucrt64-release -L p5d # P5-D regression (13 本)
ctest --test-dir build/ucrt64-release -L p5e # P5-E product test (17 本)
pwsh scripts/lint.ps1
```

frozen regression (E4 のみ、clean worktree で実行する):

```powershell
pwsh scripts/p2-matrix.ps1
pwsh scripts/run-p4-formal-matrix.ps1
pwsh scripts/p3-c2-matrix.ps1
```

各 slice の完了条件 (`docs/phase5-plan.md` §14 gate):

1. 新しい positive test
2. その検査が無ければ落ちる negative / fail-closed test
3. 変更 component に対応する targeted test
4. ordinary CTest
5. closure risk に応じた frozen regression (E4 で実施)

---

## 8. 注意点

- `docs/phase5-plan.md` §17: 「source count、resolution、DPR、audio count、adapter の現行 envelope を
  permanent API limit と書かない」。エラーメッセージへ `P5-E` のような slice 名や `2` の literal を
  埋め込まず、capability 値を参照した文言にする。
- 同 §17: 「test expectation を production helper から生成しない」。composition validator や
  `CheckedOutputTimebase` を呼んで期待値を作らない。
- fixture A は 271 MB / fixture B は 66 MB で、二 source 同時 decode は VRAM とデコード帯域を
  要求する。product test の `TIMEOUT` は P5-D の 60 秒より長め (90 秒) を初期値にし、
  実測後に締める。
- P5-E固有の二source/二layer検証は`apps/p5e_preview_smoke`へ閉じ込める。
  `apps/p5c_preview_smoke`はE3 capability `2/2`への最小追随として、二source目の受理後に削除して
  一layer検証を継続する。`apps/p5d_preview_smoke`のtransport semanticsは変更しない。
