# Phase 2 の所見

記述は Phase 0 と同じ規則で分類する。混ぜない。

| 印         | 意味                                               |
| ---------- | -------------------------------------------------- |
| `[事実]`   | 実際に実行して観測した。再現手順を併記する         |
| `[推測]`   | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない       |
| `[回避策]` | 現在の対処。恒久策とは限らない                     |
| `[exit]`   | exit criteria への影響                             |

---

## 1. P2-D4A Playback formal の source coverage 修正

### 1.1 [事実] P2-D2 の Playback 測定区間は source frame 0 から始まっていなかった

P2-D2 formal の raw / summary は `formal_contract_version = P2-D1-1` のまま保存し、
`p2_pass = false` という当時の判定を変更していない。P2-D4A ではこの成果物を
上書きせず、修正後の smoke を `build/ucrt64-release/p2-d4a-smoke/` へ分離した。

旧 harness は warmup で進んだ 60 Hz scheduler と source buffer を、そのまま
measurement へ引き継いでいた。このため「60 秒の Playback formal」が source の
frame 0..3599 を測る契約になっておらず、source 末尾への到達と EOF を性能問題から
区別できなかった。

修正後の formal contract は `P2-D4-1` とする。変更した意味は次のとおり。

- warmup 終了時に render thread から scheduler 停止の ACK を返し、その後にだけ
  A/B を pause、frame 0 へ exact seek、source generation 同期する
- measurement 開始 callback で QPC 起点と scheduler frame 0 を同時に設定する
- 測定区間を `[start, end)` とし、60 秒 formal の slot を frame 0..3599 の
  3600 件に固定する。frame 3600 は含めない
- source frame count、coverage、measurement 区間の missing pair / EOF A / EOF B を
  raw JSON に記録し、formal checker は fail-closed で検査する

### 1.2 [事実] checker の negative test で新しい契約を固定した

対照群に加え、scheduled 3601 / 3599、coverage false、source A 3599 frame、
missing pair 1、EOF A 1、EOF B 1 をそれぞれ拒否する検査を追加した。
Release / Debug とも、P2 checker を含む限定回帰は 24/24 通過した。
既存の fps 55 以上、drop rate 2% 以下、seek p95 / observed max の閾値は変更していない。

### 1.3 [事実] 5 秒 warmup + 15 秒 measurement の単一 smoke

RTX 4090、H.264 / HEVC 各 3600 frame、seed 20260808、fence backend で 1 process だけ
実行した。formal 3 run x 60 秒は P2-D4A では実行していない。

- required frame 900、source coverage true
- first measurement output frame 0
- scheduled 900、displayed 897、deadline drop 3
- measurement missing pair 0、EOF A 0、EOF B 0
- QPC elapsed 15.0125577 秒
- effective fps 59.7499785、drop rate 0.0033333（いずれも smoke の診断値）

### 1.4 [exit] D4B へ進めるが、P2 の最終判定はまだ更新しない

P2-D4A の source coverage 修正と短縮経路確認は成立したため、D4B の parallel
dual seek 実装・短縮検証へ進める。formal Playback / Seek 全6 runの再実行はD5で
clean HEADから行う。P2-D4A の smoke 値を P2 の合否根拠には使わない。
D5で `P2-D4-1` の rawから集計するまでは、P2-D2 の `p2_pass = false` が最後の
正式判定である。

## 2. P2-D4B parallel dual-source exact seek

### 2.1 [事実] A/B seekの直列待機をsource-local workerへ分離した

`SourceDecodeWorker` に request / wait を分けた非同期seek commandを追加した。
sourceごとにoutstandingは最大1件で、2件目は `RejectedBusy`、古いticketは
`StaleTicket`、stop中は `Stopped` completionとしてfail-closedにする。
seek本体は既存worker threadで実行し、seekごとのthread生成は行わない。

decoder seek、generation更新、exact target decode、source-local buffer submitは
共通executorに一本化した。`seekBlocking` はstartupとD4A resetの同期挙動を保つため
caller同期の共通executor呼び出しとし、async outstanding中は拒否する。

controllerはA/Bへ先にrequestをdispatchし、tickで両completionをpollする。
A/B両方のrequestId、target、decoded frame、generation、resource epochを検証するまで
composition generationと`requestedOutput`を更新しない。

### 2.2 [事実] 64点exact integration

