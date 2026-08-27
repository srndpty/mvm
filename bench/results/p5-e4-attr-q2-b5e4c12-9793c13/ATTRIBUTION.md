# ATTR-Q2 attribution

## First-failure snapshot comparison

headではaudio underflowとclock regressionはいずれも39 processで0件だったため、
first-failure snapshotはすべて`null`だった。

parentでは3回のseek underflowが次の同一境界で再現した。

| field                                   | attempt 1 seek run 2 | attempt 2 seek run 1 | attempt 3 seek run 3 |
| --------------------------------------- | -------------------: | -------------------: | -------------------: |
| seek ordinal                            |                  523 |                  523 |                  523 |
| target frame                            |                 3892 |                 3892 |                 3892 |
| source generation                       |                  524 |                  524 |                  524 |
| engine state                            |    3 (`WaitDisplay`) |                    3 |                    3 |
| decode ready / seek pending / presented |  true / true / false |                 同左 |                 同左 |
| requested sample start / count          |        3119840 / 480 |                 同左 |                 同左 |
| queue before / consumed / after         |        288 / 288 / 0 |                 同左 |                 同左 |
| shortage                                |    192 samples (4ms) |                 同左 |                 同左 |
| audio master sample                     |              3114063 |              3114082 |              3114064 |

underflowはrequest前、flush/preroll中、通常再生中ではなく、exact decode completion後かつ
requested-frame presentation前の`WaitDisplay`で起きた。target sample 3113600に対して、endpointが
要求したstartは6240 samples先であり、その時点のqueue残量は288 samplesだけだった。

clock regressionはhead/parentとも0件であり、historical pause-resume FAILの3-way siteは今回確定できなかった。

## Other diagnostic failures

- head `PAUSE-PREFIX` attempt 3 seek run 3:
  seek 604でAV deltaが-59.146msとなり、abs max 33.334msを超えた。1000/1000 exact、underflow 0、
  clock regression 0、projection failure 0で、ATTR-Q1 snapshot対象外だった
- parent `SEEK-PREFIX` attempt 3 playback run 3: GPU teardown timeout
- parent `PAUSE-PREFIX` attempt 2 pause-resume run 2:
  clock frozen=true、generation stable=trueだがpause video advance zero=false

## Attribution verdict

今回のpaired evidenceは、historical `bb65ea5`のunderflow/clock regressionを再現しなかった。
反対にunderflowはparentだけで3回、同じseek境界に再現した。したがって、E3変更をproduction regressionの
原因として選択的にrevertする根拠は得られていない。headをparentへ近づけるrevertは、今回のunderflow
観測方向とは逆である。

`mvm_p3_av_sync_spike`は`mvm::preview_qt`経由で`mvm::preview_engine`をtransitive linkする一方、
P3 controller/compositor/audioの実行経路はPreviewEngine APIを呼ばない。`06182a2..bb65ea5`で
failing executableへ入るproduction差分は主に`src/preview_engine` translation unitである。

## Next selective-revert candidate

**production selective revert候補は現時点ではなし**とする。

次に行うならfix目的のrevertではなく、link/timing attribution用ablationとして、head由来buildで
`src/preview_engine/preview_engine.cpp`、`preview_engine_internal.h`、`preview_types.h`のE3差分だけを
parent内容へ戻す群が最小候補である。ただしparent buildが既にunderflow側へ悪化しているため、
このablationがparent signatureを再現してもfix候補にはしない。

先に追加すべき診断は、seek ordinal 523付近のqueue supply marginと、head seek AV外れ値の
first-threshold snapshot、pause video advanceのfirst display identityである。threshold、buffer/preroll、
counter semantics、formal checkerは変更しない。
