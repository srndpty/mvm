# Phase 4 — Audio-master Dynamic Composition Snapshot Spike 契約案

> 状態: **Phase 4 scope recovery で freeze する実装前契約案**
>
> この文書は Phase 4 実装を含まない。Phase 1～3 の historical contract、formal result、
> threshold を変更しない。また、Phase 4 の合格を全 phase の FINAL PASS や製品採用決定と
> 読み替えない。

## 1. scope recovery の結論

Phase 4 は既存の canonical documentation では定義されていなかった。

- `phase1-plan.md` は冒頭で Phase 1 のみを規定すると明記し、§16 も Phase 2 へ進めるかを
  判断するところまでしか定めない
- `phase2-plan.md` は Phase 2 を規定し、Phase 3 へ進む条件までを定める
- Phase 3 の各 contract は P3-A / P3-C-1 / P3-C-2 の条件だけを定める
- `phase3-findings.md` の Phase 4 言及は「Phase 4へ進まない」「Phase 4は実行しない」という
  closure 時の履歴だけで、Phase 4 の goal、scope、architecture、exit criteria を持たない
- README と ADR 0002 も Phase 1 の説明に留まり、Phase 5 または全 phase の exit criteria を
  定義していない

したがって本書の Phase 4 は既存定義の回収ではなく、Phase 1～3 の成立済み architecture を
弱めずに置く**新規の最小 vertical slice 提案**である。

## 2. Phase 4 goal

**単一の audio master による連続再生中、固定された composition schedule を output frame
境界で切り替え、各表示が exact source/frame/generation と、その frame に対応する
composition snapshot を同時に満たすことを実証する。**

Phase 2 は二動画合成と独立した layout stress、Phase 3 は固定 layout の audio-master scheduling を
検証した。Phase 4 は両者の未検証だった結合部だけを閉じる。

## 3. fixed scope

### 3.1 formal workload

- display baseline は P3-C-2 と同じ 1920x1080 Window / Surface / actual RHI target、DPR 1.0
- Source A / Source B / audio は既存の P3 fixture を再利用し、新しい media fixture は作らない
- video は 1920x1080、60 fps の二 source、audio は 48 kHz stereo float32 internal PCM の
  一 sourceとする
- audio device の `IAudioClock` を唯一の master clock とし、QPC fallback を許さない
- 5 秒 warmup 後に video frame 0 / audio sample 0 へ exact seek し、60 秒を測定する
- output frame 0、600、1200、1800、2400、3000 を state boundary とし、次の既存 Phase 2 layout を
  決定論的に切り替える

| state | Source A | Source B |
| --- | --- | --- |
| S0 | 全面、opacity 1.0 | 右下 960x540、opacity 0.75 |
| S1 | 全面、opacity 1.0 | 左上 960x540、opacity 0.75 |
| S2 | 全面、opacity 1.0 | 左上 960x540、opacity 0.50 |
| S3 | 全面、opacity 1.0 | 右下 960x540、opacity 0.50 |

state sequence は `S0, S1, S2, S3, S0, S1` に固定する。schedule は playback 開始前に
immutable value として publish し、formal workload 中は変更しない。

### 3.2 Phase 4 が追加する能力

- output frame number から期待 composition state を一意に解決する小さな schedule
- audio-master scheduler が決めた target frame に対し、pairing より前に該当 state を adopt する
- state が実際に変わるときだけ `CompositionEpoch` を進め、同一 state の再採用は no-op とする
- display ledger に output frame、state id、composition epoch、全 layer identity を値で記録する
- 各 boundary 後の最初の新 state 表示について、小領域 probe で layout / opacity を検査する

## 4. explicit non-goals

Phase 4 では次を実装しない。

