# Phase 4 — Audio-master Dynamic Composition Snapshot Spike 固定契約

> 状態: **実装・計測前に freeze 済み**
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
弱めずに置く**採用済みの最小 vertical slice**である。

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

### 3.2 canonical schedule

formal schedule の canonical UTF-8 serialization は、末尾改行を含まない次の ASCII 文字列に
固定する。

```text
0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1
```

SHA-256 は canonical string の UTF-8 byte列に対して計算し、lowercase hexadecimal で
`5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79` とする。

formal rawは`schedule_kind = "formal"`を持ち、次の3表現をすべて保存する。

- `canonical_schedule`: 上記文字列と完全一致
- `canonical_schedule_sha256`: 上記 SHA-256 と完全一致
- `schedule`: boundaryを整数、stateを文字列で持つ6要素のparsed array

parsed arrayのJSON表現も次に固定する。

```json
[
  {"boundary": 0, "state": "S0"},
  {"boundary": 600, "state": "S1"},
  {"boundary": 1200, "state": "S2"},
  {"boundary": 1800, "state": "S3"},
  {"boundary": 2400, "state": "S0"},
  {"boundary": 3000, "state": "S1"}
]
```

checkerは固定文字列からarrayを独立parseし、raw arrayとの完全一致とhashを再計算する。
空白、末尾separator、末尾改行、state別名を許さない。formal結果取得後にserializationを変更しない。
formal modeはbuilt-inの上記scheduleだけを選択できる。CLIからboundary、state、canonical string、
hashを上書きするoptionは設けず、`schedule_kind = "formal"`で固定値以外を受け取った場合は
formal workload開始前にfail-closedとする。

### 3.3 CompositionEpoch semantics

measurement開始前にS0をactiveにし、frame 0に対応するactual displayの
`CompositionEpoch`をbaseline `E0`としてrawへ保存する。`E0`のabsolute valueは規定しない。
measurement ledgerのfirst recordはoutput frame 0 / state S0でなければならない。

| output frame | expected state | expected epoch |
| --- | --- | --- |
| 0..599 | S0 | E0 |
| 600..1199 | S1 | E0 + 1 |
| 1200..1799 | S2 | E0 + 2 |
| 1800..2399 | S3 | E0 + 3 |
| 2400..2999 | S0 | E0 + 4 |
| 3000..3599 | S1 | E0 + 5 |

epochを進めるのはlayout stateが変わる5 boundaryだけである。seek、decoder generation、
audio generation、同一stateの再採用では進めない。formal intervalのMUSTは
`composition_epoch_increment_count == 5`とし、counterはmeasurement baselineとの差分である。

### 3.4 resolve / adopt / noop semantics

- resolve: target output frameからimmutable scheduleのstateを一意に求める
- adopt: resolved stateがcurrent stateと異なるため、active stateを変更しepochを1進める
- noop: resolved stateがcurrent stateと同じため、stateとepochを変更しない
- reject: schedule、state、epochまたはadoption preconditionが不正で変更を拒否する

initial S0 adoptionはmeasurement開始前に完了させ、formal counter baselineから除外する。
したがってformal intervalでは`composition_state_adoption_count == 5`、
`composition_state_reject_count == 0`をMUSTとする。noopの絶対件数はdropやschedule attempt数に
依存するため固定せず、`resolve == adoption + noop + reject`の自己整合だけを要求する。

### 3.5 every-display invariant

checkerはproducerのmismatch counterを信用せず、measurement中の**全actual display ledger
record**についてcanonical scheduleから次を独立再計算する。

```text
expectedState(frame) = frameを含む最大boundaryのstate
expectedEpoch(frame, E0) = E0 + frameを含むsegment index
```

各recordはoutput frame、actual state、actual composition epoch、A/Bそれぞれのsource id、
frame number、`SourceGeneration`、`ResourceEpoch`を必須fieldとして持つ。次をすべてMUSTとする。

- actual state `== expectedState(outputFrame)`
- actual composition epoch `== expectedEpoch(outputFrame, E0)`
- A/B frame number `== outputFrame`
- A/B source id、`SourceGeneration`、`ResourceEpoch`がそのrunでadopt済みの期待identityと一致

