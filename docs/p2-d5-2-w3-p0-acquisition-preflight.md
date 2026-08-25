# P2-D5-2 W3-P0 Workstation / Acquisition Preflight Attribution

W2-D / W2-E closure 後のフル ctest は 1004/1006 だった。残る 2 失敗を W3 fresh
acquisition の precondition として attribution する。目的は「テストを緑にする」ことでは
なく、**W3 acquisition validity へ影響するかを exact に確定する**ことである。

```text
W3-P0-A  p2_present_id_oracle_live   HARD: live presentation acquisition substrate
W3-P0-B  proxy_resolver_unit         SOFT: loader failure dependency
```

## W3-P0-A — p2_present_id_oracle_live

### 症状

```text
ORACLE_SAMPLING_GAP: transition数が不足しています   (probe exit=4 / checker exit=1)
```

### [事実] 回帰ではない

保持されている artifact 8 件すべてが `INVALID / ORACLE_SAMPLING_GAP` である。
最古は 08-22 で、W2-D / W2-E 着手 (08-25) より 3 日前から継続している。

```text
08-22 18:12  trans= 227/900  poll_valid=False  max_poll/nominal= 6.00  vblank_gap=126
08-23 09:13  trans= 700/900  poll_valid=False  max_poll/nominal= 6.00  vblank_gap=199
08-23 09:14  trans= 757/900  poll_valid=False  max_poll/nominal= 4.01  vblank_gap=144
08-23 16:33  trans= 463/900  poll_valid=False  max_poll/nominal=11.01  vblank_gap=435
08-23 16:37  trans= 605/900  poll_valid=False  max_poll/nominal=13.00  vblank_gap=295
08-24 01:46  trans= 140/900  poll_valid=False  max_poll/nominal= 4.02  vblank_gap=132
08-25 23:40  trans= 191/900  poll_valid=False  max_poll/nominal= 5.01  vblank_gap=129
08-25 23:45  trans= 312/900  poll_valid=False  max_poll/nominal= 4.00  vblank_gap=129
```

### [事実] gap の exact な原因は sampler consumer thread の starvation

contract は poller が 1 refresh 期間に 2 回以上 poll することを要求する。

```text
max_poll_interval_qpc * 2 < nominal_period_qpc
```

実測は `max_poll_interval / nominal_period` が 4.00〜13.00 であり、poller は
**4〜13 refresh 期間ぶん descheduled されている**。その結果 900 submission のうち
140〜757 の PresentCount transition しか観測できず、`observed_ids_complete=false` になる。

一方、substrate 側は健全である。

```text
statistics_failure_count      = 0     GetFrameStatistics は失敗していない
statistics_disjoint_count     = 0     frame statistics は disjoint でない
vblank_ring_overflow_count    = 0     VBlank observer の ring は溢れていない
vblank_wait_failure_count     = 0     WaitForVBlank は失敗していない
window_output_stable          = true  run 中に output は変化していない
sampler_high_priority         = true  THREAD_PRIORITY_HIGHEST は取れている
sampler_baseline_ready        = true  baseline は確立できている
```

