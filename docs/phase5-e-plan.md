# P5-E 実装プラン — Product composition (二source / 二layer)

- 状態: **P5-E1 / P5-E2 / P5-E3 完了 / P5-E4実装済み・closure BLOCKED
  (P2-D5-2/W4-C3 3/3 EXACT CLOSED、canonical P2 corrective gate未実行)**
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
     `check-p2-contract.ps1` (P2-D5-2) を実行する
   - P2-D5-2 W3 canonical performance — `CanonicalPresentMonLive`のfresh 3 runから
     formal-v2 authorityでthresholdを評価する。`p2-matrix.ps1`のlegacy presentation metricsは
     diagnostic-onlyであり、これをcanonical PASSの代用にしない
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

### E4 closure再開監査 (2026-08-28)

P2-D5-2/W4-C3はcheckpoint `4e170fe`でformal 3/3すべて
`W4_C3_CAUSAL_REPLAY_EXACT`となり、`root_cause_determined=true`で**CLOSED**した。
actual causal chainは、completed refresh ordinalから導出したtargetがsource-domain terminal predicateを
成立させ、`DOMAIN_TERMINAL`がcapture gateを閉じたことである。

ただしW4-C3はdiagnostic-onlyであり、canonical performance authorityへ昇格しない。
したがってW3の`CANONICAL_PERFORMANCE_FAIL`（29.033 fps / drop 51.611%）を書き換えず、
W4-C3の3/3 EXACTをP5-E4のfrozen P2 PASSへ読み替えない。W4-C3以前のT2未採取は現在のblockerではなく、
後段W4で置き換えられたhistorical investigationとして保持する。

exit criteriaの再監査結果は次のとおりである。

| gate | 現在の判定 | 根拠 / 未実行事項 |
| --- | --- | --- |
| §10.2要求とpositive/negative testの対応、対象0件防止 | **充足済み** | 対応表とP5-E literal 17件検査を実装済み |
| 製品契約のcapability `2 / 2 / 1`、layer順、remove semantics | **充足済み** | `preview-engine-contract.md` §6 / §7 / §7.4を更新済み |
| historical P3-C-2 blockerの解消 | **充足済み** | QUAL-F2 checkpointで独立2 campaignが各9/9 PASS。ただしfinal checkpoint gateとは分ける |
| P2-D5-2 root-cause attribution | **充足済み** | W4-C3 formal 3/3 EXACT、root cause determined |
| P2-D5-2 canonical performance | **未充足** | W3 canonical verdictはFAILのまま。production correction、fresh W3 3/3、frozen threshold PASSが未実行 |
| current closure checkpointのordinary / P5-C / P5-D / P5-E | **未実行** | W4-C3 merge後の最終candidateでは未走行 |
| current closure checkpointのP2 correctness / Seek | **未実行** | correction後の`p2-matrix.ps1` Playback 3/3 + Seek 3/3が未走行 |
| current closure checkpointのP3-C-2 | **未実行** | correction後の`p3-c2-matrix.ps1` 9/9が未走行 |
| current closure checkpointのP4 | **未実行** | QUAL-F2 final suiteはP2 FAILでstopしたため、`run-p4-formal-matrix.ps1` 3/3が未走行 |
| final docs / artifact / manifest audit | **未実行** | final checkpointと全gateのSHA-256/provenance確定後に実施する |

現在のblockerは、W4-C3で確定したactual causal chainに対する**production correctionが未設計・未実装**で、
canonical P2 performance PASSが存在しないことである。W4-C3のscope外である「+1なら早期terminalを避ける」
というcounterfactualを、そのまま修正根拠にはしない。

closureは次の順序で再開する。今回は計画更新だけを行い、test/captureはまだ実行しない。

1. W4-C3のexact chainから、変更するproduct invariantと変更しないrequired/source domain、threshold、
   authorityを明文化し、修正が無ければ落ちるnegativeを先に固定する。
2. 最小のproduction correctionを実装し、targeted unit / architecture / dry-runで経路を閉じる。
3. clean release checkpointを固定し、ordinary CTestとP5-C / P5-D / P5-E product regressionを実行する。
4. 同一checkpointでP2 correctness/Seek 6/6と、fresh W3 canonical performance 3/3を実行する。
   authority/protocol/accountingがVALIDで、frozen 55 fps / 2% thresholdを全runで満たすことを要求する。
5. 同一checkpointでP3-C-2 9/9、P4 3/3を実行する。FAIL時はstopし、変更前後の複数runで帰属する。
6. raw、summary、provenance、manifestを不変保存し、`docs/phase5-plan.md` §10.4と本節を最終判定へ更新する。

#### P2-D5-2 B0 — Same-output Authority Attribution design

W4-C3 CLOSED後の最初のcorrective designとして、mixed output provenanceをsame-callbackでexact比較する
[B0設計契約](p2-d5-2-b0-same-output-authority-attribution.md)をfreezeした。capture、test実行、production
behavior変更はまだ行っていない。

static API inventoryでは、Windows 8.1以降`DwmGetCompositionTimingInfo`の`hwnd`は`NULL`必須で、
target HWND指定は`E_INVALIDARG`となる。したがってliteralな「HWND-bound DWM counter」はproduction候補として
static rejectionである。B0 schemaは非NULL呼出しを診断事実として保存するが、NULL fallback、nearest-QPC、
cadence tolerance、counter clamp、sequential ordinal `+1`で救済しない。

B0はNULL / HWND-attempt / existing W4-C3 sequenceをinvocation serialでjoinし、supportedな環境でのみ
HWND shadow ordinal / target / predicateをreplayする。`past_source_domain && required_intent_membership`を
successful measurement completionにしないproduct invariantと、`required_intent_count` / actual
`source_frame_count`のauthority分離も同設計へ固定した。代替product behaviorは未実装であり、
P5-E4は引き続きBLOCKEDである。

#### P2-D5-2 B1 — Target-output Physical Authority Attribution design

B0でHWND-bound DWM counterをstatic rejectionしたため、次の候補として既存target `IDXGIOutput` physical
VBlank observerをcurrent NULL DWM / actual scheduler ordinalと同一fresh captureで比較する
[B1設計契約](p2-d5-2-b1-target-output-physical-authority-attribution.md)をfreezeした。instrumentation、negative
test、capture、production behavior変更はまだ行っていない。

B1 amendmentでobserver sampleの命令位置を再監査した。sample QPCは`WaitForVBlank()`成功return後にobserver
threadが`QueryPerformanceCounter()`を実行した時刻であり、physical VBlank boundary timestampではない。
したがってcallback QPCを隣接observer wake QPCへhalf-open bracketしてcompleted physical ordinalを得る初版contractを
撤回した。nearest-QPC、cadence tolerance、midpoint、interpolationで救済しない。

current scheduler originはfirst committed swapの`frameSwapped` callback中に取得したpost-swap NULL DWM counterで
確立する。初版のfirst committed Presentをdisplayed physical ordinalへ後からjoinするoriginは別causal boundaryで
あるため、これも撤回した。pre-renderと同じfirst post-swap causal sample pointの双方でtarget-output counterを
直接得るsupported authorityが確立するまで、physical joinとshadow replayは`NOT EVALUABLE`である。

delta contractも「全点でtarget deltaがintent deltaと不一致」から、NULL sequenceのactual sequenceへの全点exact
一致、exact common domain上のtarget physical sequence非同一性（少なくとも1点差）、full shadow causal outcome
differenceへ修正した。現行observerでは後二者を評価できないため、現在のverdictはcandidate rejectionではなく
`TARGET_OUTPUT_PHYSICAL_NOT_EVALUABLE`である。

output migration、observer failure/overflow/regression、publication ambiguity、causal-boundary/join/coverage欠損は
fail-closeする。render callbackの`WaitForVBlank()`、QPC heuristic、counter clamp、sequential ordinal `+1`、
threshold変更、required-set縮小は禁止したままである。
`past_source_domain && required_intent_membership`をsuccessful completionにしないinvariantは継続してfreeze済みで、
代替product behaviorは未選択である。P5-E4は引き続きBLOCKEDである。

#### P2-D5-2 B2 — Supported Exact Target-output Counter Authority

B1 shadowを再有効化できるsupported counterを
[B2 static inventory](p2-d5-2-b2-supported-exact-target-output-counter-authority.md)で評価した。production、test、
capture、instrumentationは変更していない。

current QRhiはwindowed D3D11 `FLIP_DISCARD` swapchainで、public `QRhiD3D11NativeHandles`はdevice/contextだけを
公開する。一方、local Qt patchのnative Present hookはactual `IDXGISwapChain*`を所有thread上で取得できるため、
underlying objectへの到達自体は可能である。しかし`IDXGISwapChain::GetFrameStatistics`の`SyncRefreshCount` /
`SyncQPCTime`はAPI call時点ではなくschedulerが最後にmachine timeをsampleしたpairであり、pre-renderとfirst
post-swapのsame causal sampleを直接表さない。さらにMicrosoftがmulti-monitor等でstatisticsをunreliableと
明記しており、current workloadはsingle-monitor専用contractではないためstatic rejectionとした。

`IDXGIOutput::GetFrameStatistics`はfull-screen限定でcurrent windowed pathではunsupported、
`D3DKMTGetScanLine`はscanline/VBlank statusだけでcompleted countを返さず、
`D3DKMTWaitForVerticalBlankEvent(2)`はblocking waitでcount/boundary timestampを返さない。scanline transition、
observer wake QPC、nearest、derived cadence、sequential counterで救済しない。

全候補をstatic rejectionし、`EXACT_TARGET_OUTPUT_COUNTER_AUTHORITY_UNAVAILABLE`でB2を閉じた。B1 physical shadowは
`NOT EVALUABLE`のまま維持し、production correctionはtarget-output counter置換系統から、未設計の
`B3 Counter-free Required-intent Completion Correction`へ切り替える。P5-E4は引き続きBLOCKEDである。

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

fixed-target integrationだけは、exact display後もaudio callbackを観測する診断専用100 ms holdを使用する。
通常経路では0 msであり、canonical matrixのphase遷移やtimingを変更しない。total terminal silence量は
callback数に依存するため固定せず、first terminal shortageのidentityだけを検査する。

#### QUAL-F2実装・再qualification結果

clean production candidate `31eda0d8d080dcf4b1680149d85b8293f618cd57`で、current generationの
decoder drain EOFだけをqueueへpublishし、要求sampleからterminal endまで連続するsuffixだけを
`TerminalEof`へ分類した。WASAPIは従来どおりsilenceを出力するがunderflowを増やさず、terminal
silence callback/sampleを別counterへ記録する。EOS未確定、EOS前gap、stale generation、generation
resetのnegativeを含むqueue unitと雑なEOF-flag mutationを通した。

fixed target 3890 / 3891 / 3892は各3/3 PASSだった。3892は全3回でunderflow 0、exact identityを維持し、
first terminal shortageはPCM 288 / silence 192 / end 3,120,128で一致した。canonical P3-C-2は独立
campaign A/Bがともに9/9 PASSし、全provenanceも不変だった。ordinary CTestはrelease/debugともに
477/477 PASSである。よって`QUAL-F2 implementation: PASS / P3-C-2 requalification: PASS`とする。

ただしfinal closure suiteのP2 playback run 1はeffective 58.2343 fps、deadline drop 105、drop rate
2.9167%でformal FAILとなった。marker/probe/mixed/stale/device/lifecycle errorは0、teardownは成功した。
QUAL-F2差分にP2 compositor / gpu preview source変更はなくcausal attributionは未成立だが、formal FAILを
無効化しない。stop ruleによりP4 matrixは未実行で、P5-E closureは**BLOCKED**のままとする。raw、summary、
manifestは
[`bench/results/p5-e4-qual-f2-31eda0d/`](../bench/results/p5-e4-qual-f2-31eda0d/README.md)
へ保存し、後続runで上書きしない。

### P2-Q1 — deadline drop attribution

QUAL-F2 final closureのP2 FAILについて、historical rawを先に比較した。`bb65ea5`のplayback 3 runは
deadline drop 30～47、QUAL-F2 run 1は105だった。一方decoded A/Bは全runで各3,583、present callbackも
3,597～3,598であり、QUAL-F2 runではrepeated presentが103へ増えた。decode供給量ではなく、presentation
deadline付近のtiming差をleadとした。

Q1-Aでは`31eda0d`と`bb65ea5`をdetached clean worktreeから同一recipeでlink map付きbuildした。
archive member setは35対35で一致し、audio object 5件は両方の最終PEへlinkされていた。candidate `.text`は
4,384 bytes大きく、controller `tick()`、coordinator `compose()`、RHI `render()`のRVAは同一だったが、
GPU issue / output schedulerは`+0x3c0`移動した。binary/layout差は存在するが、この比較だけでは因果を
確定できなかった。

Q1-Bはformalと同じP2 playback条件を保ち、`C1→B1→B2→C2→C3→B3→B4→C4→C5→B5`で5 paired
attemptsを実行した。candidateは4/5 checker PASS、deadline drop min / median / max / meanが
38 / 62 / 73 / 57.2、baselineは3/5 PASS、51 / 68 / 77 / 66.4だった。両cohortに2%超のFAILがあり、
baseline平均はcandidateより悪かった。全10 runのdecoded A/Bは各3,583、callbackは3,596～3,598で、
deadline dropとrepeated presentが連動した。

したがってcandidate固有production regressionへのattributionは不成立で、selective revertへ進まない。
OS / GPU / Qt render-loop schedulingの外部timingを次のleadとする。このpaired campaignは診断専用であり、
formal PASSへ流用しない。P5-E closureは**BLOCKED**を維持する。raw、summary、binary/link evidenceは
[`bench/results/p5-e4-p2-q1-31eda0d-vs-bb65ea5/`](../bench/results/p5-e4-p2-q1-31eda0d-vs-bb65ea5/README.md)
へ保存した。