H.264 A / HEVC Bのdeterministic 64点で、A exact 64/64、B exact 64/64、
pair exact 64/64、実行区間overlap 64/64を観測した。mismatch、timeout、
stale completion acceptance、busy acceptance、generation cross-impact、software fallback、
CPU full-frame readback、worker join leakはいずれも0。A/B textureは同一D3D11 deviceだった。

### 2.3 [事実] D3比較用256 seek x 3

seed 20260808、fence backend、3 independent processを
`build/ucrt64-release/p2-seek-profile-d4b/`へ出力した。旧D3 rawは上書きしていない。

| run | A request-ready p95 | B request-ready p95 | dual ready p95 (D3 -> D4B) | request-display p95 (D3 -> D4B) | overlap |
| --- | ------------------: | ------------------: | -------------------------: | ------------------------------: | ------: |
| 1   |            76.78 ms |            74.45 ms |         105.28 -> 78.02 ms |              114.62 -> 83.41 ms | 256/256 |
| 2   |            75.94 ms |            74.00 ms |         108.81 -> 77.75 ms |              120.65 -> 83.36 ms | 256/256 |
| 3   |            75.39 ms |            71.88 ms |         107.85 -> 76.29 ms |              116.28 -> 83.39 ms | 256/256 |

dual ready p95は25.9% / 28.5% / 29.3%、request-display p95は
27.2% / 30.9% / 28.3%短縮した。全runでmismatch、timeout、stale、busy acceptance、
software fallback、CPU full-frame readback、device lost、join leakは0だった。

**[事実]** parallel化後のdecoder D3D11 lock wait p95はA 1.127..1.130 ms、
B 1.122..1.130 msへ増えた。maxはA 2.90..3.46 ms、B 2.87..3.29 ms。
render lock wait p95は全run 0.1 us、max 11.0..45.8 usだった。
lockを外す変更は行っていない。

### 2.4 [事実] D4A Playback回帰は安定してMUSTを満たさなかった

5秒warmup + 15秒measurementを複数回確認した。全runでfirst output 0、scheduled 900、
EOF A/B 0、coverage true、CPU readback 0、device lost 0だったが、
measurement開始直後の`WaitingForSource`によりmissing pairが1件になるrunがあった。
観測4 run中、missing pair 0は1 run、missing pair 1は3 runだった。

async wrapperが原因かを切り分けるため`seekBlocking`をcaller同期の共通executorへ戻しても
再現した。したがってparallel seekの性能値は改善したが、D4B exit criteriaの
「Playback D4A regression成功」は安定して成立していない。

### 2.5 [未検証] D5 formalへは進まない

Release / Debugの対象30 testは各test単体では成功を観測したが、まとめ実行では
`p2_dual_decode_integration`がまれに30秒completion timeoutとなるrunも観測した。
256 x 3 diagnosticではtimeout 0だが、この不安定性を無視して「deadlock 0」とは書かない。

Playback missing pairとintegration timeoutの再現条件を解消するまで、D5のclean HEAD
formal全6 runへ進めない。P2-D2の`p2_pass = false`を引き続き最後の正式判定とする。

## 3. P2-D4C playback pre-roll / parallel seek reliability closure

### 3.1 [事実] 測定開始前に固定8 frameのpre-rollを追加した

Playbackの測定開始を `MeasurementResetStart`、`MeasurementResetWait`、
`MeasurementPrimeStart`、`MeasurementPrimeWait`、`MeasureStartWait`、`Measure` に分けた。
reset後はschedulerを停止したままA/B workerだけを再生し、buffer先頭が現generationの
frame 0、かつA/B両方のdepthが8以上になるまでconsumer popを開始しない。watermarkは8、
buffer capacityは16、timeoutは2000 msで固定し、測定値による調整はしていない。

formal contractを`P2-D4-2`へ更新し、設定watermark、pre-roll成立、A/B depth、A/B frontを
raw JSONとcheckerへ追加した。depth 7、front 1、成立false、設定値7を個別に拒否する
negative testと、generation / EOF / fatal / timeout / scheduler開始前 / 非破壊判定の
pure testを追加した。

修正後の1秒warmup + 2秒measurementを20 independent processで実行した結果は
`build/ucrt64-release/p2-d4c-reliability/playbackstartup-ucrt64-release-summary.json`にある。
20/20でfirst output 0、pre-roll depth A/Bは各8..16、front A/Bは0、missing pair、EOF A/B、
non-deadline drop、device lost、worker join leakはいずれも0だった。

### 3.2 [事実] 5秒 + 15秒 Playback回帰は3/3でmissing pair 0になった

