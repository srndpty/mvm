# P5-E4 / ATTR-Q2 paired prefix evidence

このdirectoryは`P5-E4 / ATTR-Q2 — Paired Prefix Reproduction`のimmutable diagnostic artifactである。
prefix結果はformal PASS authorityではなく、P5-E closureは引き続きBLOCKEDである。

## Provenance

| cohort | base SHA | diagnostic SHA | executable SHA-256 |
| --- | --- | --- | --- |
| head | `bb65ea50aeadc88901743a03b8a55d3758a6a16a` | `b5e4c12d19009da98bcdbc58cd745b974d6515ea` | `4f81a0816a27dd87b7a0411aa2e8f09449e6b25035963e39c37cee8654e0ea8e` |
| parent | `06182a23140a5ed2fe87cde87244a54b8208cf39` | `9793c13a5699752e25975102da44b11a8e999e0b` | `a5687ca3d370c5fd10f66b06b21d49c093484afec1a1ea06b9cc23166810f355` |

- 両diagnostic commitのATTR-Q1 stable patch-id:
  `f301d8bb5fbb030845a480e2d9f982fcb943dd68`
- fixture A SHA-256: `d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308`
- fixture B SHA-256: `fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479`
- GPU: `NVIDIA GeForce RTX 4090`
- audio endpoint: 48000 Hz / 2 channels / `flt`
- display: `DELL U2412M`, landscape, 1920x1200, DPR 1、RHI target 1920x1080
- hardware/display signatureは78 processを通して不変

## Execution

各profileをattempt 1から3まで、各attempt内でhead、parentの順に直列実行した。

- `SEEK-PREFIX`: playback x3 + seek x1
- `PAUSE-PREFIX`: playback x3 + seek x3 + pause-resume x3
- 各process: warmup 5秒、measurement 60秒、seed `20260808`、seek 1000回、
  display timeout 3000ms、`--formal-contract-c2`
- 完了: 12/12 prefix、78/78 process

`paired-summary.json`と各profile/attempt/cohort配下の`summary.json`にrunner結果を保存した。
raw内の`raw_path`は実行時のscratch pathであり、artifact固定後の現在位置は同じ相対directory構造である。

## Result

| cohort | process | underflow | clock regression | その他のFAIL |
| --- | ---: | ---: | ---: | --- |
| head | 39 | 0 | 0 | seek AV abs max 59.146msが1件 |
| parent | 39 | 3 | 0 | teardown timeout 1件、pause中video advance 1件 |

ATTR-Q1対象failureはparentのseek underflow 3件だけで、すべてseek ordinal 523、target frame 3892、
generation 524で一致した。詳細比較と次候補は`ATTRIBUTION.md`に記録する。

`manifest.sha256`は自身を除く全fileを対象とする。今後のrunでこのdirectoryを上書きしない。