- product timeline model、timeline editor、track UI、undo / redo、product UI
- source clip の追加・削除・差し替え、source time mapping、trim、ripple edit
- multiple audio mixing、volume automation、pan、time stretch、variable-speed playback
- scrub audio、transition、keyframe animation、text、subtitle、effect
- proxy 生成または GPU preview path への proxy resolver 統合
- export / encode / offline render
- device lost / adapter change からの recovery
- 3 本以上の video、複数 GPU、HDR / tone mapping / ICC、別 graphics backend
- software decode fallback、CPU preview path

上記を Phase 4 の実装量へ束ねない。Phase 4 後の帰属も既存 docs では未定義であり、必要なら別の
contract で定める。Phase 5 という名称は本書では新設しない。

## 5. preserved architecture

次を Phase 4 の前提契約とし、暗黙に緩めない。

```text
FFmpeg demux/decode
  -> D3D11VA hardware frame
  -> exact A=N / B=N / generation / CompositionEpoch pairing
  -> D3D11 GPU compositor
  -> QQuickRhiItem

single AudioDecodeWorker
  -> 48 kHz stereo float32 queue
  -> WASAPI shared event-driven sink
  -> IAudioClock master
  -> audio-master video scheduler
```

- per-frame full CPU readback、`av_hwframe_transfer_data`、swscale RGBA preview は禁止
- per-frame full-frame GPU copy は禁止
- frame / SRV / lifetime payload は GPU completion serial 完了まで retire しない
- stale / future generation、stale epoch、missing pair、device mismatch は fail-closed
- exact pair が無いときに latest frame または片側の古い frame を代用しない
- QPC は timestamp / latency / provenance にだけ使い、master clock にしない

## 6. ownership と architecture

### 6.1 state / generation ownership

| state | owner | Phase 4 で進める条件 |
| --- | --- | --- |
| `SourceId` | `SourceRegistry` | source register のみ。Phase 4 transition では不変 |
| `ResourceEpoch` | 各 decoder | open / decode pool 再作成のみ |
| `SourceGeneration` | 各 source worker | seek / flush のみ。layout transition では不変 |
| audio `SourceGeneration` | `AudioDecodeWorker` | audio seek / flush のみ |
| composition schedule | Phase 4 harness/controller | playback 前に immutable publish |
| active composition state | render path | target output frame から解決 |
| `CompositionEpoch` | `CompositorCoordinator` | resolved state が実際に変わるときだけ |

decoder、audio worker、scheduler は `CompositionEpoch` を発行しない。display 時に mutable な
current epoch を後付けせず、`ComposedFrame` が採用時の state id と epoch を値で保持する。

### 6.2 thread ownership

| thread | responsibility |
| --- | --- |
| GUI thread | immutable schedule の構築と開始前 publish、操作、shutdown orchestration |
| video decode thread A / B | source-local demux/decode、seek、bounded buffer |
| audio decode thread | single audio source の decode / resample / queue |
| WASAPI render thread | endpoint 書き込み、`IAudioClock` query / anchor 更新 |
| Qt render thread | audio clock から得た target frame の受領、state resolve / adopt、exact pair、GPU compose、ledger record |

formal workload 開始後に GUI thread から mutable layout を直接書き換えない。

### 6.3 component impact

| component | expected change |
| --- | --- |
| Decoder / `SourceDecodeWorker` | 変更不要 |
| `SourceRegistry` | 変更不要 |
| `CompositionCoordinator` | state id の値保持と target frame に対応した state adoption を追加 |
| `GpuCompositor` | shader / copy path は変更不要。既存 layout を描画するだけ |
| `QQuickRhiItem` / `CompositorRhiItem` | pair 前の schedule resolve/adopt と ledger metric を追加 |
| `AudioDecodeWorker` | 変更不要 |
| `WasapiAudioSink` | 変更不要 |
| `AudioMasterClock` | 変更不要 |
| audio-video scheduler | target frame の決定式は変更不要 |
| fixture / harness | 既存 fixture を使う Phase 4 専用 app/controller、checker、matrix、negative test を追加 |

