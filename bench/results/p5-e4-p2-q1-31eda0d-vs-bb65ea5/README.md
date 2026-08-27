# P5-E4 / P2-Q1 Deadline Drop Attribution

このartifactは診断専用であり、formal closure PASS authorityではない。historical evidenceを変更せず、
QUAL-F2 product candidate `31eda0d8d080dcf4b1680149d85b8293f618cd57`（C）と、historical E3
`bb65ea50aeadc88901743a03b8a55d3758a6a16a`（B）を比較した。

## Q1-A binary / link attribution

両SHAをdetached clean worktreeで同じUCRT64 Release recipeとlink map optionによりbuildした。

- candidate executable SHA-256: `78cde287e6a91a6413adf90723eea87c36bae9471b7fff87e8879aa822bd283a`
- baseline executable SHA-256: `891161bea4231fce707cb40efb9f98dc5113fe7cba3e81dc012a13d95578001c`
- archive member set: 35対35、差分0
- 両方へlinkされたaudio member: `audio_clock`、`audio_decode_worker`、`audio_frame_queue`、
  `audio_video_scheduler`、`wasapi_audio_sink`
- candidate `.text`: 626,736 bytes、baseline `.text`: 622,352 bytes（差 +4,384 bytes）
- `CompositorSpikeController::tick()`、`CompositorCoordinator::compose()`、
  `CompositorRhiRenderer::render()`は同一RVA
- `GpuCompositor::issueComposition()`と`OutputScheduler60Hz::takeDueBefore()`はcandidateで
  `+0x3c0`移動

QUAL-F2までの変更にはP2 executableへ到達するaudio / preview diagnostic codeが含まれるため、
最終PEは同一ではない。ただし主要hot pathの一部は同一配置、一部は移動しており、この結果だけでは
candidate固有deadline dropを帰属できない。link map、`.text` image、hash、RVAは`binary/`へ保存した。

## Q1-B counterbalanced paired playback

条件はformal P2 playbackと同じwarmup 5秒、measurement 60秒、seed 20260808、fence completion、
同一fixture / checkerである。順序は`C1→B1→B2→C2→C3→B3→B4→C4→C5→B5`とした。

| cohort | contract PASS | deadline drop min / median / max | mean | effective fps mean |
| --- | ---: | ---: | ---: | ---: |
| candidate 31eda0d | 4/5 | 38 / 62 / 73 | 57.2 | 59.0302 |
| baseline bb65ea5 | 3/5 | 51 / 68 / 77 | 66.4 | 58.8770 |

candidate run 3は73 drops（2.0278%）、baseline run 2は77 drops（2.1389%）、baseline run 5は
73 drops（2.0278%）でchecker FAILだった。全10 runでdecoded A/Bは各3,583、present callbackは
3,596～3,598で、deadline dropとrepeated presentが連動した。

したがって今回のcounterbalanced sampleはcandidate固有regressionを支持せず、selective revert candidateは
選定しない。次のleadはOS / GPU / Qt render-loop schedulingの外部timing attributionであり、必要ならETW等の
低摂動traceでdeadline miss前後を比較する。P5-E closureはBLOCKEDのままとする。

`paired/summary.json`は全run順序、process/checker exit、実測集計、source SHA、executable / fixture /
runner / checker SHA-256を保持する。各raw JSONがadapter、output size、completion backendを保持する。