### P2-Q2 — same-binary pass/fail scheduling attribution

Q1でcandidate固有regressionが支持されなかったため、`31eda0d`の同一code imageを固定し、formal P2
playback条件を変更せず、process外部からWPR GeneralProfile / GPU / DesktopCompositionを採取した。
Q1実走後のsection抽出が元PEを再書き込みしていたためQ1 full-file hashは再利用不能だが、Q1と同一の
`.text` SHA-256を確認したPEをQ2用pathへ固定し、全runで同じfull-file SHA-256を検証した。

最大6 runまでFAILは発生せず、6/6 checker PASSだった。deadline dropは6 / 33 / 39 / 47 / 23 / 30、
平均29.7で、repeated present平均は27.5だった。Q1 candidateの平均57.2 / 54.6より両counterがともに
低下した。display refresh 59 Hz、GPU driver、AC状態、power planは全runの開始／終了で不変だった。

xperf trace integrityはrun 1～5が正常、run 6は51,773 events lostのため詳細解析対象外とした。FAIL traceが
ないため、render thread deschedule / DPC・ISR / GPU・Present waitのPASS/FAIL attribution出口は未達である。
heavy WPR profileのtrace perturbationを除外できないため、この6 PASSはformal evidenceへ流用しない。
production code、threshold、scheduler policyを変更せず、より低摂動のexternal observationへ進む。
P5-E closureは**BLOCKED**を維持する。raw、provenance、ETL hash、tracestatsは
[`bench/results/p5-e4-p2-q2-31eda0d/`](../bench/results/p5-e4-p2-q2-31eda0d/README.md)へ保存した。

### P2-Q3 — scheduler phase aliasing attribution

heavy ETWの摂動を避けるため、`31eda0d`由来のdiagnostic buildへ8192件の固定長POD ringを追加した。
render callbackではallocation、mutex、file I/O、loggingを行わず、single writerがcallback QPC、
scheduler deadline state、due / skip / output decisionだけを記録する。分類とJSON化はmeasurement停止後に行う。
production scheduler policy、threshold、formal checkerは変更していない。

clean diagnostic SHA `c800536caa12c90e1718c3db0aa0ab0368263486`から同一executable
（SHA-256 `327fb722ddb9bbbccace8190496eeb9af872c04807cb55df25a6f21c281a22f4`）を使い、
formal playbackと同じ5秒warmup / 60秒measurement / fence条件を反復した。5 run目でPASS/FAILの
両方が揃い、stop ruleに従って終了した。run 1～4はdrop 95 / 104 / 105 / 101、drop rate
2.6389%～2.9167%でFAIL、run 5はdrop 49、1.3611%でPASSだった。全runでprocess exit 0、ring
record数とpresent callback数は一致し、overflow 0、454 deadline dropの全件を分類できた。

集計は`PHASE_PAIR=155 (34.1%) / LONG_CALLBACK_GAP=15 (3.3%) / UNPAIRED_SKIP=284 (62.6%)`だった。
strictなPHASE_PAIR支配ではなく、UNPAIRED_SKIPが支配した。284件はすべて直前callbackがdue=trueで、
直前deadlineを既に通過していた。callback intervalは16.686～33.259 ms（median 19.343 ms）、直前の
latenessはmedian 15.429 ms、次callbackがskip境界を越えた量はmedian 0.806 msだった。したがって
33.333 ms以上のcallback消失が主因ではなく、late callbackの次も1周期より遅れてsynthetic deadlineを
わずかに跨ぐ`due → skip` transitionが主因である。PHASE_PAIRと合わせると454件中439件 (96.7%)が
2 slot未満のcallback cadence / phase transitionであり、minimal ETWへ進む根拠は支持されない。

このcampaignはdiagnostic-onlyで、run 5のPASSをclosure evidenceへ流用しない。次はthresholdを緩めず、
actual render opportunityを失った場合とsynthetic 60 Hz境界を跨いだ場合を分離するP2 scheduler / harness
semanticsの修正設計へ進む。P5-E closureは**BLOCKED**を維持する。raw、checker、summary、manifestは
[`bench/results/p5-e4-p2-q3-c800536/`](../bench/results/p5-e4-p2-q3-c800536/README.md)へ保存する。

### P2-Q4 — render opportunity contract proof

Q3 historical evidenceとP2-D5-1のFAILを変更せず、production runtimeへ接続しないpure/offline replayを
作成した。current replayは既存`OutputScheduler60Hz::takeDueBefore()`をQ3 ringの`scheduler_now_qpc`へ
適用し、callbackごとのdue / skipped count / output frameに加えてscheduled / displayed / deadline drop /
repeated / first frameをrawと照合する。Q3ではdue後のsource不足repeatが0件だったため、ringだけで
displayedとrepeatedも欠損なく再現できる。

candidateはnominal 60 Hz deadline間のmidpointで時間軸を非重複slotへ分割し、midpoint tieを後続slotへ
決定的に割り当てる。exact 60 Hz、slot内jitter、callback 1本欠落、multi-slot gap、duplicate callback、
midpoint tie、measurement end、burst callbackの8 synthetic scenarioを固定した。1本欠落はexactly 1 drop、
3本欠落は3 dropとなり、duplicate / burstで欠落を隠せないことを確認した。threshold 2%はoffline proofの
exit criteriaに使用していない。

clean diagnostic SHA `75fdb09`でQ3の5 traceをreplayし、current algorithmは5/5でrawを完全再現した。
candidateも全runでscheduled=3600、frame 0開始、identity strictly increasing、measurement `[start,end)`を
満たした。しかしdropはrunごとに`95→98 / 104→109 / 105→91 / 101→70 / 49→44`で、合計も
`454→412`に留まった。run 1 / 2では悪化しており、static nearest-slotはQ3 aliasingを安定して除去せず、
「actual missing opportunityに相当する少数drop」へ収束しない。

したがってnearest-slot候補によるP2-D5-2 contract correctionは**提案しない**。Q4はcurrent replay
authorityと一般counterexampleを確立したが、candidate semanticsの採用根拠は未成立である。
`OutputScheduler60Hz`のruntime利用は`CompositorRhiItem`のvideo-master branchで、P3 audio-master時計とは
別だが、採用候補が成立するまでproduction componentの変更範囲は決めない。minimal ETWへも戻らず、次は
固定synthetic deadlineへの最近傍量子化ではなく、実際のrender callback opportunity列からlossを定義できるかを
追加proofする。P5-E closureは**BLOCKED**を維持する。結果は
[`bench/results/p5-e4-p2-q4-75fdb09/`](../bench/results/p5-e4-p2-q4-75fdb09/README.md)へ保存する。

### P2-Q5 — presentation opportunity authority proof

Q4 evidenceとP2-D5-1 historical FAILを変更せず、diagnostic flag時だけQt `frameSwapped` hookと
render completionを別々の8192件fixed ringへ記録した。render hot pathではallocation、mutex、file I/O、
loggingを追加しない。render ordinal、selected/submitted frame、post-render QPCをswap ordinal、最後に
submittedされたframe identityへ関連付ける。flagなしではsignal connectionと追加raw fieldを作らない。

presentation clock authorityは対象windowのmonitorを`MonitorFromWindow`で特定し、`QueryDisplayConfig`の
active pathからexact refresh rationalを取得した。DWM composition timingのrefresh rational、VBlank QPC、
refresh periodもmeasurement開始前／停止後に保存する。実測したmodeは整数59 Hzではなく、両runとも
`59950/1000 = 59.95 Hz`だった。

clean diagnostic SHA `2be57c6fdd94a402d9251abbd55feccc14b6c33b`、executable SHA-256
`45c9063868581a7bb7a1b1908913d68d0565e4034c60aebd9d3b0aae5dfe4262`でformal playback条件を
反復し、run 2でPASS/FAILが揃ったためstop ruleに従って終了した。run 1はcallback / render / swapが
3596 / 3596 / 3596、actual presentation opportunities 3597、unique presented frames 3532、synthetic
deadline drop 68でformal PASSだった。run 2は3598 / 3598 / 3598、opportunities 3597、unique frames
3510、synthetic drop 90でformal FAILだった。全ring overflowは0、render/scheduler/swap identity対応は
完全で、`AMBIGUOUS=0`だった。

各skip時の隣接swap QPCをexact display refreshへ投影し、60 fps media opportunity間隔に対して本当に
present機会を失ったか分類した。合計158 synthetic deadline dropsのうち
`FALSE_DEADLINE_SKIP=148 (93.7%) / TRUE_OPPORTUNITY_LOSS=10 (6.3%) / AMBIGUOUS=0`だった。
Q3で支配した`UNPAIRED_SKIP`型に限っても101件中94件がfalse、7件がtrueだった。特にformal FAILの
run 2は90 drop中87件がfalseで、actual loss対応は3件だけだった。

したがって、現行counterはactual presentation opportunity lossではなくcallback begin QPCとsynthetic
60.000 Hz deadlineのcrossingを主に測っていることが実present authorityから確認できた。
`FALSE_DEADLINE_SKIP`支配の出口を満たすため、次はhistorical P2-D5-1 FAILを保持したまま、threshold 2%を
変えずにP2-D5-2 contract correctionとscheduler/harness fixを設計する。Q5 diagnostic PASSをclosureへ
流用せず、P5-E closureは**BLOCKED**を維持する。raw、classification、summary、manifestは
[`bench/results/p5-e4-p2-q5-2be57c6/`](../bench/results/p5-e4-p2-q5-2be57c6/README.md)へ保存する。

### P2-Q6 — opportunity-ordinal scheduler proof

Q5 evidenceとP2-D5-1 historical FAILを変更せず、productionへ接続しないpure/offline schedulerを
追加した。actual swap間隔を開始時と終了時で不変なexact refresh rationalへ整数演算で投影し、最初の
swapをopportunity ordinal 0として以後のordinalを構成する。source targetは
`floor(ordinal * sourceFpsNumerator * refreshDenominator /
(sourceFpsDenominator * refreshNumerator))`で決める。同じtargetはrepeat、targetが2以上進む場合は
中間source frameをtrue drop、frame 3600以降はsource domain外として表示しない。

60 / 59.95 / 120 / 30 Hz、actual opportunity 1件欠落、連続2件欠落、duplicate / burst、measurement
start / end境界の15 scenarioを固定した。refresh rational変更、DWM authority欠損・discontinuity、
render↔swap対応欠損、midpoint ambiguity、ordinal regressionは救済せずfail-closedにする。15/15がPASSし、
既存Q4 unitと合わせて2/2 CTestがPASSした。threshold 2%はproofのexit criteriaに使用していない。

Q5のimmutable 2 traceをreplayした結果、run 1は`displayed=3582 / trueDropped=18 / repeated=14`、
run 2は`3585 / 15 / 12`となった。両runでframe 0開始、unique frame strictly increasing、frame 3600
非表示、`displayed + trueDropped = 3600`を満たした。Q5の`TRUE_OPPORTUNITY_LOSS`は現行synthetic
skip発生地点だけを分類した値なので、Q6の全ledger accountingとは直接同値ではない。差は次の恒等式で
全数を説明した。

```text
Q6 trueDropped
  = complete ordinal列に固有のcadence/domain loss
  + Q5 TRUE_OPPORTUNITY_LOSS at synthetic skip sites
  + synthetic skip地点以外のordinal gap source loss

run 1: 18 = 5 + 7 + 6
run 2: 15 = 3 + 3 + 9
```

したがってQ6 proofは成立し、P2-D5-2 contract correctionを提案できる。D5-2はP2 formal playback
harness固有のpresentation-opportunity schedulerとし、P3 audio-masterを含む共有時計へ波及させない。
historical P2-D5-1 FAILは再分類せず、D5-2でもdrop threshold 2%を維持する。このQ6 checkpoint時点では
offline proofのみであり、D5-2 runtime / checkerは未実装、P5-E closureは**BLOCKED**のままである。
clean checkpoint後の再現とimmutable artifact生成には次を使う。

```powershell
pwsh scripts/build.ps1 -Target mvm_test_p2_opportunity_ordinal
pwsh scripts/build.ps1 -Target mvm_p2_opportunity_ordinal_replay
pwsh scripts/p5-e4-p2-q6-opportunity.ps1 `
  -OutputDirectory bench/results/p5-e4-p2-q6-<clean-sha>
