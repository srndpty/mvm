# Phase 1 / P1・P1.1・P1.2 の所見

記述は Phase 0 と同じ規則で分類する。混ぜない。

| 印 | 意味 |
| --- | --- |
| `[事実]` | 実際に実行して観測した。再現手順を併記する |
| `[推測]` | 観測から導いた説明。ソースを読んで確かめてはいない |
| `[未検証]` | まだ測っていない。できると仮定してはいけない |
| `[回避策]` | 現在の対処。恒久策とは限らない |
| `[exit]` | exit criteria への影響 |

**数値の出典は `build/ucrt64-release/p1-matrix/summary.json` である。**
このファイルの数値は `scripts/p1-matrix.ps1` が生 JSON から再計算したものであり、
本文へは要点だけを引く。判定は summary.json だけで行う。

再現:

```powershell
pwsh scripts/build.ps1
pwsh scripts/p1-matrix.ps1
```

---

## 1. Qt と FFmpeg の D3D11 device 共有

**[事実] Qt Quick の `ID3D11Device` をそのまま FFmpeg の decode device にできる。**

`QRhi::nativeHandles()` を render thread 上で `QRhiD3D11NativeHandles` へ落とし、
`dev` / `context` を `AVD3D11VADeviceContext::device` / `device_context` に渡した。
9 run すべてで `same_device == true`（device ポインタが完全一致）。

観測した device ポインタは Qt 側と FFmpeg 側で同一の値であり、
FFmpeg 側の値は **decode 結果の texture から `GetDevice` で遡って**取っている。
設定値の照合ではない。

**[事実] `ID3D10Multithread::SetMultithreadProtected(TRUE)` は必須である。**

decode thread と Qt render thread が同じ immediate context を共有する。
加えて `AVD3D11VADeviceContext::lock` / `unlock` に自前の
`std::recursive_mutex` を渡し、renderer 側も同じ mutex を取る。
二重に直列化している。片方だけで足りるかは **[未検証]**。
外して壊れることを確かめていないので、「必要だから入れた」ではなく
「安全側に倒して両方入れた」が正確である。

**[事実] `AVD3D11VADeviceContext::BindFlags` に
`D3D11_BIND_SHADER_RESOURCE` を入れないと表示できない。**

入れないと decode 出力 texture から shader resource view を作れず、
CPU へ落とす以外の手段が無くなる。
`mvm_test_gpu_decode no-shader-bind` がこの検査が効いていることを確かめている
（BindFlags 無しの texture を渡すと変換パスが失敗し、CPU readback へ退避しない）。

## 2. zero-copy の範囲

**[事実] decode 出力の画素は CPU へ 1 度も降りない。**

9 run すべてで `cpu_full_frame_readback_count == 0`。

経路は次のとおり。

```
NV12 texture array (FFmpeg decode pool, BIND_DECODER|BIND_SHADER_RESOURCE)
  -> Texture2DArray SRV (Y=R8_UNORM / UV=R8G8_UNORM, FirstArraySlice=index)
  -> pixel shader で YUV->RGB
  -> QQuickRhiItem の color texture (RGBA8) へ直接描画
```

**[事実] これは「zero-copy」だが「pass 0」ではない。**
NV12 のまま Qt Quick の scene graph に載せる手段が無いので、
**GPU 上で 1 pass の変換を必ず通る**。60 秒あたり約 4,700〜4,900 回の
`gpu_copy_count` はこの変換 pass の回数である（表示 3,600 + marker 7 + warm-up 分）。

CPU 転送は 0、GPU pass は 1。この 2 つを同じ言葉で報告しない。

**[事実] marker 検証だけは 1216x64 の帯を CPU へ読む。**
1080p 全画素の 3.7%、4K の 0.9%。`marker_band_readback_count` として
full-frame とは別に数えており、判定には使わない。
計測区間中は marker 検証を止めているので、fps の測定には混ざらない。

## 3. seek の着地（P1 で最も高くついた所見）

**[事実] `AVSEEK_FLAG_BACKWARD` は「目標以前のキーフレームへ飛ぶ」ことを保証しない。**

1080p60 HEVC（benchmark, 60 秒 / 3600 frame）で frame 299 を要求すると
**300 に着地した**。1000 点のランダム seek のうち **65 点 (6.5%)** が
同じ形で 1 frame 行き過ぎた。marker 代表点では 299 と 1799 が外れた。

**同じ time_base (1/15360)・同じ fps (60/1) の H.264 では 1 件も起きない。**
素材の生成条件も尺も同じで、違うのは codec と GOP / B frame 構造だけである。

**[推測]** mp4 の index は DTS で並んでおり、B frame があると PTS != DTS になる。
backward seek が選んだキーフレームの **表示時刻**が目標より後になりうる。
ソースを読んで確かめてはいない。

**[回避策] 着地を完全一致のみ成功とし、行き過ぎたら手前へ戻して decode し直す。**

戻し幅は 1 秒相当から 4 倍ずつ広げ、先頭まで戻っても届かなければ失敗として報告する。
戻した回数は `seek_backoff_count` として JSON に出す（隠さない）。
HEVC では 1000 点あたり 67 回この経路を通っている。

**[exit] 当初の実装は `f.frameNumber >= target` を成功としていた。**
この条件では 1 frame ずれが成功として飲み込まれ、marker 検査も通ってしまう。
**編集点が 1 frame ずれるという、最も気づきにくい形の不具合になる。**
`gpu_seek_exact_landing_hevc` / `gpu_marker_exact_landing_hevc` を
回帰テストとして CTest に入れた（60 秒素材があるときのみ登録。
5 秒の smoke 素材では再現しない）。

## 4. 表示レートの上限

**[事実] `effective_fps` は swapchain の present に律速される。**

判定ホストのリフレッシュレートは 59.95 Hz で、
9 run すべてで `present_rate_hz` も `effective_fps` も 59.87〜59.96 に収まった。

したがって `effective_fps >= 55` は
**「vsync のほぼ毎回に新しいフレームを間に合わせた」**の意味であり、
**この経路の最大スループットではない。**

**[未検証] 余力（headroom）は測っていない。** vsync を外した最大 fps、
および複数動画を同時に decode したときの限界は P2 以降の課題である。
4K60 H.264 も 59.87 fps 出ているが、これも上限に張り付いているだけで、
「4K に余裕がある」と読んではいけない。

