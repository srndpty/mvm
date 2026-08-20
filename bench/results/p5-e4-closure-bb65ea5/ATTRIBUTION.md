# P3-C-2 regression attribution

## Formal FAIL identity

| cohort | mode/run | process / contract exit | producerを不合格にしたpredicate | その他の主要identity |
| --- | --- | --- | --- | --- |
| `bb65ea5` attempt 1 | seek / 1 | 4 / 1 | `measurement_audio_underflow_count == 1` | requested/exact 1000/1000、timeout/busy/stale/generation mismatch 0 |
| `bb65ea5` attempt 2 | pause-resume / 3 | 4 / 1 | `measurement_clock_regression_count == 1` | pause frozen/video advance/generation stableは全てtrue、AV abs p95/maxは15.208/16.583 ms |

いずれもproducerの`pass`がfalseとなり、metrics書き出し後にprocess exit 4となった。checkerはまず
non-zero process exitを拒否し、P3-C-2 checkerも継承したP3-C-1不合格として拒否した。

未変更親`06182a2`のattempt 1/2では、seek run 1とpause-resume run 3の対応fieldはいずれも0であり、
全matrixが9/9 PASSした。変更後2回のFAIL predicateは互いに異なるため、N-way seekの同一predicateが
連続して壊れたとは判定しない。一方、cohortとの相関を否定できないためclosure blockerは維持する。

## Source-path comparison

`06182a2..bb65ea5`では、次のP3実行経路とformal判定経路にsource差分がない。

- `apps/p3_av_sync_spike/`
- `src/media/gpu_preview/`
- `src/media/audio_preview/`
- `src/app/preview/`
- `scripts/p3-c2-matrix.ps1`
- `scripts/check-p3-c-contract.ps1`
- `scripts/check-p3-c2-contract.ps1`

E3のproduction差分は`src/preview_engine/`にあり、P3 spikeは`PreviewEngine`を生成しない。ただしP3 targetは
`mvm::preview_qt`へlinkしており、relinkやbinary layoutを介したtiming差まで否定する証拠にはならない。

## Mode限定診断 (formal authorityではない)

current E4 worktreeのbuildで、正式threshold/checkerを変更せず、原因切り分けだけを目的として次を反復した。

- seek 200回 × 5 process: 5/5 PASS、計1000/1000 exact、underflow 0、clock regression 0
- pause-resume × 10 process: 10/10 PASS、underflow 0、clock regression 0、pause三不変条件は全run true

短縮・mode限定runなのでformal PASSへ使用しない。常時再現ではなくtiming-sensitiveであることだけを示す。
次の診断では、audio underflow発生時のrequested/consumed sampleとqueue depth、およびclock regressionを
増分した3分岐 (scheduler projection invalid / scheduler decision / display projection invalid) を区別して記録する。