```

### P2-D5-2 — presentation-opportunity formal contract / harness fix

Q6 clean SHA `0b5463b56f35d393ca4eb4489e30ed21b73a5cc4`からrunnerを再実行し、synthetic
15/15、Q5 trace 2/2 PASSをmanifest付きartifactとして
[`bench/results/p5-e4-p2-q6-0b5463b/`](../bench/results/p5-e4-p2-q6-0b5463b/README.md)へ固定した。
このevidence-only commitとD5-2実装は分離している。

D5-2ではraw schemaを`mvm-p2-formal-2`、contractを`P2-D5-2`へ更新する。formal Playback専用の
`PresentationOpportunityScheduler`は、直前に完了したswap QPCとexact display refresh rationalだけから
次のopportunity ordinalをrender前に決定する。`frameSwapped`でactual ordinalとrender対象を照合してから
ledgerへcommitするため、未来のswapを使うretrospective再分類ではない。shared `OutputScheduler60Hz`と
P3 audio-master schedulerは変更しない。旧synthetic deadline schedulerはframe selectionから外し、
`diagnostic_synthetic_deadline_drop_count`としてのみ残す。

raw ledgerの各recordにはopportunity ordinal、swap QPC、refresh rational、expected/presented source frame、
repeat、直前gapのtrue dropを保存する。checkerはproducer summaryを信じず、整数rational mapping、unique、
repeat、gap、tail、`displayedUnique + trueDrop == required domain`、2% thresholdをrawから再計算する。
render↔swap欠落・不一致、ordinal/QPC regression、midpoint ambiguity、overflow、refresh/DWM authority変更は
contract/runtime failureとしてfail-closedにする。historical P2-D5-1 FAILはD5-2 PASSへ再分類しない。

pure schedulerは60 / 59.95 / 120 / 30 Hz、単一・連続opportunity欠落、duplicate callback、tail、
correspondence欠落・不一致、regression、overflowの12分類を検査し、12/12 PASSした。checkerはraw ledger
1件のpresented frame改竄、ordinal gap無視、refresh改竄、authority failure、tail改竄をnegativeとして持つ。
targeted live dry-runは59950/1000 HzでPlayback / Seek各1/1 PASSだった。Playbackはledger 119件、
`unique=119 / trueDrop=1 / tail=1 / scheduled=120`でauthority `NONE`、domain conservationが成立した。
current worktreeでordinary CTestはRelease 485/485、Debug 485/485が通過し、lint / architecture / layer
isolation / PSScriptAnalyzerもPASSした。
これは経路検証でありformal closure evidenceではない。clean production SHAのP2 6/6とfinal closure suiteは
まだ未実行なので、P5-E closureは引き続き**BLOCKED**である。

### P2-D5-2 / F1 — Causal Opportunity Reconciliation

`3b6818a`を対象に取得した`bench/results/p2-d5-2-formal-3b6818a/`は、出力artifact自身が
worktree provenanceを変更したため、formal authorityとしては **acquisition: PROTOCOL_INVALID** とする。
一方、Playback run 1でcommitted ordinal 102の後に観測した`RENDER_SWAP_MISMATCH`はruntime
implementation blockerであり、無効化も後続runによる上書きもしない。この履歴artifactはevidence-only
commit `dbc167a`で固定済みである。Playback run 2/3、Seek、closureは実行していない。

F1ではrender時点のpredicted ordinalをtentativeとし、swap到着時のactual ordinalを因果authorityとして
reconcileする。`actual == predicted`はexact commit、`actual > predicted`かつDWM authorityが連続している
場合はforward opportunity lossとしてcommitし、lost opportunityを記録してactualから次状態をrebaseする。
render済みsource frameはpredicted targetのままledgerへ記録し、次のunique presentationまたはtailで未表示の
source-domain gapをtrue dropへaccountする。`actual < predicted`、DWM discontinuity、render/swap pairing不明は
fail-closed fatalを維持する。

raw ledgerとfirst-eventにはlast committed / predicted / actual ordinal、render begin/end / swap QPC、
pre-render / post-swap DWM refresh count・qpcVBlank・refresh rational、predicted / actual target / rendered
source frame、render / swap ordinal、authority continuityを保存する。checkerはsummaryを信頼せず、QPCから
predicted/actual ordinalを再計算し、forward loss、source-domain gap、repeat、tailを独立に検査する。

fatal中のmeasurementはteardown前にrender threadへstop snapshotを要求して採取する。開始・停止の有無と
利用可能性を明示し、取得済み差分が負ならartifactを利用可能と扱わない。負値の0 clampは行わない。
non-dry formalのOutputDirectoryはgit worktree配下を拒否し、既定値もOSの一時directory配下とする。

F1 implementation reviewとbounded diagnostic liveが完了するまで再formalは行わない。diagnosticの出口は
`actual>predicted + continuous`、regression、authority discontinuity、pairing defectのいずれかへ一意化し、
その後にclean production SHAから外部artifact rootでPlayback run 1から再取得する。

[事実] 2026-08-22にformal matrixではないbounded Playback diagnosticをworktree外へ4本取得した。
測定時間は2秒、3秒、3秒、10秒で、ledgerはそれぞれ118、179、179、599 recordだった。全runで
`formal_opportunity_error=NONE`、`formal_forward_reconciliation_count=0`、
`formal_first_reconciliation_event.classification=NONE`、measurement stop snapshot利用可能、dry-run checker
PASSだった。historical blocker位置のordinal 102を全runで越えたが、今回のbounded windowではdivergence
自体を再現しなかったため、A〜Dのmismatch分類は発生していない。これはformal evidenceではなく、旧fatal
条件を除去したruntime pathの経路確認に限る。

[事実] `completion_fatal` negativeではmeasurement開始後にexit 3となり、stop snapshotは採取済み、
`measurement_available=true`、elapsed 0.0333572秒、全measurement count差分は非負だった。未開始または
snapshot不成立時のdelta fieldは0へclampせずJSON `null`として出力する。

[事実] runnerのnon-formal dry-runはPlayback 1/1、Seek 1/1、各contract、matrix中のsource fingerprint
不変をすべて満たし、`dry_run_harness_pass=true`、`p2_pass=false`だった。F1対象のpure/checker/output-path
contractは44/44、ordinary suiteはRelease 489/489、Debug 489/489、lint・architecture・layer isolation・
PSScriptAnalyzerがPASSした。再現には次を使う。

```powershell
pwsh scripts/p2-matrix.ps1 -DryRun -StopOnFailure -OutputDirectory build/p2-f1-matrix-dry
ctest --test-dir build/ucrt64-release -R "p2_(presentation_opportunity_scheduler|formal_checker|formal_output_path)"
ctest --test-dir build/ucrt64-release -R compositor_qt_completion_fatal_fail_closed
pwsh scripts/test.ps1
pwsh scripts/lint.ps1
```

### P2-D5-2 / F2 — Refresh-Count Anchored Opportunity Commit

[事実] clean production SHA `5ebedc2f`から再buildし、worktree外
`p2-d5-2-formal-5ebedc2-20260822-142634`へcanonical P2 matrixを`-StopOnFailure`で実行した。
Playback run 1が`process_exit_code=3` / `contract_exit_code=3`で停止し、run 2/3とSeek 3 runは実行していない。
`provenance_unchanged_during_matrix=true`、`dirty_worktree=false`、`p2_pass=false`である。runtime blockerは
`formal_opportunity_error=AMBIGUOUS_OPPORTUNITY`、`formal_opportunity_authority_valid=false`で、7本目の
compositionで中断した。ledgerは6件、`displayed_composition_count=7`だった。直前recordはordinal 6の
`FORWARD_OPPORTUNITY_LOSS`（authority continuous、render 82,734 tick、swap間隔281,619 tick = 1.688 refresh）で、
次のswapが半refresh未満で着地した。このrunは**P2-D5-2 formal FAIL**として不変保存し、F2以後のPASSで
書き換えない。

F1の残存bugは、opportunity序数を`roundedRefreshIntervals(swapQpc - lastSwapQpc)`でinterval毎に丸めた点にある。
`round(1.688)=2`、`round(<0.5)=0`となり、refresh phaseの残差がinterval毎の丸めで捨てられる。これは
presentation authorityの喪失ではなく、Q6が正常scenarioとして証明済みのduplicate / burstとも整合しない。

F2ではQ5のDWM refresh count / VBlank mappingをpure helper
`src/media/gpu_preview/presentation_refresh_authority.h` へ抽出し、opportunity序数の一次authorityを
refresh countにする。`ordinal = refreshCount - originRefreshCount`であり、丸めも独自の`+1`規則も持たない。
originは最初のswapのpost-swap refresh countで固定する。QPCはrender/swapのcontinuity cross-checkと
diagnosticsに限定し、序数の根拠にはしない。

`frameSwapped`ごとに即finalizeせず、refresh opportunityごとにpending latest candidateを保持する。

```text
actual > pending   直前pendingをlatest candidateでfinalizeし、間のopportunityをlossとしてaccountする
actual == pending  同一opportunity内の追加swap。ambiguousではなくsupersedeとして最新candidateを保持する
actual < pending   regression fatal
authority discontinuity / render↔swap pairing不明   fatal
```

pre-render予測も`lastSwapQpc + T`ではなくpre-render DWM authorityから作る
（`predicted = (preRenderRefreshCount - origin) + 1`、最初のrenderのみopportunity 0）。measurement endでは
pending opportunityをfinalizeしてからtail accountingへ入る。counterは
`swapped composition` / `finalized opportunity` / `unique displayed source frame`を分離し、
`frameSwapped` countとformal `displayed`を同義にしない。

ledgerとsummaryには`last_finalized_opportunity_ordinal`、`superseded_candidate_count`、
`forward_reconciliation`、`formal_opportunity_origin_refresh_count`、`formal_swapped_composition_count`、
`formal_finalized_opportunity_count`、`formal_superseded_candidate_count`を追加する。checkerはproducer
summaryを信じず、記録されたrefresh count sampleとoriginだけからpredicted / actual ordinalを再計算し、
swap ordinalの連続性を`previous + 1 + superseded`で検査し、supersede、loss、repeat、source gap、tailを
独立に再計算する。checkerからは丸め規則（`Rounded-Intervals`）を削除した。

D5-2 Playbackでは`measurement_displayed_composition_count`をswapped composition数と等値で検査する。
59.95 Hz / 60 fpsではopportunityごとにtargetが必ず前進するためrepeatは発生せず、描画しないcallbackの
swapが混ざればこの等式が崩れてfail-closedになる。repeatやduplicate callbackが実測で現れた場合は
契約違反として扱い、等式を緩めない。

[事実] 今回観測した`1.688T → <0.5T`列をdeterministic regressionとして固定した。同一refresh countの
2 swapがordinalを前進させないこと、latest candidateのみをfinalizeすること、refresh count `+1` / `+N`、
count regression、qpcVBlank discontinuity、refresh rational変更、render/swap pairing欠落、swap ordinal
飛び、measurement endのpending finalizeを`mvm_test_presentation_opportunity_scheduler`で検査する。
checker側は`SupersededGood`を対照群とし、`NegativeOpportunitySupersededSwapOrdinal`（同一opportunityを
新ordinal扱い）、`NegativeOpportunityFirstCandidate`（first candidate保持）、
`NegativeOpportunityDisplayedCount`（displayedをfinalized opportunity数と同義化）、
`NegativeOpportunityOrigin`（origin ずらし）で1 fieldずつ壊して検出を確認する。

### P2-D5-2 / F3 — Window-Output VBlank Authority

[事実] clean production SHA `906c5ed5` (A2) から、worktree外の新規root
`p2-d5-2-formal-906c5ed-20260822-160910`へcanonical P2 matrixを`-StopOnFailure`で実行した。
Playback run 1が`contract_exit_code=3`で停止し、run 2/3とSeek 3 runは実行していない。
`provenance_unchanged_during_matrix=true`、`p2_pass=false`である。**runtime blockerは無い**。
`formal_opportunity_error=NONE`、`formal_opportunity_authority_valid=true`、finalized
opportunity 1743件、superseded 0、repeat 0、`displayed unique + true drop = 1743 + 1857 = 3600`
で、checkerは1743 record全てのordinal・source target・repeat・loss・swap ordinal連続性・tailを
記録済みrefresh count sampleとoriginだけから独立再計算し、producerと一致した。唯一のcontract
失敗は`drop_rate=0.5158`である。このrunは**P2-D5-2 formal FAIL**として不変保存し、F3以後のPASSで
書き換えない。

原因はF2のauthority binding誤りである。measurement 29.107秒に対しswapは1743本 (平均間隔16.68 ms、
59.98 presentations/s) と健全だったが、序数に使う`DwmGetCompositionTimingInfo(NULL)`の`cRefresh`は
同区間で3595 tick進み、約123.7 Hzだった。ordinal→source frameの写像は
QueryDisplayConfigの59950/1000のままなので、1 swapごとに序数もtargetも約2進み、3600 frameのsource
domainが29秒で尽きた。序数のauthority (DWM composition clock) とrateのauthority (windowが載る
monitorのdisplay mode) が別物だった、という配線の欠陥である。

Option 1 (DWMの`cRefresh`と`rateRefresh`をセットで使う) は採らない。数学的には自己整合するが、契約が
測る対象が「このwindowが実際に表示できたpresentation opportunity」から「DWM compositor clockの全tickに
このwindowが新frameを出したか」へ変わる。またWindows 8.1以降`DwmGetCompositionTimingInfo`のhwndは
NULL必須で、window固有のmonitor timingは取得できない。したがってDWM cRefresh / rateRefreshは
**diagnostic-onlyへ降格**する。

F3ではformal authorityを次の三点へ一本化する。

```text
display identity : windowのHMONITORに一致するDXGI output
rate authority   : その同じoutputのQueryDisplayConfig exact rational
ordinal authority: その同じoutputのphysical VBlank sequence
```

#### F3-A: authority probe

schedulerを変更する前に、production-connectedなdiagnostic probe
`apps/p2_vblank_authority_probe`を先に入れた。windowのHMONITORからDXGI outputを特定し、同じoutput
専用threadで`IDXGIOutput::WaitForVBlank`を回してVBlank sequenceだけをfixed ringへ記録する。
判定はwindow output側だけで行い、DWMの値は併記するだけで使わない。

pure helperは`src/media/gpu_preview/window_output_vblank_authority.h`に置いた。VBlank列の単調性、
preflight cadence整合 (観測VBlank数と 経過QPC×rational の差が1 VBlank以内)、swapを
`V_k.qpc <= swapQpc < V_(k+1).qpc`で一意にbracketする関数を含む。上側VBlank未観測のswapは
解決しない (`AfterLast`)。

[事実] 60秒のprobeを2条件で取得した。observer threadが通常優先度のときは3595 sample、
観測59.8986 Hz、公称周期の1.5倍以上へ伸びたintervalが12本、最短interval 47570 tick (0.29周期) で、
`WaitForVBlank`のwake遅れによる取りこぼしが起きていた。ordinalは自前counterなので連続性検査では
検出できず、interval側でのみ検出できる。observer threadを`THREAD_PRIORITY_TIME_CRITICAL`にすると
3598 sample、3597 interval、観測**59.9502 Hz**、長すぎるinterval 0本、interval範囲は
163038〜171428 tick (公称166805 tickの0.977〜1.028倍) となり、60秒窓全体でも
QueryDisplayConfig rationalと1 VBlank以内で一致した。

[事実] 同じprobe実行でのDWM composition clockは、idle windowで`cRefresh`が5秒に1 tick
(約0.0167〜0.2 Hz) しか進まなかった。A2 formal run中は約123.7 Hzだった。すなわちDWMの`cRefresh`は
window outputのVBlank数ではなく、composition活動量で変わる。59.95 Hzとは両極 (0.02 Hz / 123.7 Hz)
で乖離しており、formal ordinal authorityには使えないことが実測で確定した。

probeのauthority-validity条件は次の通りで、新しい性能thresholdは導入していない。

```text
VBlank sequence status = OK
observer ring overflow = 0 / WaitForVBlank failure = 0
公称周期の1.5倍以上のinterval = 0
preflight窓 (120 VBlank) のcadenceがrationalと1 VBlank以内で一致
window output identity (HMONITOR / DXGI output / adapter LUID / GDI名 / rational /
desktop座標) がstart-endで不変
```

full窓のcadence差はpanel実cadenceと公称値の差を含むためdiagnosticとして併記する。

checkerは`scripts/check-p2-vblank-authority.ps1`で、producerの真偽値を信じずpreflight窓のdeviationと
toleranceを生の観測値から再計算する。contract testは対照群と、1 fieldだけを壊した8 negative
(probe_pass、output identity変更、sequence status、long interval、wait failure、ring overflow、
約123.7 Hz cadence、deviation再計算不一致) に加えて、**DWM clockがwindow outputと乖離していても
probeは通る**ことを固定する`DwmClockMismatchIsDiagnosticOnly`を持つ。

#### F3-B0: shadow mapping proof

F3-Aが証明したのはobserverのcount/cadenceがwindow outputのrationalと一致することであり、
個々のswapがどのphysical VBlank opportunityへ属するかまでは示していない。`WaitForVBlank`の
return時刻はhardware VBlankそのものではなくthreadが実行された時刻なので、production scheduling
を変更する前にshadow modeで対応を証明する。

compositor spikeへ`--vblank-observer`を追加し、measurement開始時にwindow outputを解決して
observerを起動、measurement停止後はwindowを延長せずobserverだけbounded drain (最大100 ms) して
最後のin-window swapのupper bracketを取得してから停止する。physical VBlank列はformal logicへ
一切入力せず、raw JSONの`presentation_opportunity.physical_vblank`として保存するだけである。

[事実] 60秒のshadow runを3本独立に取得した。3本とも
`time_critical_priority=true`、`window_output_stable=true`、`sequence_status=OK`、ring overflow 0、
wait failure 0、公称周期の1.5倍以上のinterval 0本、0.5倍未満のinterval 0本、累積progressionは
rationalと1 VBlank以内 (最大でも0.011 VBlank / 60秒) だった。

```text
run1  VBlank 3605  swap 3597  mapped 3591  same_opportunity 6
run2  VBlank 3604  swap 3596  mapped 3594  same_opportunity 2
run3  VBlank 3604  swap 3593  mapped 3588  same_opportunity 5

