# ATTR-Q2B invalid campaign — display orientation changed

このdirectoryは`ATTR-Q2B — Counterbalanced Order`の最初の実行がdisplay environment変化により
無効になった歴史的証跡である。Q2B attribution、formal PASS、closure evidenceのいずれにも使わない。

- ordering: parent → head
- diagnostic SHA / executable / fixture: ATTR-Q2と同一
- 12/12 prefix、78/78 process fileは生成された
- 正式workload開始: 7 process
- preflight拒否: 71 process
- 変化前: DELL U2412M、landscape、1920x1200、window 1920x1080、RHI 1920x1080
- 変化後: DELL U2412M、portrait、1200x1920、window 1204x1080

遷移は`SEEK-PREFIX / attempt 1`のparent全4 processとhead playback 3本の後、head seek開始前に
発生した。head playback run 3のGPU teardown timeoutを除き、変化前の6 processはchecker PASSだった。
以後は`display_target_preflight_pass == false`としてworkload開始前にfail-closedした。

このcampaignを後続の有効なP→H campaignで上書きしない。`manifest.sha256`は自身を除く全fileを対象とする。

