# preview 性能の実測メモ (S7)

対象: MLT 7.36.1 / MSYS2 UCRT64
ビルド: RelWithDebInfo (debug の値は判定に使わない)
計測: `mvm_bench preview-bench`、実行: `scripts/preview-matrix.ps1`

---

## 何を preview と呼ぶか

**[事実]** S6 までの `compose_frame` 連続呼び出しは preview ではない。
あれは「毎回 seek して 1 枚取る」経路であり、
consumer の read-ahead も worker thread も通らない。

S7 では `mlt_consumer` に tractor を接続し、
**consumer 側が連続して frame を要求する経路**だけを preview と呼ぶ。
frame の到着は `consumer-frame-show` イベントで数える。

---

## MLT ソースで確認した事実

以下は推測ではなく MLT 7.36.1 のソースを読んで確認した内容である。

### real_time の意味 (`src/framework/mlt_consumer.c`)

| 値 | 意味 |
| --- | --- |
| `> 0` | 非同期。**フレームドロップあり**。`1` は read-ahead thread 1 本 |
| `< 0` | 非同期。**フレームドロップなし**。`-1` は read-ahead thread 1 本 |
| `= 0` | 同期。**`mlt_frame_get_image` を呼ばない** |

**render thread 数 = `abs(real_time)`。**
`consumer_work_start` に `int n = abs(priv->real_time);` があり、
`abs(real_time) > 1` のときその数だけ `consumer_worker_thread` を作る。

`mlt_consumer_rt_frame` は 3 経路に分かれる。

- `real_time > 1` または `< -1` → `worker_get_frame()` (複数 worker)
- `real_time == ±1` → read-ahead thread 1 本 + queue
- `real_time == 0` → `mlt_consumer_get_frame()` を呼んで
  `rendered` を 1 にするだけ。**画像は作らない**

### real_time=0 は計測に使えない

**[事実]** `real_time = 0` では `mlt_frame_get_image` が呼ばれない。
さらに `null` consumer (`src/modules/core/consumer_null.c`) の
`consumer_thread` も `get_image` を呼ばない。
取得した frame をそのまま `consumer-frame-show` で流して close するだけである。

つまり **`real_time=0` + `null` consumer では何も描画されない。**
それでも frame は「配信」されるので、
何も測らずに高い fps が出てしまう。

`preview-bench` はこの組み合わせを**受け付けない** (exit 3)。
黙って速い値を出すより失敗させる。

### frame drop の取得

**[事実]** `drop_count` は consumer の int property である。
read-ahead / worker thread が描画した frame には `rendered` が 1 で立つ。
描画されなかった frame は `rendered` が立たないまま配信され、
そのとき `drop_count` が増える。

```c
if (!mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "rendered")) {
    int dropped = mlt_properties_get_int(properties, "drop_count");
    mlt_properties_set_int(properties, "drop_count", ++dropped);
```

**この事実が計測に直結する。**
`consumer-frame-show` の回数を fps と呼ぶと、
ドロップした frame まで「画が更新された」と数えてしまう。

実測でもそうなった (下表)。`real_time=1` は
配信 59.6 fps に対し、実際に描画されたのは 13.5 fps しかない。

したがって本ハーネスは 2 つを分けて出す。

- `delivered_fps` = 配信された frame / wall
- **`effective_fps` = 描画された frame (`rendered=1`) / wall** ← M7 の判定に使う

### consumer の start / stop / 完了待ち

**[事実]** `consumer_null.c` は `terminate_on_pause` を見る。
producer が終端に達して speed が 0 になると `terminated` が立ち、
スレッドが抜けて `mlt_consumer_stopped()` が呼ばれる。

完了待ちは `mlt_consumer_is_stopped()` を polling する。
`mlt_consumer_stop()` を呼んでから `is_stopped` が立つまで待つ。

`mlt_consumer_purge` は `priv->queue` に溜まった frame を捨てる。
S7 では使っていない (scrub の purge 比較は未実施のまま)。

### event の所有権 — 実際にヒープを壊した

**[事実]** `mlt_events_listen` の戻り値を `mlt_event_close` してはいけない。

```c
event->ref_count = 0;
mlt_properties_set_data(listeners, temp, event, 0,
                        (mlt_destructor) mlt_event_close, NULL);
mlt_event_inc_ref(event);          /* -> ref_count = 1 */
```

**ref_count 1 は listeners 側の所有分**であり、呼び出し側は参照を持っていない。
ここで `mlt_event_close` を呼ぶと ref_count が 0 になって `free` され、
その後 consumer の properties が閉じるときに destructor が
解放済みポインタをもう一度 close する。

実際に `0xC0000374` (STATUS_HEAP_CORRUPTION) で落ちた。
listener を外したいときは `mlt_events_disconnect(properties, listener_data)` を使う。

**[推測]** MLT のドキュメントコメントは「return an event」としか書いておらず、
所有権については何も書いていない。同じ間違いをする実装は他にもありそうである。

---

## 計測方法

- consumer: `null` (画面表示は含まない)
- `mlt_image_format = rgba` を明示
- warm-up 5 秒を**別の start/stop** で回し、計測には含めない
- 計測は **wall 時間で 60 秒**打ち切り
- 各構成 3 回、**run ごとに別プロセス**
- 集計はスクリプトが生 JSON から再計算する

### wall 時間で打ち切る理由

素材の尺で打ち切ると、遅い構成ほど長時間走ることになり、
構成間で「同じ時間あたり何枚出たか」を比較できなくなる。

### 経過時間の測り方 (実際に踏んだ罠)