修正後のrawと機械集計は
`build/ucrt64-release/p2-d4c-reliability/playbackregression-ucrt64-release-summary.json`
に保存した。

| run | pre-roll depth A/B | first | scheduled | displayed | deadline drop | missing / EOF A / EOF B | effective fps | drop rate |
| --- | -----------------: | ----: | --------: | --------: | ------------: | ----------------------: | ------------: | --------: |
| 1   |            16 / 16 |     0 |       900 |       891 |             9 |               0 / 0 / 0 |        59.283 |    1.000% |
| 2   |            16 / 16 |     0 |       900 |       888 |            12 |               0 / 0 / 0 |        59.154 |    1.333% |
| 3   |            16 / 16 |     0 |       900 |       898 |             2 |               0 / 0 / 0 |        59.817 |    0.222% |

全dropはscheduler deadline分類であり、missing source、generation、composition epoch、
render failureによるdropは全runで0だった。この3 runは短縮回帰であり、formal判定値ではない。

### 3.3 [事実] completion publishをfail-closed化し、timeout時の状態を可視化した

`SourceSeekMailbox::publish`はsilent returnを廃止し、`Published`、outstandingなし、
二重公開、request不一致、stop completionによる置換を別の結果として返す。通常実行中の
非`Published`はfatalにし、publish rejectとrequest mismatchをraw JSONへ記録する。
requestがinvalid / busy / stoppedで拒否された場合はplayback stateを変更しない。

seek executorは `Idle`、`Queued`、`WaitingDecoderMutex`、`DecoderSeek`、
`GenerationReset`、`RequestExactFrame`、`SubmitExactFrame`、`PublishingCompletion`、
`Completed` を記録する。snapshotはdecoder mutexを待たず、request id、target、phase開始QPC、
最終進捗QPC、mailbox pending / outstanding / completion ready / current ticketを取得する。
統合testのA/B待機は逐次30秒 + 30秒ではなく、共通30秒deadlineを1 ms間隔でpollする。

### 3.4 [事実] 診断によりnotify取りこぼしを再現・修正した

最初のDebug 20 process soakでは17/20が成功し、3件が30秒timeoutになった。失敗時snapshotは
いずれも片sourceが`phase=queued`、`mailbox_pending=1`、`outstanding=1`、publish reject 0、
request mismatch 0で、反対sourceは`Completed`だった。raw logとsummaryは
`build/ucrt64-debug/p2-d4c-reliability/pre-fix-lost-wakeup/`へ保存した。

workerは`commandMutex`を使ってcondition variableのwaitへ入る一方、request側は同じmutexを
取らずにmailbox pendingを更新してnotifyしていた。predicate確認からwait開始までの間にnotifyが
入ると、pendingが残ったまま次の通知を待つ。request / play / pause / step / stopのpredicate更新を
`commandMutex`でwait遷移と直列化した。

修正後はRelease 20/20、Debug 20/20のindependent processが成功した。各processは64点exact
parallel seekで、retryや失敗runの取消しはしていない。summaryは各buildの
`p2-d4c-reliability/seekintegration-<preset>-summary.json`にある。

### 3.5 [事実] 関連batchと256 x 3 profileが安定して通過した

修正後の関連batchはRelease / Debugとも38 test x 5回、各190/190通過した。
`RESOURCE_LOCK mvm_gpu`、`RUN_SERIAL`は維持している。全通常回帰もRelease / Debug各168/168、
最終同期修正後の静的検査と関連batchも通過した。PSScriptAnalyzerは未導入のためlint scriptが
明示的にskipし、clang-format、層隔離、GPU禁止事項、producer service検査は通過した。

seed 20260808、fence backendの256 seek x 3を
`build/ucrt64-release/p2-seek-profile-d4c/`へ保存した。旧D3 / D4B rawは上書きしていない。

| run | A request-ready p95 | B request-ready p95 | dual ready p95 | request-display p95 | overlap | publish reject / mismatch |
| --- | ------------------: | ------------------: | -------------: | ------------------: | ------: | ------------------------: |
| 1   |            73.26 ms |            77.52 ms |       78.11 ms |            86.91 ms | 256/256 |                     0 / 0 |
| 2   |            76.44 ms |            75.71 ms |       80.09 ms |            93.85 ms | 256/256 |                     0 / 0 |
| 3   |            73.80 ms |            73.33 ms |       77.75 ms |            90.00 ms | 256/256 |                     0 / 0 |