既存 component に変更不要と書いた項目へ、都合のよい fallback や Phase 4 専用分岐を追加しない。

## 7. lifecycle

1. P3-C-2 display preflight を decoder / audio pipeline open 前に通す
2. Source A / B と audio source を open し、immutable schedule を検証して publish する
3. frame 0 / sample 0 の exact seek、generation adoption、video / audio pre-roll を完了する
4. WASAPI endpoint を開始し、`IAudioClock` anchor を確立する
5. audio-master scheduling を enable にする
6. render thread は各 target frame の state を resolve してから exact pair を compose する
7. 終了時は scheduler disable、audio sink stop、audio/video worker stop + join の順に進める
8. worker join 後だけ render teardown を要求し、GPU retirement を有限 timeout で drain する
9. device / Qt resource release 後に final metrics を保存する

thread join 前の device release、未追跡 GPU submission の release、drain timeout はすべて失敗とする。

## 8. failure semantics

次は process failure とし、formal summary で平均や別 run による救済をしない。

- schedule が空、frame 0 state が無い、boundary が非単調、state id が未知
- target frame に対応する state を一意に解決できない
- state adoption の拒否、意図しない epoch increment / regression / no-increment
- expected state と display ledger の state / epoch / layer identity が不一致
- transition 後に旧 state / epoch を表示
- exact pair 不足を stale/latest frame で代用
- audio clock failure、QPC master fallback、device lost、lifecycle violation
- full-frame CPU readback、full-frame GPU copy、software video fallback
- metric missing / null、schema 不一致、NaN / Infinity、件数自己不整合

失敗時も取得済み provenance と counter を保存し、nonzero で終了する。無効な schedule を
固定 layout へ暗黙に縮退させない。

## 9. metrics

P3-C-2 の全 field に加え、raw へ少なくとも次を保存する。

- contract / raw schema version、schedule SHA-256 または canonical serialization hash
- boundary frame、expected state id、expected epoch の一覧
- `composition_state_resolve_count`
- `composition_state_adoption_count`
- `composition_state_noop_count`
- `composition_state_reject_count`
- `composition_epoch_increment_count`
- `composition_state_display_mismatch_count`
- `old_state_after_boundary_count`
- `transition_activation_lag_frames` の raw 5 件
- transition ごとの first displayed output frame / state / epoch / layer identity
- transition probe checked / mismatch
- source generation change during layout transition

producer が出した percentile や verdict を checker は信用せず、raw から再計算する。

## 10. fixtures と validation

### 10.1 fixtures

- `tests/assets/p3_audio/p3_av_h264_aac.mp4`
- `tests/assets/p3_audio/p3_video_hevc_b.mp4`
- 既存 manifest と SHA-256

期待 layout と opacity は shader 実装から作らず、contract 固定値から checker / test の期待値を
独立に持つ。新しい media fixture は不要である。

### 10.2 smoke validation

- 10 秒、1 秒 warmup、少なくとも 3 state を通す短縮 playback
- schedule parser / resolver の pure unit test
- boundary 前後、同一 state no-op、非単調 boundary、未知 state の negative test
- stale epoch / wrong state / missing metric / false Boolean を checker が拒否する negative test
- transition probe の positive / negative
- Release / Debug ordinary CTest。対象件数 0 は失敗

smoke は経路確認であり Phase 4 の formal PASS に使わない。

### 10.3 formal validation

- clean worktree の Release build
- 5 秒 warmup + 60 秒 measurement、3 independent processes
- 全 run が独立 PASS。平均による救済をしない
- 5 transition のすべてで state / epoch / layer identity が一致
- 各 transition の activation lag は 0～2 output frame
- transition probe mismatch 0、old state after boundary 0、state reject 0
- layout transition による video / audio source generation change 0
- P3-C-2 の Playback performance / A/V / correctness threshold を全て維持
- teardown と provenance を含む summary を raw から再計算する

## 11. regression matrix