**[事実]** `Sleep(5)` は既定のタイマ分解能では約 15ms 眠る。
`Sleep` の回数を足し込んだ値を経過時間として使うと、
「6 秒計測」のつもりが 12.3 秒になる。実際にそうなった。
現在は `QueryPerformanceCounter` の実測値で打ち切っている。

---

## 実測: 1080p60 5 トラック (native)

計測条件は上記のとおり。値は 3 回の中央値。

| real_time | thread | drop | wall(s) | delivered | rendered | dropped | **effective_fps** | delivered_fps | drop_rate | startup(ms) | interval p95(ms) | CPU util | thread peak | priv peak(MB) | M7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | あり | 60.01 | 3574 | 817 | 2761 | **13.61** | 59.55 | 0.7717 | 100.5 | 73.49 | 2.79 | 91 | 534.7 | 不合格 |
| -1 | 1 | なし | 60.01 | 1191 | 1191 | 0 | **19.85** | 19.85 | 0.0 | 97.7 | 72.61 | 2.72 | 91 | 530.7 | 不合格 |
| -4 | 4 | なし | 60.0 | 935 | 935 | 0 | **15.58** | 15.58 | 0.0 | 1392.3 | 125.09 | 3.31 | 94 | 801.6 | 不合格 |
| -8 | 8 | なし | 60.0 | 872 | 872 | 0 | **14.53** | 14.53 | 0.0 | 1177.6 | 145.79 | 3.2 | 98 | 802.7 | 不合格 |
| -16 | 16 | なし | 60.0 | 873 | 873 | 0 | **14.55** | 14.55 | 0.0 | 1270.4 | 161.91 | 3.19 | 106 | 805.8 | 不合格 |

### 何が言えるか

**[事実] M7 は全構成で不合格。** 基準は effective_fps >= 50、drop_rate <= 0.05。
最良でも `real_time=-1` の 19.85 fps で、基準の 4 割である。

**[事実] `real_time=1` の 59.55 fps は「更新された画」の数ではない。**
配信 3574 枚のうち描画されたのは 817 枚だけで、drop_rate は 0.77 である。
`consumer-frame-show` の回数をそのまま fps と呼ぶと、
**60fps 出ているように見えて実際は 13.6fps** という報告になる。
このハーネスは rendered と delivered を分けて出す。

**[事実] render thread を増やしても速くならない。**
19.85 (1 本) -> 15.58 (4 本) -> 14.53 (8 本) -> 14.55 (16 本) と、
むしろ 1 本が最も速い。CPU 使用率は 2.7〜3.3 コア相当で頭打ちしており、
32 論理コアに対して余っている。**スレッド数がボトルネックではない。**

**[事実] worker thread を使うと起動待ちとメモリが増える。**
startup latency 98ms -> 1.2〜1.4 秒、PrivateUsage ピーク 531MB -> 806MB。

**[推測]** 律速は 1 フレームあたりの合成コスト
(1080p の avformat producer 2 本 + 音声 2 本 + 文字描画 + affine 合成 +
1920x1080 RGBA への変換) であり、スレッド分割で並列化できていない。
S6 の seek 実測 (p50 129〜154ms) とも整合する。
ただし **MLT 内部のどこで直列化しているかは特定していない。**

**[未検証]** proxy を使わない 1080p native での改善余地
(producer の共有、`noimagecache`、consumer の `buffer`/`prefill` 調整)。


---

## S12 の位置づけ (S7.1 で明確化)

**[事実] native の M7 は MLT / RGBA 供給層の時点で不合格である。**
最良の `real_time=-1` で 19.85 fps、基準の 40% しかない。
これは Qt を一切含まない、MLT が RGBA を作り終えるまでの値である。

**[事実] S12 は null consumer の上限を改善しない。**
Qt 経路は MLT が作った RGBA を受け取る側であり、
**供給側より速くなることはない。** 足し算しかされない。

したがって:

- **S12 を「M7 を確認するため」だけに実施しない。** 結果は既に決まっている
- S12 に意味があるのは、**別 GPU 経路 / zero-copy へ変える**場合だけである。
  `mlt_frame_get_image` の RGBA コピーを経ずに
  GPU テクスチャへ直接書く経路を作れるなら供給層の上限自体が変わる。
  ただしそれは MLT の外側を作り直すことであり、Phase 0 の範囲ではない

**[未検証] Qt 転送を含む end-to-end の値は測っていない。**
以下は S12 でしか得られないが、**現時点の判定を変える見込みは無い。**

- `mlt_frame_get_image` で得た RGBA を `QSGTexture` へ上げるコスト
- UI スレッドのフレーム時間 (M14)
- 音声クロックへの追従、画面の垂直同期

null consumer の値は「MLT が供給できる上限」であって、
「mvm が画面に出せる fps」ではない。**上限が基準未満である。**

---

## S7.1: all-video proxy でも上限は変わらない

**[事実]** preview で使う video source を全て proxy 化しても
**19.67 fps** であり、1080p native (19.85 fps) をわずかに下回る。

| 構成 | effective_fps |
| --- | --- |
| 1080p native (5 トラック) | 19.85 |
| 4K original | 10.00 |
| V1-only proxy (partial) | 14.88 |
| **all-video proxy** | **19.67** |

**素材の解像度を下げても 20 fps 付近で頭打ちになる。**
律速は解像度ではなく、5 トラック合成と 1920x1080 RGBA 生成そのものである。

ablation では qtext 除去で +12.12 fps、V2/affine 除去で +14.31 fps 得られるが、
**両方を捨てても 30〜36 fps** で基準に届かない
(`docs/research/proxy-notes.md` の ablation 節)。
