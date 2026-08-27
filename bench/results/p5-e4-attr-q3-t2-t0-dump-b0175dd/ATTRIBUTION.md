# ATTR-Q3-T2 — T0 full dump 無摂動解析

## 判定

この artifact は診断専用であり、formal PASS authority を持たない。

T0 historical hang は **Playback clock stall** へ帰属した。WASAPI render thread が
`IAudioClient::GetCurrentPadding()` の `HRESULT 0x88890004`
(`AUDCLNT_E_DEVICE_INVALIDATED`) を記録して終了した後も、controller が sink failure を
終了条件へ伝播せず、停止した audio master clock が 60 秒到達条件を満たすのを待ち続けていた。

```text
GetCurrentPadding() = AUDCLNT_E_DEVICE_INVALIDATED
  -> WasapiAudioSink::recordFailure()
  -> playing=false / metrics.running=false / deviceFailureCount=1
  -> renderLoop() が終了
  -> audio master clock が 951,850 samples (19.830208 s) で停止
  -> P3AvSyncController は Phase::Playback のまま
  -> required 2,880,000 samples (60 s) に到達せず shutdown 未開始
  -> main thread は Qt event dispatcher 内で待機
```

これは shutdown、GPU teardown、metrics write の blocking hang ではない。また、この証拠だけで
endpoint invalidation の外因まで E3 production change に帰属させない。現在確定した product-side
liveness defect は、非同期 WASAPI failure が controller の fatal/termination path へ伝播しないことである。

## dump 観測値

- T0 diagnostic source SHA: `b0175dd781dffd741f238c5ee623f3bd81284c32`
- T0 記録済み executable SHA-256: `cdd0352d12618479c12a168908d5d57f4edf3f8bda06f53ce4853d005adeb1e2`
- dump SHA-256: `85be6e3470365488cfda3a0e65248f8ff251dc09654eeb787f6fda304401d7e5`
- 959 MBのdump本体はlocal preservationとし、Gitには入れない。T0 manifestのhashは保持する
- debugger: Windows Debugger 10.0.29617.1000 AMD64
- controller phase: `5` (`Playback`)
- warmup reset complete: `true`
- audio clock: running、generation 3、951,850 samples (`19.830208 s`)
- formal completion threshold: 2,880,000 samples (`60 s`)
- sink snapshot: open=true、running=false、joined=false、device failure count=1
- sink error: `WASAPI padding を取得できません: HRESULT 0x88890004`
- render thread state bytes: acceptingCommands=true、threadRunning=true、playing=false
- 68 thread の stack に `WasapiAudioSink::renderLoop()` は存在しない

main thread は `NtUserMsgWaitForMultipleObjectsEx` から Qt event dispatcher / event loop を通り
`QCoreApplication::exec()` に至る待機 stackだった。video decode worker 2本とaudio decode worker 1本は
condition variable待ち、QSG render threadは`QRhi::beginFrame()`配下のwaitだった。`!runaway`にも
CPU spinはない。

## symbol / layout の制約

T0で実行した元のexeファイルは保存されていなかった。同じ source SHAをdetach worktreeへcheckoutし、
公式build scriptで再構築した参照exeのSHA-256は
`0d9eab2ad2003a16fd98ecfc8c7b69fbbd6d6ef6f24908785a9adf10f4e414c9`であり、元の記録値とは一致しない。
したがって参照exeを「exact binary」とは扱わない。

RVA / DWARF layoutの解釈は、dump moduleと参照exeの`ImageSize=0x018db000`一致、formal config列、
controller vptr、列挙値、pointer先objectの整合する値で相互確認した。元exe不在という制約は残るが、
phase、clock、sink error、thread absenceが同じcausal chainを独立に支持するため、T2の出口には十分である。

## 実行した解析

- `!analyze -hang -v`
- `~* k`
- `!runaway`
- `lm` / `lmvm mvm_p3_av_sync_spike`
- formal config patternからcontroller候補を同定し、controller / audio clock / sink objectをread-onlyで確認
- Windows SDK / MSYS2 UCRT64 headerで`AUDCLNT_E_DEVICE_INVALIDATED`を確認

最初の`.reload /f`付き解析は、元image不在のためsymbol reloadが進まず手動終了した。そのログも
`cdb-analysis.txt`として残し、成功したlean解析で上書きしていない。

## 次の候補

production fixに進む場合の第一候補は、render thread内で記録したdevice failureをcontrollerへ
fail-closedに伝播し、停止したclockを無期限に待たず既存failure shutdownへ入れることである。
実装時は実際の`recordFailure()`経路を使うnegative testを追加し、threshold、buffer / pre-roll、
counter semantics、formal checkerは変更しない。fix後のformal matrixはclean production SHAで別途行う。

T0 dumpだけでT2出口を満たしたため、exact T0 binaryを使う追加workload、live attach、atomic probeは
実施していない。T1の30/30正常runもhistorical T0 hangを無効化せず、probe effectを否定しない。