## 5. 4K60 H.264（診断のみ・判定に使わない）

**[事実] 表示は 1080p と同じく上限に張り付く。marker も 21/21 一致。**

**[事実] seek だけが明確に遅い。** seek p95 は 1080p の 3〜4 倍。
P1 の判定対象ではないので閾値判定はしていないが、
**4K を原寸で編集する構成は seek 応答の面で成立しない**ことを示している。
Phase 0 の結論（4K は proxy 前提）と矛盾しない。

## 6. 色空間

**[事実] 同じ内容の素材でも、encoder によって metadata が違う。**

`v1080p60_h264` は `bt709`、`v1080p60_hevc` は `bt601` と申告する。
どちらも 1920x1080 であり、内容は同じスクリプトで生成している。

P1 は metadata をそのまま信じ、未指定のときだけ解像度から推定して
`color_space_inferred` を立てる。**推定を黙って確定値にしない。**

**P1 の時点では [未検証] だった。P1.1 で測った。**
既知 YUV patch の fixture (BT.709 limited / full、BT.601 limited、
診断で BT.2020 NCL と P010) を作り、表示と同じ shader で RGB 化して照合した。
結果は §8.2 / §8.3 のとおりで、**実装にも生成側にも欠陥が見つかった**。
修正後は 5 fixture すべて最大差 0〜1 (許容 3)。

marker 一致を色の証拠に使ってはいけない、という判断は正しかった。
marker は白 235 / 黒 16 なので、1 LSB の色かぶりでは絶対に落ちない。

## 7. 10bit / P010

**[事実] 4K HEVC 10bit も同じ経路で表示でき、marker も読める。**
`gpu_marker_4k_hevc10_diagnostic` が確認している。
SRV format を R16_UNORM / R16G16_UNORM に切り替え、
10bit を 16bit の上位へ詰める分（65535/65472）を shader で補正している。

P1.1 で P010 の color patch も測った (診断)。最大差 1。
ただし **[未検証] バンディングや階調の評価はしていない**。判定対象は 8bit である。

## 8. P1.1 で見つけた欠陥

いずれも **P1 の計測では緑のまま通っていた**。
「動いている」ことと「正しい」ことは別である。

### 8.1 [事実] resource epoch が decoder インスタンスごとだった

SRV cache の key に `resource_epoch` を入れたが、その epoch を
**decoder インスタンスのメンバ**として 0 から数えていた。
新しい decoder はどれも epoch 1 になるので、key が衝突する。

open/close soak (100 cycle) で発覚した。
旧 epoch と判定されず 1 件も retire されず、
**SRV cache が 100 cycle で 4 -> 400 entry まで増え続けた**
(`retired_srv_entries=0` / `active_decoder_pools=100`)。

texture のアドレスは pool 解放後に再利用されるので、衝突した状態では
**前の pool 用の SRV を新しい pool のフレームに使う**危険もあった。

`[回避策]` ではなく修正: epoch をプロセス全体で単調増加させる。
修正後は `srv_cache 4 (plateau) -> 4 (last)` / `retired_srv_entries=396` /
`active_decoder_pools=1`。

**soak を書かなければ気づけなかった。** 1 回の open/close では再現しない。

### 8.2 [事実] shader の chroma 中立点が 0.5 だった

8bit の chroma 中立点は `128/255 = 0.50196` であり 0.5 ではない
(10bit は `512/1023 = 0.50049`)。
0.5 を使うと全画素に約 1 LSB の色かぶりが出る。

color patch 検査を書いて初めて分かった。
**marker 一致では絶対に検出できない** (marker は白 235 / 黒 16 なので
1 LSB ずれても読める)。修正後、5 fixture すべてで最大差 0〜1。

### 8.3 [事実] color fixture の生成側にも欠陥があった

最初の fixture は BT.601 だけ一致し、BT.709 / BT.2020 が 10〜25 ずれた。
原因は実装ではなく **生成側**だった。

ffmpeg は raw video の入力を「BT.601」と仮定するので、
出力に `-colorspace bt709` を指定すると **colorspace 変換を自動挿入する**。
その結果、書き込んだはずの YUV とは別の値が焼かれていた
(V=240 と書いて 229 で復元された)。

BT.601 fixture だけ一致していたのは、仮定と一致していたからにすぎない。
**「1 つ通っているから検査は正しい」と考えてはいけなかった。**

`[回避策]` ではなく修正: `setparams` フィルタで入力フレームに色情報を付け、
変換が挿入されないようにする。

### 8.4 [事実] GUI が decoder 内部を無排他で読んでいた

P1 の `info()` / `decodeAdapter()` / `lastError()` は、
decode thread が書き換えている実体への **const 参照**を返していた。
`std::string` の書き換え中に読めば壊れた文字列が見える。

観測はできていない (Windows / MSYS2 で TSAN が使えない)。
**「観測していない」ことを「起きていない」と書かない。**
契約の側を直した: GUI へ返すのは `DecoderSnapshot` の値コピーだけにし、
`decoder_` には `decoderMutex_` 無しで触らない。

決定論的な thread test (`snapshot-race`) を追加した。
別スレッドから snapshot を叩き、codec 名 / 解像度 / decode 数の整合を検査する。
実測 45,261,726 回で違反 0 件。
**これは「race が無い証明」ではなく「この形の破損は出ていない」という観測である。**

## 9. P1.1 の soak が示したこと

**[事実]** H.264 / HEVC を交互に 100 cycle 開閉して:

- marker mismatch 0 / device lost 0
- `cpu_full_frame_readback = 0`
- SRV cache は 4 entry で頭打ち、retire 396 件、active pool 1
- retirement は timeout 0 で drain でき、未完了の解放 0 件

**[事実] handle 数は単調増加ではない。**
first 1259 / mid 3188 / last 1987。中盤で増えて終盤で減っている。

**[推測]** driver 側の cache か遅延解放だと思われるが、
**機構は特定していない**。PrivateUsage も
first 122MB / mid 150MB / peak 155MB / last 147MB で、
増えたまま戻らない分がある。

**[未検証]** これがリークなのか定常状態なのかは、
100 cycle では判断できない。数千 cycle か長時間再生で測る必要がある。
**「問題ない」とは書かない。生データを残す。**

## 10. P1.2 で直した設計上の穴

P1.1 の実装は **測ると緑になる**が、正しさの根拠が足りていなかった。
なぜ気づけなかったかも含めて残す。

