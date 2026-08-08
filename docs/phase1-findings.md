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

P2-D4A の source coverage 修正と短縮経路確認は成立したため、D4B の正式 matrix
再実行へ進める。P2-D4A の smoke 値を P2 の合否根拠には使わない。
`P2-D4-1` の clean worktree で formal Playback / Seek を再実行し、その raw から
集計するまでは、P2-D2 の `p2_pass = false` が最後の正式判定である。
