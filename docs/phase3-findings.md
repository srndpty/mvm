# Phase 3 の所見

記述は Phase 0 と同じ規則で分類する。混ぜない。

| 印         | 意味                                               |
| ---------- | -------------------------------------------------- |
| `[事実]`   | 実際に実行して観測した。再現手順を併記する         |
| `[推測]`   | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない       |
| `[回避策]` | 現在の対処。恒久策とは限らない                     |
| `[exit]`   | exit criteria への影響                             |

---

## 1. P3-C-1 formal実行とP3 closure判定

### 1.1 [事実] clean HEADと通常回帰をformal前に固定した

formal対象HEADは`5ed555a9939705768f1372385456113a0ba1439c`である。開始時とformal開始直前の
`git status --porcelain`は空だった。formal前のordinary CTestはperformance / stabilityを除外し、
Release / Debugとも215/215通過した。

P3-C-1 summaryではstart/endのgit commit、dirty状態、fixture A/B SHA-256、executable SHA-256、
contract versionが一致した。GPU adapterは両時点ともNVIDIA GeForce RTX 4090、audio endpointは
48,000 Hz、2 channel、`flt`だった。`provenance_unchanged = true`、
`hardware_provenance_unchanged = true`である。

### 1.2 [事実] P3-C Playback formalは3/3通過した

`pwsh -NoProfile -File scripts/p3-matrix.ps1`を`DryRun`、`StopOnFailure`なしで一度だけ実行した。
Playbackは5秒warmup後、60秒、3,600 frameを3 independent processで測定した。

| run | displayed / skipped / required | effective fps | drop rate | AV abs p95 | AV abs max | underflow / overflow |
| --- | -----------------------------: | ------------: | --------: | ---------: | ---------: | -------------------: |
| 1   |               3590 / 10 / 3600 |       59.8333 |   0.2778% |  16.083 ms |  17.458 ms |                0 / 0 |
| 2   |               3583 / 17 / 3600 |       59.7167 |   0.4722% |  16.000 ms |  17.500 ms |                0 / 0 |
| 3   |               3582 / 18 / 3600 |       59.7000 |   0.5000% |  16.083 ms |  17.354 ms |                0 / 0 |

各runでfirst frame 0、`displayed_unique + skipped == 3600`、AV raw countとdisplayed uniqueの
一致が成立した。duplicate display、non-increasing display、AV projection failure、marker mismatch、
mixed pair/generation、stale composition epoch、video ahead violation、clock regression、QPC fallback、
audio clock query failureは0だった。

### 1.3 [事実] P3-C Seek formalは3/3通過した

seed 20260808の1000 deterministic integrated seekを3 independent processで実行した。

| run |       exact | request-display p95 | observed max | first-display AV abs p95 | AV abs max | timeout / busy / stale / generation mismatch |
| --- | ----------: | ------------------: | -----------: | -----------------------: | ---------: | -------------------------------------------: |
| 1   | 1000 / 1000 |         133.7115 ms |  183.3806 ms |                9.0625 ms | 10.2917 ms |                                0 / 0 / 0 / 0 |
| 2   | 1000 / 1000 |         134.5040 ms |  183.1812 ms |                8.9375 ms | 10.2292 ms |                                0 / 0 / 0 / 0 |
| 3   | 1000 / 1000 |         149.7502 ms |  200.0543 ms |                9.0833 ms | 10.2083 ms |                                0 / 0 / 0 / 0 |

各runでrequested audio sample、first audio sample、first displayed video frame、first-display AV projectionの
exact contractが成立した。run 3のp95は150.000 ms閾値近傍だが、丸め、平均、再試行による救済は
行っていない。

### 1.4 [事実] P3-C PauseResume formalは3/3通過した

| run | clock frozen | video advance zero | generation stable | AV abs p95 | AV abs max |
| --- | ------------ | ------------------ | ----------------- | ---------: | ---------: |
| 1   | true         | true               | true              |  14.313 ms |  15.208 ms |
| 2   | true         | true               | true              |  16.646 ms |  17.021 ms |
| 3   | true         | true               | true              |  10.458 ms |  11.667 ms |