### 10.1 [事実] seek の表示待ちに race があった

P1.1 の `seekAndWaitForDisplay` は

```
waiting = false
seekBlocking()      <- ここで frame が queue に入る
waiting = true      <- arm
```

の順だった。**submit 後・arm 前に render thread が描くと、その display は
誰にも記録されない。** 待機側は次の display を待つが、静止画では来ないので
timeout する。seek 直後は「1 枚だけ入ってすぐ描かれる」状況なので、
この窓に入る確率は低くない。

**計測では露見していなかった。** 9 run x 1000 点で
`seek_display_mismatch` は 0 だった。「測って緑だった」は
「race が無い」ではない。

修正: render thread が **待機の有無に関わらず全 display を記録**し、
待機側は seek より前に取った baseline より後の記録を探す (`DisplayLedger`)。
arm のタイミングに依存しない。

決定論的テストを 5 件足した。中心は「arm 前に描かれた display を拾えること」で、
**timeout 0 で成功すること**を要求する (sleep に依存しない)。

### 10.2 [事実] decoder が composition epoch を発行していた

P1.1 の所見に「compositionEpoch と resourceEpoch を分離済み」と書いたが
**誤りだった。** 実際には `GenerationId{compositionEpoch, sourceGeneration}` の
compositionEpoch に decoder の resource epoch を入れていた。
つまり decoder が合成構成の世代を発行していた。

1 本の decoder では困らない。P2 で source が 2 本になると
**source A の open が source B のフレームを future / stale にする。**

修正: 3 つに分け、発行者を固定した。

| 概念 | 発行者 |
| --- | --- |
| `ResourceEpoch` | decoder (open ごと) |
| `SourceGeneration` | decoder (seek / flush ごと) |
| `CompositionEpoch` | **compositor** (P1.2 では preview 層が暫定所有) |

3 つとも別の型にした。P1.1 の取り違えは、同じ `unsigned long long` だったから
コンパイルが通ってしまったのが原因である。

queue の stale / future 判定も source 単位にした。
「source A の seek が source B の generation を変えない」ことを単体テストで検査する。

### 10.3 [事実] device 変更時に decode thread の足元で device を Release しえた

P1.1 は render thread が device 変化を検出したその場で
`converter.release()` / `completion.release()` / `device.release()` まで行っていた。
**このとき decode thread はまだ動いている。**
`queue.stop()` は submit を止めるだけで、decode 自体は止まらない。

修正: 検出 -> GUI が `DecodeWorker::stop()` (join まで) -> その後に teardown、
という順序を `DeviceChangeCoordinator` で強制した。
`mayTeardown()` は join 済みのときしか true を返さない。

**[未検証] 実際に device が差し替わる状況は再現していない。**
順序の検査は状態機械の単体テストで行っている。
また **P1.2 は復帰を実装していない**。検出したら fail-closed で停止する。
`device_change_handled_count` は「完全な復帰が成立した回数」なので常に 0 である。

### 10.4 [事実] GPU completion の API 戻り値を検査していなかった

`ID3D11DeviceContext4::Signal` の HRESULT、`CreateQuery` の失敗、
`GetData` の `S_FALSE` と `FAILED` の区別を、P1.1 は見ていなかった。
失敗しても serial を進めていたので、**追跡できない serial で retire** しうる。
それは「GPU 完了を待った」ことにならない。

修正: `SubmissionResult` / `CompletionPollResult` を返し、
追跡できない submission では retire しない
(決して完了しない serial で保持し、shutdown の drain で必ず表面化させる)。

**[事実] event query backend を実機で走らせた。**
`--gpu-completion event_query` / `soak ... event_query` で強制できるようにし、
12 cycle の soak が通った。

**[事実] その過程で drain の timeout を踏んだ。**
event query は `DONOTFLUSH` で poll しているので、以降 GPU へ何も投入されないと
最後の query が永久に未完了になる。payload が 3 件残り、drain が timeout した。

修正: **shutdown の直前に 1 度だけ** flush する。
これは「毎 frame Flush して見かけ上解決する」こととは別である
(通常の render 経路からは呼ばない)。

**[未検証] fallback が「必要になる状況」は再現していない。**
fence 非対応の機体を持っていないので、切り替え条件そのものは検証していない。
走らせたのは経路であって、切り替えの判断ではない。

### 10.5 [事実] color fixture が無いと color test が黙って未登録になっていた

P1.1 の CMake は `if(EXISTS manifest)` で囲っており、
**clean checkout では color test が 1 件も登録されなかった。**
fixture は `.gitignore` の `/tests/assets/` に飲まれていて追跡されていない。
その状態で CTest は「全部通った」と報告する。

Phase 0 の「対象 0 件のテスト群が全部通ったと報告される」罠と同じ形である。

修正: fixture (全部で 40KB) を追跡対象にし、
無ければ **configure を失敗させ**、登録件数が 5 件であることを検査する。
実際に fixture を隠して configure が落ちることを確認した。

### 10.6 名前と実態のずれ

- `frames_released_before_completion` -> `payloads_released_before_completion`
  (queue に入るのは frame の lifetime token だけではない。SRV holder も入る)
- `active_decoder_pools` -> `srv_cache_texture_groups`
  (実際は cache 内の (epoch, texture) の異なる組み合わせ数であって、
   open している decoder の数ではない)
- `extra_hw_frames = 8` の由来コメントを更新
  (retain 深さではない。保持期間を決めるのは GPU の完了 serial である)

## 11. 測っていないこと

「動くはず」を「動く」と書かないために、明示しておく。

- **NVIDIA RTX 4090 の 1 台でしか測っていない。** Intel / AMD の内蔵 GPU、
  複数 GPU 環境、ノート PC の切り替え可能 GPU は範囲外。
  **「Windows で動く」と一般化しない**
- device lost / device 変更からの**復帰**。P1.2 は検出して fail-closed で止めるだけで、
  新しい device で開き直す実装を持たない（9 run とも発生 0）
- device が実際に差し替わる状況の再現。順序の検査は状態機械の単体テストのみ
- fence 非対応機での event query fallback の**切り替え条件**
  （経路は強制オプションで実機実行したが、切り替えの判断は未検証）
- 長時間再生（数十分〜数時間）でのリーク。60 秒 x 3 run しか測っていない
- 音声、A/V 同期、複数トラック、export
- Qt の patch release をまたいだときの QRhi 互換
- vsync を外した最大スループット（§4）
- 表示色の正しさ（§6）

