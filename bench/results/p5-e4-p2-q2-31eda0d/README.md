# P5-E4 / P2-Q2 Same-Binary Pass/Fail Scheduling Attribution

このcampaignは診断専用であり、formal closure PASS authorityではない。production code、threshold、
scheduler policy、formal checkerを変更せず、固定した`31eda0d` executable 1本を6 runすべてで使用した。

Q1実走後のsection抽出が元PEを再書き込みしていたため、Q1で実走したfull-file hashは再利用できない。
Q1と同一の`.text` SHA-256
`273ef3d76b2df29b212e11dd9ba959e136002c15957b765f67ca61bd75665af0`を持つPEをQ2開始前に
別pathへ固定し、全runでfull-file SHA-256
`efd2e11b55d421fd6b5c4766f19ca1732c932f07038ad9909eb9a940c5f8b6b7`を検証した。

## 結果

GeneralProfile、GPU、DesktopCompositionを同時に採取したところ、最大6 runまでFAILは発生しなかった。

| run | checker | deadline drop | drop rate | repeated present | effective fps |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | PASS | 6 | 0.1667% | 4 | 59.8836 |
| 2 | PASS | 33 | 0.9167% | 31 | 59.4338 |
| 3 | PASS | 39 | 1.0833% | 36 | 59.3337 |
| 4 | PASS | 47 | 1.3056% | 45 | 59.2005 |
| 5 | PASS | 23 | 0.6389% | 21 | 59.6004 |
| 6 | PASS | 30 | 0.8333% | 28 | 59.4838 |

deadline dropは平均29.7、repeated presentは平均27.5だった。Q1の同じcandidateはそれぞれ57.2と54.6で、
5 run中1本がFAILしていた。ETW下では両counterがともに低下し、6/6 PASSになった。

refresh rate 59 Hz、GPU driver `32.0.15.9186`、AC online、高パフォーマンスpower planは全runの
開始／終了で不変だった。fixture、checker、executableのSHA-256も全runで固定した。

## ETL integrityと判定

xperf `tracestats -detail`はrun 1～5で正常終了した。run 6は51,773 events lostを報告したため、
詳細scheduler attributionには使用しない。各reportは`analysis/run-N/`へ保存した。

FAIL traceを1本も採取できなかったため、render thread deschedule、DPC/ISR、GPU/DWM/Present waitの
PASS/FAIL比較出口は未達である。Q1比でdeadline dropが約半減したことから、heavy WPR profileによる
trace perturbationを除外できない。指定分岐に従い、WPRでPASSを得たことをformal evidenceへ流用せず、
より低摂動のexternal observationへ進む。P5-E closureは**BLOCKED**のままとする。

ETL 6本（合計約30.4 GB）とNGEN PDBはlocal preservationとしGitから除外する。manifestはこれらを含む
全artifactのSHA-256を保持する。