全run  ambiguous 0  observer_gap 0  before_first 0  after_last 0
```

全swapが`V_k <= swapQpc < V_(k+1)`で一意に解決した。`same_opportunity`は同一physical VBlank内の
追加swapであり、F2で実装済みのpending/latest-candidate semanticsが扱う対象である。
`after_last=0`はbounded drainが機能していることを示す。

authority validityはF3-Aの`>=1.5T`に加えて`<0.5T`も無効とする。通常優先度で観測した0.29Tのような
short intervalは、直前のwakeが遅れて隣接VBlankと断定できないことの証拠だからである。これは性能
thresholdではなくmidpoint criterionである。あわせてorigin基準の累積discrepancyも記録する。
self-counterはmissed VBlankを自分では知れないため、intervalと累積の双方を残す。

#### F3-B0.6-R1: Complete Present-ID Oracle

[事実] 既存のpresent identity probeは900回の成功Presentに対して742件程度の
`GetFrameStatistics` transitionしか採取できていなかった。`GetFrameStatistics`は最後のrender frameの
統計を返すため、この差だけから未観測Presentを`FLIP_DISCARD`によるdiscardへ帰属できない。
従来artifactは不完全なoracleとして保持し、mapperの成否判定には使わない。

[回避策] swap chain条件は`FLIP_DISCARD / BufferCount 3 / SyncInterval 1`のまま変えず、成功した各
`Present(1, 0)`の直後に`GetLastPresentCount`を取得する。raw recordは
`submission_index / present_id / render_end_qpc / present_return_qpc`を持ち、Present IDがexact `+1`で
連続することを要求する。

statistics samplerはF3-Aと同じphysical VBlank observerのring transitionで駆動し、各VBlank後に
公称周期の3/4を
上限とするbounded high-rate pollで`PresentCount / PresentRefreshCount / SyncRefreshCount /
SyncQPCTime / poll_qpc`の全transitionを記録する。poll間隔が0.5周期以上、PresentCountの飛び、disjoint、
最終submitted Present IDまでのdrain未完了は`ORACLE_SAMPLING_GAP`としてrunをINVALIDにする。
`DwmFlush` fallbackは最初のrunでは使わない。

oracle-validityはsubmitted Present ID列と、同じID範囲のstatistics transition列が同順序・同件数で
一致し、最後の`PresentCount`が最後のsubmitted IDと一致した場合に限る。checkerはproducerの集計値を
信じずraw二列から再計算する。notification delayの0 / 0.1T / 0.3T / 0.8T / 1.2Tはlive processへ
入れず、Present完了後に`notify_qpc = present_return_qpc + synthetic_delay`としてJSON上だけで生成する。

[exit] 完全oracleが取得できるまでは`mapper proof = NOT YET EVALUABLE`、
`FLIP_DISCARD frame discard claim = NOT ESTABLISHED`、F3-B1は未開始、P2-D5-2はBLOCKEDとする。
完全oracle取得後も`submission order + delayed notify QPC + physical VBlank ring`だけで900/900 exact identityを
再構成できない場合はmonotone/backfill heuristicを追加せず棄却し、Qt actual Present IDをformal harnessへ
露出する経路を検討する。`FLIP_SEQUENTIAL / BufferCount 2 / MaximumFrameLatency 1`は完全oracle取得不能時の
secondary controlにのみ使う。

[事実] `mvm_p2_present_identity_probe`のrelease build後、上記primary条件で900 Present live runを
実行した。全submissionのPresent IDはexact `+1`だったが、checkerはstatistics transition不足、
sampler自身のphysical VBlank trigger gap、poll interval超過、final drain未完了を検出し、runを
`ORACLE_SAMPLING_GAP / INVALID`にした。値はraw JSONからcheckerが再計算し、文書へ転記しない。
再現手順は`ctest --test-dir build/ucrt64-release -R '^p2_present_id_oracle_live$'
--output-on-failure`である。

[事実] Hardware Flip Queue向けの`DwmFlush` oracle-only fallbackも900 Presentで試したが、checkerは
transition不足を検出してINVALIDにした。fallbackはsamplingを完全化せず、primary runの既定値には
していない。

[exit] したがって現在もmapper proofは評価不能であり、未観測Presentをdiscardへ帰属しない。
F3-B1は未開始、P2-D5-2はBLOCKEDのままとする。live runnerは同名artifactがある場合、次run前に
timestamp付きcopyを作り、不完全artifactも上書きで失わない。

#### F3-B0.6-R2: ETW Present-History Oracle

[事実] R1で使った`GetFrameStatistics`は最後のrender frameを返すsnapshot APIであり、全Presentの
event historyではない。primaryと`DwmFlush` fallbackの双方で完全列を取得できなかったため、R1 artifactを
不変保存し、このpolling samplerの追加調整を打ち切る。これはmapperの失敗ではなくoracle acquisitionの
失敗である。

[回避策] PresentMon v2.3.1のPresentDataをcommit
`717c5bf14e80a4a06b70cd16415ae8d40a7ce201`へ固定し、通常CSVを介さずETLから
`PresentStartTime / ProcessId / ThreadId / SwapChainAddress / Hwnd / SyncInterval / PresentFlags /
Displayed[] / PresentIds`をraw JSONへ出すdiagnostic-only decoderを導入する。`Displayed`のQPCは丸めず
そのまま保存し、空列は`UNPRESENTED`、非空列の先頭は初回opportunity、残りはphysical repeatとする。

最初の対象はself-created probeではなくactual Qt compositor shadowとする。app側は既存の
`frameSwapped QPC / swap ordinal / source frame identity / measurement window / physical VBlank ring`を
保存する。target PID、measurement window、swapchainで絞った後のjoinはcountと順序のexact一致だけを
許可する。nearest timestampによる個別対応や欠落救済は行わない。target swapchainが一意、sequenceが
連続、全`SyncInterval == 1`、ETW lost eventとPresentData ring overflowが0の場合だけ次へ進む。

`DisplayedQPC`は最初の120 physical VBlank sampleからexact refresh rationalでphase/originをfreezeし、
measurement全体を最寄りのphysical opportunity ordinalへ投影する。ちょうど半周期のboundary ambiguity、
観測範囲外、physical VBlank authority不成立はrun全体をINVALIDにする。app swapのVBlank bracketは
collision evidenceの分類にだけ使い、oracle opportunityの算出には使わない。

checkerは`scripts/check-p2-etw-present-history.ps1`へ一本化し、先に既存の
`check-p2-vblank-shadow.ps1`を呼ぶ。collision有無の対照群2件に加え、count、sequence、swapchain、window、SyncInterval、ETW lost、
raw QPC欠落、boundary ambiguity、physical authorityを各1箇所だけ壊した
9 negativeでfail-closedを固定する。runnerは同条件の未計測baselineとWPR計測runのswap cadence比を
diagnosticに残すが、新しい性能thresholdにはしない。

[exit] R2のexit criteriaは、app/ETW exact count、sequence/order mismatch 0、ETW lost 0、raw
`DisplayedQPC`取得、physical ordinal ambiguity 0、およびactual Qtのsame-bracket collisionを少なくとも
1件解決することである。offline synthetic delayを使う場合もlive processへdelayを入れず、同型のidentityを
別artifactで証明する。oracle-valid traceが得られるまでmapperは変更せず、
`mapper proof = NOT YET EVALUABLE`、F3-B1未開始、P2-D5-2 BLOCKEDを維持する。PresentMonでもcomplete
historyを取得できない場合はheuristicを足さず、Qt D3D11 QRhi backendの`Present`直前・直後とserialを
露出するdiagnostic-only hookへ切り替える。

再現手順は次の通り。runnerは既存directoryを上書きせず、ETL、app raw、PresentData raw、oracle、
hash manifestを同じartifact rootへ保存する。

```powershell
pwsh scripts/bootstrap-p2-etw-decoder.ps1
pwsh scripts/build.ps1 -Target mvm_compositor_spike
# 管理者PowerShellで実行
pwsh scripts/p2-etw-present-history.ps1 -OutputDirectory bench/results/f3-b0.6-r2-<timestamp>
```

現在の状態:

```text
F3-B0.6-R1 GetFrameStatistics oracle : INVALID / path exhausted
F3-B0.6 mapper proof                 : NOT YET EVALUABLE
F3-B0.6-R2 ETW oracle               : ORACLE_VALID / COLLISION_NOT_OBSERVED
F3-B1                               : NOT STARTED
P2-D5-2                             : BLOCKED
```

#### F3-B0.6-R3: Oracle-backed Synthetic Collision Corpus

[事実] 15秒actual Qt artifact `bench/results/f3-b0.6-r2-20260822-1919`は、更新後のR2 checkerで
`ORACLE_VALID / COLLISION_NOT_OBSERVED`となった。app swapとETW Presentは900/900 exact join、全recordの
raw `DisplayedQPC`をphysical opportunityへ一意に投影でき、ETW lostとPresentData overflowは0である。
oracle validityとcollision観測を別fieldへ分離し、自然collisionが無いことだけで完全oracleをINVALIDと
呼ばない。元artifactの`summary.json`は軸分離前の粗い`ORACLE_INVALID`表記なので書き換えず保存し、
R3の`source-oracle.json`と`corpus-index.json`をhash付き再分類evidenceとする。

[事実] 同じ環境の60秒artifactはETW lostが非0なので`ETW_LONG_TRACE_CAPACITY_INVALID`として保存し、
oracleやcorpus sourceには再利用しない。app側rawにもsame-bracket pairは無かったため、長時間ETW再採取と
WPR profile調整は打ち切った。

[回避策] `scripts/make-p2-etw-synthetic-corpus.ps1`は15秒artifactのmanifest、app JSON、ETW JSONの
SHA-256を固定し、hash不一致またはR2 oracle再検査失敗時は生成しない。synthetic変更はcallback QPCだけに
限定し、各caseを次の2ファイルへ分離する。

```text
mapper-input.json:
  submission index / original callback QPC / synthetic callback QPC / delay ticks
  synthetic callback bracket / SyncInterval / sourceから切り出した連続VBlank列

