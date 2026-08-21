# P5-E4 / P2-Q5 — Presentation Opportunity Authority Proof

- authority: `DIAGNOSTIC_ONLY_NOT_CLOSURE_EVIDENCE`
- tested SHA: `2be57c6fdd94a402d9251abbd55feccc14b6c33b`
- executable SHA-256: `45c9063868581a7bb7a1b1908913d68d0565e4034c60aebd9d3b0aae5dfe4262`
- display refresh: `59950/1000 = 59.95 Hz`
- stop rule: 最大6 run、PASSとFAILの両方を採取した時点で停止
- result: 2 runで停止、PASS 1 / FAIL 1

| run | formal | callback/render/swap | actual opportunities | unique frames | synthetic drop | false skip | true loss | ambiguous |
| ---: | :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | PASS | 3596/3596/3596 | 3597 | 3532 | 68 | 61 | 7 | 0 |
| 2 | FAIL | 3598/3598/3598 | 3597 | 3510 | 90 | 87 | 3 | 0 |
| 合計 | — | — | — | — | 158 | 148 | 10 | 0 |

全skipの93.7%が`FALSE_DEADLINE_SKIP`だった。Q3型`UNPAIRED_SKIP`でも94/101件がfalseである。
formal FAILのrun 2はactual loss対応が3件しかない一方、現行counterは90 dropを報告した。

refresh rationalは対象windowのmonitorとactive display pathを対応付けて取得し、開始／終了で不変だった。
DWM VBlank QPCとQt `frameSwapped`を併用し、全render ordinalにswap eventが一意に対応した。ring overflow、
欠落対応、ambiguous classificationはいずれも0である。

この結果はP2-D5-1を後からPASSへ変更しない。次段階でP2-D5-2を提案する根拠であり、このdiagnostic
campaign自体はclosure PASS authorityではない。threshold 2%、production scheduler、formal checkerは
変更していない。