## 12. P2-D4A Playback formal の source coverage 修正

### 12.1 [事実] P2-D2 の Playback 測定区間は source frame 0 から始まっていなかった

P2-D2 formal の raw / summary は `formal_contract_version = P2-D1-1` のまま保存し、
`p2_pass = false` という当時の判定を変更していない。P2-D4A ではこの成果物を
上書きせず、修正後の smoke を `build/ucrt64-release/p2-d4a-smoke/` へ分離した。

旧 harness は warmup で進んだ 60 Hz scheduler と source buffer を、そのまま
measurement へ引き継いでいた。このため「60 秒の Playback formal」が source の
frame 0..3599 を測る契約になっておらず、source 末尾への到達と EOF を性能問題から
区別できなかった。

修正後の formal contract は `P2-D4-1` とする。変更した意味は次のとおり。

- warmup 終了時に render thread から scheduler 停止の ACK を返し、その後にだけ
  A/B を pause、frame 0 へ exact seek、source generation 同期する
- measurement 開始 callback で QPC 起点と scheduler frame 0 を同時に設定する
- 測定区間を `[start, end)` とし、60 秒 formal の slot を frame 0..3599 の
  3600 件に固定する。frame 3600 は含めない
- source frame count、coverage、measurement 区間の missing pair / EOF A / EOF B を
  raw JSON に記録し、formal checker は fail-closed で検査する

### 12.2 [事実] checker の negative test で新しい契約を固定した

対照群に加え、scheduled 3601 / 3599、coverage false、source A 3599 frame、
missing pair 1、EOF A 1、EOF B 1 をそれぞれ拒否する検査を追加した。
Release / Debug とも、P2 checker を含む限定回帰は 24/24 通過した。
既存の fps 55 以上、drop rate 2% 以下、seek p95 / observed max の閾値は変更していない。

### 12.3 [事実] 5 秒 warmup + 15 秒 measurement の単一 smoke

RTX 4090、H.264 / HEVC 各 3600 frame、seed 20260808、fence backend で 1 process だけ
実行した。formal 3 run x 60 秒は P2-D4A では実行していない。

- required frame 900、source coverage true
- first measurement output frame 0
- scheduled 900、displayed 897、deadline drop 3
- measurement missing pair 0、EOF A 0、EOF B 0
- QPC elapsed 15.0125577 秒
- effective fps 59.7499785、drop rate 0.0033333（いずれも smoke の診断値）

### 12.4 [exit] D4B へ進めるが、P2 の最終判定はまだ更新しない

P2-D4A の source coverage 修正と短縮経路確認は成立したため、D4B の parallel
dual seek 実装・短縮検証へ進める。formal Playback / Seek 全6 runの再実行はD5で
clean HEADから行う。P2-D4A の smoke 値を P2 の合否根拠には使わない。
D5で `P2-D4-1` の rawから集計するまでは、P2-D2 の `p2_pass = false` が最後の
正式判定である。

## 13. P2-D4B parallel dual-source exact seek

### 13.1 [事実] A/B seekの直列待機をsource-local workerへ分離した

`SourceDecodeWorker` に request / wait を分けた非同期seek commandを追加した。
sourceごとにoutstandingは最大1件で、2件目は `RejectedBusy`、古いticketは
`StaleTicket`、stop中は `Stopped` completionとしてfail-closedにする。
seek本体は既存worker threadで実行し、seekごとのthread生成は行わない。

decoder seek、generation更新、exact target decode、source-local buffer submitは
共通executorに一本化した。`seekBlocking` はstartupとD4A resetの同期挙動を保つため
caller同期の共通executor呼び出しとし、async outstanding中は拒否する。

controllerはA/Bへ先にrequestをdispatchし、tickで両completionをpollする。
A/B両方のrequestId、target、decoded frame、generation、resource epochを検証するまで
composition generationと`requestedOutput`を更新しない。

### 13.2 [事実] 64点exact integration

H.264 A / HEVC Bのdeterministic 64点で、A exact 64/64、B exact 64/64、
pair exact 64/64、実行区間overlap 64/64を観測した。mismatch、timeout、
stale completion acceptance、busy acceptance、generation cross-impact、software fallback、
CPU full-frame readback、worker join leakはいずれも0。A/B textureは同一D3D11 deviceだった。

### 13.3 [事実] D3比較用256 seek x 3

seed 20260808、fence backend、3 independent processを
`build/ucrt64-release/p2-seek-profile-d4b/`へ出力した。旧D3 rawは上書きしていない。

| run | A request-ready p95 | B request-ready p95 | dual ready p95 (D3 -> D4B) | request-display p95 (D3 -> D4B) | overlap |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 76.78 ms | 74.45 ms | 105.28 -> 78.02 ms | 114.62 -> 83.41 ms | 256/256 |
| 2 | 75.94 ms | 74.00 ms | 108.81 -> 77.75 ms | 120.65 -> 83.36 ms | 256/256 |
| 3 | 75.39 ms | 71.88 ms | 107.85 -> 76.29 ms | 116.28 -> 83.39 ms | 256/256 |

dual ready p95は25.9% / 28.5% / 29.3%、request-display p95は
27.2% / 30.9% / 28.3%短縮した。全runでmismatch、timeout、stale、busy acceptance、
software fallback、CPU full-frame readback、device lost、join leakは0だった。

**[事実]** parallel化後のdecoder D3D11 lock wait p95はA 1.127..1.130 ms、
B 1.122..1.130 msへ増えた。maxはA 2.90..3.46 ms、B 2.87..3.29 ms。
render lock wait p95は全run 0.1 us、max 11.0..45.8 usだった。
lockを外す変更は行っていない。

### 13.4 [事実] D4A Playback回帰は安定してMUSTを満たさなかった

5秒warmup + 15秒measurementを複数回確認した。全runでfirst output 0、scheduled 900、
EOF A/B 0、coverage true、CPU readback 0、device lost 0だったが、
measurement開始直後の`WaitingForSource`によりmissing pairが1件になるrunがあった。
観測4 run中、missing pair 0は1 run、missing pair 1は3 runだった。

async wrapperが原因かを切り分けるため`seekBlocking`をcaller同期の共通executorへ戻しても
再現した。したがってparallel seekの性能値は改善したが、D4B exit criteriaの
「Playback D4A regression成功」は安定して成立していない。