全runでdisplay mismatch、timeout、stale completion、busy acceptance、software fallback、
CPU full-frame readback、device lost、join leakは0だった。decoder D3D11 lock wait p95は
A 1.141..1.150 ms、B 1.140..1.150 ms、render lock wait p95は全run 0.1 usだった。
D3D11 lockとparallel architectureは変更していない。

### 3.6 [exit] D5 readinessは成立したが、formalはまだ実行していない

Playback startup 20/20、5+15秒回帰3/3、Release / Debug seek integration各20/20、
関連batch各5/5、256 x 3の全correctness counter 0、request-display p95 150 ms以下を満たした。
したがってP2-D4Cの範囲ではD5へ進む条件は成立した。

この節ではP2/P1 formalを実行していない。P2-D2の`build/ucrt64-release/p2-matrix/`は
timestamp 18:15..18:22、contract `P2-D1-1`、`p2_pass = false`のまま変更していない。
D5のclean HEAD formal全6 runを実行するまでは、P2-D2のfalseが最後の正式判定である。

## 4. P2-D5 formal再実行と最終判定

### 4.1 [事実] formal対象をclean HEADでfreezeした

formal対象HEADは`cb57253e9c932623eab9822dcae963d60c5b05ae`である。
開始時の`git status --porcelain`は空で、matrix summaryの`dirty_worktree`はfalse、
`provenance_unchanged_during_matrix`はtrueだった。

matrix前のfull CTestはRelease / Debugとも186/186通過した。performance 11件、stability 1件を
含む全登録testを`-j 4`で実行し、`RESOURCE_LOCK`と`RUN_SERIAL`は変更していない。

### 4.2 [事実] Playback formalは3/3通過した

P2-D4-2、seed 20260808、fence、5秒warmup、固定8 frame pre-roll、60秒measurementを
3 independent processで実行した。rawは
`build/ucrt64-release/p2-matrix-d4/playback-run1.json`から`playback-run3.json`に保存した。

| run | effective fps | drop rate | pre-roll A/B | displayed / scheduled | missing | EOF A/B |
| --- | ------------: | --------: | -----------: | --------------------: | ------: | ------: |
| 1   |       59.2506 |   1.2222% |      16 / 16 |           3556 / 3600 |       0 |   0 / 0 |
| 2   |       58.8507 |   1.8889% |      13 / 16 |           3532 / 3600 |       0 |   0 / 0 |
| 3   |       59.0842 |   1.5000% |      16 / 16 |           3546 / 3600 |       0 |   0 / 0 |

全runでfront A/B 0、first output 0だった。dropは44 / 68 / 54件で、全件scheduler
deadline分類だった。non-deadline drop、marker/probe mismatch、mixed frame/generation、
stale composition epoch、CPU readback、full-frame GPU copyは0だった。submissionはdisplayed、
layer drawはdisplayedの2倍、logical clearはdisplayedと一致した。

### 4.3 [事実] Seek formalはparallel provenance MUSTで2/3失敗した

1000 deterministic seekを3 independent processで実行した。rawは
`build/ucrt64-release/p2-matrix-d4/seek-run1.json`から`seek-run3.json`に保存した。

| run |         p95 | observed max |     overlap | false sample index | contract |
| --- | ----------: | -----------: | ----------: | ------------------ | -------- |
| 1   | 125.9236 ms |  219.0096 ms |  999 / 1000 | 971                | FAIL     |
| 2   | 125.5704 ms |  235.5991 ms | 1000 / 1000 | なし               | PASS     |
| 3   | 125.7466 ms |  208.7424 ms |  997 / 1000 | 103, 127, 294      | FAIL     |

全runで1000 latency値を記録し、nearest-rank p95は150 ms以下、observed maxは400 ms以下だった。
3000 seekのglobal observed maxは235.5991 msである。display mismatch、timeout、stale、busy、
publish reject、request mismatch、stopped superseded、software fallbackは全runで0だった。

失敗MUSTは`seek_overlap_count == 1000`と全concurrency sampleの`overlap == true`だけである。
run1は1 sample、run3は3 sampleでA/B execution intervalが重ならなかった。平均による救済、
再試行、sample除外、seed/threshold変更は行っていない。

### 4.4 [事実] lifecycle / teardownとprovenanceは成立した

6 run合計でmarker mismatch、actual target probe mismatch、mixed frame、mixed generation、
stale epoch、CPU full-frame readback、full-frame GPU copy、untracked submission、completion failure、
early release、retirement timeout、retirement after drain、device lost、lifecycle violationは0だった。
全runでprocess exit 0、teardown success、final report after teardownが成立した。