全runでunderflow、clock regression、QPC fallbackは0だった。

### 1.5 [事実] P3-C global correctnessと正式summaryはPASSだった

9 run合計でCPU full-frame readback、full-frame GPU copy、software video fallback、device lost、
lifecycle violation、audio decode/render thread join leakは0だった。全runでvideo worker join、
teardown success、final report after teardownが成立した。

raw producerの`formal_verdict`は全9件とも設計どおり`NOT_RUN`のままである。正式summaryは
`build/ucrt64-release/p3-matrix/summary.json`に保存し、schemaは
`mvm-p3-matrix-summary-1`、`expected_processes = 9`、`completed_processes = 9`、
`formal_verdict = PASS`、`all_runs_pass = true`、`p3_c_pass = true`である。

### 1.6 [事実] P3-A standalone regressionは通過した

P3-C PASS後に既存`pwsh -NoProfile -File scripts/p3-a-smoke.ps1`を変更せず実行した。
playback 15秒 x 3、exact audio seek 64/64、pause/resume、audio marker 6/6がすべて通過し、
`build/p3-a-smoke/summary.json`の`verdict`は`PASS`、`errors`は空だった。

### 1.7 [事実] P2-D5-1 formal regressionはoutput size MUSTで6/6失敗した

P3-A PASS後に既存`pwsh -NoProfile -File scripts/p2-matrix.ps1`を変更せず一度だけ実行した。
Playback 3 runとSeek 3 runの全process自体はexit 0だったが、全6 rawで
`actual_output_width = 1204`、`actual_output_height = 1080`だった。P2-D5-1 checkerが要求する
width 1920と一致しないため、全runのcontract exitは3、per-run `pass`はfalseになった。

Playbackのeffective fpsは59.6844 / 59.7505 / 59.7830、drop rateは0.5000% / 0.3889% /
0.3333%だった。Seekのp95は83.3333 / 83.2379 / 83.2814 ms、observed maxは
150.6075 / 150.1728 / 150.3128 ms、parallel dispatch validは各1000/1000だったが、これらの
診断値でoutput size MUSTを救済していない。

P2 summaryは`build/ucrt64-release/p2-matrix-d5/summary.json`に保存した。
`provenance_unchanged_during_matrix = true`、`all_playback_runs_pass = false`、
`all_seek_runs_pass = false`、`p2_pass = false`である。原因の推測、performance fix、checker変更、
threshold変更、同じHEAD/seedでの再試行は行っていない。

### 1.8 [exit] P3 FINAL FAIL under P3-C-1

P3-C-1 new path formalとP3-A regressionはPASSしたが、後続のP2-D5-1 formal regressionが
1 run以上でMUST失敗したため、指示どおりP1 formal regressionは実行していない。したがって
Phase 3 closureの最終判定は **P3 FINAL FAIL under P3-C-1** である。

P3-C-1 raw / summaryとP2-D5-1 regression raw / summaryは保存し、Phase 4へは進まない。

## 2. P3-C-2 display-target contract hardening

### 2.1 [事実] P3-C-1のhistorical判定は変更しない

§1のP3-C-1 formal PASS、P3-A regression PASS、P2-D5-1 regression FAIL、P1 NOT_RUN、
**P3 FINAL FAIL under P3-C-1**はhistorical resultとして維持する。P3-C-1のraw schema、checker、
matrix summaryの意味をP3-C-2へ合わせて変更していない。

P2-D5-1 regression時、1920x1200 monitorはWindows portrait orientationだった。全6 rawのactual
RHI targetは1204x1080だったが、screen geometry、orientation、availableGeometry、DPR、Windowと
Surfaceのlogical sizeは保存していなかった。その後landscapeへ戻した同一環境のshort diagnosticでは
P2 actual RHI targetが1920x1080になった。orientationに伴うWindows/Qt window sizing issueを強く
支持するが、historical screen telemetryが無いため完全な因果証明とは扱わない。

### 2.2 [事実] P3-C-2を独立contractとして追加した