### 13.5 [未検証] D5 formalへは進まない

Release / Debugの対象30 testは各test単体では成功を観測したが、まとめ実行では
`p2_dual_decode_integration`がまれに30秒completion timeoutとなるrunも観測した。
256 x 3 diagnosticではtimeout 0だが、この不安定性を無視して「deadlock 0」とは書かない。

Playback missing pairとintegration timeoutの再現条件を解消するまで、D5のclean HEAD
formal全6 runへ進めない。P2-D2の`p2_pass = false`を引き続き最後の正式判定とする。

## 14. P2-D4C playback pre-roll / parallel seek reliability closure

### 14.1 [事実] 測定開始前に固定8 frameのpre-rollを追加した

Playbackの測定開始を `MeasurementResetStart`、`MeasurementResetWait`、
`MeasurementPrimeStart`、`MeasurementPrimeWait`、`MeasureStartWait`、`Measure` に分けた。
reset後はschedulerを停止したままA/B workerだけを再生し、buffer先頭が現generationの
frame 0、かつA/B両方のdepthが8以上になるまでconsumer popを開始しない。watermarkは8、
buffer capacityは16、timeoutは2000 msで固定し、測定値による調整はしていない。

formal contractを`P2-D4-2`へ更新し、設定watermark、pre-roll成立、A/B depth、A/B frontを
raw JSONとcheckerへ追加した。depth 7、front 1、成立false、設定値7を個別に拒否する
negative testと、generation / EOF / fatal / timeout / scheduler開始前 / 非破壊判定の
pure testを追加した。

修正後の1秒warmup + 2秒measurementを20 independent processで実行した結果は
`build/ucrt64-release/p2-d4c-reliability/playbackstartup-ucrt64-release-summary.json`にある。
20/20でfirst output 0、pre-roll depth A/Bは各8..16、front A/Bは0、missing pair、EOF A/B、
non-deadline drop、device lost、worker join leakはいずれも0だった。

### 14.2 [事実] 5秒 + 15秒 Playback回帰は3/3でmissing pair 0になった

修正後のrawと機械集計は
`build/ucrt64-release/p2-d4c-reliability/playbackregression-ucrt64-release-summary.json`
に保存した。

| run | pre-roll depth A/B | first | scheduled | displayed | deadline drop | missing / EOF A / EOF B | effective fps | drop rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 16 / 16 | 0 | 900 | 891 | 9 | 0 / 0 / 0 | 59.283 | 1.000% |
| 2 | 16 / 16 | 0 | 900 | 888 | 12 | 0 / 0 / 0 | 59.154 | 1.333% |
| 3 | 16 / 16 | 0 | 900 | 898 | 2 | 0 / 0 / 0 | 59.817 | 0.222% |

全dropはscheduler deadline分類であり、missing source、generation、composition epoch、
render failureによるdropは全runで0だった。この3 runは短縮回帰であり、formal判定値ではない。

### 14.3 [事実] completion publishをfail-closed化し、timeout時の状態を可視化した

`SourceSeekMailbox::publish`はsilent returnを廃止し、`Published`、outstandingなし、
二重公開、request不一致、stop completionによる置換を別の結果として返す。通常実行中の
非`Published`はfatalにし、publish rejectとrequest mismatchをraw JSONへ記録する。
requestがinvalid / busy / stoppedで拒否された場合はplayback stateを変更しない。

seek executorは `Idle`、`Queued`、`WaitingDecoderMutex`、`DecoderSeek`、
`GenerationReset`、`RequestExactFrame`、`SubmitExactFrame`、`PublishingCompletion`、
`Completed` を記録する。snapshotはdecoder mutexを待たず、request id、target、phase開始QPC、
最終進捗QPC、mailbox pending / outstanding / completion ready / current ticketを取得する。
統合testのA/B待機は逐次30秒 + 30秒ではなく、共通30秒deadlineを1 ms間隔でpollする。

### 14.4 [事実] 診断によりnotify取りこぼしを再現・修正した

最初のDebug 20 process soakでは17/20が成功し、3件が30秒timeoutになった。失敗時snapshotは
いずれも片sourceが`phase=queued`、`mailbox_pending=1`、`outstanding=1`、publish reject 0、
request mismatch 0で、反対sourceは`Completed`だった。raw logとsummaryは
`build/ucrt64-debug/p2-d4c-reliability/pre-fix-lost-wakeup/`へ保存した。

workerは`commandMutex`を使ってcondition variableのwaitへ入る一方、request側は同じmutexを
取らずにmailbox pendingを更新してnotifyしていた。predicate確認からwait開始までの間にnotifyが
入ると、pendingが残ったまま次の通知を待つ。request / play / pause / step / stopのpredicate更新を
`commandMutex`でwait遷移と直列化した。

修正後はRelease 20/20、Debug 20/20のindependent processが成功した。各processは64点exact
parallel seekで、retryや失敗runの取消しはしていない。summaryは各buildの
`p2-d4c-reliability/seekintegration-<preset>-summary.json`にある。

### 14.5 [事実] 関連batchと256 x 3 profileが安定して通過した

修正後の関連batchはRelease / Debugとも38 test x 5回、各190/190通過した。
`RESOURCE_LOCK mvm_gpu`、`RUN_SERIAL`は維持している。全通常回帰もRelease / Debug各168/168、
最終同期修正後の静的検査と関連batchも通過した。PSScriptAnalyzerは未導入のためlint scriptが
明示的にskipし、clang-format、層隔離、GPU禁止事項、producer service検査は通過した。

seed 20260808、fence backendの256 seek x 3を
`build/ucrt64-release/p2-seek-profile-d4c/`へ保存した。旧D3 / D4B rawは上書きしていない。

| run | A request-ready p95 | B request-ready p95 | dual ready p95 | request-display p95 | overlap | publish reject / mismatch |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 73.26 ms | 77.52 ms | 78.11 ms | 86.91 ms | 256/256 | 0 / 0 |
| 2 | 76.44 ms | 75.71 ms | 80.09 ms | 93.85 ms | 256/256 | 0 / 0 |
| 3 | 73.80 ms | 73.33 ms | 77.75 ms | 90.00 ms | 256/256 | 0 / 0 |

