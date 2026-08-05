# Phase 0 所見

最終更新: 2026-08-04
対象: S0（リポジトリ初期化）、S1（UCRT64 依存導入 + prebuilt MLT hello world）、
R0（凍結復元検証）、S2（検証素材生成）、S4（mvm_bench と V2）、V10 の一部

判定そのものは S16 で [phase0-report.md](phase0-report.md) にまとめる。
本書は途中で得た事実を、忘れないうちに記録するためのもの。

## 本書の読み方

記述は必ず次のいずれかに分類する。混ぜない。

| 印 | 意味 |
| --- | --- |
| **[事実]** | 実際にコマンドを実行して観測した。再現手順を併記する |
| **[推測]** | 観測から導いた説明。ソースを読んで確かめてはいない |
| **[未検証]** | まだ測っていない。できると仮定してはいけない |
| **[回避策]** | 問題に対して現在採っている対処。恒久策とは限らない |
| **[exit]** | exit criteria への影響 |

特に「MLT が FFmpeg 8 に対してビルドされている」ことと
「各 codec を正しく decode できる」ことは別問題である。前者は S1 の構成上の事実、
後者は S4 で初めて実測した。

---

## 確定した構成

| | version | 備考 |
| --- | --- | --- |
| MSYS2 UCRT64 | — | `C:\msys64\ucrt64` |
| gcc | 16.1.0 | |
| CMake / Ninja | 4.4.2 / 1.13.2 | いずれも UCRT64 版 |
| Qt | **6.11.1** | pacman 版。既存の MSVC 版 6.8.3 とは別物 |
| MLT | **7.36.1** | prebuilt |
| FFmpeg | **8.1.2** | |

全 184 パッケージの正確な version は [deps-lock.txt](deps-lock.txt)。
実体 369 MB は `third_party/pkgs/` に凍結済み（git 管理外）。

### FFmpeg 8 と MLT 7 の組み合わせについて

**[事実]** MSYS2 の MLT 7.36.1 は FFmpeg 8.1.2 に対してビルドされている。
モジュール `libmltavformat.dll` は正常にロードでき、producer / consumer とも登録された。

**これは「リンクとロードが成立している」ことしか意味しない。**
実際に各 codec を decode できるかは別問題であり、S4 で初めて実測した（後述の V2 の節）。
S1 時点でこの 2 つを混同してはいけない。

### 凍結物の性質: 最小依存集合ではなく環境スナップショット

**[事実]** `third_party/pkgs/` にあるのは、`pacman -Q` が返す
**UCRT64 環境の全インストール済みパッケージ 184 件**である。
`scripts/freeze-deps.ps1` はインストール済み一覧をそのまま走査しているため、
mvm が直接指定した 24 件の依存 closure に限定されていない。

つまりこれは **「UCRT64 環境スナップショット」であり「最小依存集合」ではない。**

影響:

- 再現性の観点では安全側に倒れている（余分に含む方向の誤差）
- 一方で、mvm が本当に必要とする最小集合は分かっていない。
  V11 の staging で何を同梱すべきかの答えにはならない
- 別プロジェクトの都合で入ったパッケージが混ざりうる。
  凍結サイズ 369 MB のうち、どれが mvm に必要かは未分離

**[未検証]** 最小依存 closure の再構築。今回の範囲外とした。
S13（再現ビルド）または V11（staging）で必要になった時点で行う。
その際は `pacman -Qi` の依存グラフから closure を計算する方式が使える。

---

## S1 の所見

### 所見 1: MLT はモジュール・データの場所を実行ファイル相対で推測し、外れても失敗しない

`mlt_factory_init(NULL)` は、モジュールとデータの場所を実行ファイルからの相対で推測する。
開発ビルドでは実行ファイルが `build/<preset>/bin` にあるため推測は必ず外れ、
`build/<preset>/lib/mlt` を探して何も見つけられない。

**このとき MLT は失敗しない。** モジュール 0 件のまま静かに初期化を完了する。
実際に観測した状態:

```
[producers] 0 件   [filters] 0 件   [transitions] 0 件   [consumers] 0 件
```

stderr に `mlt_repository_init: no plugins found in ...` が 1 行出るのみで、
`mlt_factory_init` は有効な repository を返す。

**影響**: この失敗は後の工程で「producer が見つからない」という原因の分かりにくい形でしか
表面化しない。V11（clean 環境への配置）では、依存や resource を 1 つ落とすだけで
同じ状態になり、VM 上では単なる機能欠落として現れる。

**対処**: 場所は常に明示的に与える。環境変数には依存させない
（ユーザー環境の `MLT_REPOSITORY` に引きずられる事故を避けるため、自プロセス内で設定する）。
pkg-config が `moduledir` / `mltdatadir` を公開しているので、開発ビルドではこれを
ビルド時に埋め込む（[cmake/FindMLT.cmake](../cmake/FindMLT.cmake)）。
V11 では実行ファイル相対の同梱パスへ差し替える。

**MSYS2 のレイアウト**（上流ドキュメントの `lib/mlt-7` ではない点に注意）:

| | |
| --- | --- |
| モジュール | `C:/msys64/ucrt64/lib/mlt` |
| データ | `C:/msys64/ucrt64/share/mlt` |
| profile | `C:/msys64/ucrt64/share/mlt/profiles` |

