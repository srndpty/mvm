# ATTR-Q2B invalid campaign — screen name changed

このdirectoryは`ATTR-Q2B — Counterbalanced Order`の再実行がdisplay provenance変化により
無効になった歴史的証跡である。Q2B attribution、formal PASS、closure evidenceのいずれにも使わない。

- ordering: parent → head
- diagnostic SHA / executable / fixture: ATTR-Q2と同一
- 12/12 prefix、78/78 processを完了
- hardware provenance: 1 signature、不変
- display geometry / orientation / DPR / RHI target: 全processで同一
- `screen_name`: `\\.\DISPLAY1`と`DELL U2412M`の2 signature
- contract failure: display `screen_name` mismatch 2件、parent pause-resume process exit 4が1件
- head / parentともaudio underflow 0、clock regression 0

`screen_name`はPAUSE attempt 1 parent seek run 1だけ`DELL U2412M`となって次runで戻り、
PAUSE attempt 3 head seek run 3だけ再び`DELL U2412M`となって次runで戻った。他のdisplay fieldは
変化していないが、既存contractは`screen_name`も不変条件とするため、このcampaignはfail-closedで
invalidとする。

parentのPAUSE attempt 3 pause-resume run 3は`pause_video_advance_zero=false`、
`application_av_projection_failure_count=1`、AV abs max 14.042msだった。この症状もinvalid campaign内の
観測であり、order attributionの判定には使わない。

このcampaignを後続の有効なP→H campaignで上書きしない。`manifest.sha256`は自身を除く全fileを対象とする。