P3-C-2はraw schema `mvm-p3-formal-2`、matrix summary schema
`mvm-p3-matrix-summary-2`、新規output directory
`build/ucrt64-release/p3-matrix-c2/`を使う。P3-C-1の性能、A/V、seek、pause/resume、correctness、
teardown条件は変更せず、新checkerからP3-C-1 checkerを呼んで再検査する。

各rawはrequested size、screen name/orientation、screen/available geometry、DPR、QQuickWindowと
CompositorSurfaceのlogical size、actual RHI target pixel sizeをstart/endで保存する。native window
outer/client sizeも診断値として保存する。orientation名とavailableGeometryの大小は単独のMUSTに
せず、Window / Surface / RHIが1920x1080、DPRが1.0であることをMUSTにした。

### 2.3 [事実] preflightをpipeline open前にfail-closedで実行する

Qt/D3D11 device ready後、render callbackがactual color textureの`pixelSize()`をthread-safeなpacked
snapshotへ公開する。GUI threadのcontrollerがWindow、Surface、QScreen、native windowとRHI
snapshotを取得し、decoder/audio pipelineをopenする前にpure preflightを実行する。

target未初期化中は最大10秒待つ。不一致ならresizeで補正せず、formal workloadを開始しないままrawを
保存してnonzero終了する。最初のDryRunではactual size公開がframe pair合成後だったためpreflightが
ready timeoutになり、3 processとも`formal_workload_started = false`で停止した。公開位置をframeの
有無に依存しないrender callback先頭へ移し、pipeline前preflightとの循環を解消した。

### 2.4 [事実] checkerとpreflightのpositive/negativeは21/21通過した

production preflight helperのpure testは1920x1080 / DPR 1.0を受理し、1204x1080を拒否して
workload開始を許可しない。C2 checkerはGoodC2 1件とnegative 19件を登録した。missing telemetry、
requested/window/surface/RHI/DPR、start/end orientation/geometry/DPR/window/surface/RHI、run間
display mismatch、null、NaN、型違反をそれぞれfail-closedで拒否した。

Release限定の先行実行と、後続のRelease / Debug ordinary CTest内の両方でC2関連21/21が通過した。

### 2.5 [事実] landscape環境のP3-C-2 DryRunは3/3通過した

最終Release executableで`pwsh -NoProfile -File scripts/p3-c2-matrix.ps1 -DryRun`を実行した。
Playback 5秒、Seek 64件、PauseResumeの各1 independent processがC2 checkerと継承したC1 checkerを
通過した。全rawのstart/endでWindow、Surface、actual RHI targetは1920x1080、DPRは1.0だった。

summaryは3/3 complete、`all_runs_pass = true`、source/hardware/display environment provenanceは
すべてunchangedだった。rawとsummaryの`formal_verdict`は`NOT_RUN`、`p3_c_pass = false`である。
正式9 runのPASSを表す結果ではない。

### 2.6 [事実] P2 sanityと既存回帰は通過した

P2 formalは再実行せず、short Playbackを1 processだけ実行した。process exit 0、actual RHI targetは
1920x1080、adapterはNVIDIA GeForce RTX 4090、completion backendはfenceだった。このsanityで
historical P2-D5-1 regression FAILを置き換えない。

P3-A standalone smokeはplayback 15秒 x 3、exact seek 64/64、pause/resume、marker 6/6を通過した。
P3-B playback sanityは1/1通過した。Release / Debug ordinary CTestは各236/236通過し、format、lint、
`git diff --check`も通過した。

### 2.7 [未検証] P3-C-2 formalはまだ実行していない

P3-C-2のcontract/harness hardening、negative、DryRun、P2 sanity、既存回帰は完了し、clean HEADを
用意した後にformalへ進める状態である。正式commandは
`pwsh -NoProfile -File scripts/p3-c2-matrix.ps1`だが、本ラリーでは実行していない。

P2/P1 formal、Phase 4、windowの強制resize、commit、pushも実行していない。

### 2.8 [事実] formal前にBoolean fieldのfail-closed検査を追加した

P3-C-2 checkerの`display_target_preflight_pass`と
`formal_workload_started`について、PowerShellのtruthinessに依存せず、
JSON boolean型かつexact `true`であることを要求するようhardeningした。