### 所見 2: `mlt_profile_init()` は解決に失敗しても NULL を返さず、既定値へ静かにフォールバックする

`mlt_profile_init("atsc_1080p_60")` は、profile 定義が見つからなくても非 NULL を返す。
返るのは既定値（`720x576 @ 25/1`、dv_pal 相当）である。

**つまり「非 NULL が返ったこと」は成功を意味しない。** 実際に S1 で、
MLT_DATA が未解決の状態でこの誤検知が発生した。

**影響**: profile は解像度・fps・SAR を決めるため、**V12（preview と final render の一致）の
前提そのもの**である。取り違えたまま進むと、原因の分からない尺ずれ・幾何ずれとして
表面化する。Phase 0 で最も高くつく種類のバグになりうる。

**対処**: profile は必ず「返ってきた値」を検証する。mvm 側の adapter でも、
profile を作った直後に期待値と照合する設計にする。

### 所見 3: MSYS2 の MLT は必須モジュールの依存を optional 扱いにしており、既定では欠落する

MSYS2 の `mingw-w64-ucrt-x86_64-mlt` は以下を **suggested（optional）依存**としており、
既定では導入されない。しかしモジュール DLL 自体はこれらにリンクされた状態で同梱される。
結果、該当モジュールは「ファイルは存在するのに dlopen できない」状態になる。

| モジュール | 不足していた DLL | 失われる機能 |
| --- | --- | --- |
| `libmltplus` | `libebur128`, `libfftw3` | **`affine`（V7 transform/scale）、`dynamictext`（V3 文字レイヤ）** |
| `libmltresample` | `libsamplerate` | 音声リサンプル |
| `libmltrubberband` | `librubberband` | 音程保持のタイムストレッチ |
| `libmltsox` | `libsox` | 音声フィルタ |
| `libmltrtaudio` | `librtaudio` | 音声出力（preview の代替経路） |

MLT はロード失敗を stderr に 1 行出すだけで処理を継続する。
`plus` モジュールが落ちると V3 と V7 の中核機能が丸ごと消えるが、
その因果は表示されない。

**対処**: `libebur128` `fftw` `libsamplerate` `rubberband` `sox` `rtaudio` を
必須として `bootstrap-msys2.ps1` に追加した。

さらに、症状ではなく原因を検知するため、hello world にモジュールディレクトリを
走査して `LoadLibraryA` を試す検査を入れた。これは V11 の staging 検証でも
そのまま必要になる。

### 所見 4: UCRT64 の gcc は PATH が通っていないとエラーを出さずに失敗する

`C:\msys64\ucrt64\bin` が PATH に無い状態で `gcc.exe` を絶対パス起動すると、
依存 DLL を解決できず**標準出力・標準エラーとも空のまま** exit 1 で終了する。

CMake からは次のようにしか見えない:

```
The C compiler "C:/msys64/ucrt64/bin/gcc.exe" is not able to compile a simple test program.
  ...
  FAILED: [code=1] CMakeFiles/cmTC_xxxxx.dir/testCCompiler.c.obj
  (出力なし)
```

**対処**: `project()` より前に PATH を検査して実情を伝える
（[CMakeLists.txt](../CMakeLists.txt)）。加えて `scripts/build.ps1` が
PATH を整えてから cmake を呼ぶ。

### 所見 5: `avfilter` は個別名で登録される

MLT は avfilter を `avfilter.<name>`（例 `avfilter.scale`）として個別に登録する。
`avfilter` という名前の filter は存在しない。フィルタ総数が 474 件と多いのはこのため。

### 所見 6: `toolchain` は group であり pin できない

`mingw-w64-ucrt-x86_64-toolchain` は package ではなく group。`pacman -Q` で引けず、
version を pin することもできないため deps-lock の目的に反する。
実パッケージ（`gcc` `binutils` `gdb` `make`）を明示する形に変更した。

---

## R0 / S2 / S4 の所見

### 所見 7: Windows の `main(argc, argv)` は UTF-8 ではなく ANSI

**[事実]** 日本語を含むディレクトリを引数に渡したところ、
mvm 側の UTF-8 → UTF-16 変換だけが失敗し、MLT 自身は成功するという食い違いが起きた。

原因は `main` が受け取る `argv` がプロセスの ANSI コードページ（日本語環境では CP932）で
エンコードされていることである。UTF-8 前提で `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`
にかけると失敗する。さらに悪いことに、ANSI コードページで表現できない文字
（絵文字など）は argv の時点で復元不能に壊れている。

**[回避策]** `GetCommandLineW` + `CommandLineToArgvW` から引数を取り直し、
UTF-8 へ変換する（`mvm_win_get_utf8_args`）。Windows で Unicode 引数を正しく
受け取る方法はこれしかない。`mvm_mlt_hello` と `mvm_bench` の両方で使っている。

**[exit]** M12（日本語パス）の前提。これを踏まないと、パス処理が正しくても
引数の受け渡しで壊れる。

### 所見 8: MLT の UTF-8 対応は「素材パス」と「モジュール/データ ディレクトリ」で非対称

Phase 0 で最も重要な V10 の所見。

**[事実] 素材ファイルのパスは真に UTF-8 対応している。**

CP932 で表現**できない**文字を含むパスで検証した:

```
...\媒体 🎬 테스트 简体\映像🎥test.mp4      (絵文字 + ハングル + 簡体字)
```

結果: `probe` は MLT と ffprobe が完全一致（mismatches なし）、
`decode --frame 42 --expect-marker` はマーカー値 42 で一致。
つまり ANSI コードページに依存していない。

日本語 + 半角空白 + 全角空白 + 全角記号を含むパスも同様に成功
（CTest の `probe_jp_path_*` / `decode_marker_jp_path`）。

**[事実] 一方、モジュール/データ ディレクトリは UTF-8 を受け付けない。**

日本語を含むディレクトリへモジュールを配置し UTF-8 のまま
`mlt_factory_init` へ渡すと、**必須 service 17 件すべてが解決できなくなる**
（`services_missing=17`）。しかも `mlt_factory_init` は成功を返す。

同じディレクトリに ANSI（CP932）でパスを渡した場合は解決できた。

**[推測]** MLT は repository のディレクトリ走査に *A 系 API を使っている。
ソースは読んでいないため断定はしない。

**この非対称は mvm にとって都合がよい。**
素材パスはユーザーが決めるので制御できないが、モジュール/データ ディレクトリは
mvm 自身が配置する（V11 staging）ので ASCII に保てる。

**[回避策]** `mvm_mlt_runtime_init` が module_dir / data_dir に非 ASCII 文字を
検出したら警告を出す。気づかないまま縮退することだけは防ぐ。
V11 では staging を ASCII パスに置く方針とする。

