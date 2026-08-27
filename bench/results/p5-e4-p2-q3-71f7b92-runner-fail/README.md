# P2-Q3 runner failure evidence

- authority: `DIAGNOSTIC_ONLY_NOT_CLOSURE_EVIDENCE`
- tested SHA: `71f7b9241e09e8f2f31ba50e1e5ec30c8ee755d4`
- workload: playback、warmup 5秒、measurement 60秒、fence、formal checker
- process: exit 0
- checker: PASS
- protocol verdict: INVALID（run後のsummary集計でPowerShellのproperty解決に失敗）

workloadとraw/checker生成は完了したが、campaign runnerがsummary生成時に停止したため、
このrootはQ3 campaign結果へ混ぜない。rawでは50 deadline dropの内訳が
`PHASE_PAIR=21 / LONG_CALLBACK_GAP=10 / UNPAIRED_SKIP=19`、ringは
`3586/3586` records、overflow 0だった。この値はleadとしてのみ保持する。