hidden-oracle.json:
  PresentIds / raw DisplayedQPC[] / displayed status / actual physical opportunity
  expected construction solution class
```

mapper inputには`present_ids / displayed / actual_physical_opportunity_ordinal /
expected_solution_class`を出さない。sourceのPresentData `PresentIds` vectorは対象900件すべて空だったため、
値を補完・推測せず空列のまま不変保存する。submission identityはexact sequence join、display identityは
raw `DisplayedQPC`で保持する。

[事実] `bench/results/f3-b0.6-r3-corpus-20260822-v2`へ52 caseを機械生成した。pair collision、triple
collision、measurement start/middle/end、1 bracket late、visible opportunity容量による`UNIQUE /
NO_SOLUTION / AMBIGUOUS`を含む。source oracleにactual presentation gapは無いためgap caseは生成せず、
その理由を`corpus-index.json`へ記録した。

R3のsolution classはmapper結論ではなく、各caseが公開する連続VBlank sliceをopportunity universeとした
construction propertyである。N個のordered recordからM個のvisible opportunityへのstrict monotone・
injective・order-preserving assignmentをcheckerが列挙し、0件を`NO_SOLUTION`、1件を`UNIQUE`、複数を
`AMBIGUOUS`とする。R4はこの期待値を入力せずraw mapper-visible JSONだけから独立に判定する。

checker `scripts/check-p2-etw-synthetic-corpus.ps1`はsource R2 oracleを毎回再生成し、source hash、hidden
oracle identity、callback order、`synthetic-original == delay`、連続VBlank slice、collision bracket、
solution classを再計算する。対照群と、oracle opportunity変更、delayをDisplayed側にも適用、record順序交換、
VBlank削除、実際にはsame bracketでないcollision claimの5 mutationをcontract testで固定した。

[exit] R3 corpus checkerはPASSしたがmapperはまだ実装・評価していない。次はF3-B0.6-R4だけを開始し、
mapperへは`mapper-input.json`だけを渡す。R4で合法解が複数ならbackfill heuristicを採用せず、
production-observable情報だけではexact identityを一意復元できないと結論し、Qt D3D11 Present serial hookへ
切り替える。

```text
F3-B0.6-R1 GetFrameStatistics oracle : INVALID / path exhausted
F3-B0.6-R2 ETW oracle                 : ORACLE_VALID / COLLISION_NOT_OBSERVED
F3-B0.6-R3 synthetic corpus           : PASS
F3-B0.6-R4 offline mapper proof       : NEXT
F3-B1                                 : NOT STARTED
P2-D5-2                               : BLOCKED
```

再現手順:

```powershell
pwsh scripts/make-p2-etw-synthetic-corpus.ps1 `
  -OutputDirectory bench/results/f3-b0.6-r3-corpus-<timestamp>
pwsh scripts/check-p2-etw-synthetic-corpus.ps1 `
  -CorpusDirectory bench/results/f3-b0.6-r3-corpus-<timestamp>
```

#### F3-B0.6-R4: Offline Mapping Proof

R4のadmissibility relationはhidden oracleを開く前に次で固定した。

```text
VISIBLE_PREFIX: opportunity_start_qpc <= synthetic_callback_qpc
```

各recordが使えるのは、R3が公開する有限な連続VBlank slice内でcallback時点までに開始した
opportunityの全prefixである。これはcallbackがdisplay後に遅延し得るというB0/B0.5の仮説に基づく。
`k`または`k-1`のようなoracle適合windowは作らない。mapper-visible入力はsynthetic callback QPC、
submission order、physical VBlank sequence、measurement boundary、`SyncInterval == 1`だけを持つ。
`DisplayedQPC`、ETW actual opportunity、PresentIds、original callback QPC、synthetic delay、期待solution
classはstrict schemaで拒否する。

mapperはstrict monotone / injective / order-preserving assignmentの個数をexact DPで数え、0 / 1 / 2以上へ
saturateする。結果はそれぞれ`NO_SOLUTION / UNIQUE / AMBIGUOUS`であり、複数解から代表解を選ばない。
pure testはrecord 1〜3件、opportunity 1〜5件の全candidate matrixを独立brute forceと照合する。
opportunity再利用、first-solution採用、`<=`から`<`へのboundary変更、future opportunity混入、callback
並べ替えのmutation、およびmapper-visible JSONへのoracle / delay field漏洩をnegative testで固定した。

[事実] `bench/results/f3-b0.6-r4-offline-20260822-v3`でR3の52 caseをmapper-visible JSONへ投影し、
全mapper実行が完了した後にcheckerだけがhash固定済みhidden oracleを開いた。solution classは
`UNIQUE 36 / AMBIGUOUS 8 / NO_SOLUTION 8`で期待値と完全一致した。UNIQUE 36 caseの84 recordは
assignmentしたphysical opportunity ordinalがETW oracleと84/84完全一致した。pure/property、mutation、
漏洩拒否のtargeted CTestは6/6通過した。

[exit] R4 offline mapper proofは**PASS**である。backfill heuristicやQt Present serial hookへの切替条件には
該当しない。次はF3-B1 runtime wiringへ進む。ただしsource traceは900/900がDisplayedであり、
`UNPRESENTED`のidentityは証明していない。production runtimeへはまだ接続していないため、
P2-D5-2はBLOCKEDのままである。

```text
F3-B0.6-R1 GetFrameStatistics oracle : INVALID / path exhausted
F3-B0.6-R2 ETW oracle                 : ORACLE_VALID / COLLISION_NOT_OBSERVED
F3-B0.6-R3 synthetic corpus           : PASS
F3-B0.6-R4 offline mapper proof       : PASS (36 / 8 / 8, UNIQUE 84/84 exact)
F3-B1 runtime wiring                  : NEXT
P2-D5-2                               : BLOCKED
```

再現手順:

```powershell
pwsh scripts/run-p2-r4-offline-proof.ps1 `
  -OutputDirectory bench/results/f3-b0.6-r4-offline-<timestamp>
pwsh scripts/check-p2-r4-offline-proof.ps1 `
  -CorpusDirectory bench/results/f3-b0.6-r3-corpus-20260822-v2 `
  -RunDirectory bench/results/f3-b0.6-r4-offline-<timestamp>
ctest --test-dir build/ucrt64-release --output-on-failure `
  -R '^p2_(presentation_opportunity_mapper_pure|r4_visible_input_)'
```

#### F3-B1: Incremental Runtime Wiring

B1はoffline solverをそのままruntimeへ持ち込まず、B1a incremental equivalenceとB1b live shadowの
2段階に分けた。admissibility relationはR4から変更せず、`VISIBLE_PREFIX`だけを使う。incremental
mapperは全合法解が共有する連続prefixだけをcommitし、次のVBlankを観測して閉じたcallbackだけを
判定対象にする。合法解0件は`NO_SOLUTION`、measurement終了時に複数解が残る場合は
`AMBIGUOUS_MAPPING`としてfail-closedにする。first solution、backfill、window拡張は行わない。

[事実] B1aは`bench/results/f3-b1a-incremental-20260822`でR3の52 caseをevent-by-event replayした。
最終classは`UNIQUE 36 / AMBIGUOUS 8 / NO_SOLUTION 8`、UNIQUEのidentityは84/84 exact、
commit regressionは0件だった。各event時点の全合法解をcheckerが独立brute forceし、実装のcommitが
consensus prefixだけであることを検査した。first-solutionを早期commitするmutationは拒否した。

[事実] Qt 6.11.1のsource preconditionはtagとpeeled commitを固定して確認した。
[`qrhid3d11.cpp`](https://github.com/qt/qtbase/blob/v6.11.1/src/gui/rhi/qrhid3d11.cpp)
（qtbase `59c81a3c2247b821b9b84b4eb8d939b77e07e276`）は通常経路を
`Present(swapInterval, presentFlags)`、`presentFlags = 0`とし、tearing flagは`swapInterval == 0`の
場合だけ設定する。[`qsgthreadedrenderloop.cpp`](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/qsgthreadedrenderloop.cpp)
（qtdeclarative `a02bed441965ee1f18f856352c7d5ee5ba35d795`）はsurface formatの
`swapInterval == 0`の場合だけ`NoVSync`を設定する。B1b runnerはruntime Qt 6.11.1、D3D11、
requested swap interval 1、`QSG_NO_VSYNC`未設定、Present flags 0、`DXGI_PRESENT_RESTART`なし、
tearingなしを開始時に検査し、不一致なら計測前に失敗する。

[事実] 5秒のlive smokeではB1b shadow checkerがPASSした。しかし正式な非ETW 60秒 x 3 runの
`bench/results/f3-b1b-shadow-60s-20260822-v2`はrun 1で停止した。既存のphysical checkerは
`mapped=3592 / same_opportunity=6 / observer_gap=0 / ambiguous=0 / vblank_samples=3604`を独立再計算し、
physical authority自体はPASSした。一方、incremental mapperはterminal VBlankでclosed record 1593件に
対しvisible opportunityが1592件しかなく、直前のVBlank domain内にcallbackが2件あったため、injective
assignmentが存在せず`NO_SOLUTION`になった。process exitは3、formal counter authorityは変更していない。

[exit] B1aは**PASS**だが、public `frameSwapped` callbackとphysical VBlankを`VISIBLE_PREFIX`で対応付ける
B1b runtime mapping仮説は**FAIL / path exhausted**である。短いsmokeの成功で60秒の反例を上書きしない。
relationのwindow拡張やbackfillは行わず、残り2 runを停止した。shadow gateを通らなかったため15秒ETW
再採取は実行しない。F3-B1のformal promotion、P2-D5-2、P5-EはBLOCKEDのままとし、次はQt D3D11
Present側でserial/timestampを直接採るnative hookを独立した診断・formal harnessとして検討する。
`UNPRESENTED` identityは引き続き未証明である。

```text
F3-B1a incremental equivalence       : PASS (36 / 8 / 8, UNIQUE 84/84 exact)
Qt 6.11.1 source/runtime precondition : PASS
F3-B1b 60s non-ETW shadow             : FAIL / PUBLIC_FRAMESWAPPED_PATH_EXHAUSTED
F3-B1b 15s ETW                        : NOT RUN (shadow gate failed)
F3-B1 overall                         : FAIL / hypothesis rejected
P2-D5-2                               : BLOCKED
P5-E                                  : BLOCKED
next                                  : native Qt D3D11 Present serial/timestamp hook
```

再現手順:

```powershell
pwsh scripts/run-p2-b1a-incremental-proof.ps1 `
  -OutputDirectory bench/results/f3-b1a-incremental-<timestamp>
pwsh scripts/check-p2-b1a-incremental-proof.ps1 `
  -CorpusDirectory bench/results/f3-b0.6-r3-corpus-20260822-v2 `
  -R4Directory bench/results/f3-b0.6-r4-offline-20260822-v3 `
  -RunDirectory bench/results/f3-b1a-incremental-<timestamp>
pwsh scripts/check-p2-b1-shadow-failure.ps1 `
  -Json bench/results/f3-b1b-shadow-60s-20260822-v2/run-1.json `
  -ProcessExitCode 3