Phase 4 は `CompositionCoordinator`、Qt render path、Phase 3 integrated harness という shared path を通る。
Phase 4 closure には次をすべて要求する。

| gate | reason |
| --- | --- |
| Phase 4 ordinary Release / Debug CTest | 新規 contract と negative を検証 |
| Phase 4 formal 3/3 | new path の判定 |
| P3-C-2 formal 9/9 | audio-master playback / seek / pause-resume と display baseline の回帰 |
| P3-A standalone regression | audio decode / WASAPI / clock の単独回帰 |
| P2-D5-1 formal regression | exact pairing、compositor、layout、GPU lifetime の回帰 |
| P1 formal regression | decode -> QQuickRhiItem と zero-copy invariant の回帰 |

shared source を変更しなかったという理由だけで gate を省略しない。未実行は PASS にしない。

## 12. Phase 4 exit criteria

次をすべて満たした場合だけ **Phase 4 FINAL PASS under this contract** とする。

- formal 3/3 が §10.3 の全条件を満たす
- P3-C-2 Playback の `effective_video_fps >= 55`、`drop_rate <= 0.02` を維持
- application A/V absolute p95 `<= 20.000 ms`、observed max `<= 33.334 ms`
- marker / pair / generation / state / epoch / probe mismatch がすべて 0
- CPU full-frame readback、full-frame GPU copy、software fallback、QPC master fallback が 0
- device lost、GPU completion failure、early release、retirement timeout、lifecycle / join leak が 0
- display preflight と start/end/matrix provenance が成立
- §11 の regression gate がすべて PASS
- start / end HEAD 一致、worktree clean、fixture / executable / contract provenance 一致

1 項目でも未測定または不合格なら Phase 4 は PASS ではない。結果を見て threshold、workload、
boundary を変更しない。

## 13. Phase 4 と全体 closure の関係

canonical docs は Phase 4 を最終 phase と定義しておらず、Phase 5 も定義していない。
ADR 0002 は preview backend の製品採用を別 ADR（0003 を想定）へ残したままである。

したがって本契約で Phase 4 が PASS しても、意味するのは §2 の vertical slice が成立したことだけである。
**Phase 4 PASS != 全 phase の FINAL PASS != preview backend の製品採用決定** とする。

preview backend spike 全体を閉じるには、少なくとも別ラリーで次を canonical に定義する必要がある。

- spike 全体の goal と exit criteria
- Phase 4 が最終 phase か、後続 phase が必要か
- Phase 1～4 の evidence から backend を採用 / 条件付き採用 / 不採用のどれにするか
- ADR 0002 を supersede / accept / reject する decision record

## 14. Phase 4 外の機能の提案上の分類

これは既存 canonical scope の回収結果ではなく、本契約案が置く境界である。

| classification | items |
| --- | --- |
| Phase 4 で行う | audio-master 再生中の固定 composition schedule、exact state / epoch display |
| 後続 contract で個別判断 | multiple audio mixing、volume automation、time stretch / variable speed、scrub audio、transition、proxy integration、device-loss recovery |
| spike phases 対象外 | product timeline editor / Project Model / undo-redo / product UI、text / subtitle / effect UI、export / encode、installer / signing、別 graphics backend、複数 GPU、HDR / ICC |

後続 contract の項目を自動的に Phase 5 と呼ばず、一度に束ねない。

## 15. 検討した代替案

| candidate | why not selected for Phase 4 |
| --- | --- |
| two-audio fixed mixer | audio queue / mixer / sink ownershipを同時に変え、P3 の single-audio clock contract より変更面が大きい |
| device-loss recovery | 重要だが、再現可能な formal fault injection と Qt / FFmpeg / WASAPI 全体の再構築を先に定義する必要があり最小 slice ではない |
| proxy integration | Phase 0 resolver 資産はあるが、P1～P3 が成立させた composition / A/V の未結合点を先に閉じる方が architecture risk を直接減らす |