**[未検証]** ユーザーのプロファイル名が日本語（`C:\Users\<日本語>\`）の場合に
staging を既定でどこに置くか。V11 で決める。

**[exit]** M12 に対しては良い材料。素材パスは MLT へのパッチ不要で動く。
staging の ASCII 制約は mvm 側の配置ルールで吸収でき、工数はほぼゼロ。

### 所見 9: MLT は静止画の length を INT_MAX で返す

**[事実]** `png_alpha.png` を avformat/qimage producer で開くと
`mlt_producer_get_length` が **2147483647**（INT_MAX）を返す。
そのまま秒へ換算すると **8.59e7 秒（約 2.7 年）** になる。

これは「尺が無限」という MLT の表現であって duration ではない。
気づかずにタイムラインへ流すと、静止画クリップが実質無限長になる。

**[回避策]** `MvmMltProbeResult.is_unbounded_length` で明示的に印を付け、
duration 計算をしない。ffprobe との比較からも除外する
（黙って丸めるのではなく、除外したことを JSON に残す）。

### 所見 10: 音声のみの素材では frame_count が profile の fps に依存する

**[事実]** `wav_48k.wav`（5 秒）に対し MLT の length は **125**。
これは素材の性質ではなく、profile の fps が 25 だからである
（`mlt_profile_from_producer` は映像が無いと fps を変えられない）。

当初、duration を「素材の fps」で割っていたため、音声のみでは fps=0 となり
`duration_sec = 0` になっていた。この状態でも ffprobe との比較は
「0 なので比較しない」となり、**WAV のテストが空振りで通過していた。**

**[回避策]** duration は profile の fps で割る。
`125 / 25 = 5.0 秒` となり ffprobe の 5.0 と一致する。これで比較が実質化した。

**[exit]** M2 の検証設計そのものへの教訓。
「一致した」ではなく「本当に比較したか」を確認しないと、通ったテストが無意味になる。

### 所見 11: ffprobe の起動から cmd.exe を排除した

**[回避策]** 当初 `mvm_bench` は `cmd.exe /C "... > file 2>nul"` で ffprobe を
起動していた。これは日本語や記号を含むパスに対して危険である。
cmd.exe を挟むと、引数の解釈が Windows の標準規則の上にもう一段乗り、
`^` `&` `|` やリダイレクト記法の扱いが環境依存になるためである。

現在は `CreateProcessW` を直接使い、stdout / stderr には
継承可能ハンドルとして開いた一時ファイルを渡している。
引数は Windows の標準規則に従って自前で quote する。

**[事実]** 変更後も日本語パス・CP932 外の文字を含むパスで
probe が成功することを確認済み（CTest の `probe_jp_path_*`）。

**[未検証]** 一時ファイルではなくパイプで受ける方式。
現状で困っていないため変更していない。V11 の staging で
一時ディレクトリの扱いを決める際に再検討する。

---

## V2（素材読み込み）— 実測結果

**[事実]** smoke 素材（5 秒 / 60fps / 300 フレーム）に対し、
MLT と ffprobe の解析結果を比較した。

### 実際に比較している項目

「全項目一致」と書くと何を比較したか分からなくなるので、明示する。
`mvm_bench probe` が mismatch 判定に用いるのは以下である。

| 項目 | 比較方法 | 対象 |
| --- | --- | --- |
| `has_video` / `has_audio` | 真偽の一致 | 全素材 |
| `video_codec` | 文字列の一致 | 映像がある素材 |
| `pix_fmt` | 文字列の一致 | 映像がある素材 |
| `width` / `height` | 整数の一致 | 映像がある素材 |
| `sample_aspect_ratio` | **gcd で正規化した有理数**の一致 | 映像がある素材（静止画含む） |
| `fps_num` / `fps_den` | 有理数のまま整数一致 | 静止画を除く映像 |
| `frame_count` | 整数の完全一致 | 静止画を除く映像 |
| `duration_sec` | 許容差 **±0.005 秒** | 静止画を除く全素材 |
| `audio_codec` | 文字列の一致 | 音声がある素材 |
| `sample_rate` / `channels` | 整数の一致 | 音声がある素材 |

SAR を文字列比較しないのは、`1:1` と `2:2` が等価だからである。
ffprobe は `N:M`、MLT は分子分母の整数で返すため、
どちらも gcd で正規化してから比較する。`0:1` や空文字は「未指定」として扱う。

**比較していない項目**（意図的な除外。黙って丸めているのではない）:

- 静止画の `fps` / `frame_count` / `duration`
  — MLT は静止画の length を INT_MAX で返し、ffprobe は既定の 25/1 と 1 フレームを返す。
  比較しても両者の設計差しか分からない。`is_image` として JSON に記録する
- `container` — MLT 側に対応する概念が無い
- `display_aspect_ratio` — SAR と解像度から導出されるため冗長

### 結果

| 素材 | codec | 解像度 | pix_fmt | SAR | fps | frame_count | duration | 音声 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `v1080p60_h264` | h264 | 1920x1080 | yuv420p | 1/1 | 60/1 | 300 | 5.000 | aac 48000/2ch |
| `v1080p60_hevc` | hevc | 1920x1080 | yuv420p | 1/1 | 60/1 | 300 | 5.000 | aac 48000/2ch |
| `v4k60_h264` | h264 | 3840x2160 | yuv420p | 1/1 | 60/1 | 300 | 5.000 | aac 48000/2ch |
| `v4k60_hevc10` | hevc | 3840x2160 | yuv420p10le | 1/1 | 60/1 | 300 | 5.000 | aac 48000/2ch |
| `png_alpha` | png | 512x512 | rgba | 1/1 | 除外 | 除外 | 除外 | なし |
| `wav_48k` | なし | — | — | 未指定 | — | — | 5.000 | pcm_s16le 48000/2ch |

上表の各欄で MLT と ffprobe が一致した（mismatches 0 件）。

smoke 素材は CFR で生成しているため、**fps と frame_count は完全一致を要求**している。
duration のみコンテナのタイムベース丸めを考慮し、許容差 **±0.005 秒** を設けた
（60fps の 1 フレーム = 16.7ms より十分小さい）。不一致は丸めず、そのまま報告する。

### アルファ

**[事実]** `png_alpha` は MLT のパイプラインを通した後も
`alpha_min=0 / alpha_max=253` でグラデーションが保持されている。

pix_fmt の文字列だけでなく、実際に取り出したフレームのアルファ値の
最小・最大を見て判定している。さらに manifest では値域そのものを検証する
（`alpha_min_le=5` / `alpha_max_ge=250`）。
pix_fmt が `rgba` でも中身が全て 255 ならアルファは死んでいるので、
真偽値だけの検証では足りない。

### フレーム固有マーカーによる decode 検証

**[事実]** 焼き込んだフレーム固有マーカー（仕様は
[test-media-format.md](research/test-media-format.md)）を読み戻し、
要求フレームと一致することを確認した。OCR は使っていない。

| 素材 | 要求フレーム | マーカー値 | 同期 |
| --- | --- | --- | --- |
| H.264 1080p60 | 0 / 1 / 137 / 299 | 0 / 1 / 137 / 299 | OK |
| HEVC 1080p60 | 137 | 137 | OK |
| H.264 4K | 42 | 42 | OK |
| **HEVC 10bit 4K** | 137 | 137 | OK（luma 15–234） |
| 日本語パス H.264 | 200 | 200 | OK |
| 絵文字/ハングル/簡体字パス H.264 | 42 | 42 | OK |

マーカーは H.264 / HEVC の圧縮を通しても正しく読める。
**10bit（yuv420p10le）→ RGBA の変換を経ても読める**ことも確認した。
probe が通るだけでは「ピクセルが正しく出てくるか」は分からないため、
10bit 素材にもマーカー照合を入れている。

**[未検証]** これは「単発の decode が要求フレームに着地する」ことの確認であり、
**V4（連続 seek と scrub の精度・速度）ではない。** M4/M5/M6 は S6 で測る。

### 破損・退化素材（negative test）

**[事実]** 以下の入力に対し、`probe` / `decode` とも失敗（exit 3）した。
成功扱いされることはない。

| 入力 | MLT の挙動 |
| --- | --- |
| 0 バイト | `mlt_factory_producer` が NULL |
| ランダムバイト列 64KB | 同上 |
| 先頭 20% だけの mp4（途中切断） | 同上 |
| 拡張子だけ mp4 のテキスト | 同上 |
| 字幕のみの mp4（コンテナは正当、映像も音声も無い） | 同上 |

**[事実・前回の記述の訂正]** 以前「MLT は開けなかった素材でも producer を返す」と
書いたが、これは**測って確かめた事実ではなく仮定だった**。
実測したところ、service に `"avformat"` を明示指定する限り、
上記 5 種すべてで `mlt_factory_producer` は NULL を返す。

そのため `mvm_mlt_probe_file` に追加した
「`nb_streams <= 0`」「映像も音声も無い」の 2 つの検査は、
**現時点では到達しない防御的な検査**である。残している理由は 2 つ。

- service を NULL（loader / 自動判定）にすると、MLT は未知の入力に対して
  別の producer へフォールバックしうる。その場合 producer は非 NULL で返り、
  「無音の黒 1 フレーム」として静かに流れる可能性がある
- コストがほぼゼロで、失敗を静かに通すリスクの方が高い

**[未検証]** loader 経路の実際のフォールバック挙動。
service を可変にする時点で必ず確かめること。

### 素材生成の決定論性

**[事実]** 同一設定で再生成し照合した結果、
**メタデータ不一致 0 件、hash 不一致も 0 件**だった
（`make-testmedia.ps1 -Mode Smoke -Force -VerifyRegeneration`）。

この FFmpeg 8.1.2 ビルドと今回の引数では、libx264 / libx265 の出力も
バイト一致した。ただし方針としては hash 一致を**無条件の合格条件にはしない**。
スレッド数やビルドが変われば崩れうる非本質的な性質だからである。
合格条件はメタデータと期待フレーム数の一致とする。

---

## R0（凍結パッケージ復元）— 実測結果

**[事実]** クリーンな MSYS2 ベース上で **9 項目中 0 件失敗**。

検証環境の作り方が結果の意味を決めるので明記する:

- `msys2-base-x86_64-20260611.tar.xz` を **repo.msys2.org から新規に取得**して展開した
- 既存 `C:\msys64` の**コピーではない**。展開直後に
  `ucrt64/bin/libmlt-7.dll` が存在しないことをスクリプトが確認している
  （存在したら「クリーンなベースではない」として中断する）
- 既存の `C:\msys64` と `C:\Users\lambe\sdk\Qt\6.8.3` は削除・変更・移動していない。
  スクリプトはこれらを `-TestRoot` に指定できないよう明示的に拒否する
- **署名検証は有効**。`pacman-key --init && --populate msys2` を実行し、
  凍結した 184 件の `.sig` で検証して導入した

| # | 項目 | 結果 |
| --- | --- | --- |
| R0-0 | クリーンな MSYS2 ベースを用意（UCRT64 未導入を確認） | OK |
| R0-1 | `third_party/pkgs` だけから UCRT64 を導入できる | OK |
| R0-2 | `docs/deps-lock.txt` と version が一致（184 / 184 件） | OK |
| R0-2b | 必須の直接指定パッケージ 24 件がすべて存在 | OK |
| R0-3 | 復元先の `ucrt64/bin` だけを PATH にして `mvm_mlt_hello` を実行（exit 0） | OK |
| R0-4 | モジュール 23 件すべてロード可能（failed=0） | OK |
| R0-5 | 必須 service 17 件すべて解決（`affine` / `dynamictext` / `qtext` / `avformat` / `qtblend` / `mix` 等） | OK |
| R0-6 | `atsc_1080p_60` が 1920x1080 / 60|1 / SAR 1|1 | OK |
| R0-7 | optional 扱いだった依存の DLL 5 件もロード可能 | OK |

PATH は復元先の `ucrt64\bin` と `system32` のみ。
開発機の `C:\msys64\ucrt64\bin` は含めていない（含めるとどちらの DLL を
使ったのか分からなくなり、検証の意味が消える）。

**[事実]** 検証中に判明した実装上の落とし穴を 2 つ潰した。

- `scripts/freeze-deps.ps1` が `.sig` を退避していなかった。
  署名が無いと `SigLevel=Never` へ落とすしかなく、「復元できた」ことの意味が弱くなる。
  184 件の署名を退避するよう修正した
- `pacman-key --init` が常駐させる `gpg-agent` が検証ルートを掴んだままになり、
  次回実行時に削除できなくなる。検証ルート配下から起動されたプロセスだけを
  停止する処理を入れた（開発機の `C:\msys64` のプロセスは止めない）

**[事実]** S5 に進む前の見直しで、R0 の判定にも空振りがあったため修正した。

- R0-3 が「実行できた」だけを見ており、`$exit` を確認していなかった。
  `mvm_mlt_hello` は問題検出時に exit 2 を返すため、
  検査が落ちていても R0-3 は成功していた。`exit -eq 0` を要求するよう修正
- lock に無い**余分なパッケージ**を検出していなかった。
  欠落と同じく「lock の示す構成と別物」を意味するため、失敗にするよう修正
- キーリング初期化に失敗すると**自動で署名検証を切って続行**していた。
  これでは「復元できた」の意味が静かに弱くなる。
  現在は失敗させ、署名なしを許すのは `-NoSignatureCheck` を明示した場合だけとした

**[exit]** M1（再現ビルド）の主要部分を達成。
残るのは MLT の from-source ビルドと 2 回連続の再現確認（S13）。

---

## 検証済みの項目

### V1（再現ビルド）— 部分的に達成

- `bootstrap-msys2.ps1` → `build.ps1` で clean build が成功する
- 184 パッケージの version を `deps-lock.txt` に記録済み
- パッケージ実体 369 MB + 署名 184 件を凍結済み
- **達成**: 凍結パッケージからの復元をクリーンな MSYS2 ベースで検証（R0、9/9）
- **未達**: MLT の from-source ビルドと 2 回連続の再現確認は S13 で行う
- **未達**: 最小依存 closure の特定（現在の凍結物は環境スナップショット）

### ツールチェーン分離 — 達成

既存 Qt 6.8.3 (MSVC) を誤って拾わないことを、実際に弾かせて確認した:

```
CMake Error at cmake/mvm_toolchain_guard.cmake:66 (message):
  [mvm toolchain guard] Qt6_DIR が UCRT64 の外を指しています。
    実際:   c:/users/lambe/sdk/qt/6.8.3/msvc2022_64/lib/cmake/qt6
    必須:   c:/msys64/ucrt64/ 配下
    検出された既知の誤参照パターン: 'sdk/qt'