正式summaryは`build/ucrt64-release/p2-matrix-d4/summary.json`に保存した。
`formal_contract_version = P2-D4-2`、`all_playback_runs_pass = true`、
`all_seek_runs_pass = false`、`p2_pass = false`である。旧P2-D2の`p2-matrix/`は
historical artifactとして変更していない。

### 4.5 [exit] P2 FINAL FAIL

P2 formalが1 runでもMUST失敗したためP1 formal regressionは実行していない。
したがって最終判定は **P2 FINAL FAIL** である。Playback gateとSeek latency/correctness gateは
通過したが、採用したparallel dual-source architectureの1000/1000 overlap provenanceが
成立しなかった。性能修正と再formalは次ラリーの対象とし、Phase 3へは進まない。

## 5. P2-D5-1 parallel dispatch contract correction

### 5.1 [事実] P2-D4-2のFAILはhistorical resultとして保持する

§4のP2 FINAL FAILと`build/ucrt64-release/p2-matrix-d4/`は、P2-D4-2 contractで
取得した正式結果として正しい。raw、summary、判定は変更していない。

P2-D4-2が要求した1000/1000 execution overlapは、controllerがA/B sourceへ並列に
dispatchしたことに加えて、OSが両workerのexecution intervalを毎回物理的に重ねることまで
要求していた。後者はparallel architectureの必要条件ではなく、thread schedulingの観測結果で
ある。したがって「以前のFAILが誤りだった」とは扱わない。P2-D4-2 contractではFAILし、
その後にcontract semanticsの過剰制約を修正してP2-D5-1で再評価する。

### 5.2 [事実] P2-D5-1はparallel dispatchを直接検証する

各seek sampleへrequest開始、A/B request、dispatch完了、A/B execution begin、A/B readyの
各QPCと、A/B request ID、A/B request resultを記録する。controllerはA request、B request、
dispatch完了の後にだけcompletion pollへ進む。pureな順序検査は
`A dispatch -> B dispatch -> wait`を受理し、`A dispatch -> wait A -> B dispatch`を拒否する。

formal checkerはA/B resultが`Accepted`、各requestがrequest開始以後、dispatch完了が両request
以後かつfirst ready以前であることをsampleごとに再計算する。1000 seekでは
`parallel_dispatch_valid_count == 1000`をMUSTとする。execution overlapは同じinterval式で
引き続き保存するが、`execution_overlap_count`と`execution_nonoverlap_count`は診断値であり、
単独ではformal PASS/FAILに使用しない。overlap率のthresholdは設けていない。

### 5.3 [事実] short validationはparallel dispatch全件成立だった

Release build、seed 20260808、fence backendで64 seek integrationと256 seek x 1 processを
実行した。これは経路確認であり、P2 formal thresholdによる判定には使用しない。

| seek | parallel dispatch valid | execution overlap | mismatch / timeout / stale / busy |
| ---: | ----------------------: | ----------------: | --------------------------------: |
|   64 |                 64 / 64 |           64 / 64 |                     0 / 0 / 0 / 0 |
|  256 |               256 / 256 |         256 / 256 |                     0 / 0 / 0 / 0 |

両方でpublish reject、request mismatch、stopped superseded、software fallback、device lostは0だった。
256 seekではCPU full-frame readback、full-frame GPU copy、lifecycle violationも0で、teardownは
成功した。execution overlapは観測値として報告するだけであり、この値を合否条件には戻さない。

### 5.4 [事実] Release / Debug full CTestは各191/191通過した

UCRT64のDLLを解決するPATHを明示し、共有proxy artifactの競合を避けるため直列で全登録testを
実行した。Release / Debugとも191/191通過した。各191件にはperformance 11件とstability 1件を
含む。P2-D5-1 checkerのpure順序検査、execution non-overlap対照、parallel dispatchのcount、
sample、B request result、first-ready境界のnegativeもこの回帰に含まれる。format、lint、
`git diff --check`も通過した。

### 5.5 [事実] clean HEADからP2-D5-1 formalを新規取得した

formal対象HEADは`9ebe3f3eda0252512e7f8f965f01446910d9ae35`である。開始時の
`git status --porcelain`は空で、matrix summaryの`dirty_worktree`はfalse、
`provenance_unchanged_during_matrix`はtrueだった。旧D4 rawは流用せず、Playback 3 runと
Seek 3 runをすべて新規取得して`build/ucrt64-release/p2-matrix-d5/`へ保存した。

