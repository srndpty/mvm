# P5-E4 frozen regression evidence

- artifact root: `bench/results/p5-e4-closure-bb65ea5`
- P5-E3 HEAD: `bb65ea50aeadc88901743a03b8a55d3758a6a16a`
- 未変更の第一親: `06182a23140a5ed2fe87cde87244a54b8208cf39`
- fixture A SHA-256: `d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308`
- fixture B SHA-256: `fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479`
- 全fileのSHA-256: `manifest.sha256`

## Formal results

| 対象 | commit | 結果 | summary |
| --- | --- | --- | --- |
| P2 | `bb65ea5` | 6/6 PASS | `p2/summary.json` |
| P4 | `bb65ea5` | 3/3 PASS | `p4/summary.json` |
| P3-C-2 attempt 1 | `bb65ea5` | 8/9 PASS、seek run 1がexit 4 | `p3-c2/summary.json` |
| P3-C-2 attempt 2 | `bb65ea5` | 8/9 PASS、pause-resume run 3がexit 4 | `p3-c2-bb65ea5-attempt2/summary.json` |
| P3-C-2 parent attempt 1 | `06182a2` | 9/9 PASS | `p3-c2-parent-06182a2-attempt1/summary.json` |
| P3-C-2 parent attempt 2 | `06182a2` | 9/9 PASS | `p3-c2-parent-06182a2-attempt2/summary.json` |

P3-C-2の変更後FAILを後続結果で上書きしていない。変更後2回にFAILがあり、未変更親2回がPASSしたため、
このartifactだけからP5-E closure PASSとは判定しない。帰属確認または修正後に新しいartifact rootで
formal matrixを再実行する。