1件でも不一致、missing、null、型不一致ならFAILとする。

### 3.6 activation lag

各boundary `b in {600,1200,1800,2400,3000}`について次のように固定する。

```text
firstDisplayedFrameAfterBoundary(b)
  = actual display ledgerに存在する frame >= b の最小frame
activationLag(b)
  = firstDisplayedFrameAfterBoundary(b) - b
```

5件すべてで`0 <= activationLag <= 2`をMUSTとする。boundaryから2 frame以内にactual displayが
無ければFAILである。これは旧state表示の猶予ではない。最初のdisplay自体がnew state / new epochを
満たさなければevery-display invariantでFAILとし、lag値では救済しない。
`old_state_after_boundary_count == 0`も独立MUSTとして維持する。

### 3.7 Phase 4 が追加する能力

- output frame number から期待 composition state を一意に解決する小さな schedule
- audio-master scheduler が決めた target frame に対し、pairing より前に該当 state を adopt する
- state が実際に変わるときだけ `CompositionEpoch` を進め、同一 state の再採用は no-op とする
- display ledger に output frame、state id、composition epoch、全 layer identity を値で記録する
- 各 boundary 後の最初の新 state 表示について、小領域 probe で layout / opacity を検査する

### 3.8 smoke schedule

smokeはformal scheduleを短縮して使わず、次の専用scheduleに固定する。

```text
0:S0;200:S1;400:S2
```

- `schedule_kind = "smoke"`
- measurementは10秒、output frames `[0, 600)`
- segmentsは`0..199 = S0`、`200..399 = S1`、`400..599 = S2`
- canonical UTF-8 serializationは上記文字列、末尾改行なし
- smoke SHA-256は`418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8`
- `formal_verdict = "NOT_RUN"`

parsed arrayは次に固定する。

```json
[
  {"boundary": 0, "state": "S0"},
  {"boundary": 200, "state": "S1"},
  {"boundary": 400, "state": "S2"}
]
```

smoke checkerもcanonical string、smoke hash、3要素parsed arrayを相互検査する。formal scheduleの
SHA-256をsmoke rawへ書かず、smoke結果をformal PASSへ使用しない。

initial S0はmeasurement前にactiveとし、smoke counter baselineから除外する。smoke intervalの
MUSTはtransition 2、adoption 2、epoch increment 2、reject 0、全displayのstate / E0相対epoch /
layer identity一致、`source_generation_change_due_to_layout_count == 0`である。

smokeではactivation lagとtransition probeの経路も検査する。対象boundaryは200 / 400だけとし、
lag rawは2件で各0..2、probeは2点 x 2 transitionの4件、probe mismatch / old state after boundary /
render-thread blocking waitは0とする。formalの5 lag / 10 probeをsmokeへ要求しない。

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
| `ResourceEpoch` | `FFmpegD3D11Decoder` | open / decode pool 再作成のみ |
| `SourceGeneration` | `FFmpegD3D11Decoder` / `SourceDecodeWorker` | decoderがseek / flushで発行し、workerがsource-localにpublish。layout transitionでは不変 |
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
| `CompositorCoordinator` | state id の値保持と target frame に対応した state adoption を追加 |
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
2. Source A / B と audio source を open し、canonical schedule / hash / parsed arrayを検証してpublishする
3. frame 0 / sample 0 の exact seek、generation adoption、video / audio pre-roll を完了する
4. S0をactiveにしてinitial adoptionをformal counter baselineから除外する
5. WASAPI endpoint を開始し、`IAudioClock` anchor を確立する
6. initial S0 setup後、measurement開始直前にcounter baselineを確定する
7. audio-master scheduling を enable にしてmeasurementを開始する
8. measurementのfirst actual displayをframe 0 / S0と検査し、そのepochを`E0`として記録する
9. render thread は各 target frame の state を resolve してから exact pair を compose する
10. 終了時は scheduler disable、audio sink stop、audio/video worker stop + join の順に進める
11. worker join 後だけ render teardown を要求し、GPU retirement を有限 timeout で drain する
12. device / Qt resource release 後に final metrics を保存する

thread join 前の device release、未追跡 GPU submission の release、drain timeout はすべて失敗とする。

## 8. failure semantics