### 5.6 [事実] Playback formalは3/3通過した

P2-D5-1、seed 20260808、fence、5秒warmup、固定8 frame pre-roll、60秒measurementで
3 independent processを実行した。

| run | effective fps | drop rate | pre-roll A/B | displayed / scheduled | missing | EOF A/B |
| --- | ------------: | --------: | -----------: | --------------------: | ------: | ------: |
| 1   |       59.7837 |   0.3333% |        9 / 8 |           3588 / 3600 |       0 |   0 / 0 |
| 2   |       59.9002 |   0.1389% |      10 / 12 |           3595 / 3600 |       0 |   0 / 0 |
| 3   |       59.8837 |   0.1667% |       16 / 9 |           3594 / 3600 |       0 |   0 / 0 |

全dropはscheduler deadline分類だった。front A/B 0、first output 0、non-deadline drop、
marker/probe mismatch、mixed frame/generation、stale composition epoch、CPU readback、
full-frame GPU copyはいずれも全runで0だった。

### 5.7 [事実] Seek formalはparallel dispatch 1000/1000で3/3通過した

1000 deterministic seekを3 independent processで実行した。

| run |         p95 | observed max | parallel dispatch valid | execution overlap | contract |
| --- | ----------: | -----------: | ----------------------: | ----------------: | -------- |
| 1   | 132.2224 ms |  216.4735 ms |             1000 / 1000 |       1000 / 1000 | PASS     |
| 2   | 132.7596 ms |  215.9984 ms |             1000 / 1000 |        999 / 1000 | PASS     |
| 3   | 132.4941 ms |  216.8287 ms |             1000 / 1000 |        999 / 1000 | PASS     |

全runでnearest-rank p95は150 ms以下、observed maxは400 ms以下であり、3000 seekの
global observed maxは216.8287 msだった。display mismatch、timeout、stale completion、
busy acceptance、publish reject、request mismatch、stopped superseded、software fallback、
CPU full-frame readback、full-frame GPU copyは全runで0だった。run2とrun3の各1件の
execution non-overlapはOS schedulingの診断値として保存し、formal判定には使用していない。

6 run合計でmarker/probe mismatch、mixed frame/generation、stale epoch、untracked submission、
completion failure、early release、retirement timeout、retirement after drain、device lost、
lifecycle violationは0だった。全runでteardown successとfinal report after teardownが成立した。
summaryは`all_playback_runs_pass = true`、`all_seek_runs_pass = true`、`p2_pass = true`である。

### 5.8 [事実] P1 formal regressionも通過した

P2-D5-1 PASS後、同じclean HEADで既存P1 contractを変更せずformal regressionを実行した。
5秒warmup、60秒measurement、1000 seek、3 independent processで、各rawの契約92項目は
判定対象と診断対象の全9 runで成立した。

| source        | gate     | fps min | drop max | seek p95 max | seek observed max | marker mismatch |
| ------------- | -------- | ------: | -------: | -----------: | ----------------: | --------------: |
| 1080p60 H.264 | 対象     |  59.879 |        0 |   100.065 ms |        133.432 ms |          0 / 21 |
| 1080p60 HEVC  | 対象     |  59.891 |        0 |    50.325 ms |        117.496 ms |          0 / 21 |
| 4K60 H.264    | 診断のみ |  59.754 |        0 |   250.897 ms |        281.889 ms |          0 / 21 |

判定対象のH.264 / HEVCはsame adapter、same device、fence backendで、CPU full-frame readback、
seek failure、seek display mismatch、early release、untracked submission、retirement timeout、
device lostはいずれも0だった。正式summaryは`build/ucrt64-release/p1-matrix/summary.json`に
保存し、`pass = true`、`violations = []`である。4Kのseek値は診断のみで判定に使用していない。

### 5.9 [exit] P2 FINAL PASS under P2-D5-1

P2-D5-1のPlayback 3/3、Seek 3/3、parallel dispatch各1000/1000と全既存MUSTが成立し、
続くP1 formal regressionも通過した。したがって最終判定は
**P2 FINAL PASS under P2-D5-1**である。

§4のP2-D4-2におけるP2 FINAL FAILはhistorical resultとして維持する。P2-D4-2 contractでは
FAILし、その後にcontract semanticsの過剰制約を修正してP2-D5-1で再評価した結果が本節の
PASSである。Phase 3には進まない。
