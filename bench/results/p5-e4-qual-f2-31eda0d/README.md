# P5-E4 / QUAL-F2 — generation-scoped audio EOF classification

## 判定

- QUAL-F2 implementation: **PASS**
- QUAL-F2 targeted integration: **9/9 PASS**
- P3-C-2 requalification campaign A: **9/9 PASS**
- P3-C-2 requalification campaign B: **9/9 PASS**
- ordinary CTest: release **477/477 PASS**、debug **477/477 PASS**
- final closure suite: **FAIL**（P2 playback drop-rate）
- P5-E closure: **BLOCKED**

tested production SHAは`31eda0d8d080dcf4b1680149d85b8293f618cd57`である。historical
ATTR-Q3-A1およびQUAL-F1 FAIL evidenceは変更していない。

## QUAL-F2 contract evidence

queue unit testは次を固定した。

1. 288 PCM + authoritative EOF + 480 requestを192 sampleのterminal silenceとし、underflowを増やさない。
2. EOS未確定の同じ不足をstarvationとする。
3. queue末尾からEOSまでgapがある場合はstarvationとする。
4. stale generationのEOS publishを拒否する。
5. generation前進後はEOS authorityを未確定へ戻す。

「EOF flagがあれば全不足をTerminalEofにする」mutationではEOS前gap testがexit 1となり、検査が
実装の雑な緩和を検出することを実測した。正規実装復元後はp5d 16/16 PASSだった。

fixed-targetでは3890 / 3891 / 3892を各3回実行し、全runでunderflow 0、exact audio/video identityを
維持した。3892のfirst terminal shortageは全3回でrequested start 3,119,840、request 480、PCM 288、
silence 192、authoritative end 3,120,128だった。total silence量は100 ms診断観測窓のcallback数に依存
するためcontract値にしない。

## canonical P3-C-2

独立campaign A/Bはそれぞれplayback / seek / pause-resume各3本、合計9/9 PASSだった。両campaignで
git、executable、fixture、hardware、display provenanceは不変である。seed 20260808、1000 seeks、
warmup 5秒、measurement 60秒、threshold、checkerは変更していない。

## final closure blocker

P2 frozen matrixはplayback run 1で停止した。

```text
process exit                         0
contract exit                        3
effective fps                 58.2343004
drop rate                       2.9166667%  (required <= 2%)
scheduled / displayed            3600 / 3495
scheduler deadline drop                 105
repeated present                       103
decoded A / B                    3583 / 3583
marker / probe / mixed / stale             0
device lost / lifecycle                    0
teardown success                       true
```

historical `bb65ea5` P2 PASS 3本のdeadline dropは41 / 47 / 30だった。QUAL-F2差分はaudio preview、
P3 harness、test/docs/scriptsに限定され、P2 compositor / gpu preview pathのsource差分はないため、
QUAL-F2へのcausal attributionは成立していない。ただしformal P2 FAILは有効なのでclosureをBLOCKする。
後続PASSでこのartifactを上書きしない。stop ruleによりP4 frozen matrixは未実行である。

## provenance

- executable SHA-256: `22b241db17d7460208a0f74ca064b1044fa0861b7b91c6f4b913f2323e8136d2`
- fixture A SHA-256: `d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308`
- fixture B SHA-256: `fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479`
- GPU: NVIDIA GeForce RTX 4090
- audio endpoint: 48000 Hz / 2 channels / flt

各subdirectoryのraw/summaryとroot `manifest.sha256`を一次証拠とする。