```

PATH 検査も同様に発火することを確認済み。既存 Qt 6.8.3 は削除・変更・移動していない。

### MLT モジュール解決 — 達成

`mvm_mlt_hello.exe` が exit 0。23 モジュール全てロード可能。

| | 件数 | Phase 0 で必要なもの |
| --- | --- | --- |
| producers | 32 | `avformat` `qimage` `xml` `color` |
| filters | 474 | `avfilter.scale` `affine` `crop` `brightness` `volume` `dynamictext` `qtext` |
| transitions | 7 | `qtblend` `mix` `composite` |
| consumers | 11 | `avformat` `sdl2` `null` |

profile `atsc_1080p_60` が `1920x1080 @ 60/1 (SAR 1/1)` として正しく解決される。

Qt は 6.11.1 / ABI `x86_64-little_endian-llp64` / prefix `C:/msys64/ucrt64` で起動を確認。

---

## S6 完了時点の判定（S7 で更新済み。下の「S7 完了時点の判定」を参照）

| 基準 | 実測 | 判定 |
| --- | --- | --- |
| **M3** | PiP・文字・マーカー・音声 mix のすべてを実測で確認（下表） | **合格** |
| **M4** | affine グラフで再測 214/214 一致、mismatch **0** | **合格** |
| **M5** | p50 154.3ms / **p95 223.8ms**（3 回中央値）/ **観測 max 476.4ms** | **不合格**（p95・max とも） |
| **M6** | monotonic display 契約で再測（8 条件 × 3 回）。**linear は 17〜30 updates/sec で合格、random は 7.0〜8.7 で不合格** | **条件付き**（下記） |

**M5 の max について:** max の中央値 275.7ms を「基準内」と扱わない。
**3 回を通して観測した max は 476.4ms** であり基準 400ms を超えている。
外れ値を除外して合格とするなら、warm-up 条件を明示的に定義してから
再測定する必要がある。現時点でその定義が無いので不合格とする。

**M6 について:** 表示契約を 2 度直した。詳細は
[seek-scrub-notes.md](research/seek-scrub-notes.md)。

1. 当初の `result.generation < lastAcceptedGeneration` は**判定式が成立せず**、
   古い結果をすべて表示していた（当時の 11.9〜61.6 updates/sec は使えない）。
2. 次に「`generation == latestSubmitted` のときだけ表示する」strict latest-only
   にしたところ、**投入間隔より decode が遅い間、入力が止まるまで画が出なくなった**
   （約 3 updates/sec）。coalescing は正しかったが、契約が UI 要求を満たさなかった。

現在は **monotonic display** 契約である。保証するのは
**「表示が巻き戻らないこと」**であって「常に最新であること」ではない。
最新でなくても表示中より新しい decode 結果は表示する（`DisplayLagging`）。
追従の遅れは `generation_lag` として別に測る。

**[事実] 契約の変更だけで linear の updates/sec が約 3 → 17〜30 になった。**
decode 速度は変えていない。約 3 という数字は
**decode 速度ではなく表示契約が作っていた。**

**[事実] random パターンはどの投入間隔でも基準を満たさない**（7.0〜8.7 ups、
p95 239〜276ms）。実際のスクラブ操作は linear に近いが、
マーカー間のジャンプやクリップ境界への移動は random に近い。
**条件を選べば合格する、という書き方で M6 を合格にはしない。**

**[未検証]** proxy 導入後の再測定（S7）。**M6 の最終判定は S7 の後に行う。**

### M3 の内訳（すべて loader 修正後の値）

| 項目 | 実測 | 判定 |
| --- | --- | --- |
| PiP 縮小配置 | 差分外接矩形 = `1260,700 640x360`（期待と完全一致）、矩形内差分 1.00 / 矩形外 0.00 | 合格 |
| 日本語文字描画 | qtext で矩形内差分 0.5685 / 矩形外 0.00。全角空白・数学記号・改行を目視確認 | 合格 |
| マーカー保持 | frame 0/1/137/299 すべて一致 | 合格 |
| 音声 A1+A2 の mix | mixed に 4 周波数すべて存在（下表） | 合格 |
| gain | 実測 **-6.00072dB**（許容 ±1dB） | 合格 |
| clipping / DC / NaN | clip 率 0、DC ~1e-6、peak 0.746、NaN/Inf なし | 合格 |

**M3 正式ゲートの実測値**（`render-audio` + `verify-audio`）:

| 出力 | L | R |
| --- | --- | --- |
| A1-only | 1000Hz=**0.4986**（SNR 5579） | 500Hz=**0.5014**（SNR 17981） |
| A2-only (-6dB) | 1500Hz=**0.2506**（SNR 1e6） | 750Hz=**0.2506**（SNR 1e6） |
| **mixed** | 1000Hz=**0.4986** + 1500Hz=**0.2505** | 500Hz=**0.5014** + 750Hz=**0.2506** |

### 素材の使い分け（周波数が 2 系統ある理由）

混同しやすいので明記する。**目的も素材も別である。**

| 用途 | 素材 | A1 | A2 |
| --- | --- | --- | --- |
| **M3 の正式ゲート** | `tests/assets/smoke/` の 5 トラックシナリオ | L 1000Hz / R 500Hz | L 1500Hz / R 750Hz |
| **原因切り分け（診断用）** | `tests/assets/smoke/_diag/` | L 997Hz / R 613Hz | L 1429Hz / R 823Hz |

診断用は整数倍関係を避けてある。1000/500 や 1500/750 では
2 倍高調波と他トラックのターゲットが重なり、
「高調波」と「別トラックからの漏洩」を区別できないためである。

### 原因（S5 で最も重要な所見）

**[事実]** 音声破損と volume filter クラッシュは**同一原因**だった。

> `mlt_factory_producer(profile, "avformat", path)` と service を直接指定していた。
> 正しくは `NULL`（= `loader`）を渡す。

`loader` は avformat producer に音声の正規化 filter を付ける。
これが無いと AAC/MP4 の音声が壊れ、`volume` filter でクラッシュする。
どちらも `producer -> consumer` の最小構成から再現し、
playlist / tractor / mix とは無関係だった。
`melt` が正常なのは既定が `loader` だからである。

詳細は [composition-notes.md](research/composition-notes.md) の所見 I。
この誤りを繰り返さないよう `scripts/lint.ps1` で機械的に禁止している。

### CTest の件数

| 区分 | 件数 |
| --- | --- |
| S5 音声で追加（`audio_*` / `render_audio_*` / `verify_audio_*`） | **17** |
| それ以前（doctor / probe / decode / verify-media / 破損素材など） | 50 |
| **合計** | **67** |

内訳: 最小グラフ切り分け 8（A〜H）、negative 2、render 4、verify 3。

**[事実]** PiP は `affine` transition + typed rect で解決した。
`qtblend` は typed API で設定・読み戻しが一致しても描画は正しくならない。

**[事実]** M4 は合成グラフを affine へ変更した後も 214/214 一致を維持した。
グラフ変更が seek 精度を壊していないことを確認済み。

**[事実]** M5 は affine 化で p95 が 232ms → 259ms とやや悪化した。
合成が正しくなった分の処理が増えたためと考えられる（**[推測]**）。

---

## 修正前・参考: loader 修正前に観測していた値

**以下はすべて `avformat` を直接指定していた頃の記録である。**
現在は再現しない。原因究明の経緯として残す。

音声（consumer 経由）:

| 対象 | L の主成分 | R の主成分 |
| --- | --- | --- |
| 素材 A1（ffmpeg で直接抽出） | 1000Hz = 0.4986 | 500Hz = 0.5015 |
| 素材 A2（WAV そのもの） | 1500Hz = 0.5000 | 750Hz = 0.5000 |
| A2-only（MLT 経由） | 1500Hz = 0.4999 | 750Hz = 0.4999 |
| **A1-only（MLT 経由）** | 1000Hz = **0.0013** | 500Hz = **0.0108** |
| **mixed（MLT 経由）** | 1500Hz = 0.223（A2 のみ） | 750Hz = 0.308（A2 のみ） |

素材は両方とも正しく、MLT を通した A1 側だけが壊れていた。

`volume` filter は当時どの構成でもクラッシュした。

| 設定 | 結果（当時） |
| --- | --- |
| gain 0dB（filter を付けない） | 成功 |
| `level="-6.0000"` | アクセス違反 |
| `level` + `mlt_filter_set_in_and_out` | heap 破壊 |
| `gain="-6.0000dB"` | アクセス違反 |

**いずれも producer service を `loader` にすることで解消した。**

---

## S5 / S6 の暫定判定（修復前・参考）

詳細は [composition-notes.md](research/composition-notes.md) と
[seek-scrub-notes.md](research/seek-scrub-notes.md)。

| 基準 | 内容 | 実測 | 判定 |
| --- | --- | --- | --- |
| **M3** | 動画2+音声2+文字1 が意図どおり合成され、日本語が正しく描画される | 5 トラック構造・マーカー保持は達成。**V2 の縮小配置ができない**。文字の合成内描画は未確認 | **未達** |
| **M4** | seek 要求フレームと取得フレームが 100% 一致 | 214/214 一致、不一致 0（ランダム 200 点含む） | **合格** |
| **M5** | seek p95 ≤ 150ms、max ≤ 400ms | p50 129ms / **p95 232ms** / max 307ms | **不合格**（p95） |
| **M6** | scrub ≥ 15 updates/sec、request→display p95 ≤ 200ms | coalescing は正常（最終要求は必ず表示、marker 不一致 0）。**updates/sec ≈ 3** | **不合格**（※） |

※ この約 3 updates/sec は strict latest-only 契約下の値であり、
**表示契約の変更によって無効になった**。上の「S6 完了時点の判定」を参照。

**[事実]** M5 と M6 は**一部**同一原因である。5 トラック合成の 1 フレーム取得に
130〜340ms かかることは random パターンの M6 不合格に直結する。
ただし当時「M6 の不合格は M5 と同じ原因である」と書いたのは**誤りだった**。
約 3 updates/sec の主因は decode 速度ではなく表示契約であり、
契約を直しただけで linear は 17〜30 updates/sec になった。

**[未検証]** 原因の切り分けは未完了。proxy・consumer による先読み・
MLT のキャッシュ設定・purge のいずれも試していない。
現時点の数値は「何も最適化していない状態」の値であり、
**MLT の限界を示すものではない。**

### S5 で見つかった新しい静かな失敗

**[事実]** `hide` をトラック（playlist）ではなく clip の producer に設定すると、
**音声トラックを 1 本足しただけで上位トラックの映像合成がまるごと無効化される。**
エラーも警告も出ず、出力は最下位トラックだけになる。

**[事実]** `qtblend` transition に `rect` を設定しないと、
b_track がまったく合成されない。これもエラーは出ない。

**[事実]** `qtblend` の *filter* 版に `rect` を設定して producer へ attach すると、
そのトラックが合成結果から消える。これもエラーは出ない。

**[事実]** `composite` transition に `geometry` を設定すると
**アクセス違反 (0xC0000005) でプロセスが落ちる。**

**[exit]** M15（抽象化可能性）への追加観点:
これら 4 つはいずれも「設定を間違えると、エラーではなく
静かに間違った絵が出る」類である。mvm の adapter 層は、
構築後に**実際に合成結果を検査する**手段を持たないと、
この種の事故を検出できない。

### 検証側の空振り（再発）

**[事実]** S4 に続き、S5 でも検証が偽陽性で通っていた。

- text 領域の分散検査は、背景のカラーバーだけで閾値を超える
- V2 の合成検査は、比較した 2 領域が元々別内容だったため常に通る
- 根本原因として、**V1(H.264) と V2(HEVC) が同一の testsrc2 から生成されていた**ため、
  全画面合成と縮小合成を画素から区別できなかった

**[回避策]** HEVC 素材を `smptehdbars` に変更し、V1 と区別可能にした。

**教訓**: 検証を書いたら「検証対象が存在しないときに本当に落ちるか」を先に確かめる。
テスト素材が互いに区別可能であることも前提条件である。

---

## 現時点の未検証事項

「できると仮定してはいけない」もの。

| 項目 | 予定 |
| --- | --- |
| 連続 seek と scrub の精度・速度（M4 / M5 / M6） | S6。今回は単発 decode の着地しか見ていない |
| 5 トラック合成、日本語文字レイヤの字形（V3） | S5 |
| 1080p60 の実効 preview 性能（M7） | S7 |
| 4K proxy 経由の preview（M8） | S7 |
| Benchmark モードの素材（60 秒）の生成 | 未生成。smoke のみ |
| HEVC 10bit のピクセル正当性 | probe は通ったが、マーカー照合をしていない |
| エフェクト 6 種（V7） | S8 |
| 書き出しと preview/final 一致（V8 / V12） | S9 |
| MLT の from-source ビルドと 2 回連続の再現（M1 残り） | S13 |
| 最小依存 closure | S13 または V11 |
| clean VM への staging（M13） | S14 |
| 日本語ユーザープロファイル配下での staging 配置 | V11 で方針決定 |

## 判定への影響（暫定）

現時点で MLT 不採用に傾く材料は無い。V2 は良好で、
**日本語どころか CP932 外の文字を含む素材パスまで、パッチ不要で動作する。**

一方、S1 の所見 1〜3 と S4 の所見 9〜10 は共通の性質を持つ。

> MLT は失敗しても失敗と分からない形で縮退する。

- モジュールが 0 件でも初期化は成功する
- profile が解決できなくても既定値を返す
- 静止画の尺は INT_MAX
- 音声のみの尺は profile 依存

さらに S4 では、**検証する側にも同じ危険がある**ことが分かった。
duration の計算式が原因で WAV のテストが空振りで通過していた。
「一致した」ではなく「本当に比較したか」まで確認する必要がある。

exit criteria の M15（抽象化可能性）を評価する際は、
「mvm の adapter 層が MLT の静かな失敗を検知して上位に伝えられるか」を
明示的な観点として加えること。
`mvm_bench doctor` が negative test 5 件で実証している通り、
検知自体は可能である。

## 実行したコマンド（再現用）

```powershell
# 依存導入と凍結
pwsh scripts/bootstrap-msys2.ps1
pwsh scripts/freeze-deps.ps1

# ビルド
pwsh scripts/build.ps1 -Preset ucrt64-release
pwsh scripts/build.ps1 -Preset ucrt64-debug

# R0: 凍結復元検証（クリーンな MSYS2 ベースを新規取得して実施）
pwsh scripts/verify-frozen-restore.ps1

# S2: 検証素材
pwsh scripts/make-testmedia.ps1 -Mode Smoke
pwsh scripts/make-testmedia.ps1 -Mode Smoke -Force -VerifyRegeneration

# S4: 検証
build/ucrt64-release/bin/mvm_bench.exe doctor
build/ucrt64-release/bin/mvm_bench.exe probe tests/assets/smoke/v1080p60_h264.mp4
build/ucrt64-release/bin/mvm_bench.exe verify-media tests/assets/smoke/manifest.json
ctest --output-on-failure        # build/ucrt64-release と build/ucrt64-debug の両方
```
