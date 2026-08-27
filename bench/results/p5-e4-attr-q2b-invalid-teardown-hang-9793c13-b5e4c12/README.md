# ATTR-Q2B invalid campaign — teardown hang

このdirectoryは`ATTR-Q2B — Counterbalanced Order`の再実行中に、parentの
`SEEK-PREFIX / attempt 1 / playback run 2`が規定60秒を大幅に超えて終了せず、
手動で当該processだけを停止した不完全な歴史的証跡である。Q2B attribution、formal PASS、
closure evidenceのいずれにも使わない。

- ordering: parent → head
- diagnostic SHA / executable / fixture: ATTR-Q2と同一
- `playback-run1.json`生成後、playback run 2が約12分残存した
- command lineでrepo所属を確認したPIDだけを停止した
- runnerはplayback run 3とseek run 1へ進んだが、run 2のrawが存在しないためcampaignを中止した
- 保存済みraw: playback run 1、playback run 3

後続の完走campaignでこのdirectoryを上書きしない。`manifest.sha256`は自身を除く全fileを対象とする。
