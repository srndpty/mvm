# Phase 3 / C-1 formal contract

本書は Phase 3 / C の合否条件を、正式計測を実行する前に固定する。
raw schema は `mvm-p3-formal-1`、matrix summary は
`mvm-p3-matrix-summary-1` とする。P3-B の結果は historical diagnostic であり、
P3-C の正式判定には使用しない。

## Matrix と provenance

Playback / Seek / PauseResume を各 3 independent process、合計 9 process 実行する。
平均値による救済は行わず、全 run が独立に PASS しなければならない。
正式実行は clean worktree のみ許可し、開始・終了時の commit、dirty 状態、fixture
SHA256、実行ファイル SHA256、contract version が一致しなければ FAIL とする。

## Playback の区間

startup 後に audio/video を 5 秒 warmup し、scheduler と sink を停止して video A/B
frame 0、audio sample 0 へ exact seek する。generation を adopt し、video 8 frame、
audio source 100 ms、endpoint prefill を満たしてから audio clock を sample 0 に anchor
して測定を開始する。warmup の counter は測定値へ含めない。

48 kHz の 60 秒を `[0, 2,880,000)` sample と定義する。sample 0..799 は frame 0、
sample 800 は frame 1、sample 2,879,999 は frame 3599 である。sample 2,880,000
以上では frame 3600 を schedule せず、audio media sample で測定終了を決める。
wall/QPC elapsed は診断値であり終了判定には使わない。

actual composition display ledger は frame identity が strictly increasing、first frame 0
でなければならない。各 gap と frame 3599 までの tail を skipped とし、
`displayed_unique + skipped == 3600` を要求する。duplicate identity は 0 とする。

- `effective_video_fps = displayed_unique / 60.0` は 55 以上
- `drop_rate = skipped / 3600` は 0.02 以下

各 actual display record の QPC を timestamped `IAudioClock` へ射影し、
`(frame * 800 - audioSampleAtDisplay) * 1000 / 48000` を
`application_av_delta_ms` とする。physical speaker/monitor sync とは呼ばない。
raw signed 値を全件保存し、checker が nearest-rank で独立再計算する。sample count は
displayed unique count と一致し、projection failure は 0 でなければならない。

- absolute p95 は 20.000 ms 以下（20.001 ms は FAIL）
- absolute observed max は 33.334 ms 以下（33.335 ms は FAIL）

## Integrated seek

seed `20260808` で deterministic target を 1000 件作る。target は video frame `N`、
audio sample `N * 800` とする。A/B/audio の全 request を先に Accepted にしてから、
3 source 共通の 5 秒 deadline で completion を並行に待つ。短時間 CV wait/poll を使い、
busy loop と source ごとの逐次 5 秒 wait を禁止する。

全 seek で A/B exact frame、first audio sample、first actual displayed video frame、
generation/pair identity が target と一致し、timeout、busy acceptance、stale completion、
generation mismatch、underflow、QPC fallback が 0 でなければならない。
request から first integrated actual display までの raw 1000 件について nearest-rank
p95 150.000 ms 以下、observed max 400.000 ms 以下とする。first display の application
A/V delta も 1000/1000 valid とし、Playback と同じ 20.000 / 33.334 ms 境界を使う。

## Pause / Resume

各 run は play 2 秒、pause 1 秒、resume 2 秒とする。pause 中は audio clock frozen、
actual composition display delta 0、generation 不変、QPC fallback 0 を要求する。resume
では audio clock を同じ generation へ re-anchor し、video monotonic、underflow と clock
regression 0 を要求する。resume 後を含む application A/V delta は Playback と同じ境界を
適用する。

## 共通 correctness

測定区間の audio underflow/overflow、marker mismatch、mixed pair/generation、stale
composition epoch、video ahead、clock regression、audio clock query failure、QPC fallback
はすべて 0 とする。global では full-frame CPU readback、full-frame GPU copy、software
video fallback、device lost、lifecycle violation、thread join leak を 0 とし、teardown
成功後に final report を書く。

正式 checker は missing/null、NaN、±Infinity、非 integer の integer field、負数禁止
field、array 件数不一致を fail-closed で拒否する。producer の percentile helper や summary
値を信頼せず raw から再計算する。

## C0 の実行範囲

この段階では `pwsh scripts/p3-matrix.ps1 -DryRun` の Playback 5 秒（warmup 1 秒）、
Seek 64 件、PauseResume 1 process だけを実行する。A/V 値は報告するが formal verdict は
`NOT_RUN` のままとする。正式 9 run は `pwsh scripts/p3-matrix.ps1` で別途実行する。