次は process failure とし、formal summary で平均や別 run による救済をしない。

- schedule が空、frame 0 state が無い、boundary が非単調、state id が未知
- canonical schedule文字列、SHA-256、parsed arrayのいずれかが不一致
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

- contract / raw schema version、`schedule_kind`、`formal_verdict`
- modeに対応するcanonical schedule文字列、SHA-256、parsed array
- measurement baseline `E0`
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
- `transition_probe_checked_count`
- `transition_probe_mismatch_count`
- `transition_probe_render_thread_blocking_wait_count`
- `source_generation_change_due_to_layout_count`
- measurement中の全actual display ledger recordと、その完全なstate / epoch / layer identity

formal intervalのcounterはinitial S0 setup後のbaselineとの差分とする。producer が出した
percentile、expected state / epoch、mismatch counter、verdictをcheckerは信用せずrawから再計算する。

### 9.1 formal expected counts

各formal runの期待値を次に固定する。

| metric | expected |
| --- | ---: |
| schedule segments | 6 |
| state transitions | 5 |
| `composition_state_adoption_count` | 5 |
| `composition_epoch_increment_count` | 5 |
| `transition_activation_lag_frames` raw length | 5 |
| `composition_state_reject_count` | 0 |
| `composition_state_display_mismatch_count` | 0 |
| `old_state_after_boundary_count` | 0 |
| `transition_probe_checked_count` | 10 |
| `transition_probe_mismatch_count` | 0 |
| `transition_probe_render_thread_blocking_wait_count` | 0 |
| `source_generation_change_due_to_layout_count` | 0 |

全actual display recordにstate、epoch、A/B layer identityが存在しなければならない。
`composition_state_resolve_count`と`composition_state_noop_count`の絶対値は固定しないが、
`resolve == adoption + noop + reject`をMUSTとする。

## 10. fixtures と validation

### 10.1 fixtures

- `tests/assets/p3_audio/p3_av_h264_aac.mp4`
- `tests/assets/p3_audio/p3_video_hevc_b.mp4`
- 既存 manifest と SHA-256

新しい media fixture は作らない。probe referenceは固定fixture SHA-256とactual frame numberを
入力に、product decoder / shaderと独立したtest-only CPU reference pathで得る。

### 10.2 transition probe contract

各transitionの`firstDisplayedFrameAfterBoundary`について、actual 1920x1080 RHI targetの次の2点を
各1回だけ読む。座標原点は左上、probe sizeは各`1x1 RGBA8`である。

| probe | output coordinate | purpose |
| --- | --- | --- |
| TL | (480, 270) | 左上PiP stateではoverlap、右下PiP stateではA-only |
| BR | (1440, 810) | 右下PiP stateではoverlap、左上PiP stateではA-only |

boundaryは600、1200、1800、2400、3000の5件なので、formal runあたり
`transition_probe_checked_count == 10`とする。probeはnew state / epochを持つfirst displayと同じ
output textureに対して発行する。

期待値はPhase 2の独立probe contractを共有test helperへ切り出して再利用し、Phase 4側へ係数や
blend式を複製しない。現在の根拠実装は
`tests/gpu_preview/test_p2_gpu_compositor.cpp`の`expected709`、`blend`、`probeEquals`である。
reference pathは固定fixtureの該当frameをCPUでplanar YUVとしてdecodeし、上記pixel centerに
対応するnormalized UVをlinear samplingする。product D3D11 texture、`Nv12Converter`、
product shader、output/source probe結果を期待値生成に使わない。
reference extractionはfixture SHA-256検証後、formal measurement開始前にorchestration側で行い、
appのelapsed time、preview readback counter、performance分布へ含めない。

reference sampling座標は次に固定する。

| value | normalized UV |
| --- | --- |
| A(TL) | `((480 + 0.5) / 1920, (270 + 0.5) / 1080)` |
| A(BR) | `((1440 + 0.5) / 1920, (810 + 0.5) / 1080)` |
| B(center) | `((480 + 0.5) / 960, (270 + 0.5) / 540)` |

BT.709 limitedの変換はPhase 2と同じ標準式を使う。

