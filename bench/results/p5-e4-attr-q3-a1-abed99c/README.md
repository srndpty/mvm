# P5-E4 / ATTR-Q3-A1 — Seek-523 EOF-tail attribution

## 判定

diagnostic SHA `abed99cff80844a26ac1858b8df30f0c6fd045e7`で、canonical seek domainの
末尾3 target（3890 / 3891 / 3892）を各3回固定指定した。これはattribution専用probeであり、
formal PASS authorityではない。

- target 3890: 3/3 `NO_UNDERFLOW`
- target 3891: 3/3 `NO_UNDERFLOW`
- target 3892: 3/3 `EOF_TAIL_INSUFFICIENT`
- campaign classification: **`EOF_TAIL_INSUFFICIENT`**
- provenance: start/endで不変

QUAL-F1 production fix、canonical seed、threshold、audio pre-roll、buffer、counter semantics、formal
checkerは変更していない。固定target seamとfirst-only telemetryだけを追加した。

## target 3892 の一致

3 attemptすべてで次が一致した。

```text
audio master video frame count       3900
canonical last seek target           3892
target frame                         3892
target sample                   3,113,600
decoder EOF                          true
actual decoded audio end        3,120,128 samples exclusive
underflow requested start       3,119,840
requested count                       480
queue before / consumed                288 / 288
queue last available end        3,120,128 samples exclusive
queue after                              0
endpoint buffer frames                4800
audio start -> underflow        38.0051 / 32.9374 / 34.7337 ms
audio start -> first exact display     未観測 (-1)
```

したがって各attemptで

```text
actualAudioEndExclusive - requestedStart
= 3,120,128 - 3,119,840
= 288
= actuallyConsumed
```

が成立する。直前consume traceも、1,728 → 1,248 → 768 → 288 samplesと480ずつ消費した後、
最後のcallbackが残存288 samplesだけを消費したことを示す。decoderは既にEOFへ到達し、queue末尾と
actual decoded末尾が一致するため、producer starvationやqueue accounting discontinuityではない。

repo指定のUCRT64 `ffprobe 8.1.2`でもfixture Aの全audio frameを走査し、最終PTS
3,119,104 + 1,024 samples = 3,120,128 samples exclusiveと独立確認した。

## provenance

- executable SHA-256: `27fa4522f2371a291a123ffc7dab7fc043e25687e73cd2889dd5f47cf7862c1d`
- fixture A SHA-256: `d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308`
- fixture B SHA-256: `fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479`
- GPU: NVIDIA GeForce RTX 4090
- audio endpoint: 48000 Hz / 2 channels / flt
- display: DELL U2412M / landscape / 1920x1200 / available 1920x1152 / DPR 1
- RHI target: 1920x1080

`summary.json`と9本のraw JSONが一次証拠である。`manifest.sha256`はREADME自身を含む全11 fileを
対象とし、manifest自身だけをself-hash対象外とする。

## qualification status

`QUAL-F1 implementation: PASS / requalification: FAIL / P5-E closure: BLOCKED`を維持する。
この診断はcanonical Campaign BのFAILを無効化せず、formal seek-domain、natural EOFのcounter分類、
audio-start/display marginのどこを修正するかもまだ決めない。
