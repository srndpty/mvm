# P3-5B: 書き出しの timeout を実時間で数える

2026-10-13。MLT の書き出しの待ち loop は、timeout を `Sleep(20)` の回数で数えていた。これを単調な時計
（実時間）で数えるように直した。

timeout の値（製品は 600000 ms）、監視する区間、書き出しの graph、画素、数式 group の合成、V1 の背景の保護、
出力の公開の仕方は変えていない。数式 group は、製品の書き出しでは無効のままである。commit・push はしていない。
これまでの証拠と判定は変更していない（`math_equation_sequence_product_ui` の SegFault は UNKNOWN のまま）。

証拠は `build/mlt-export-timeout-p35b-20261013/`。

## 判定

| 項目 | 結果 |
| --- | --- |
| 実時間での数え方 | **成立。** 1 秒の timeout を、待ちの開始から 1002〜1046 ms で検出（30 件）。修正の前は 1,534〜3,351 ms、進行 callback が 400 ms かかる場合は timeout しなかった |
| timeout の区間（何を含むか） | 変えていない。consumer を開始してから止まるまでの待ちだけ |
| 終わり方の優先順 | 決めて、試験した: 完了 → 取消 → 合成・背景の失敗 → timeout（3） |
| 取消・失敗 | 変えていない。取消は取消、consumer の失敗は失敗のまま（timeout にも成功にもならない） |
| 出力の後始末 | timeout・取消で、最終 file も一時 file も残らない。数式 group の transition・callback・画像も残らない |
| 数式 group の回帰 | 成立。判定の対照 11 通りと、v4 の全体 graph の 2 区間が、製品の経路と全 frame 一致 |
| 全 CTest・lint | **PASS: 1523/1523。** 実行の前後とも GUI の環境は VALID。lint 通過 |

**全体: PASS。** 新しい試験は修正の前の実装で落ち、修正の後は実時間の契約を満たす。必須の回帰は全部通った。**

## 1. 既存の契約（Task A）

### 1.1 実装が実際にしていたこと

`mvm_mlt_export.c` の 2 つの関数（`mvm_mlt_export_two_track`、`mvm_mlt_export_sequence`）は、同じ形の待ち loop を
持っていた。

```
waited = 0
while (consumer が止まっていない) {
    進行 callback (取消なら取消で終わる)
    合成・背景の失敗の確認 (two_track だけ)
    Sleep(20); waited += 20
    if (waited >= timeout_ms) → timeout
}
```

| 問い | 答え |
| --- | --- |
| 監視の始まり | consumer を開始した直後（待ち loop の直前。observer の `RENDER` の段階） |
| 監視の終わり | consumer が止まったことを待ち loop が見た時点 |
| 含むもの | consumer の実行（render・encode・mux）を待つ時間 |
| 含まないもの | 準備（stage）・graph の構築・consumer の開始、consumer の close（encoder の flush、trailer）、出力の検証、公開 |
| 数えていたもの | loop の回数 × 20 ms。**実時間ではない** |
| `timeout_ms <= 0` | 引数の誤りとして拒否（どちらの関数も）。「無制限」の指定は無い |
| 呼び出し側と値 | 製品: `TimelineExportRequest::timeoutMs` の既定値 600000（`timeline_export.h`）。試験と検証 harness は、短い値・長い値を渡す |

### 1.2 意図されていた契約

- 名前（`timeout_ms`）、error の文（「consumer が %d ms 以内に終了しませんでした」）、既存の監査の試験の
  説明（「実時間で timeout_ms の 1.5 倍以内に timeout すれば契約どおり」）から、**意図は実時間**である。
  進み具合（frame が進んでいるか）を見るものではない。
- 実装は、loop の 1 回にかかる実時間が 20 ms より長い分だけ、遅く timeout していた。loop の 1 回には、
  `Sleep(20)`（Windows の既定の粒度では約 31 ms）と、進行 callback の時間が入る。

[事実] 修正の前の実装で測った値（`before/new-test-on-old-implementation.log`。`timeout_ms` = 1000）:

| 条件 | timeout を見つけるまでの実時間 |
| --- | --- |
| 進行 callback が速い | 1,534〜1,538 ms（約 1.5 倍） |
| 進行 callback が 30 ms | 3,290〜3,351 ms（約 3.3 倍） |
| 進行 callback が 400 ms | **timeout しない**（10.9 秒かけて成功した） |

### 1.3 timeout の区間の互換性

- 値（600000 ms）と、監視する区間（待ち loop だけ）は変えていない。
- **変わるのは、実際に timeout する時刻である。** これまで製品の書き出しは、実時間では 600 秒より遅く
  timeout していた。進行 callback が速い場合で約 1.5 倍（約 900 秒）である [推測: 上の測定の比を 600 秒へ
  当てはめたもの。600 秒の timeout は実行していない]。