```

#### F3-C0: Native D3D11 Present Serial Authority Proof

B1のpublic `frameSwapped` observableはimmutable failure evidenceとして凍結する。R4は証明対象にした
observable modelについて有効であり、PASSを取り消さない。live Qt callback streamがそのmodelを満たさない
ことが判明したため、R4 relationのwindow拡張、backfill、first-solution採用は行わない。

C0はQt 6.11.1 D3D11 QRhiのactual `IDXGISwapChain::Present()` call siteだけをdiagnostic patchする。
hookは既定無効で、明示したmeasurement区間だけ有効にする。hot pathはQPC、process-local serial、
thread-local token consume、固定POD ring writeだけを行い、allocation、mutex、I/O、logging、
FrameStatistics polling、DwmFlush、ETW decodeを置かない。Present attemptごとにswapchain identity、thread id、
enter/return QPC、HRESULT、SyncInterval、flagsを採り、successful Presentとcomposition tokenを1:1にする。
missing / duplicate / stale token、failed Present、ring overflowはいずれもauthority failureである。

composition tokenはrender thread上のexact `ComposedFrame`から作り、token serial、composition epoch/state、
output frame、A/Bのsource id / generation / resource epoch / frame numberを固定する。sourceはID順に正規化し、
最大2件の固定ABIとする。QtGuiとの接続はcold pathでexportを一度だけ解決し、hook OFFを指定した場合も
patched binaryとABI versionが一致しなければ失敗する。formal counter authorityはまだ変更しない。

[事実] upstream QtBase `v6.11.1` / `59c81a3c2247b821b9b84b4eb8d939b77e07e276`へ
`qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch`を適用し、workspace内の独立buildで
QtCore、QtGui、qwindows pluginを構築した。patch SHA256は
`104f7f469ceba56f3e6440b9d72dc16d5d9af77467eb0dbbdb65783784f34851`、QtGui.dll SHA256は
`f72ffdfad91c7689fd1ade84515632dfc34593744cb07a59af9ef91d7224cf0b`である。既存の
`C:\msys64\ucrt64`は変更していない。QtGuiの4 exportと、固定patchがupstreamへreverse-applyできることを
機械で確認した。

[事実] 同じpatched binaryによる2秒controlでは、hook OFFがcomposition 120 / swap 120 / native record 0、
hook ONがcomposition 120 / swap 120 / successful native Present 120 / exact token 120だった。ONは
Present serialとtoken serialが全件strict +1、swapchain/thread identity各1件、SyncInterval 1、flags 0、
HRESULT 0で、overflow / missing / duplicate / stale / token-set failureはすべて0だった。producer summaryを
信じずraw recordを再計算するhook/ETW contract testは11/11通過した。

[事実] 管理者PowerShellからC0-Aの15秒ETW cross-checkを2回採取した。artifactは
`bench/results/f3-c0-native-etw-20260822-2`と
`bench/results/f3-c0-native-etw-20260822-3`である。両runともhook OFF/ON contract、native
Presentとswapのexact count、およびWPR有無のcadence controlは成立した。一方、両ETLで
`EventsLost != 0`かつ`BuffersLost != 0`となり、checkerはexact joinより前にfail-closedした。各raw値、
binary/source/Qt/ETL identity、checker結果はそれぞれの`summary.json`と`manifest.sha256`から再計算・検証する。
欠損列の部分照合、対象process外の欠損という推定、nearest-QPC救済は行わない。

[回避策] 同じ`GeneralProfile / GPU / DesktopComposition`による15秒WPR再採取は2回で打ち切る。
system-wide heavy WPRとは別に、固定PresentMon commitの`PMTraceSession::Start`が提供するcanonical
provider / event-ID filteringをそのまま使う`CanonicalPresentMonLive` acquisitionをrunnerとdecoderへ
追加した。実アプリPIDはwrapperが起動直後に別fileへ書き、
metricsのPIDとexact一致を要求する。decoderのsession ready後だけ測定を継続し、停止時にsession自身の
`EventsLost / BuffersLost`を取得する。既存offline ETL decodeとWPR failure artifactは変更しない。
手選別provider集合は実装していない。canonical live runでprovider closure、buffer loss、開始・終了境界を
独立に証明できるまでは、display completion oracleをPASSへ変更しない。

[事実] 最初のtargeted live artifact
`bench/results/f3-c0-native-live-etw-20260822-1`はETW loss 0とnative/ETW exact joinを満たしたが、
trace runだけがPID取得用の別process起動経路を通り、hook OFF/ON controlとGUI起動条件が一致していなかった。
このrunの`UNPRESENTED`判定を製品経路の結論には使わず、`RUNNER_CONTROL_INVALID`として不変保存する。
wrapperは全runを同じ`ProcessStartInfo`経路へ統一し、PID fileの有無だけを差分にした。1秒controlで
OFF/ONともswap 60、ONのnative record 60、PID fileとmetrics PIDの一致を確認した。

[事実] 起動経路統一後のtargeted live artifact
`bench/results/f3-c0-native-live-etw-20260822-2`は、ETW event/buffer lost、PresentData overflow、
native/ETW count・order・interval mismatchがすべて0で、composition tokenを全measurement recordへexact
joinできた。したがってoracle acquisitionは`ORACLE_VALID`である。一方、measurement内のsuccessful native
Present 899件のうちDisplayedQPCを持つのは87件、`UNPRESENTED`は812件だった。hook ON/OFFとtrace ONの
cadence controlも成立している。このhistorical checker FAILとJSONは書き換えず保存する。

#### F3-C0-R2: PresentMon Final-State Closure Audit

[事実] `scripts/audit-p2-c0-final-state-closure.ps1`は上記historical artifactのmanifestを先に検証し、
measurement 899件を`FinalState`と`DisplayedQPC`で再分類した。結果はPresented 87、explicit Discarded
812、Unknown相当0であり、empty `Displayed` 812件を一律`UNPRESENTED`と呼んだhistorical labelは
semantically overbroadだった。一方、historical rawには`IsCompleted / IsLost / PresentMode /
SeenDxgkPresent / SeenWin32KEvents / SeenInFrameEvent / WaitForFlipEvent / WaitForMPOFlipEvent`が無いため、
このartifact単独のfull display-completion closureは`NOT_YET_VALIDATED`とする。audit artifactは
`bench/results/f3-c0-r2-final-state-audit-20260823`であり、source hashとmanifestを固定した。

[回避策] decoder rawへ上記completion fieldと、`PRESENTED / DISCARDED / INCOMPLETE_UNKNOWN / LOST`の
fail-closed分類を追加した。`PRESENTED`は`FinalState == Presented && Displayed nonempty && IsCompleted`、
`DISCARDED`はexplicit `FinalState == Discarded && Displayed empty && IsCompleted`だけを許す。`IsLost`は
常に`LOST`、残りは`INCOMPLETE_UNKNOWN`とする。checkerはproducer分類を独立再計算し、UnknownまたはLostを
1件でも含むrunを拒否する。explicit Discardedはclosure failureとはせず、native Present単独authorityの
棄却根拠として別fieldへ出す。Presented対照、Discarded対照、Unknown/Lost negativeを含むcontractで固定する。

[事実] completion raw拡張後のcanonical 5秒artifact
`bench/results/f3-c0-r2-canonical-live-20260823-1`は、pinned PresentMonのprovider fingerprintとevent-ID
filtering、app/collector PID一致、native/ETW count・order・interval exact、composition-token join exactを
満たした。measurement 300件はPresented 32 / explicit Discarded 268へ全件閉じ、Unknown、Lost、未完了、
ETW event/buffer lost、PresentData overflowはすべて0だった。hook ON/OFF比とtrace/control比も1.0である。
したがって5秒display-completion closureはPASSとして固定する。全recordがPresentedであることはexit criteria
にしていない。次は同じcanonical sessionを15秒へ伸ばし、closureとcapacityを再確認する。

[事実] canonical 15秒artifact
`bench/results/f3-c0-r2-canonical-live-20260823-15s-1`もprovider fingerprint、event-ID filtering、PID、
native/ETW/token 900件のcount・order・interval exact joinを満たした。Presented 725 + explicit Discarded
175で全measurement recordへ閉じ、Unknown、Lost、未完了、ETW event/buffer lost、PresentData overflowは
すべて0だった。hook ON/OFF比とtrace/control比はいずれも約1.001である。15秒closure/capacityもPASSとして
固定し、次は同じcanonical sessionの60秒capacityだけを評価する。

[事実] canonical 60秒artifact
`bench/results/f3-c0-r2-canonical-live-20260823-60s-1`はETW event/buffer lost、PresentData overflowが
0で、cadence controlも成立した。historical checkerはnative 3598 / ETW 3597のcount差1でFAILしたが、
missingはserial 3598の末尾1件だけだった。このnative Presentは固定measurement end直前にenterし、returnが
endを約0.2ms越えたboundary-straddling recordである。hook停止は次render callback先頭なので、このrecordが
native ringだけに残ることをproducer codeとraw QPCの双方で確認した。

[回避策] measurement domainを`present_enter_qpc >= start && present_return_qpc < end`へ固定し、開始・終了を
またぐnative/ETW recordは本体から推定削除せず別countへ分離する。境界対照をcontract testへ追加した。
元artifactとhistorical FAILは書き換えず、manifest検証後のsidecar
`bench/results/f3-c0-r2-canonical-live-20260823-60s-boundary-recheck`で再判定した。domain内3597件は
Presented 1358 + explicit Discarded 2239へ全件閉じ、Unknown/Lost 0である。したがってcanonical 60秒の
semantic closureとcapacityはPASSとして固定する。

#### F3-C1: Native Present + ETW Display Authority Proof

[事実] `scripts/check-p2-c1-display-authority.ps1`はC0-R2 oracleからPresented recordだけを取り出し、
composition tokenのoutput frame identity、DisplayedQPC、physical opportunity ordinalをexactに結合する。
Discarded record数をdropへ直接足さず、Presented source-frame identityのunique列からgapとtailを一度だけ
計算する。60秒artifactではPresented record 1358、unique source frame 1355、repeat 3、gap 2244、tail 1で、
`displayed_unique_source_frames 1355 + formal_source_frame_drops 2245 == 3600`が成立した。proof artifactは
`bench/results/f3-c1-display-authority-proof-20260823`である。Unknown、Lost、domain外frame、opportunity逆行、
Discardedへのdisplay payloadを壊すnegativeを含む6/6 contract testが通過した。

[exit] F3-C1 offline identity/accounting proofはPASSである。これはnative hook + canonical PresentMonが
composition identityからactual display outcomeへのdiagnostic authorityになれることを示す。ただし
`formal_counter_authority_changed == false`であり、runtime/formal harnessへの配線はまだ行っていない。
60秒source domainのdrop rateは`2245 / 3600 = 62.3611%`であり、2%閾値を変更せずFAIL証拠として固定する。
submission contractへの変更、frameSwapped mapper復活は行わない。原因帰属なしにruntime/formal wiringへ
進まず、次はF3-C2 display discard attributionを独立gateとして実施する。P2-D5-2はBLOCKEDのままである。

#### F3-C2: Display Discard Attribution

[事実] C2-Aは新規採取を行わず、C0-R2 60秒sidecar oracleのmanifestを検証して全3597 Presentを
native serial、composition token、source frame、FinalState、PresentMode、DisplayedQPC、physical VBlank、
PresentStart、SeenDxgk/Win32K/InFrameと結合した。Presented 1358件から1357区間の時系列を再構成すると、
`intervening Discarded == physical VBlank delta - 1`は1357/1357でexactだった。source frame deltaとphysical
VBlank deltaは1347/1357で一致し、10区間はsource frameのrepeat/skipにより不一致だった。したがって物理表示の
保持gapはDiscarded列で厳密に説明できるが、source frame番号を物理表示timelineそのものとは扱えない。artifactは
`bench/results/f3-c2-display-discard-attribution-20260823`で、positive 1件とUnknown、identity欠落、payload混入、
VBlank逆行のnegative 4件が通過した。historical rawに無いReadyTime、TimeInPresent、QueueSubmitSequence、
Win32KPresentCount/BindId、Present-History tokenはnullを捏造せずavailability=falseとして記録した。

[回避策] C2-B用にpinned PresentMon `v2.3.1` / `717c5bf14e80a4a06b70cd16415ae8d40a7ce201`へ
classification-only診断patchを用意した。patchはPresentMon内の実際の6つのDiscarded確定分岐でのみ
`BACK_TO_BACK_FLIP_SUPERSEDED`、`WIN32K_TOKEN_NOT_IN_FRAME`、`DEPENDENT_PRESENT_SUPERSEDED`、
`DO_NOT_SEQUENCE`、`NOT_VISIBLE`、`BLIT_CANCEL`を記録する。事後QPC推定は行わず、provider、tracking、
FinalState、completion順序は変更しない。decoderは上記historical不足fieldとdiscard reasonをrawへ出し、checkerは
全native/token join exact、Unknown/Lost 0、ETW event/buffer loss 0、overflow 0、Discarded件数とreason件数の
一致、unknown reason 0をfail-closedで要求する。診断decoderのbuildは成功した。この時点では15秒canonical
C2-B採取は未実行であり、formal counter authorityは変更していない。

[事実] 最初のC2-B 15秒artifact
`bench/results/f3-c2-discard-attribution-20260823-15s-1`は、native/ETW/token 900件のexact join、
Presented 900、Unknown/Lost 0、ETW loss/overflow 0、cadence 60fpsを満たしたが、Discardedは0件だった。
raw measurement外を含むtarget process全1217件もPresentedであり、reasonは全件`NONE`だった。旧checkerは
`discarded_count 0 == reason_count 0`としてPASSを出したが、これは原因帰属を1件も検証しない空振りである。
artifactと旧PASS labelは書き換えず保存し、C2-B evidenceとしては`EVIDENCE_EMPTY / NOT EVALUABLE`へ
位置付ける。checkerへDiscarded 0件のnegativeを追加し、以後は同じ空振りをfail-closedで拒否する。

[事実] 次のC2-B 60秒artifact
`bench/results/f3-c2-discard-attribution-20260823-60s-1`はnative/ETW/token 3595件をexact joinし、
Presented 1345、Discarded 2246、Unknown 4、Lost 0、ETW loss/overflow 0を記録した。Unknown 4件は末尾ではなく
measurement開始約6.9～7.1秒に集中し、全件`IsCompleted=true / FinalState=Unknown / Composed_Flip /
SeenInFrame=true`だった。したがってsession drain不足ではなく、C2-B closureはFAILとして保存する。
確定済みDiscarded 2246件のreasonは全件`DEPENDENT_PRESENT_SUPERSEDED`で、他reasonは0件だった。ただし
Unknown 4件を除外した分布なので、closure PASSまでは最終分布として固定しない。

[事実] pinned PresentMon sourceでは、後続Presentedを完了するときに同一process/swapchainの先行Presentを走査し、
先行Presentの`FinalState`がUnknownでもそのまま`CompletePresent(p2)`する経路がある。上記4件の状態はこの経路の
出力と一致する。これは既存6箇所の`FinalState=Discarded`だけを列挙した診断patchが漏らした実コード上の
supersede経路である。

[回避策] 同経路で`FinalState == Unknown`の先行Presentだけを
`EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED`として記録し、Discardedへ閉じてから既存どおりCompleteする。
tracking構造、完了対象、完了順序は変更しない。既にPresentedまたはDiscardedのrecordは変更しない。
元60秒artifactは書き換えず、更新decoderによる新規runでのみclosureを再評価する。

[事実] 更新後の次の60秒artifact
`bench/results/f3-c2-discard-attribution-20260823-60s-2`はnative/ETW 3594件をexact joinし、
Presented 1120、Discarded 2474、Unknown/Lost 0、ETW loss/overflow 0だった。しかしDiscarded 6件だけ
reasonが`NONE`で、checkerは最初の該当recordでfail-closedした。残る2468件は
`DEPENDENT_PRESENT_SUPERSEDED` 2467件、`EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED` 1件である。

[事実] reason未確定6件は全てtarget processのComposed Flipだった。raw全体ではDWM側Hardware Legacy Flipに
`EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED`が7件あり、PresentMon sourceはDWM親Presentからdependentへ
`FinalState`と`Displayed`をコピーしていたが、診断追加fieldの`FinalDiscardReason`はコピーしていなかった。

[回避策] state/display provenanceと同じ分岐で`FinalDiscardReason`も親からdependentへコピーする。
新しい事後分類は追加せず、親がPresentedなら`NONE`、Discardedなら親の確定reasonをそのまま伝播する。
元60秒artifactはFAILのまま保持し、更新decoderによる新規runでexact reason accountingを再評価する。

[事実] provenance伝播補正後の60秒artifact
`bench/results/f3-c2-discard-attribution-20260823-60s-3`はnative/ETW/token 3596件をexact joinし、
Presented 535、Discarded 3061へ全件閉じた。Unknown、Lost、ETW event/buffer loss、PresentData overflow、
unknown discard reasonは全て0で、Discarded count 3061とreason count 3061がexact一致した。reason分布は
`DEPENDENT_PRESENT_SUPERSEDED` 2551件（83.34%）、
`EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED` 510件（16.66%）で、その他reasonは0件だった。Presented 535件の
reasonは全件`NONE`である。root/canonical双方のmanifest、decoder/patch hashも再検証して一致した。

[exit] F3-C2-Aでは介在Discarded数とphysical VBlank保持gapが1357/1357 exactであり、F3-C2-Bでは
全Discardedがqueue/supersede系reasonへ1:1に閉じた。したがって大量dropはvisibility/occlusionやETW欠損ではなく、
obsolete dependent/earlier Presentが後続表示までにsupersedeされる経路に支配されている。F3-C2はPASSとして
固定し、結果分岐Aに従って次を`F3-C3 — Display-paced Submission Fix`とする。ただしこのcheckpointでは
runtime/formal counter wiring、2% threshold、submission contractをまだ変更せず、P2-D5-2はBLOCKEDを維持する。

```text
F3-B1 overall                         : FAIL / PUBLIC_FRAMESWAPPED_PATH_EXHAUSTED
F3-C0 native hook implementation      : PASS
F3-C0 hook OFF/ON smoke               : PASS (120/120 token exact)
F3-C0-A heavy WPR                     : ETW_CAPACITY_INVALID / WPR_PATH_EXHAUSTED
native successful Present authority   : REJECTED
TargetedLive native/ETW submission    : PASS
TargetedLive display completion       : HISTORICAL LABEL SUPERSEDED
historical TargetedLive checker       : FAIL / PRESERVED
812 actual UNPRESENTED                : NOT YET ESTABLISHED
F3-C0-R2 canonical 5s closure         : PASS (300/300 CLOSED)
F3-C0-R2 canonical 15s closure        : PASS (900/900 CLOSED)
F3-C0-R2 canonical 60s capacity       : PASS (3597/3597 CLOSED + 1 BOUNDARY)
F3-C1 offline identity/accounting     : PASS (1355 + 2245 == 3600)
F3-C1 runtime/formal wiring           : NOT STARTED
F3-C2-A offline physical gap proof    : PASS (1357/1357 exact; source mismatch 10)
F3-C2-B first 15s reason run          : EVIDENCE_EMPTY (0 discarded; old vacuous PASS preserved)
F3-C2-B 60s-1                         : FAIL / UNKNOWN 4 (preserved)
F3-C2-B 60s-2                         : FAIL / REASON NONE 6 (preserved)
F3-C2-B 60s-3 reason closure          : PASS (3061/3061, unknown 0)
F3-C2 overall                         : PASS / QUEUE_SUPERSEDE DOMINANT
F3-C3 Display-paced Submission Fix    : NEXT / NOT STARTED
P2-D5-2                               : BLOCKED
```

再現手順:

```powershell
pwsh scripts/prepare-p2-c0-qt-source.ps1
pwsh scripts/build-p2-c0-patched-qt.ps1 -Jobs 4
pwsh scripts/build-p2-etw-decoder.ps1

