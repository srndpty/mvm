# Phase 1 / P3-A — Audio Pipeline & Master Clock Foundation 契約

この文書は P3-A 実装・計測前の固定契約である。P3-A は音声単独の decode、
buffering、WASAPI 出力、device clock を検証する。video scheduler は変更せず、P3-B の
audio-master video scheduling、mixing、time stretch、timeline、export は実装しない。

## 固定値

| 項目 | 値 |
| --- | --- |
| 内部 PCM | float32、interleaved、stereo、48 kHz |
| queue target | 250 ms（12,000 sample） |
| queue hard max | 500 ms（24,000 sample） |
| device start pre-roll | 100 ms（4,800 sample） |
| pre-roll timeout | 5,000 ms |
| playback smoke | 15 秒 × 3 independent process |
| exact seek | seed `20260808`、64 回 |

上記は P3-A の結果を見て自動調整しない。queue overflow は drop せず producer を
backpressure する。generation が stale/future の chunk は fail-closed で拒否し、PCM だけを
identity 無しで queue しない。warmup 完了後の不足は silence を device へ渡すが、実音声として
数えず underflow samples に記録する。

## clock 契約

play/resume ごとに `mediaStartSample`、`deviceStartPosition`、`qpcStart` を一つの anchor として
固定する。media sample は callback ごとの浮動小数加算ではなく、IAudioClock の absolute
position から次式に相当する整数演算で導出する。

`mediaStartSample + (devicePosition - deviceStartPosition) * 48000 / deviceFrequency`

running 中は non-decreasing、pause 中は frozen、seek 後は新 generation だけを公開する。
QPC は drift 診断にだけ使い、master clock にしない。

## fixture

`tests/assets/p3_audio/p3_av_h264_aac.mp4` は 1920x1080/60 H.264、65 秒、AAC
48 kHz stereo とする。映像 marker は既存 19-cell 方式、音声 marker は 0、1、5、10、30、59 秒の
sample 位置から 10 ms の左右同相 pulse とする。生成入口は
`scripts/make-p3-fixture.ps1` だけとし、P1/P2 fixture は変更しない。

## 合否の境界

P3-A smoke は application/device-clock level の検査であり、physical speaker output を測ったとは
表現しない。audio-vs-QPC drift と seek latency は診断値のみで、P3-A の閾値に使わない。
exact seek 64/64、generation/stale/timeout/device/lifecycle/thread leak がすべて 0、Release/Debug
通常 CTest が全通過した場合だけ P3-B へ進める。未測定は合格にしない。
