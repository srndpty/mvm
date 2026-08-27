# P5-E4 / P2-Q3 — Scheduler Phase Aliasing Attribution

- authority: `DIAGNOSTIC_ONLY_NOT_CLOSURE_EVIDENCE`
- tested SHA: `c800536caa12c90e1718c3db0aa0ab0368263486`
- executable SHA-256: `327fb722ddb9bbbccace8190496eeb9af872c04807cb55df25a6f21c281a22f4`
- stop rule: 最大8 run、PASSとFAILの両方を採取した時点で停止
- result: 5 runで停止、FAIL 4 / PASS 1

| run | verdict | deadline drop | drop rate | callback | repeated | phase | long gap | unpaired |
| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | FAIL | 95 | 2.6389% | 3596 | 91 | 33 | 2 | 60 |
| 2 | FAIL | 104 | 2.8889% | 3591 | 95 | 27 | 4 | 73 |
| 3 | FAIL | 105 | 2.9167% | 3598 | 103 | 37 | 3 | 65 |
| 4 | FAIL | 101 | 2.8056% | 3596 | 97 | 41 | 0 | 60 |
| 5 | PASS | 49 | 1.3611% | 3592 | 41 | 17 | 6 | 26 |

全454 deadline dropの内訳は`PHASE_PAIR=155 (34.1%)`、`LONG_CALLBACK_GAP=15 (3.3%)`、
`UNPAIRED_SKIP=284 (62.6%)`である。全runでrecord数とcallback数が一致し、ring overflowと
unobserved boundaryは0だった。

UNPAIRED_SKIP 284件はすべて直前callbackがdue=trueかつdeadline通過後だった。intervalは
16.686～33.259 ms（median 19.343 ms）、直前latenessはmedian 15.429 ms、current callbackが
skip境界を越えた量はmedian 0.806 msである。長いcallback消失より、late callbackに続く
`due → skip`の境界crossが支配的だった。

`summary.json`の`dominant_classification=MIXED_OR_UNPAIRED`は採取時runnerの粗い表示名であり、
exact count上のdominant classは`UNPAIRED_SKIP`である。rawとsummaryは採取後に変更していない。

最初の試行では有効な1 run後にrunner集計が失敗したため、campaignへ混ぜず
`../p5-e4-p2-q3-71f7b92-runner-fail/`へINVALID evidenceとして分離保存した。
