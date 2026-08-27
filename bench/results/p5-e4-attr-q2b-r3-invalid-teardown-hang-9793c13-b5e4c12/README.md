# ATTR-Q2B-R3 invalid campaign — parent process hang

このdirectoryは`ATTR-Q2B-R3 — Counterbalanced Retry`がparentのprocess hangにより
無効になった歴史的証跡である。Q2B attribution、formal PASS、closure evidenceのいずれにも使わない。

- ordering: parent → head
- main gate commit: `24fcf6e48f5dd379791e355b5d68e6206cf51621`
- diagnostic SHA / executable / fixture: ATTR-Q2と同一
- display preflight: 5/5 PASS、約10秒連続一致、clean committed probe
- preflight signature: `DELL U2412M|landscape|1920x1200|1920x1152|1|1920x1080`
- formal workload完了: parent playback run 1、run 2
- run 1 / run 2: 既存P3-C-2 checker PASS、underflow 0、clock regression 0
- failure: parent playback run 3が12.71分残存し、raw JSONを生成しなかった
- 停止対象: command lineでこのcampaign所属と確認したPID 42380と、そのrunner process treeだけ

run 3はfinal rawを生成していないため、内部の正確な停止stageは確定できない。ウィンドウは応答状態を
保っていたが、これはteardown完了を証明しない。したがって`teardown/protocol hang`として扱い、
`teardown_success`など未観測の値を補完しない。

R3のstop ruleに従い、Q2Bの再試行はここで終了する。このcampaignを後続調査で上書きしない。
`manifest.sha256`は自身を除く全fileを対象とする。