```text
C = Y - 16, D = U - 128, E = V - 128
R = clamp(round(1.164383*C + 1.792741*E))
G = clamp(round(1.164383*C - 0.213249*D - 0.532909*E))
B = clamp(round(1.164383*C + 2.112402*D))
```

PiP overlapのstraight-alpha期待値はchannelごとに
`round(B * opacity + A * (1 - opacity))`とする。state別期待値は次のとおり。

| state | TL expected | BR expected |
| --- | --- | --- |
| S0 / S3（右下） | A(TL) | blend(B(center), A(BR), opacity) |
| S1 / S2（左上） | blend(B(center), A(TL), opacity) | A(BR) |

S0/S1はopacity 0.75、S2/S3は0.50を使う。各RGB channelの許容差はPhase 2と同じ±3、
alphaはexact 255とする。期待値はactual first frame `f`に対して計算するため、lag 1/2でも
boundary frameの色を誤用しない。

readbackはfull-frameでなく上記10 pixelだけとする。各copyをGPU completion serialで追跡し、
Qt render threadでblocking waitしない。`transition_probe_render_thread_blocking_wait_count == 0`を
MUSTとし、performance区間に同期stallを混ぜない。small-region copy countは別counterへ記録し、
full-frame CPU readback / full-frame GPU copy countを増やさない。

### 10.3 smoke validation

- `schedule_kind = "smoke"`、10秒、frames `[0, 600)`、3 segment / 2 transition
- S0 / S1 / S2をframe 0..199 / 200..399 / 400..599にexact適用
- adoption 2、epoch increment 2、state reject / display mismatch / layout起因generation変更 0
- activation lag raw 2件、各0..2、transition probe checked 4 / mismatch 0
- old state after boundary / probe render-thread blocking wait 0
- `formal_verdict = "NOT_RUN"`
- schedule parser / resolver の pure unit test
- boundary 前後、同一 state no-op、非単調 boundary、未知 state の negative test
- stale epoch / wrong state / missing metric / false Boolean を checker が拒否する negative test
- smoke rawへformal schedule/hashまたはformal expected count 5/10を入れたnegative test
- Release / Debug ordinary CTest。対象件数 0 は失敗

smoke は経路確認であり Phase 4 の formal PASS に使わない。

### 10.4 formal validation

- clean worktree の Release build
- `schedule_kind = "formal"`かつ固定formal schedule / SHA-256以外をworkload開始前に拒否
- 5 秒 warmup + 60 秒 measurement、3 independent processes
- 全 run が独立 PASS。平均による救済をしない
- 6 segment / 5 transitionのすべてで全displayのstate / epoch / layer identityが一致
- `composition_state_adoption_count == 5`、`composition_epoch_increment_count == 5`
- 各 transition の activation lag は 0～2 output frame
- activation lag raw値5件、transition probe checked 10 / mismatch 0
- old state after boundary 0、state reject 0、probe render-thread blocking wait 0
- `source_generation_change_due_to_layout_count == 0`
- P3-C-2 の Playback performance / A/V / correctness threshold を全て維持
- teardown と provenance を含む summary を raw から再計算する

## 11. regression matrix

Phase 4 は `CompositorCoordinator`、Qt render path、Phase 3 integrated harness という shared path を通る。
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

- formal 3/3 が §10.4 の全条件を満たす
- P3-C-2 Playback の `effective_video_fps >= 55`、`drop_rate <= 0.02` を維持
- application A/V absolute p95 `<= 20.000 ms`、observed max `<= 33.334 ms`
- marker / pair / generation / state / epoch / probe mismatch がすべて 0
- state adoption / epoch incrementが各5、activation lag rawが5件、probe checkedが10
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
Phase 4 PASSはPhase 1～3のclosure結果を再判定または変更しない。

preview backend spike 全体を閉じるには、少なくとも別ラリーで次を canonical に定義する必要がある。

- spike 全体の goal と exit criteria
- Phase 4 が最終 phase か、後続 phase が必要か
- Phase 1～4 の evidence から backend を採用 / 条件付き採用 / 不採用のどれにするか
- ADR 0002 を supersede / accept / reject する decision record

## 14. Phase 4 外の機能の固定分類

これは既存 canonical scope の回収結果ではなく、本契約が置く固定境界である。

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