# ここからは管理者PowerShellで実行する。
pwsh scripts/p2-c2-discard-attribution.ps1 `
  -OutputDirectory bench/results/f3-c2-discard-attribution-<timestamp> `
  -MeasureSeconds 60 `
  -TimeoutSeconds 300
```

observer threadは`THREAD_PRIORITY_TIME_CRITICAL`への昇格に失敗した場合、normal priorityへ黙って
fallbackせず`AUTHORITY_UNAVAILABLE`として起動失敗にする。observer threadは
`WaitForVBlank` → fixed-ring write 以外を行わない。

checkerは`scripts/check-p2-vblank-shadow.ps1`で、producerの集計を信じずraw VBlank sampleと
swap recordからinterval、累積deviation、swap→opportunity mappingを独立再計算する。contract testは
対照群2件 (通常cadence / supersede) と、ambiguous swap、observer gap、after-last、before-first、
short interval、long interval、cumulative drift、normal priority、output change、sample count不一致の
10 negativeで固定する。

#### F3-C3-A: Submission Backpressure Causal Proof

[回避策] production scheduler、formal counter wiring、2% thresholdを変更せず、patched Qtのnative Present
hookへdiagnostic-onlyの3 modeを追加した。`CONTROL`はQt 6.11.1既定と同じmaximum frame latency 2、
`DWM_FLUSH_AFTER_PRESENT`はsuccessful Present後に`DwmFlush()`を1回呼ぶpositive control、
`FRAME_LATENCY_1`はQt既存の`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`、
`SetMaximumFrameLatency(1)`、`beginFrame()`でのwaitable object待機を使う。各runはswapchainから実際の
maximum latencyとwaitable flagを読み戻し、DwmFlush call/failure数もnative Present件数へexact closureする。
固定sleep、VBlank grace、scheduler deadlineは変更していない。

[回避策] PresentMon診断patchへ`DependencyBatchPresentStartTime`を追加した。DWM親Presentがdependentを
完了する分岐と、後続Presentが同一swapchainの先行Presentを完了する分岐でbatch identityを記録する。
checkerはsupersede reason全件にnonzero batch identityを要求し、batch size histogram、p50/p95、観測maxを
producer集計に依存せずoracle recordから再計算する。

[事実] 最初の循環順序artifact
`bench/results/f3-c3-submission-backpressure-20260823-15s-1`では、初版C3 checkerがdisplayed frame間の
内部gapだけを数え、先頭gapとtailを含めていなかった。raw CanonicalPresentMonLive、native/token/ETW join、
discard reason、dependency batchは有効だが、初版`source_gap_drop_count`はC1 accountingとして不完全である。
元artifactは書き換えず保存した。checkerをC1と同じ`leading/internal gap + tail`へ修正し、全source domainで
`displayed unique + formal source drops == 900`を要求するnegative付きcontractへ更新した。

[事実] 時間順バイアスを分離するため、各modeが先頭・中央・末尾へ1回ずつ現れる3循環順序を各15秒で採取し、
修正checkerで9/9を再検査した。集計artifactは
`bench/results/f3-c3-submission-backpressure-20260823-counterbalanced-summary-2`、summary SHA-256は
`10c5616854aef22f5812de77d8b47d684bc1909e638982826cd2c99376508ea0`である。全runでnative/token/ETW exact、
Unknown/Lost、ETW event/buffer loss、PresentData overflow、unattributed supersede、DwmFlush failureは0だった。

| mode | Presented / native (3 run) | Discarded | dependency / earlier | batch p95 (各run) | 観測max | displayed unique / formal source drop |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| `CONTROL` | 724 / 2700 | 1976 | 1971 / 5 | 741, 35, 1 | 741 | 723 / 1977 |
| `FRAME_LATENCY_1` | 479 / 2696 | 2217 | 2170 / 47 | 1, 13, 13 | 278 | 477 / 2223 |
| `DWM_FLUSH_AFTER_PRESENT` | 1158 / 2699 | 1541 | 1536 / 5 | 9, 16, 1 | 308 | 1158 / 1542 |

[事実] mode内のrun差は介入差より大きい。`CONTROL`のPresentedは順に1、47、676、
`FRAME_LATENCY_1`は207、129、143、`DWM_FLUSH_AFTER_PRESENT`は120、139、899だった。
DwmFlushの899/899は単独ではpositive responseに見えるが、同じmodeの他2runは120/900と139/900であり、
再現しない。latency 1もbatch p95を小さくするrunはあるが、全3runのDiscardedはCONTROLより多い。

[exit] backpressureがbatch構造へ影響することは観測したが、`DwmFlush`またはlatency 1によってsupersedeが
一貫して消えるcausal proofは得られなかった。したがってA/B/Cのいずれにも進まず、F3-C3-B production
scheduler実装を停止する。結果は`CAUSAL_PROOF_NOT_ESTABLISHED / ORDER_TIME_CONFOUND`とし、次は既存ETWの
DWM dependency completion phaseと大batch発生時点をphysical VBlankへ帰属する。P2-D5-2はBLOCKED、
formal runtime authority wiringと2% thresholdは未変更のまま維持する。

再現手順（管理者PowerShell）:

```powershell
pwsh scripts/p2-c3-submission-backpressure.ps1 `
  -OutputDirectory bench/results/f3-c3-<run1> -Order CONTROL_FRAME_DWM
pwsh scripts/p2-c3-submission-backpressure.ps1 `
  -OutputDirectory bench/results/f3-c3-<run2> -Order FRAME_DWM_CONTROL
pwsh scripts/p2-c3-submission-backpressure.ps1 `
  -OutputDirectory bench/results/f3-c3-<run3> -Order DWM_CONTROL_FRAME

& scripts/summarize-p2-c3-submission-backpressure.ps1 `
  -SourceDirectories @('bench/results/f3-c3-<run1>', 'bench/results/f3-c3-<run2>',
                       'bench/results/f3-c3-<run3>') `
  -OutputDirectory bench/results/f3-c3-<summary>
```

#### F3-C3-A2: DWM Dependency Regime / Completion-Phase Attribution

[事実] checkpoint済み9 runを先にoffline再解析した。artifactは
`bench/results/f3-c3-a2-offline-attribution-20260823-7`で、summary SHA-256は
`39be446ff02aa3a1f70571731c5dbeba09254d9466882c7b2313d7b07943bc2e`である。9/9、計8095 Presentを
個別に再計算し、観測PresentModeは全件`Composed_Flip`、mode transitionは0だった。したがって
高Presented runと低Presented runの差をPresentMode遷移では説明できない。physical/source gap pairは
2352件中2343件exact、9件は局所的な±1 compensationだった。累積spanを比較できる8 runは全て±1以内だった。
大きなparent display groupは207件、うち206件でphysical display deltaとdependent countが±1以内だった。

[回避策] 既存rawにはWAITING、parent attach、parent completion、dependent finalizationのtimestampが無かったため、
pinned PresentMon v2.3.1へbehavior-neutralな第2診断patch
`presentmon-patches/2.3.1/0002-mvm-dependency-lifecycle-diagnostic.patch`を追加した。記録対象は
`APP_PRESENT → WAITING_FOR_DWM → ATTACHED_TO_DWM_PARENT → PARENT_DISPLAY/COMPLETION → DEPENDENT_FINALIZED`と
earlier supersede linkageだけである。DwmFlush、maximum frame latency、waitable object、固定sleep、schedulerは
変更していない。第1patchの`DependencyBatchPresentStartTime`はearlier-swapchain再帰で上書きされ得るため、
actual DWM parent identityは新しい`AttachedDwmParentPresentStartTime`をauthorityとする。

[事実] CONTROL-only 60秒CanonicalPresentMonLiveを
`bench/results/f3-c3-a2-control-lifecycle-20260823-60s-1`へ1本採取した。最終集計は`attribution-2`、
summary SHA-256は`a4e34530c213b5b321eaa60190d4197e74dca48bc7158200001cf91b0304414e`である。
native/ETW 3597/3597、Presented 104、Discarded 3493、Unknown/Lost、ETW event/buffer loss、PresentData overflowは
全て0だった。全3597件が`Composed_Flip`でtransitionは0、actual DWM parent batchは111件、観測maxは2421だった。
110 dependentのbatchは直前parent displayから110 physical VBlank後、2421 dependentのbatchは2421 VBlank後に
display/completionした。direct parent path 3589件ではparent display/completionとdependent finalizationのQPCが
exact closureし、残るearlier-only 8件もsuperseding QPCとfinalizationが一致した。これは同一presentation regime内の
大batchがDWM parent consumption/display gapそのものに
一致する直接証拠である。

[事実] 同じ60秒runのdisplayed source/physical gapは103 pair中88 exact、15 mismatch、累積span差は2だった。
局所的な+1/-1 compensationが多いが、2421-gapではsource gap 2422 / physical gap 2420だった。したがってこのrun単独で
source identity gapの完全一致は主張しない。一方、parent identityとparent display QPCから独立に得た110/110、
2421/2421のclosureはこの不一致に依存しない。

[exit] 分岐は`B_DWM_CONSUMPTION_STALL`である。PresentMode transition支配(A)は棄却し、parent displayが正常なのに
dependencyだけ巨大なauthority contradiction(C)でもない。app Presentは約60/sであり、同一opportunityへのmultiple
submissionを示すDの証拠もない。F3-C3-B production admission fixへは進まず、次を
`F3-C3-A3 — DWM Consumption Stall Attribution`とする。P2-D5-2はBLOCKED、formal runtime authority wiringと
2% thresholdは未変更のまま維持する。

再現手順（offline解析は通常PowerShell、採取は管理者PowerShell）:

```powershell
pwsh scripts/p2-c3-a2-offline-attribution.ps1 `
  -SourceDirectories @('<C3-A run1>', '<C3-A run2>', '<C3-A run3>') `
  -OutputDirectory '<offline output>'

