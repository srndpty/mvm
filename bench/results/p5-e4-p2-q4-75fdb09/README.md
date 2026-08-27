# P5-E4 / P2-Q4 — Render Opportunity Contract Proof

- authority: `DIAGNOSTIC_ONLY_NOT_CLOSURE_EVIDENCE`
- tested SHA: `75fdb09`
- input: immutable P2-Q3 5 trace
- production scheduler / threshold / checker: 変更なし
- synthetic contract: 8/8 PASS
- current replay authority: 5/5 exact
- candidate invariants: 5/5 PASS

| run | current displayed | current drop | current repeated | candidate displayed | candidate drop | candidate repeated |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3505 | 95 | 91 | 3502 | 98 | 94 |
| 2 | 3496 | 104 | 95 | 3491 | 109 | 100 |
| 3 | 3495 | 105 | 103 | 3509 | 91 | 89 |
| 4 | 3499 | 101 | 97 | 3530 | 70 | 66 |
| 5 | 3551 | 49 | 41 | 3556 | 44 | 36 |
| 合計 | 17546 | 454 | 427 | 17588 | 412 | 385 |

current replayはcallback単位のdue、skip数、output frameを含めてQ3 rawと完全一致した。
nearest-slot candidateは全runでscheduled=3600、frame 0開始、strictly increasing、frame 3600を
出さないことを満たした。

一方candidate dropは454から412への減少に留まり、run 1と2では増加した。一般synthetic negativeを
通過してもQ3 traceの境界aliasingを一貫して除去できないため、このstatic nearest-slot modelを
P2-D5-2へ採用しない。2%以下になるかはexit criteriaにも判定にも使用していない。

次は固定deadlineへの別のgrace調整ではなく、実際のcallback opportunity列から本当の欠落を定義する
追加proofが必要である。P2-D5-1 historical FAILとP5-E closure BLOCKEDは維持する。