- したがって、**待ちの時間が 600 秒から約 900 秒の間だった書き出しは、これまでは成功し、これからは timeout に
  なる。** これは、意図していた契約（600 秒）に合わせた結果である。数式 group の診断用の経路での v4 の
  書き出しは 272〜294 秒（P3-5A）で、影響を受けない。

## 2. 修正（Task C）

`mvm_mlt_export.c` の 2 つの待ち loop だけを変えた。

- 時計: `QueryPerformanceCounter`（単調。既存の `mvm_mlt_export_observer_now_ns`）。待ちを始めた時刻を持ち、
  確認のたびに「今 − 始め」を ms にして `timeout_ms` と比べる。64 bit の ns の差なので、桁あふれしない。
  loop の回数は数えない。
- 待ち方: 1 回は最大 20 ms（これまでと同じ）。期限までの残りが 20 ms より短ければ、残りの分だけ眠る
  （最低 1 ms。空回りはしない）。
- timeout を見る位置: 進行 callback と失敗の確認の後、眠る前。

変えていないもの: 値、監視の始まりと終わり、error の文、取消と失敗の扱い、timeout のときに consumer を止めて
後始末へ進む道。

同じ形（`Sleep` の回数で数える）の待ちは、ほかにもある: `mvm_mlt_audiograph.c`、`mvm_mlt_compose.c` の
`mvm_mlt_compose_render_audio`、`mvm_mlt_effects_p0.c`、`mvm_mlt_overlay_p0.c`。**今回は変えていない**
（対象は製品の映像の書き出しの待ち loop）。

## 3. 終わり方の優先順（Task D）

待ち loop は 1 回ごとに、次の順に見る。同じ回で重なったら、先のものが勝つ。

1. **完了**: consumer が止まっていれば、待ちを終えて、close と出力の検証へ進む。
2. **取消**: 進行 callback が取消を返した。
3. **合成・背景の失敗**（`two_track` だけ）。
4. **timeout**: 経過が `timeout_ms` 以上。ただし、**その時点で consumer が止まっていれば完了として扱う。**

| 場合 | 結果 | 試験 |
| --- | --- | --- |
| 期限より前に完了 | 成功 | `completes-early` |
| 期限の近くで完了 | かかる時間 D を測り、期限が D + 600 ms なら成功、D − 600 ms なら timeout。ちょうどの時刻での結果は決めない | `near-deadline-*` |
| 期限の後に完了（待ち loop が確認した時点で止まっている） | **成功。** MLT は consumer が止まった時刻を教えないので、期限の前か後かは分からない。止まっていれば完了として扱う | `stalled-poll-completed` |
| 期限より前の取消 | 取消 | `cancel-before-deadline` |
| 取消と timeout が同じ回で重なる | 取消 | `cancel-races-timeout` |
| consumer の失敗と timeout が重なる | 失敗（consumer が自分で止まるので完了の道へ進み、出力の検証で失敗になる）。timeout とは報告しない | `consumer-failure-stalled` |
| 待ち loop の thread が遅れた（進行 callback が長い、thread が止められた） | consumer が動いていれば、起きた時点で timeout。遅れた分だけ検出が遅れる | `poll-400ms`、`stalled-poll-running` |
| timeout の後の後始末 | consumer を止めて閉じ、戻った後に render worker の callback は走らない | 全部の timeout の scenario |
| 同じ process で続けて書き出す | 前の書き出しの時間を引きずらない（timeout → 成功 → timeout を 2 周） | `repeat-*` |

以前の実装との違いは、1 と 4 の関係だけである。以前は、眠った後にまず回数を見ていたので、「最後の 1 回の
間に consumer が終わっていた」場合でも、回数が達していれば timeout になった。実時間で数えると、待ち loop の
thread が遅れただけで終わっていた書き出しを timeout にしてしまうことがあるので、**止まっていれば完了**と
決めた。取消・失敗の扱いは変えていない。

分からないこと: consumer が止まった正確な時刻。待ち loop が見るのは「確認した時点で止まっているか」だけで、
確認の間隔（最大 20 ms + 進行 callback の時間）より細かくは分からない。

## 4. 試験（Task B・E）

### 4.1 新しい試験 `mlt_export_timeout_real_time`

`tests/harness/test_mlt_export_timeout.cpp`。**実物の書き出しの関数**（待ち loop を含む）を、実際の動画
（`v1080p60_h264.mp4`、320×240 へ書き出し）で動かす。計算だけを取り出した試験ではない。