pwsh scripts/build-p2-etw-decoder.ps1
pwsh scripts/p2-c3-a2-control-lifecycle.ps1 `
  -OutputDirectory '<A2 output>' -MeasureSeconds 60 -TimeoutSeconds 180
```

#### F3-C3-A3-T0: DWM Stall Stage Localization

[事実] F3-C3-A2 PASS evidenceはuser-managed commit `89a729a`でcheckpointした。A3-T0では再採取せず、
既存CONTROL-only 60秒の`present-history-raw.json`、`oracle.json`、physical VBlank sampleだけを再解析した。
artifactは`bench/results/f3-c3-a3-t0-stall-localization-20260823-3`、summary SHA-256は
`55407e3aa702bdee9bb5275c41302431935a7298d450dbafabfc24647dfed323`である。

[事実] actual parent PresentStart identityとのexact matchから、DWM parent生成processはPID 2436へ一意に帰属した。
target lifecycle 3597件のactual parentは111件で全てraw parentへ一致した。measurement内ではtarget attach parent 110件、
DWM process全体のPresentは116件（Presented 110、Discarded 6、Lost 0）で、target非attach DWM Presentは6件だった。
したがってrawはtarget-attached系列とDWM-wide系列を独立比較でき、T0に必要なReadyTimeも保持していた。

[事実] previous target parentをmeasurement内に持つlarge batch 2件は、次のexact closureになった。

| dependent count | parent PresentStart gap | parent Display gap | interval内の全DWM PresentStart | Start→Ready | Start→Displayed | Start→completion |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 110 | 110 VBlank | 110 VBlank | 1（現parentのみ） | 1.5050 ms | 14.0511 ms / 1 VBlank | 14.0515 ms |
| 2421 | 2421 VBlank | 2421 VBlank | 1（現parentのみ） | 1.6409 ms | 12.8005 ms / 1 VBlank | 12.8010 ms |

観測max 2421のinterval中に、targetへattachされない別DWM PresentStartは0件だった。したがって長大gapは
「parent PresentStartは存在するがDisplayedだけ遅い」scanout stallではなく、また「DWM-wide Presentは進むが
mvmだけattachされない」eligibility分岐でもない。観測されたDWM PresentStart系列そのものが2421 VBlank空いている。
最初の847 batchはmeasurement内にprevious target parentが無いため、境界caseとしてこのpair判定から除外した。

[回避策] A2の`DWM_CONSUMPTION_STALL`はDWM threadのCPU/GPU hangを意味する名前として使わない。証明範囲は
「mvm Composed_FlipがDWM parent display/completionへ長期間取り込まれず、同じ期間に観測DWM PresentStart系列も
生成されない」である。visibility、occlusion、dirty-stateによるcomposition/wake suppressionでも説明できるため、
原因はまだ確定しない。

[exit] T0 verdictは`DWM_WIDE_PARENT_PRESENTSTART_GAP`である。次は
`F3-C3-A3-T1 — Visibility/Occlusion/Dirty-State Causal Proof`とし、window stateをrunnerで固定した
`VISIBLE_UNOCCLUDED` / `FULLY_OCCLUDED`比較へ進む。C3-B production scheduler、formal runtime wiring、
2% thresholdは変更せず、P2-D5-2はBLOCKEDのまま維持する。GPU queue / submit-to-scanout attributionへは進まない。

再現手順:

```powershell
pwsh scripts/p2-c3-a3-t0-stall-localization.ps1 `
  -CanonicalDirectory bench/results/f3-c3-a2-control-lifecycle-20260823-60s-1/canonical `
  -OutputDirectory '<A3-T0 output>'
```

#### F3-C3-A3-T1: Visibility/Occlusion/Dirty-State Causal Proof

[事実] T0 PASS evidenceはuser-managed commit `001c066`でcheckpointされた。T1ではCONTROL、
patched Qt、CanonicalPresentMonLiveを変更せず、15秒measurementを3循環で計9回採取した。
artifactは`bench/results/f3-c3-a3-t1-visibility-matrix-20260823-15s-1`、`matrix-proof.json`の
SHA-256は`d160f34e2c158da6c266af361bc5ce6cc34359014cebd06068ed6d94cd15ec9b`である。

[事実] runnerは各runでtarget HWND、visible、iconic、topmost、DWMWA_CLOAKED、window/client rect、
HMONITOR、foreground HWND、occluder HWND/rect、指定・非指定intersectionを10 Hzでraw記録した。
measurement内sampleは8 runが150件、1 runが149件で、全9 runでvisible=true、iconic=false、
cloaked=0、monitor/window/client rect一定、unexpected intersection=0だった。VISIBLE/DIRTYの指定被覆率は
0%、OCCLUDEDは100%だった。DIRTY companionは15秒に923〜932回更新し、mvm scheduler/render pathは
変更していない。

[事実] 一次authorityをDWM-wide PresentStart cadenceとdependency batchとし、Presentedはdownstream consequenceとした。
30 VBlank以上のgapとlarge batch、またはmeasurement内PresentStart/target parentが0〜1件の場合を
diagnostic `LARGE_SUPPRESSION`と事前分類した。結果は次のとおりである。`0/0`はgapが0ではなく、
PresentStartが0〜1件でpairを作れない`SPARSE_ZERO_OR_ONE`を表す。

| condition / set(position) | DWM PresentStart | DWM gap p95/max | target parent | batch p95/max | Presented/Discarded | class |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| VISIBLE 1(1) | 329 | 7/86 | 310 | 6/85 | 295/605 | LARGE_SUPPRESSION |
| VISIBLE 2(3) | 1 | 0/0 | 1 | 505/505 | 1/899 | LARGE_SUPPRESSION |
| VISIBLE 3(2) | 55 | 60/294 | 50 | 128/293 | 46/854 | LARGE_SUPPRESSION |
| OCCLUDED 1(2) | 78 | 32/33 | 78 | 31/32 | 78/822 | LARGE_SUPPRESSION |
| OCCLUDED 2(1) | 91 | 44/217 | 86 | 35/216 | 82/818 | LARGE_SUPPRESSION |
| OCCLUDED 3(3) | 0 | 0/0 | 0 | 900/900 | 0/900 | LARGE_SUPPRESSION |
| DIRTY 1(3) | 890 | 1/3 | 882 | 1/3 | 879/21 | REGULAR |
| DIRTY 2(2) | 872 | 1/2 | 870 | 1/1 | 871/29 | REGULAR |
| DIRTY 3(1) | 891 | 1/3 | 879 | 1/3 | 871/29 | REGULAR |

[事実] VISIBLEとOCCLUDEDは各positionですべて`LARGE_SUPPRESSION`、DIRTYは各positionですべて
`REGULAR`だった。したがって時間順序ではなく条件に追従し、matrix verdictは
`DIRTY_WAKE_SUPPRESSION`である。VISIBLEだけではgapが消えないため、formal harness visibility defect分岐は
棄却した。最終checkerで9個のraw authorityを再解析し、DWM count/gap max、batch max、Presentedが
matrix summaryと全件一致することも確認した。

[推測] 外部companionのdamageが定常的なDWM composition wakeを発生させ、mvmの既存Presentを
定常的にparentへ取り込ませたと考えるのが観測と一致する。ただしQt Quick/QRhiの
damage propagationのどの段階が欠けるかはまだ読み分けていない。

[exit] T1はPASS / `DIRTY_WAKE_SUPPRESSION`とする。次は`F3-C3-A3-T2 — Dirty Propagation Attribution`とし、
Qt Quick / QRhi側のdamage/wake propagationを追う。C3-B production scheduler、formal runtime wiring、2% thresholdは
未変更、P2-D5-2はBLOCKEDのままとする。

[事実] T1のtargeted proof/contractsはPASSだが、ordinary full suiteは627/628でFAILである。
失敗は`p2_present_id_oracle_live`の`ORACLE_SAMPLING_GAP` 1件であり、dirty suppressionとの整合は
diagnostic observationに留める。T1 PASSへ読み替えず、次の状態をcheckpointとする。

```text
T1 proof / targeted contracts : PASS
ordinary full suite           : FAIL 627/628
  p2_present_id_oracle_live   : ORACLE_SAMPLING_GAP
```

再現手順（管理者PowerShell）:

```powershell
pwsh scripts/p2-c3-a3-t1-matrix.ps1 `
  -OutputDirectory '<A3-T1 output>' -WarmupSeconds 12 -MeasureSeconds 15 -TimeoutSeconds 180
```

#### F3-C3-A3-T2: Dirty Propagation Attribution

[事実] T2-Aではdiagnostic-onlyのQt 6.11.1 patchを追加し、各
`QQuickRhiItemRenderer::update()`へpropagation serialを割り当てた。同じserialでrenderer update、
node schedule、`QQuickWindow::update()`、node render、`CompositorRhiRenderer::render()`、composition token、
`DirtyMaterial`、`textureChanged()`、QSG main render、QRhi endFrame、successful native Presentを固定POD ringへ記録する。
hot pathにallocation、mutex、I/O、logは追加していない。QtBaseはcommit
`59c81a3c2247b821b9b84b4eb8d939b77e07e276`、QtDeclarativeはcommit
`a02bed441965ee1f18f856352c7d5ee5ba35d795`へ固定した。

[事実] 最初のCONTROL
`bench/results/f3-c3-a3-t2-update-chain-control-20260823-1-invalid-runtime-contamination.json`は、QtQuickをpatched QtBase build treeへ
再リンクしたためPresentが58/15.209秒へ低下し、T1の約900/15秒を再現しなかった。runtime contaminationとして
INVALIDにした。QtQuick hookをQt6Gui exportの一度だけの動的解決へ変更し、Qt6Quick.dllだけを通常MSYS2 Qt runtimeへ
差し込む構成に分離した。

[事実] 最終runtimeで再採取・hash固定したCONTROL
`bench/results/f3-c3-a3-t2-update-chain-control-20260823-3-checkpoint.json`は15.012秒でPresent=900、
render callback=900、propagation record=900だった。measurement end境界は、stopを検出したrenderの
`NODE_RENDER/COMPOSITOR_RENDER`までを持つ1件と、そのrenderが要求した
`UPDATE/SCHEDULE/WINDOW_UPDATE`までを持つ1件で固定された。境界2件を除く898件は、全11段階のQPC順序、
composition token serial、Present serialが一対一でexact closureした。したがって`UPDATE_CHAIN_BREAK`は棄却する。

[事実] raw preflightはtarget `GWL_EXSTYLE=256 (0x100)`、
`QT_QPA_DISABLE_REDIRECTION_SURFACE`未設定、`QT_D3D_NO_FLIP`未設定、
`QT_D3D_MAX_FRAME_LATENCY=2`、`QSG_NO_VSYNC`未設定だった。少なくともこのCONTROLでは
`WS_EX_NOREDIRECTIONBITMAP (0x00200000)`は付いていない。

[事実] T2-Bの`TARGET_RHIITEM_PIXEL_TOGGLE`は最終offscreen RTVの2x2 pixelをcallback serialの偶奇で
切り替えるだけで、scheduler、source target、Present cadenceを変更しない。最初のrun
`bench/results/f3-c3-a3-t2-update-chain-target-pixel-20260823-1-invalid-marker-gap.json`ではrepeat callback 1件に
markerが無くINVALIDとした。repeatでも同じRTVへmarkerを発行するよう検査を閉じ、再採取
`bench/results/f3-c3-a3-t2-update-chain-target-pixel-20260823-3-checkpoint.json`は15.013秒でPresent=900、
marker=898、exact closure=898、effective fps=59.750だった。CONTROLのeffective fps=59.884であり、
diagnostic markerによりPresent cadenceが崩れていない。

[未検証] DWM parent cadenceを必要とするCONTROL / TARGET_PIXEL / EXTERNAL_DIRTY比較は、現在のWindows tokenが
非管理者でETW runnerのpreflightに拒否されたため未採取である。したがって
`TARGET_CONTENT_DIRTY_REQUIRED`、`TARGET_DWM_DAMAGE_SIGNAL_MISSING`、`TARGET_REDIRECTION_PATH_SUSPECT`の
いずれにもまだ分類しない。T2 exitは未達、P2-D5-2はBLOCKED、C3-B production scheduler、formal runtime wiring、
2% thresholdは未変更である。

再現手順（update chain、管理者権限不要）:

```powershell
pwsh scripts/invoke-p2-c0-native-run.ps1 `
  -HookMode on -Executable build/ucrt64-release/bin/mvm_compositor_spike.exe `
  -PatchedQtBin build/qtbase-c0/bin -PatchedQtQuickBin build/qtquick-t2-runtime `
  -SourceA tests/assets/benchmark/v1080p60_h264.mp4 `
  -SourceB tests/assets/benchmark/v1080p60_hevc.mp4 `
  -Metrics '<app.json>' -WarmupSeconds 12 -MeasureSeconds 15 `
  -SubmissionMode CONTROL -DirtyPropagationMode CONTROL
pwsh scripts/check-p2-c3-a3-t2-update-chain.ps1 `
  -AppJson '<app.json>' -ExpectedMode CONTROL -Output '<proof.json>'
```

次の採取（管理者PowerShell）:

```powershell
pwsh scripts/p2-c0-native-etw.ps1 `
  -OutputDirectory '<T2 CONTROL output>' -AcquisitionMode CanonicalPresentMonLive `
  -SubmissionMode CONTROL -DirtyPropagationMode CONTROL `
  -WarmupSeconds 12 -MeasureSeconds 15 -TimeoutSeconds 180
```

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