全runでdisplay mismatch、timeout、stale completion、busy acceptance、software fallback、
CPU full-frame readback、device lost、join leakは0だった。decoder D3D11 lock wait p95は
A 1.141..1.150 ms、B 1.140..1.150 ms、render lock wait p95は全run 0.1 usだった。
D3D11 lockとparallel architectureは変更していない。

### 14.6 [exit] D5 readinessは成立したが、formalはまだ実行していない

Playback startup 20/20、5+15秒回帰3/3、Release / Debug seek integration各20/20、
関連batch各5/5、256 x 3の全correctness counter 0、request-display p95 150 ms以下を満たした。
したがってP2-D4Cの範囲ではD5へ進む条件は成立した。

この節ではP2/P1 formalを実行していない。P2-D2の`build/ucrt64-release/p2-matrix/`は
timestamp 18:15..18:22、contract `P2-D1-1`、`p2_pass = false`のまま変更していない。
D5のclean HEAD formal全6 runを実行するまでは、P2-D2のfalseが最後の正式判定である。

## 15. P2-D5 formal再実行と最終判定

### 15.1 [事実] formal対象をclean HEADでfreezeした

formal対象HEADは`cb57253e9c932623eab9822dcae963d60c5b05ae`である。
開始時の`git status --porcelain`は空で、matrix summaryの`dirty_worktree`はfalse、
`provenance_unchanged_during_matrix`はtrueだった。

matrix前のfull CTestはRelease / Debugとも186/186通過した。performance 11件、stability 1件を
含む全登録testを`-j 4`で実行し、`RESOURCE_LOCK`と`RUN_SERIAL`は変更していない。

### 15.2 [事実] Playback formalは3/3通過した

P2-D4-2、seed 20260808、fence、5秒warmup、固定8 frame pre-roll、60秒measurementを
3 independent processで実行した。rawは
`build/ucrt64-release/p2-matrix-d4/playback-run1.json`から`playback-run3.json`に保存した。

| run | effective fps | drop rate | pre-roll A/B | displayed / scheduled | missing | EOF A/B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 59.2506 | 1.2222% | 16 / 16 | 3556 / 3600 | 0 | 0 / 0 |
| 2 | 58.8507 | 1.8889% | 13 / 16 | 3532 / 3600 | 0 | 0 / 0 |
| 3 | 59.0842 | 1.5000% | 16 / 16 | 3546 / 3600 | 0 | 0 / 0 |

全runでfront A/B 0、first output 0だった。dropは44 / 68 / 54件で、全件scheduler
deadline分類だった。non-deadline drop、marker/probe mismatch、mixed frame/generation、
stale composition epoch、CPU readback、full-frame GPU copyは0だった。submissionはdisplayed、
layer drawはdisplayedの2倍、logical clearはdisplayedと一致した。

### 15.3 [事実] Seek formalはparallel provenance MUSTで2/3失敗した

1000 deterministic seekを3 independent processで実行した。rawは
`build/ucrt64-release/p2-matrix-d4/seek-run1.json`から`seek-run3.json`に保存した。

| run | p95 | observed max | overlap | false sample index | contract |
| --- | ---: | ---: | ---: | --- | --- |
| 1 | 125.9236 ms | 219.0096 ms | 999 / 1000 | 971 | FAIL |
| 2 | 125.5704 ms | 235.5991 ms | 1000 / 1000 | なし | PASS |
| 3 | 125.7466 ms | 208.7424 ms | 997 / 1000 | 103, 127, 294 | FAIL |

全runで1000 latency値を記録し、nearest-rank p95は150 ms以下、observed maxは400 ms以下だった。
3000 seekのglobal observed maxは235.5991 msである。display mismatch、timeout、stale、busy、
publish reject、request mismatch、stopped superseded、software fallbackは全runで0だった。

失敗MUSTは`seek_overlap_count == 1000`と全concurrency sampleの`overlap == true`だけである。
run1は1 sample、run3は3 sampleでA/B execution intervalが重ならなかった。平均による救済、
再試行、sample除外、seed/threshold変更は行っていない。

### 15.4 [事実] lifecycle / teardownとprovenanceは成立した

6 run合計でmarker mismatch、actual target probe mismatch、mixed frame、mixed generation、
stale epoch、CPU full-frame readback、full-frame GPU copy、untracked submission、completion failure、
early release、retirement timeout、retirement after drain、device lost、lifecycle violationは0だった。
全runでprocess exit 0、teardown success、final report after teardownが成立した。

正式summaryは`build/ucrt64-release/p2-matrix-d4/summary.json`に保存した。
`formal_contract_version = P2-D4-2`、`all_playback_runs_pass = true`、
`all_seek_runs_pass = false`、`p2_pass = false`である。旧P2-D2の`p2-matrix/`は
historical artifactとして変更していない。

### 15.5 [exit] P2 FINAL FAIL

P2 formalが1 runでもMUST失敗したためP1 formal regressionは実行していない。
したがって最終判定は **P2 FINAL FAIL** である。Playback gateとSeek latency/correctness gateは
通過したが、採用したparallel dual-source architectureの1000/1000 overlap provenanceが
成立しなかった。性能修正と再formalは次ラリーの対象とし、P3へは進まない。

## 16. P2-D5-1 parallel dispatch contract correction

### 16.1 [事実] P2-D4-2のFAILはhistorical resultとして保持する

§15のP2 FINAL FAILと`build/ucrt64-release/p2-matrix-d4/`は、P2-D4-2 contractで
取得した正式結果として正しい。raw、summary、判定は変更していない。

P2-D4-2が要求した1000/1000 execution overlapは、controllerがA/B sourceへ並列に
dispatchしたことに加えて、OSが両workerのexecution intervalを毎回物理的に重ねることまで
要求していた。後者はparallel architectureの必要条件ではなく、thread schedulingの観測結果で
ある。したがって「以前のFAILが誤りだった」とは扱わない。P2-D4-2 contractではFAILし、
その後にcontract semanticsの過剰制約を修正してP2-D5-1で再評価する。

### 16.2 [事実] P2-D5-1はparallel dispatchを直接検証する

各seek sampleへrequest開始、A/B request、dispatch完了、A/B execution begin、A/B readyの
各QPCと、A/B request ID、A/B request resultを記録する。controllerはA request、B request、
dispatch完了の後にだけcompletion pollへ進む。pureな順序検査は
`A dispatch -> B dispatch -> wait`を受理し、`A dispatch -> wait A -> B dispatch`を拒否する。