- 遅い consumer: encoder 直前の RGBA の検査で、frame ごとに眠る（render worker の上で）。
- 遅い待ち loop: 進行 callback で眠る。
- 時間は、試験の側の時計（待ちの開始の段階の時刻から、最後の進行 callback が戻った時刻まで）で測る。
  書き出しの中の timeout の変数は使わない。
- 許容: `timeout_ms` より 25 ms 以上早く timeout しないこと、`timeout_ms` + 400 ms までに timeout すること
  （待ちの 1 回と、Windows の sleep の粒度・thread の切り替えの分）。進行 callback が長い scenario は、
  その分を足す。**1 秒の timeout が秒の単位で遅れることは許さない。**
- 時間を測るので、ほかの試験と並べない（`RUN_SERIAL`）。sleep を伸ばして通すことはしていない。

scenario は 14 個（3 の表のものと、worker 8 本、`mvm_mlt_export_sequence`、古い数え方の対照）。

### 4.2 修正の前の実装で落ちること

修正の前（`before/`。新しい試験と診断用の入力の定義だけを足し、待ち loop は元のままの build）で、新しい試験は
**13 か所で落ちた**（`new-test-on-old-implementation.log`）。落ちた理由は、どれも「実時間で `timeout_ms`
（+ 許容）までに timeout しない」か「timeout せずに成功した」である。

### 4.3 以前の反例の保存

- `mlt_export_timeout_accounting_deviation`（以前からある監査の試験。期待は終了コード 3 =「実時間で 1.5 倍より
  遅い」）は、**名前も期待もそのまま**残した。
- この試験が測っていたのは「そのときの実装」だったので、修正の後もそのまま動かすと 0 になり、当時の結果が
  再現しなくなる。そこで、以前の数え方を observer の診断用の入力
  （`diagnostic_legacy_timeout_accounting`）で選べるように残し、この試験はそれを指定して測る。
  製品の書き出しは observer を渡さないので、以前の数え方にはならない。
- 修正の前: 終了コード 3（実時間 3,374 ms）。修正の後（以前の数え方を指定）: 終了コード 3（実時間 3,379 ms）。
- 試験の説明（source と CMake の comment）に、この経緯を書いた。期待を 0 へ書き換えてはいない。
- 新しい試験の中にも、同じ入力を使った対照がある（scenario 14: 古い数え方は許容を超えて遅れる）。

### 4.4 修正の後

`scripts/mlt-export-timeout-p35b-verify.ps1`、`20261013-200730-final/`。

| scenario | `timeout_ms` | 結果 | 待ちの開始から、最後に確認した時点まで（ms、5 回の最小〜最大） |
| --- | --- | --- | --- |
| `slow-consumer` | 1000 | timeout | 1002〜1013 |
| `poll-30ms` | 1000 | timeout | 1041〜1045 |
| `poll-400ms` | 1000 | timeout | 1277〜1287 |
| `slow-consumer-8-workers` | 1000 | timeout | 1008〜1014 |
| `sequence-poll-30ms` | 1000 | timeout | 1023〜1046 |
| `completes-early` | 60000 | 成功 | 402〜896 |
| `near-deadline-measure` | 60000 | 成功 | 2022〜2583 |
| `near-deadline-above` | 測定した時間 ± 600 | 成功 | 2026〜2671 |
| `near-deadline-below` | 測定した時間 ± 600 | timeout | 1427〜1993 |
| `stalled-poll-completed` | 1500 | 成功 | 2905〜3408 |
| `stalled-poll-running` | 1000 | timeout | 2619〜2635 |
| `cancel-before-deadline` | 60000 | 取消 | 301〜315 |
| `cancel-races-timeout` | 1000 | 取消 | 1315〜1325 |
| `consumer-failure` | 1000 | 失敗 | 22〜37 |
| `consumer-failure-stalled` | 1000 | 失敗 | 2504〜2511 |
| `repeat-timeout` | 1000 | timeout | 1004〜1046 |
| `repeat-success` | 60000 | 成功 | 403〜993 |
| `legacy-accounting-control` | 1000 | timeout | 3157〜3400 |

- 5 回とも、全部の検査が成立した（不成立 0）。
- `consumer-failure` は、出力先の directory が無い場合で、consumer が自分で止まる。結果は失敗
  （「encoder 直前の RGBA 検査が失敗または不足しました」）で、timeout ではない。
- `legacy-accounting-control` は、以前の数え方を指定した対照で、秒の単位で遅れる（検査が効いていること）。

## 5. 出力と寿命（Task F）

上位の層を通した書き出し（検証 harness、`cmp-layers-16`。同じ run）:

| 場合 | 結果 | 最終 file | 一時 file | 残った transition・callback・画像 |
| --- | --- | --- | --- | --- |
| 従来の graph、timeout 1000 ms | EXPORT_FAILED（tractor consumerがtimeoutしました） | 無い | 無い | 0・0・0 |
| 数式 group の graph、timeout 300 ms | EXPORT_FAILED（tractor consumerがtimeoutしました） | 無い | 無い | 0・0・0 |
| 数式 group の graph、途中で取消 | CANCELLED（書き出しをキャンセルしました） | 無い | 無い | 0・0・0 |
| 数式 group の graph、期限内（600000 ms） | PASS | あり | 無い | 0・0・0 |

- timeout・取消・失敗では、最終の名前の file はできない。一時 file は、上位の層（`timeline_export.cpp`）が
  消す。
- `mvm_mlt_export_two_track` そのものは、渡された path へ直接書く。timeout のとき、その path に書きかけの
  file が残る（上の新しい試験でも「出力 = 1」と出る）。消すのは呼び出し側で、製品では上位の層が一時 file の
  名前を渡して消している。これは以前からの分担で、変えていない。
- process を外から止めた場合に一時 file が残ること（P3-4）は、そのままである。crash からの回復の後始末は
  足していない。

## 6. 回帰の gate と provenance（Task G）

| gate | 結果 |
| --- | --- |
| `mlt_export_timeout_real_time`（新しい試験）× 5 回 | 5 回とも成立 |
| 以前の数え方の記録（`mlt_export_timeout_accounting_deviation` と同じ測定、`legacy`） | どちらも終了コード 3（期待どおり） |
| 上位の層での timeout・取消・成功 | 4 つとも期待どおり |
| 数式 group の parity（判定の対照 11 通り。`math-group/`） | 全部期待どおり。ELIGIBLE は数式 group の graph で全 frame 一致、UNKNOWN・INELIGIBLE は従来の graph で全 frame 一致、誤った証拠は実行時に拒否 |
| 数式 group の parity（v4 の全体 graph、区間 1200・5280。`math-group-windows/`） | 60/60 frame ずつ一致。97 clip、affine 23、画像 2.81 MB |
| lint | 通過 |
| **release の全 CTest**（`pwsh scripts/test.ps1 -Preset ucrt64-release`） | **1523/1523 通過**（1,116 秒。`ctest-final/`）。登録数は、新しい試験 1 件が加わって 1523 |
| debug の全 CTest | 実行していない |

- 全 CTest の前後に `scripts/gui-environment-preflight.ps1` で環境を確かめた。前: VALID、後: VALID
  （monitor 1、1920×1200、DPI 96、電源 on、描画と提示の probe が成功）。
- `math-group-windows/20261013-202208-windows/` は、私の引数の渡し方の誤りで何も実行されなかった run
  （`NOTE.txt`）。取り直した `…-202239-windows/` を使った。
- 全 CTest の出力の全部（`ctest-full.log`）、`LastTest.log`、binary 158 個・runtime・変更した source の SHA-256
  （`identity-before.json`・`identity-after.json`）を残した。`test.ps1` の build が毎回 link し直す 2 つ
  （`mvm.exe`・`mvm_test_text_ui_input.exe`）以外は、前後で同じである。
- 最終の build の後は、source を書き換えていない（変異の試験は行っていない。修正の前の実装での失敗は、
  修正を入れる前の build で記録した）。

最終の file（SHA-256 の先頭 16 桁。全体は `20261013-200730-final/identity.json`）:

| file | SHA-256 |
| --- | --- |
| `src/media/mlt/mvm_mlt_export.c` | `AC763C0C202CF15B…` |
| `tests/harness/test_mlt_export_timeout.cpp` | `BA535A528E0092BC…` |
| `mvm_test_mlt_export_timeout.exe` | `A3E85972319427B7…` |
| `mvm_test_mlt_export_capacity.exe` | `41727090122339A1…` |
| `mvm_math_score_export_parity.exe` | `8FE0A8FA9DD4994B…` |
| `libmlt-7.dll` | `13D5EFA4536D3225…` |

git の HEAD は `feb6280` で、今回の変更は commit していない（差分は `ctest-final/git-diff.patch`）。

## 7. 製品での有効化までに残るもの

1. 製品から数式 group を使う入口（P3-6）。実行時に拒否されたときに従来の graph でやり直す仕組み。
2. `mvm_mlt_export.c` そのものを race の検出器の下で動かす手段が無い。
3. V1 に動画がある Project は、判定が UNKNOWN になり、従来の経路のまま。timeout が実時間になったので、
   待ちが 600 秒を超える書き出しは、これまでより早く（意図どおりの 600 秒で）timeout する。
4. 同じ形の待ち（`Sleep` の回数で数える）が、音声の render などに残っている（2）。
5. process を外から止めたときの一時 file。
6. 全 CTest の中で負荷によって落ちることのある試験（P3-5A.2 の closure の 6）。UI の SegFault（UNKNOWN）。