すなわち DXGI も VBlank observer も正常で、**observer が publish した VBlank を
probe 内の consumer thread が読み落としている**。`sampler_vblank_gap_count` は
observer の取りこぼしではなく consumer の読み落とし数である
([apps/p2_present_identity_probe/main.cpp:202-231](../apps/p2_present_identity_probe/main.cpp#L202-L231)
の `statisticsThread` が `Sleep(0)` の busy-wait で `ring().publishedCount()` を
追う構造)。

`THREAD_PRIORITY_HIGHEST` + `Sleep(0)` yield では、この開発機の負荷下で
1 refresh 期間内の再スケジュールを保証できていない。

### [事実] W3 acquisition path とは機構が独立している

```text
GetFrameStatistics の所在        apps/p2_present_identity_probe/main.cpp のみ
oracle artifact の consumer      check-p2-present-id-oracle.ps1 と
                                 test-p2-present-id-oracle-contract.ps1 のみ
```

W2-A/B/C/D/E の checker は 1 つも oracle artifact を consume していない。
formal-v2 canonical chain は native present hook + ETW PresentEvent + DisplayedQPC +
`WindowOutputVBlankObserver` で構成され、`GetFrameStatistics` を使わない。
W2-E canonical artifact も `dwm_frame_statistics_authority = false` を固定している。

### [事実] 共有部品 (VBlank observer) は同一機で starve していない

probe と compositor spike は `WindowOutputVBlankObserver` を共有する。上記の
starve した probe run でも observer 自体は `ring_overflow=0 / wait_failure=0` である。

さらに決定的な証拠として、**同じ開発機で取得した fresh-7 の 3 run すべて**で
Layer 1B guard がすべて 0 である。

```text
run 1/2/3  shadow_authority_valid=True  sequence_status=OK
           long_interval_count=0  short_interval_count=0
           ring_overflow_count=0  wait_failure_count=0
           cumulative_consistent=True  output_stable=True
           boundary_bracketed=True  prestart_vblank_preroll_completed=True
           physical_opportunity_count=299
```

W2-E は checker-level cutover であり binary を変更していないため、**fresh-7 の binary は
W3 acquisition の binary と同一**である。つまり W3 が使う経路は、この機で実際に
starvation を起こしていない。

### [事実] 仮に starve しても silent には absorb されない

W2-A checker は `sequence_status != OK` / `long_interval_count` / `short_interval_count` /
`ring_overflow_count` / `wait_failure_count` / `cumulative_consistent` /
`boundary_bracketed` / preroll をすべて fail-close する。starvation が W3 acquisition で
起きた場合は `AUTHORITY_INVALID` として検出され、performance FAIL とは区別される。

### [exit] W3-P0-A の判定

```text
attribution                        EXACT (probe-local sampler consumer starvation)
regression                         NO (08-22 から継続、W2-D/E 以前)
W3 acquisition validityへの影響    NO
  - 失敗機構 (GetFrameStatistics polling) は W3 path に存在しない
  - 共有 observer は同一機の fresh-7 3 run で guard すべて 0
  - 仮に starve しても W2-A が AUTHORITY_INVALID で fail-close する
```

したがって W3 は本件を理由に blocked ではない。ただし `p2_present_id_oracle_live`
自体は赤のままであり、**Present-ID oracle の成立は別途必要**である。修正するなら
probe の consumer を `THREAD_PRIORITY_TIME_CRITICAL` へ上げるか、`Sleep(0)` busy-wait を
観測待ちの event ベースへ変える方向になる。これは probe 側の acquisition 設計の
問題であり、formal-v2 presentation authority とは独立である。

## W3-P0-B — proxy_resolver_unit

### [事実] root cause は host toolchain の DLL を先に拾っていたこと

```text
Exit code 0xc0000139  (STATUS_ENTRYPOINT_NOT_FOUND)
```

`mvm_test_proxy_resolver.exe` は UCRT64 の `libstdc++-6.dll` / `libgcc_s_seh-1.dll` に
依存する。しかし ambient PATH 上で先に見つかるのは次だった。

```text
C:\Program Files\Git\mingw64\bin\libstdc++-6.dll
C:\Program Files\Git\mingw64\bin\libgcc_s_seh-1.dll
```

Git for Windows の MinGW runtime を読み込むためエントリポイントが解決できない。

原因は登録方法である。`mvm_add_test` は
`ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;..."` を付けるが、この関数は
tests/CMakeLists.txt:85 で定義されており、それより前にある
`scrub_coalescer_unit` / `scrub_coalescer_threaded` / `proxy_resolver_unit` は
素の `add_test()` で登録されていて PATH が付いていなかった。

UCRT64 を PATH 先頭に置いて直接起動すると、この exe は正常に動作する。

### [事実] W3 executable と loader dependency は共有していない

```text
mvm_compositor_spike.exe   invoke-p2-c0-native-run.ps1:29 が
                           C:\msys64\ucrt64\bin を PATH 先頭へ置いてから起動する
                           (p2-d5-2-w2a-live-shadow.ps1 / p2-d5-2-w2-b1-live.ps1 も同様)
mvm_present_history_decoder.exe
                           libstdc++ / libgcc / libwinpthread への依存が無い
                           (MinGW C++ runtime 非依存)
```

したがって W3 acquisition executable はこの DLL 解決経路を共有していない。

### [対応] 規約どおり PATH を明示した

AGENTS.md の「ホストの版へフォールバックしない」に従い、3 テストへ UCRT64 PATH を
明示した (tests/CMakeLists.txt)。`mvm_add_test` は定義位置の関係で使えないため
`set_tests_properties` で同じ ENVIRONMENT を与えている。

```text
proxy_resolver_unit        Exit 0xc0000139 -> PASS
scrub_coalescer_unit       PASS (維持)
scrub_coalescer_threaded   PASS (維持)
```

`scrub_coalescer_threaded` は同じ素の `add_test` でありながら偶然通っていた。
同じ潜在欠陥なのでまとめて閉じた。

### [exit] W3-P0-B の判定

```text
attribution                      EXACT (host MinGW libstdc++ の先行解決)
W3 executableとの依存共有        NO
W3 blocker                       解除
状態                             FIXED (PATH を規約どおり明示)
```

## まとめ

```text
W3-P0-A present-id oracle   attribution EXACT / W3 validityへの影響なし
                            oracle 自体は赤のまま (probe consumer の設計問題)
W3-P0-B proxy loader        attribution EXACT / FIXED
```