formal checkerはA/B resultが`Accepted`、各requestがrequest開始以後、dispatch完了が両request
以後かつfirst ready以前であることをsampleごとに再計算する。1000 seekでは
`parallel_dispatch_valid_count == 1000`をMUSTとする。execution overlapは同じinterval式で
引き続き保存するが、`execution_overlap_count`と`execution_nonoverlap_count`は診断値であり、
単独ではformal PASS/FAILに使用しない。overlap率のthresholdは設けていない。

### 16.3 [事実] short validationはparallel dispatch全件成立だった

Release build、seed 20260808、fence backendで64 seek integrationと256 seek x 1 processを
実行した。これは経路確認であり、P2 formal thresholdによる判定には使用しない。

| seek | parallel dispatch valid | execution overlap | mismatch / timeout / stale / busy |
| ---: | ---: | ---: | ---: |
| 64 | 64 / 64 | 64 / 64 | 0 / 0 / 0 / 0 |
| 256 | 256 / 256 | 256 / 256 | 0 / 0 / 0 / 0 |

両方でpublish reject、request mismatch、stopped superseded、software fallback、device lostは0だった。
256 seekではCPU full-frame readback、full-frame GPU copy、lifecycle violationも0で、teardownは
成功した。execution overlapは観測値として報告するだけであり、この値を合否条件には戻さない。

### 16.4 [事実] Release / Debug full CTestは各191/191通過した

UCRT64のDLLを解決するPATHを明示し、共有proxy artifactの競合を避けるため直列で全登録testを
実行した。Release / Debugとも191/191通過した。各191件にはperformance 11件とstability 1件を
含む。P2-D5-1 checkerのpure順序検査、execution non-overlap対照、parallel dispatchのcount、
sample、B request result、first-ready境界のnegativeもこの回帰に含まれる。format、lint、
`git diff --check`も通過した。

### 16.5 [事実] clean HEADからP2-D5-1 formalを新規取得した

formal対象HEADは`9ebe3f3eda0252512e7f8f965f01446910d9ae35`である。開始時の
`git status --porcelain`は空で、matrix summaryの`dirty_worktree`はfalse、
`provenance_unchanged_during_matrix`はtrueだった。旧D4 rawは流用せず、Playback 3 runと
Seek 3 runをすべて新規取得して`build/ucrt64-release/p2-matrix-d5/`へ保存した。

### 16.6 [事実] Playback formalは3/3通過した

P2-D5-1、seed 20260808、fence、5秒warmup、固定8 frame pre-roll、60秒measurementで
3 independent processを実行した。

| run | effective fps | drop rate | pre-roll A/B | displayed / scheduled | missing | EOF A/B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 59.7837 | 0.3333% | 9 / 8 | 3588 / 3600 | 0 | 0 / 0 |
| 2 | 59.9002 | 0.1389% | 10 / 12 | 3595 / 3600 | 0 | 0 / 0 |
| 3 | 59.8837 | 0.1667% | 16 / 9 | 3594 / 3600 | 0 | 0 / 0 |

全dropはscheduler deadline分類だった。front A/B 0、first output 0、non-deadline drop、
marker/probe mismatch、mixed frame/generation、stale composition epoch、CPU readback、
full-frame GPU copyはいずれも全runで0だった。

### 16.7 [事実] Seek formalはparallel dispatch 1000/1000で3/3通過した

1000 deterministic seekを3 independent processで実行した。

| run | p95 | observed max | parallel dispatch valid | execution overlap | contract |
| --- | ---: | ---: | ---: | ---: | --- |
| 1 | 132.2224 ms | 216.4735 ms | 1000 / 1000 | 1000 / 1000 | PASS |
| 2 | 132.7596 ms | 215.9984 ms | 1000 / 1000 | 999 / 1000 | PASS |
| 3 | 132.4941 ms | 216.8287 ms | 1000 / 1000 | 999 / 1000 | PASS |

全runでnearest-rank p95は150 ms以下、observed maxは400 ms以下であり、3000 seekの
global observed maxは216.8287 msだった。display mismatch、timeout、stale completion、
busy acceptance、publish reject、request mismatch、stopped superseded、software fallback、
CPU full-frame readback、full-frame GPU copyは全runで0だった。run2とrun3の各1件の
execution non-overlapはOS schedulingの診断値として保存し、formal判定には使用していない。

6 run合計でmarker/probe mismatch、mixed frame/generation、stale epoch、untracked submission、
completion failure、early release、retirement timeout、retirement after drain、device lost、
lifecycle violationは0だった。全runでteardown successとfinal report after teardownが成立した。
summaryは`all_playback_runs_pass = true`、`all_seek_runs_pass = true`、`p2_pass = true`である。

### 16.8 [事実] P1 formal regressionも通過した

P2-D5-1 PASS後、同じclean HEADで既存P1 contractを変更せずformal regressionを実行した。
5秒warmup、60秒measurement、1000 seek、3 independent processで、各rawの契約92項目は
判定対象と診断対象の全9 runで成立した。

| source | gate | fps min | drop max | seek p95 max | seek observed max | marker mismatch |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p60 H.264 | 対象 | 59.879 | 0 | 100.065 ms | 133.432 ms | 0 / 21 |
| 1080p60 HEVC | 対象 | 59.891 | 0 | 50.325 ms | 117.496 ms | 0 / 21 |
| 4K60 H.264 | 診断のみ | 59.754 | 0 | 250.897 ms | 281.889 ms | 0 / 21 |

判定対象のH.264 / HEVCはsame adapter、same device、fence backendで、CPU full-frame readback、
seek failure、seek display mismatch、early release、untracked submission、retirement timeout、
device lostはいずれも0だった。正式summaryは`build/ucrt64-release/p1-matrix/summary.json`に
保存し、`pass = true`、`violations = []`である。4Kのseek値は診断のみで判定に使用していない。

### 16.9 [exit] P2 FINAL PASS under P2-D5-1

P2-D5-1のPlayback 3/3、Seek 3/3、parallel dispatch各1000/1000と全既存MUSTが成立し、
続くP1 formal regressionも通過した。したがって最終判定は
**P2 FINAL PASS under P2-D5-1**である。

§15のP2-D4-2におけるP2 FINAL FAILはhistorical resultとして維持する。P2-D4-2 contractでは
FAILし、その後にcontract semanticsの過剰制約を修正してP2-D5-1で再評価した結果が本節の
PASSである。P3には進まない。