`false`値および文字列`"false"`を拒否するnegative 4件を追加し、
C2 checkerはGood 1件 + negative 23件の24/24、
display-target pure unitを含めて25/25通過した。

既存P3-C-2 DryRun raw 3件は新checkerでも3/3通過した。
Release / Debug ordinary CTestは各240/240通過した。
format、lint、`git diff --check`も通過した。

P3-C-2 formalはまだNOT_RUNである。

## 3. P3-C-2 formal実行とP3 closure判定

### 3.1 [事実] clean HEADと通常回帰をformal前に固定した

formal対象HEADは`bc57eb7654c12d05514d3524d644feb16513b595`である。開始時の
`git status --porcelain`は空だった。formal前のordinary CTestはperformance / stabilityを除外し、
Release / Debugとも実測240/240、100%通過した。

P3-C-2 summaryではstart/endとも同じHEADで`dirty_worktree = false`、git statusは空だった。
fixture A/B SHA-256、executable SHA-256、contract versionも一致し、
`provenance_unchanged = true`だった。GPU adapterはNVIDIA GeForce RTX 4090、audio endpointは
48,000 Hz、2 channel、`flt`で、`hardware_provenance_unchanged = true`だった。

### 3.2 [事実] display-target MUSTは全9 processのstart/endで成立した

`pwsh -NoProfile -File scripts/p3-c2-matrix.ps1`を`DryRun`、`StopOnFailure`なしで一度だけ実行した。
Playback、Seek、PauseResumeを各3 independent process、合計9 process取得した。同じHEAD / seedの
救済rerun、threshold変更、fixture変更、測定中のbuildは行っていない。

全9 rawのstart/end、合計18 telemetryでdisplay signatureは次の1種類だけだった。

| 項目 | 実測値 |
| --- | --- |
| requested output | 1920x1080 |
| screen | DELL U2412M、landscape |
| screen geometry | 1920x1200 |
| available geometry | 1920x1152 |
| Window logical | 1920x1080 |
| CompositorSurface logical | 1920x1080 |
| actual RHI target | 1920x1080 |
| DPR | 1.0 |
| native client / outer | 1920x1080 / 1936x1119 |

全rawで`display_target_preflight_pass`と`formal_workload_started`はJSON boolean `true`だった。
process内のstart/endにも9 process間にも差はなく、summaryの
`display_environment_provenance_unchanged = true`が成立した。

### 3.3 [事実] P3-C-2 Playback formalは3/3通過した

5秒warmup後、60秒、3,600 frameを各runで測定した。

| run | displayed / skipped / required | effective fps | drop rate | AV abs p95 | AV abs max |
| --- | -----------------------------: | ------------: | --------: | ---------: | ---------: |
| 1 | 3586 / 14 / 3600 | 59.7667 | 0.3889% | 16.125 ms | 18.833 ms |
| 2 | 3584 / 16 / 3600 | 59.7333 | 0.4444% | 15.958 ms | 20.875 ms |
| 3 | 3558 / 42 / 3600 | 59.3000 | 1.1667% | 15.875 ms | 17.750 ms |

全runで`displayed + skipped = 3600`、fps 55以上、drop 2%以下、AV abs p95 20.000 ms以下、
AV abs max 33.334 ms以下が成立した。

### 3.4 [事実] P3-C-2 Seek formalは3/3通過した

seed 20260808の1000 deterministic integrated seekを各runで実行した。

| run | exact | request-display p95 | observed max | first-display AV abs p95 | AV abs max |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 1000 / 1000 | 133.8814 ms | 183.8592 ms | 8.833 ms | 15.396 ms |
| 2 | 1000 / 1000 | 133.9670 ms | 183.6489 ms | 8.958 ms | 10.208 ms |
| 3 | 1000 / 1000 | 149.9264 ms | 200.2404 ms | 9.000 ms | 10.333 ms |

全runでrequest-display p95 150.000 ms以下、observed max 400.000 ms以下、first-display AV abs
p95 20.000 ms以下、AV abs max 33.334 ms以下だった。timeout、busy acceptance、stale completion、
generation mismatchはすべて0だった。run 3のp95は閾値近傍だが、丸めや再試行で救済していない。

