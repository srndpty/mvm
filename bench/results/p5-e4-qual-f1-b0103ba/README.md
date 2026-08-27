# P5-E4 / QUAL-F1 — P3-C-2再qualification

## 判定

clean production candidate `b0103ba54dbc5da355279e8e4db712c451661886`に対し、canonical
P3-C-2 matrixを独立2 campaignとして実行した。

- campaign A: **9/9 PASS**、formal verdict PASS
- campaign B: playback 3/3 PASS後、seek run 1でFAIL。stop ruleにより4/9で終了
- QUAL-F1 requalification: **FAIL**
- P5-E closure: **BLOCKED**
- final closure suite: 未実行

PASS runを選別せず、AとBを同じartifact rootへ保存する。campaign BのFAILを再実行で上書きしない。

## campaign B first failure

```text
mode                         seek
run                          1
process_exit_code            4
contract_exit_code           1
producer pass                false
shutdown detail              integrated seek 完了
measurement underflow        1
seek exact                   1000 / 1000
seek timeout                 0
generation mismatch          0
device failure               0
teardown success             true
```

first underflow snapshot:

```text
seek ordinal                 523
target frame                 3892
requested sample start       3,119,840
requested sample count       480
queue before                 288
consumed                     288
queue after                  0
shortage                     192 samples
audio master position        3,114,093
source generation            524
engine state                 WaitDisplay (3)
```

これはATTR-Q2でparent cohortに観測したordinal 523 / target 3892 / requested start 3,119,840 /
queue 288 / consumed 288と同じfailure identityである。QUAL-F1が対象としたasync WASAPI device failureは
0件であり、今回のFAILをQUAL-F1 fail-close実装のregressionとは分類しない。一方、canonical matrixの
formal FAILであるためclosure blockerとして扱う。

## provenance

- executable SHA-256: `f2c5df644eba09d696e357168a9a99fa4cb94a0117933df3cba58f663fc2bc87`
- fixture A SHA-256: `d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308`
- fixture B SHA-256: `fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479`
- GPU: NVIDIA GeForce RTX 4090
- audio endpoint format: 48000 Hz / 2 channels / flt
- display: DELL U2412M、landscape、1920x1200、available 1920x1152、DPR 1
- RHI target: 1920x1080

両campaignともgit SHA、dirty state、executable / fixture hash、hardware、display provenanceは
start/endで不変だった。runner、checker、warmup 5秒、measurement 60秒、1000 seeks、seed 20260808、
thresholdは変更していない。