## 17. P3-C-1 formal実行とP3 closure判定

### 17.1 [事実] clean HEADと通常回帰をformal前に固定した

formal対象HEADは`5ed555a9939705768f1372385456113a0ba1439c`である。開始時とformal開始直前の
`git status --porcelain`は空だった。formal前のordinary CTestはperformance / stabilityを除外し、
Release / Debugとも215/215通過した。

P3-C-1 summaryではstart/endのgit commit、dirty状態、fixture A/B SHA-256、executable SHA-256、
contract versionが一致した。GPU adapterは両時点ともNVIDIA GeForce RTX 4090、audio endpointは
48,000 Hz、2 channel、`flt`だった。`provenance_unchanged = true`、
`hardware_provenance_unchanged = true`である。

### 17.2 [事実] P3-C Playback formalは3/3通過した

`pwsh -NoProfile -File scripts/p3-matrix.ps1`を`DryRun`、`StopOnFailure`なしで一度だけ実行した。
Playbackは5秒warmup後、60秒、3,600 frameを3 independent processで測定した。

| run | displayed / skipped / required | effective fps | drop rate | AV abs p95 | AV abs max | underflow / overflow |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3590 / 10 / 3600 | 59.8333 | 0.2778% | 16.083 ms | 17.458 ms | 0 / 0 |
| 2 | 3583 / 17 / 3600 | 59.7167 | 0.4722% | 16.000 ms | 17.500 ms | 0 / 0 |
| 3 | 3582 / 18 / 3600 | 59.7000 | 0.5000% | 16.083 ms | 17.354 ms | 0 / 0 |

各runでfirst frame 0、`displayed_unique + skipped == 3600`、AV raw countとdisplayed uniqueの
一致が成立した。duplicate display、non-increasing display、AV projection failure、marker mismatch、
mixed pair/generation、stale composition epoch、video ahead violation、clock regression、QPC fallback、
audio clock query failureは0だった。

### 17.3 [事実] P3-C Seek formalは3/3通過した

seed 20260808の1000 deterministic integrated seekを3 independent processで実行した。

| run | exact | request-display p95 | observed max | first-display AV abs p95 | AV abs max | timeout / busy / stale / generation mismatch |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1000 / 1000 | 133.7115 ms | 183.3806 ms | 9.0625 ms | 10.2917 ms | 0 / 0 / 0 / 0 |
| 2 | 1000 / 1000 | 134.5040 ms | 183.1812 ms | 8.9375 ms | 10.2292 ms | 0 / 0 / 0 / 0 |
| 3 | 1000 / 1000 | 149.7502 ms | 200.0543 ms | 9.0833 ms | 10.2083 ms | 0 / 0 / 0 / 0 |

各runでrequested audio sample、first audio sample、first displayed video frame、first-display AV projectionの
exact contractが成立した。run 3のp95は150.000 ms閾値近傍だが、丸め、平均、再試行による救済は
行っていない。

### 17.4 [事実] P3-C PauseResume formalは3/3通過した

| run | clock frozen | video advance zero | generation stable | AV abs p95 | AV abs max |
| --- | --- | --- | --- | ---: | ---: |
| 1 | true | true | true | 14.313 ms | 15.208 ms |
| 2 | true | true | true | 16.646 ms | 17.021 ms |
| 3 | true | true | true | 10.458 ms | 11.667 ms |

全runでunderflow、clock regression、QPC fallbackは0だった。

### 17.5 [事実] P3-C global correctnessと正式summaryはPASSだった

9 run合計でCPU full-frame readback、full-frame GPU copy、software video fallback、device lost、
lifecycle violation、audio decode/render thread join leakは0だった。全runでvideo worker join、
teardown success、final report after teardownが成立した。

raw producerの`formal_verdict`は全9件とも設計どおり`NOT_RUN`のままである。正式summaryは
`build/ucrt64-release/p3-matrix/summary.json`に保存し、schemaは
`mvm-p3-matrix-summary-1`、`expected_processes = 9`、`completed_processes = 9`、
`formal_verdict = PASS`、`all_runs_pass = true`、`p3_c_pass = true`である。

### 17.6 [事実] P3-A standalone regressionは通過した

P3-C PASS後に既存`pwsh -NoProfile -File scripts/p3-a-smoke.ps1`を変更せず実行した。
playback 15秒 x 3、exact audio seek 64/64、pause/resume、audio marker 6/6がすべて通過し、
`build/p3-a-smoke/summary.json`の`verdict`は`PASS`、`errors`は空だった。

### 17.7 [事実] P2-D5-1 formal regressionはoutput size MUSTで6/6失敗した

P3-A PASS後に既存`pwsh -NoProfile -File scripts/p2-matrix.ps1`を変更せず一度だけ実行した。
Playback 3 runとSeek 3 runの全process自体はexit 0だったが、全6 rawで
`actual_output_width = 1204`、`actual_output_height = 1080`だった。P2-D5-1 checkerが要求する
width 1920と一致しないため、全runのcontract exitは3、per-run `pass`はfalseになった。

Playbackのeffective fpsは59.6844 / 59.7505 / 59.7830、drop rateは0.5000% / 0.3889% /
0.3333%だった。Seekのp95は83.3333 / 83.2379 / 83.2814 ms、observed maxは
150.6075 / 150.1728 / 150.3128 ms、parallel dispatch validは各1000/1000だったが、これらの
診断値でoutput size MUSTを救済していない。

P2 summaryは`build/ucrt64-release/p2-matrix-d5/summary.json`に保存した。
`provenance_unchanged_during_matrix = true`、`all_playback_runs_pass = false`、
`all_seek_runs_pass = false`、`p2_pass = false`である。原因の推測、performance fix、checker変更、
threshold変更、同じHEAD/seedでの再試行は行っていない。

### 17.8 [exit] P3 FINAL FAIL under P3-C-1

P3-C-1 new path formalとP3-A regressionはPASSしたが、後続のP2-D5-1 formal regressionが
1 run以上でMUST失敗したため、指示どおりP1 formal regressionは実行していない。したがって
Phase-1 P3 closureの最終判定は **P3 FINAL FAIL under P3-C-1** である。

P3-C-1 raw / summaryとP2-D5-1 regression raw / summaryは保存し、P4へは進まない。