### 3.5 [事実] P3-C-2 PauseResume formalは3/3通過した

| run | clock frozen | video advance zero | generation stable | AV abs p95 | AV abs max |
| --- | --- | --- | --- | ---: | ---: |
| 1 | true | true | true | 8.417 ms | 13.917 ms |
| 2 | true | true | true | 15.167 ms | 16.771 ms |
| 3 | true | true | true | 14.625 ms | 15.417 ms |

全runでclock freeze、video advance zero、generation stableとAV gateが成立した。

### 3.6 [事実] global correctnessとP3-C-2 summaryはPASSだった

9 run合計でaudio underflow / overflow、marker mismatch、mixed pair / generation、stale composition
epoch、QPC fallback、clock regression、CPU full-frame readback、full-frame GPU copy、software video
fallback、device lost、lifecycle violation、audio decode / render thread join leakはすべて0だった。
全runでvideo worker join、teardown success、final report after teardownが成立した。

raw producerの`formal_verdict`は全9件とも設計どおり`NOT_RUN`だった。正式summaryは
`build/ucrt64-release/p3-matrix-c2/summary.json`に保存した。schemaは
`mvm-p3-matrix-summary-2`、contract versionは`P3-C-2`、expected / completed processは9 / 9、
`all_runs_pass = true`、source / hardware / display provenanceはすべてunchanged、
`formal_verdict = PASS`、`p3_c_pass = true`である。

### 3.7 [事実] P3-A standalone regressionは通過した

P3-C-2 PASS後に既存`pwsh -NoProfile -File scripts/p3-a-smoke.ps1`を変更せず実行した。
playback 15秒 x 3、exact seek 64/64、pause/resume、audio marker 6/6がすべて通過した。
`build/p3-a-smoke/summary.json`は`verdict = PASS`、`errors = []`だった。

### 3.8 [事実] P2-D5-1 formal regressionは6/6通過した

P3-A PASS後に既存`pwsh -NoProfile -File scripts/p2-matrix.ps1`を変更せず一度だけ実行した。
全6 rawのactual outputは1920x1080で、historical portrait runの1204x1080結果は変更していない。

Playbackのeffective fpsは59.6337 / 59.6170 / 59.4171、drop rateは0.5833% / 0.6111% /
0.9444%だった。Seekのp95は83.0909 / 83.6808 / 89.6029 ms、observed maxは149.6082 /
151.0818 / 182.5047 ms、parallel dispatch validは各1000/1000だった。

summaryはPlayback 3/3、Seek 3/3、source provenance unchanged、全global correctness counter 0、
`p2_pass = true`だった。

### 3.9 [事実] P1 formal regressionは通過した

P2-D5-1 PASS後に既存`pwsh -NoProfile -File scripts/p1-matrix.ps1`を変更せず実行した。
5秒warmup、60秒measurement、1000 seek、3 independent processで、各rawの契約92項目は
判定対象と診断対象の全9 runで成立した。

| source | gate | fps min | drop max | seek p95 max | seek observed max | marker mismatch |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p60 H.264 | 対象 | 59.937 | 0 | 67.644 ms | 100.581 ms | 0 / 21 |
| 1080p60 HEVC | 対象 | 59.815 | 0 | 50.443 ms | 132.993 ms | 0 / 21 |
| 4K60 H.264 | 診断のみ | 59.799 | 0 | 250.098 ms | 274.210 ms | 0 / 21 |

判定対象の既存MUSTはすべて成立し、summaryは`violations = []`、`pass = true`だった。
4K60 H.264は既存どおりdiagnosticのみで、closure判定には使用していない。

### 3.10 [exit] P3 FINAL PASS under P3-C-2

P3-C-2 formal、P3-A regression、P2-D5-1 formal regression、P1 formal regressionはすべて
PASSした。したがってPhase 3 closureの最終判定は
**P3 FINAL PASS under P3-C-2**である。

§1の**P3 FINAL FAIL under P3-C-1**はhistorical resultとして維持する。Phase 4、commit、pushは
実行しない。
