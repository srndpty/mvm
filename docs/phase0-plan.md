# mvm Phase 0 — MLT 技術スパイク計画

> **進捗**: S0（リポジトリ初期化）と S1（UCRT64 依存導入 + prebuilt MLT hello world）が完了。
> S2 以降は未着手。実測結果と S1 で判明した MLT の挙動は
> [phase0-findings.md](phase0-findings.md) を参照。
>
> S1 で計画から変わった点:
> - `toolchain` は group で pin できないため実パッケージ（gcc / binutils / gdb / make）に変更
> - MLT モジュールの実行時依存 6 件（libebur128 / fftw / libsamplerate / rubberband / sox / rtaudio）を
>   必須パッケージに追加。MSYS2 では optional 扱いだが、欠けると `affine`（V7）と
>   `dynamictext`（V3）が消える
> - MSYS2 のモジュール配置は `lib/mlt`・`share/mlt`（上流ドキュメントの `lib/mlt-7` ではない）
> - `scripts/build.ps1` を追加（PATH を整えてから cmake を呼ぶ。無いと gcc が無言で失敗する）

## Context

mvm は YouTube 向け技術・数学解説動画を主対象とする個人用ノンリニア動画編集ソフト（C++20 / Qt 6 / QML / Windows 11）。Premiere Pro を操作上のベンチマークとしつつ、タイムライン編集・自動字幕・数式アニメーションを単純な操作で行うことを優先する。

編集・プレビュー・書き出しエンジンを内製するか既存フレームワークに載せるかは、製品全体のコスト構造を決める最大の分岐点である。第一候補である **MLT 7** は Shotcut / Kdenlive の実績があるが、Windows における公式ビルドは MSYS2 / mingw-w64 前提であり、MSVC ビルドは上流非サポートである。したがって「MLT が使えるか」は、機能の可否だけでなく **Windows でのビルド・配布・ABI の可否** を含めて判定する必要がある。

Phase 0 の唯一の成果物は **MLT 採用可否の判断根拠** である。製品 UI も本番用 Project Model も作らない。スパイクコードは判断後に破棄されうる前提で書く。

### 確定済みの方針（ユーザー決定）

- ツールチェーンは **MSYS2 UCRT64 に統一**する。Qt / MLT / FFmpeg / アプリ本体で CRT および C++ ABI を混在させない。
- MLT へのアクセスは mvm 内部の media engine adapter から **MLT C API (`mlt_*`) のみ**で行う。Mlt++ の型を Project Model・UI・公開 interface へ露出させない。
- Phase 0 では MSVC との ABI 接続、およびエンジンの別プロセス化を **実装しない**（設計上の退避経路としてのみ記録する）。
- ビルドに用いた MSYS2 package 名と正確な version を記録する。
- clean environment 検証は **クリーン Windows 11 の Hyper-V VM を正式な exit gate** とする。性能・NVENC・latency 測定は RTX 4090 搭載ホストで行い、VM は性能判定に使わず、依存 DLL / plugin / resource / codec / PATH 汚染の検出にのみ使う。VM には MSYS2・Qt・MLT・FFmpeg・開発ツールを入れない。staging 成果物のみをコピーし、日本語を含むパスから起動・読み込み・短い CPU export を確認する。

### 現状の環境（調査済み）

#### Phase 0 で使う環境

| 項目 | 状態 |
| --- | --- |
| `c:\dev\soft\mvm` | 空。git 未初期化 |
| MSYS2 | `C:\msys64` 導入済。`mingw64` / `ucrt64` / `clang64` ルートあり。**Qt6・MLT は未導入** |
| Phase 0 用 Qt | **`C:\msys64\ucrt64`（pacman で導入する）**。`C:\Qt` は作成しない |
| CMake / Ninja | **MSYS2 の `mingw-w64-ucrt-x86_64-cmake` / `-ninja` を使う**。ホストの pip 版 cmake (`Python310\Scripts\cmake.exe`) は使わない |
| GPU | RTX 4090 / driver 591.86 / NVENC h264・hevc・av1 利用可 |

#### 保持するが Phase 0 では一切参照しない環境

| 項目 | 状態 | 扱い |
| --- | --- | --- |
| **既存 Qt 6.8.3** | `C:\Users\lambe\sdk\Qt\6.8.3\msvc2022_64` | **他プロジェクト用として保持。削除・変更・移動を行わない。** MSVC ビルドのため UCRT64 とは ABI・CRT の両面で非互換 |
| Visual Studio | 2019 / 2022 導入済 | Phase 0 では未使用 |
| FFmpeg CLI | `C:\tools`（n6.1 系 2023-10）、winget（8.1.2） | 参照実装（比較用）としてのみ使用。リンク対象にしない |
| vcpkg | 未導入 | 本方針では不要 |

現時点で `CMAKE_PREFIX_PATH` / `Qt6_DIR` / `QTDIR` はいずれも未設定、PATH にも Qt のエントリ無し（調査時点で汚染なし）。ただし他プロジェクトの作業で将来設定されうるため、**受動的に「今は綺麗だから大丈夫」とせず、能動的な検証で担保する**（下記）。

#### Qt の取り違えを防ぐ仕組み（必須）

既存 Qt 6.8.3 を誤って拾うと、**リンクは通るのに実行時に不可解なクラッシュを起こす**（MSVC ABI と mingw ABI の混在）。原因究明が極めて困難な事故になるため、機械的に防ぐ:

- `CMakePresets.json` の `ucrt64-*` preset で `CMAKE_PREFIX_PATH` に `C:/msys64/ucrt64` を明示し、`CMAKE_FIND_ROOT_PATH` でも同ディレクトリに限定する
- preset は `CMAKE_PREFIX_PATH` / `Qt6_DIR` / `QTDIR` を**環境から継承せず明示的に上書き**する
- configure 時に以下を検証し、違えば `FATAL_ERROR` で失敗させる:
  - `Qt6::Core` の `IMPORTED_LOCATION` 実体が `C:/msys64/ucrt64` 配下であること
  - `CMAKE_CXX_COMPILER` が `C:/msys64/ucrt64` 配下であること
  - `pkg-config` の解決先が `C:/msys64/ucrt64` 配下であること
  - `Qt6_DIR` に `sdk/Qt` や `msvc` が含まれていないこと
- 解決された Qt / compiler / pkg-config のフルパスを configure ログに必ず出力する
- VSCode は MSYS2 UCRT64 版 cmake を使う（`.vscode/settings.json` で `cmake.cmakePath` を明示。CMake Tools の kit 自動検出に任せない）

### 明記する仮定

1. Phase 0 は **10 稼働日**を上限の time box とする。超過した時点で、達成済み項目のみで採否を判定する。
2. 想定する最大編集規模は 1080p60、動画 2 層 + 音声 2 層 + 文字 1 層、尺 20 分程度。4K は素材としてのみ扱い、proxy 前提とする。
3. 配布はインストーラを作らず、staging フォルダをそのままコピーする方式で Phase 0 は判定する（署名・MSI は範囲外）。
4. FFmpeg は MSYS2 の `mingw-w64-ucrt-x86_64-ffmpeg` を単一のソースとする。ホスト上の 2 つの CLI ビルドは**参照実装（比較用）としてのみ**使い、mvm のリンク対象にはしない。ffprobe は MSYS2 版を staging に同梱する。
5. Python worker は Phase 0 で実装しない。ただし「生フレームを渡さない」制約を守れる境界（ファイルパス / PNG / JSON のみ）を設計メモとして残す。
6. MLT の version は spike 開始時点の MSYS2 提供版を基準とし、from-source ビルドでは同一 tag に pin する。

---

## 1. 調査すべき既存コード・公式資料

判断の質はここの読み込み量でほぼ決まる。実装より先に **S1 と並行して**進める。

### MLT 本体（最重要）

| 対象 | 見るべき理由 |
| --- | --- |
| `mltframework.org/docs/framework/` | `mlt_producer` / `mlt_consumer` / `mlt_tractor` / `mlt_multitrack` / `mlt_playlist` / `mlt_transition` / `mlt_filter` / `mlt_profile` の関係。**mvm の Project Model がどこにマップされるか**の答えがここ |
| `src/framework/mlt_consumer.c` | `real_time` プロパティの実装。フレームドロップとスレッド構成。プレビュー性能の上限を決める |
| `src/framework/mlt_producer.c` の `seek` / `position` | seek の実体。`mlt_producer_seek` が何をするか、consumer 側の purge が必要か |
| `src/modules/avformat/` (`producer_avformat.c`) | H.264 / HEVC のデコード経路、`threads` / `seekable` / `noimagecache` プロパティ、hw decode の有無 |
| `src/modules/avformat/consumer_avformat.c` | export のプロパティ名。`vcodec=h264_nvenc` が通るか、`preset` / `rc` などの pass-through 方法 |
| `src/modules/qt/` (`producer_qtext`, `filter_qtext`, `transition_qtblend`) | 文字レイヤと合成。**日本語グリフのシェーピング**がここに依存する |
| `src/modules/plus/filter_affine.c`, `transition_affine.c` | transform / scale / crop の実体と `mlt_rect` の座標系 |
| `src/modules/core/` (`filter_brightness`, `filter_gain`/`volume`, `filter_crop`, `transition_composite`) | opacity / fade / audio gain の担当モジュール特定 |
| MLT XML DTD / `mlt_xml` モジュール | **正本にはしないが**、デバッグ時のダンプ形式として使う。読める必要はある |
| `melt` CLI ソース | 最小の consumer 駆動サンプル。S2 のひな形 |

### 実アプリ実装（Qt + MLT の生きた参照）

| 対象 | 見るべき理由 |
| --- | --- |
| Shotcut `src/mltcontroller.cpp` | Qt アプリから MLT を駆動する最も完成された例。profile 管理、consumer 生成、seek の扱い |
| Shotcut `src/glwidget.cpp` / `src/qmltypes/` | **MLT フレームを Qt の GL テクスチャに載せる方法**。Phase 0 の preview 実装の直接の参考 |
| Shotcut `src/models/multitrackmodel.cpp` | マルチトラックを MLT の tractor にどう写像しているか。逆に **mvm が真似すべきでない密結合**の実例でもある |
| Shotcut `src/jobs/` | 長時間処理（proxy 生成・export）をどう UI スレッドから外しているか |
| Kdenlive `src/mltcontroller/` / `src/monitor/glwidget.cpp` | 2 つ目の独立した実装。Shotcut の設計が MLT 由来か Shotcut 由来かを切り分けられる |
| Kdenlive の proxy 実装 | proxy 差し替えを「プロジェクト側」で行う設計例 |

### 周辺

- Qt 6: `QQuickItem` / `QSGTexture` / `QQuickWindow::beforeRendering` / `QVideoSink` / `QAudioSink`、および `QThread` と `Qt::QueuedConnection` の設計指針
- FFmpeg: NVENC ドキュメント（`h264_nvenc` の `preset` / `tune` / `rc` / `cq`）、`ffprobe -show_streams -of json`
- MSYS2: `ucrt64` パッケージ命名規則、`pacman` のローカルキャッシュとパッケージ pin 手法
- Windows: `GetShortPathNameW`、UTF-8 manifest（`activeCodePage`）、`SetDllDirectory` と DLL 検索順序

**成果物**: `docs/research/mlt-notes.md`（MLT の型と mvm の概念の対応表を含む）

---

## 2. 推奨ディレクトリ構成

```
mvm/
  CMakeLists.txt
  CMakePresets.json                 # ucrt64-debug / ucrt64-release
  .gitignore  .gitattributes  .editorconfig  .clang-format
  README.md
  cmake/
    FindMLT.cmake                   # pkg-config 経由の薄いラッパ
    mvm_warnings.cmake
  docs/
    adr/0001-media-backend-choice.md
    research/mlt-notes.md
    phase0-report.md                # ← 最終判定書
  scripts/
    bootstrap-msys2.ps1             # pacman で依存導入 + version 記録
    freeze-deps.ps1                 # .pkg.tar.zst を third_party/pkgs へ退避
    make-testmedia.ps1              # 検証素材の決定論的生成
    stage.ps1                       # staging フォルダ構築（DLL/plugin/resource）
    verify-staging.ps1              # VM 上で走らせる検証スクリプト
  third_party/
    pkgs/                           # 凍結した MSYS2 パッケージ（再現性の担保）
    LICENSES/
  src/
    core/                           # Qt 非依存・MLT 非依存
      Time.h  Rational.h  Result.h  Log.h  Uuid.h
    project/                        # mvm 独自 Project Model（純データ）
      Project.h  Track.h  Clip.h  Effect.h  ProjectJson.cpp
    media/
      IMediaEngine.h                # backend 交換点
      ITimelinePlayer.h  IRenderJob.h  IMediaProbe.h
      MediaTypes.h                  # mvm 独自の型のみ
      mlt/                          # ★ mlt_*.h を include してよい唯一の場所
        MltEngine.cpp  MltTimelineBuilder.cpp  MltFrameSource.cpp
        MltProps.cpp                # 文字列プロパティ名の集中管理
      probe/
        FfprobeMediaProbe.cpp
    platform/
      WinPaths.cpp                  # UTF-8 / wide / short path 変換
    app/                            # Qt/QML スパイクシェル
      main.cpp  PreviewItem.cpp  SpikeController.cpp
    ui/qml/
      Main.qml  PreviewPane.qml  ScrubBar.qml
  tests/
    unit/                           # GoogleTest or Catch2
    guard/
      test_no_mlt_leak.cmake        # media/mlt 以外での mlt_ 参照を検出
    harness/
      mvm_bench.cpp                 # Qt 非依存の性能計測 CLI
    assets/                         # 生成物。git 管理外
  bench/
    scenarios/*.json                # 計測シナリオ定義
    results/                        # 計測結果 CSV/JSON（コミットする）
```

### 構成上の要点

- **`src/media/mlt/` 以外に `mlt_` を出さない。** これを CI のテキスト検査で機械的に強制する（§7）。制約「Project Model を MLT に依存させない」の唯一の実効的な担保。
- `src/core` と `src/project` は Qt にも MLT にも依存しない静的ライブラリにする。単体テストが常に可能になる。
- `MltProps.cpp` に MLT のプロパティ文字列を全て集約する。MLT の version 差分による破壊が 1 ファイルに閉じる。
- `bench/results/` はコミットする。判定は主観ではなく記録された数値で行う。

---

## 3. dependency 取得・ビルド方式の候補比較

決定は済んでいる（A 案）。ここでは **なぜ他を採らないか**と、A の弱点への対処を記録する。

| 方式 | 内容 | 判定 |
| --- | --- | --- |
| **A. MSYS2 UCRT64 統一（採用）** | `pacman` で qt6-base/qt6-declarative/mlt/ffmpeg/cmake/ninja/gcc を導入。mvm も ucrt64 gcc でビルド | **採用**。Phase 0 の目的（MLT の可否判定）に最短で到達する。ABI 混在ゼロ |
| B. MSVC + mingw 製 MLT DLL を C ABI 境界で接続 | Qt/アプリは MSVC | Phase 0 では不採用。判定が遅れる。C API 限定設計により **将来の移行余地は保持**される |
| C. MLT を MSVC で from source ビルド | 上流非サポート。POSIX 依存の除去が必要 | 不採用。工数が非有界 |
| D. エンジン別プロセス + IPC/shm | ABI 完全分離 | Phase 0 では不採用。ただし配布・安定性の観点で Phase 1 以降の有力案として ADR に残す |
| E. vcpkg / Conan | MLT の port が未成熟 | 不採用 |

### A 案の弱点と対処

| 弱点 | 対処 |
| --- | --- |
| MSYS2 は rolling repo で、古い version が消える → **再現性が壊れる** | `scripts/freeze-deps.ps1` で導入した `.pkg.tar.zst` を `third_party/pkgs/` に退避。`bootstrap-msys2.ps1` は「凍結パッケージから復元」モードを持つ。導入直後に `pacman -Q` の全出力を `docs/deps-lock.txt` として記録 |
| VS デバッガが使えない | gdb + VSCode の `cppdbg` (MinGW) 構成を `.vscode/launch.json` に用意。§8 の検証にも使う |
| **既存 Qt 6.8.3 (MSVC) / pip 版 cmake / `C:\tools\ffmpeg.exe` を誤って拾う** | ビルドは MSYS2 UCRT64 シェル内で完結させる。CMake 構成時に compiler・Qt6::Core・pkg-config の解決先をログに出し、`C:/msys64/ucrt64` 配下でなければ configure を失敗させる（上記「Qt の取り違えを防ぐ仕組み」） |
| Qt6 の QML モジュールが多数の DLL / qml ディレクトリに分散し、staging 漏れが起きる | `windeployqt` は mingw Qt では信頼度が落ちるため、**`ldd` ベースの依存収集 + MLT の `lib/mlt-7` モジュール一括コピー**を `stage.ps1` に実装し、VM で検証する（§8） |

### 導入パッケージ（初期リスト）

`mingw-w64-ucrt-x86_64-` プレフィクス:

- ツールチェーン: `toolchain`, `cmake`, `ninja`, `pkgconf`
- Qt: `qt6-base`, `qt6-declarative`, `qt6-shadertools`, `qt6-multimedia`, `qt6-svg`, `qt6-5compat`
- メディア: `mlt`, `ffmpeg`, `SDL2`, `frei0r-plugins`
- テスト: `gtest`

`qt6-svg` はアイコン・ロゴ素材と MLT の Qt モジュール、`qt6-5compat` は MLT の Qt モジュールが依存しうる Qt5Compat（`QRegExp` 等）のために初期から入れる。

---

## 4. 最小限の C++/Qt/MLT スパイク構成

段階を分ける。**各段が独立に判断材料を生む**ようにし、途中で止まっても無駄にならない構成にする。

### Stage 1 — `mvm_bench`（Qt 非依存 CLI）

- `mlt_factory_init` → `mlt_profile` 生成 → producer/playlist/tractor 構築 → consumer 駆動
- consumer は `sdl2`（目視）と `null`（計測）を切り替え可能
- 標準出力に 1 フレームごとの timestamp を出し、CSV で `bench/results/` へ
- **これが検証項目の大半（読み込み・合成・seek・性能・export・NVENC・日本語パス）を担う**。Qt を挟まずに MLT 単体の限界を測れるのが要点

### Stage 2 — `mvm_spike`（Qt6 / QML シェル）

- QML: プレビュー矩形、再生/停止、スクラブ用スライダ、フレーム番号表示のみ
- `PreviewItem` は `QQuickItem` 派生。ワーカースレッドが `mlt_frame_get_image()` で `mlt_image_rgba` を取得 → `QSGTexture` へアップロード → `update()`
- 音声は MLT の `sdl2_audio` consumer に任せ、映像はプル。**音声クロックを基準**にして映像を追従させる
- UI スレッドでは decode / encode / ファイル I/O を一切行わない。これを §7 の自動検査で担保

### Stage 3 — `IMediaEngine` 抽象化の実証

```
IMediaEngine
  probe(path) -> MediaInfo                 // mvm 独自型のみ
  buildPlayer(const Timeline&) -> ITimelinePlayer
  startRender(const Timeline&, RenderSpec) -> IRenderJob

ITimelinePlayer
  play() / pause() / seek(FrameIndex) / setScrubMode(bool)
  frameReady(FrameIndex, ImageRef)          // シグナル相当
```

- `Timeline` は mvm 独自の純データ構造。MLT の型・プロパティ名を一切含まない
- `MltEngine` がこれを MLT の tractor へ変換する。**変換は一方向**（Project Model → MLT）。MLT XML から読み戻さない
- 検証として **`NullEngine`**（黒フレームを返すだけ）を実装し、`mvm_spike` が MLT 抜きで起動できることを示す。これが「backend 交換可能」の実証になる

### スパイクで書かないもの

タイムライン UI ウィジェット、undo/redo、プロジェクト保存の本実装、エフェクト UI、キーフレーム編集。

---

## 5. 各検証項目の具体的な実験方法

### 検証素材（`scripts/make-testmedia.ps1` で決定論的に生成）

| ID | 内容 |
| --- | --- |
| `v1080p60_h264` | 60s, 1080p60, H.264, `testsrc2` + タイムコード焼き込み + 1kHz サイン波 |
| `v1080p60_hevc` | 同上を HEVC で |
| `v4k60_h264` | 60s, 3840x2160p60, H.264, 高ビットレート |
| `v4k60_hevc10` | 60s, 4K, HEVC 10bit（デコード限界の確認用） |
| `png_alpha` | アルファ付き PNG（ロゴ / 数式画像を模す） |
| `wav_48k` | 48kHz/16bit ステレオ、既知のトーン列 |
| `jp_path/` | `素材\日本語 テスト\第1回 微分積分\` 配下に上記を複製 |

タイムコード焼き込みは必須。**preview と final の一致確認、A/V 同期確認の両方がこれで検証可能になる。**

### V1. Windows 上で MLT を再現可能にビルドできる

1. `bootstrap-msys2.ps1` を実行 → `docs/deps-lock.txt` 生成 → `freeze-deps.ps1` でパッケージ凍結
2. **prebuilt MLT で先に Stage 1 を通す**（数時間で可否の一次情報を得る）
3. その後 MLT を tag pin して from source ビルドし直し、`scripts/build-mlt.ps1` として手順を固定
4. **クリーン clone + 凍結パッケージ復元から 2 回連続でビルド成功**することを確認（1 回目の副作用に依存していないことの証明）

### V2. H.264 / HEVC / PNG / WAV 読み込み

- `mvm_bench --probe <file>` で `producer_avformat` の解決結果と `length` / `fps` / `sample_aspect` を出力
- 同一ファイルを `ffprobe -of json` でも解析し、**両者の frame count・duration・fps を突き合わせる**。ズレは編集の尺ズレに直結するため、ここでの不一致は重大所見として記録
- PNG はアルファチャンネルが保持されるか（`mlt_image_rgba`）を出力 PNG で確認
- WAV は `mlt_frame_get_audio` のサンプルレート / チャンネル数 / サンプル値を確認

### V3. 動画 2 層 + 音声 2 層 + 文字 1 層の合成

- `mlt_tractor` に 5 トラック。V2 に `qtblend` transition、A2 に `mix` transition、文字は `dynamictext` または `qtext`
- **日本語文字列（漢字・かな・全角記号）を必ず含める。** 豆腐 / 文字化けは MLT の Qt モジュールと fontconfig の問題であり、この製品では blocking 級の所見
- 合成結果を PNG 連番で出力し、各層の寄与を目視 + ピクセル値の点検査（既知座標の RGBA を assert）で確認

### V4. 任意フレームへの seek と連続 scrub

- **seek**: ランダムに 200 フレーム選び、`seek 要求 → 対応フレーム取得完了` までの実測を記録。p50 / p95 / max を出す
- キーフレーム直後 / GOP 末尾 / クリップ境界 / 尺の末尾を意図的に含める
- **scrub**: 30 秒間、スライダを模した連続 seek を最大速度で発行し、達成できた更新レート（updates/sec）と「要求を捨てられたか」を測る。捨てるのが正しい挙動なので、**最新要求で古い要求を潰す coalescing** を実装したうえで測る
- 取得フレームの焼き込みタイムコードを OCR ではなく **1 フレーム内の既知パターン**で自己申告させ、要求フレームと一致するかを検証（seek 精度の確認。ここが不正確な NLE は使い物にならない）

### V5. 1080p60 のニアリアルタイム preview

- V3 の 5 トラック構成を 60 秒再生。`real_time=1`（1 スレッド）と `real_time=-N`（N=4,8,16、ドロップ許容）の両方で計測
- `null` consumer（MLT 単体の上限）と Stage 2 の Qt 経路の両方で計測し、**差分が Qt 統合のコストである**ことを明示
- ドロップフレーム数を consumer から取得（`mlt_consumer` の frame drop 統計、なければ position の飛びで計数）

### V6. 4K からの proxy 生成と preview

- `ffmpeg -i 4k.mp4 -vf scale=960:540 -c:v h264_nvenc -g 12 -c:a aac proxy.mp4`（all-intra 版 `-g 1` も比較）
- proxy 生成時間、ファイルサイズ、scrub 更新レートを original と比較
- **重要な設計確認**: proxy への差し替えは mvm の Project Model 側の path resolver で行い、MLT へ渡す時点で既に proxy パスになっていること。MLT XML に proxy 情報を持たせない
- proxy ↔ original の切り替えで **フレーム番号がずれない**ことを検証（尺・fps が一致しているか）

### V7. transform / scale / crop / opacity / fade / audio gain

各エフェクトについて「MLT のどのモジュールか」「プロパティ名」「座標系・値域」「アニメーション記法」を表にまとめ、以下を実施:

- 静的値で適用 → 出力 PNG の既知座標のピクセルを assert
- 時間変化（fade in/out、位置アニメーション）を `mlt_animation` 記法で適用 → 開始・中間・終了フレームで検証
- audio gain は出力 WAV の RMS を測定して dB 差を検証
- **座標系の定義を必ず文書化する**（左上原点か中心原点か、正規化か絶対ピクセルか、SAR の扱い）。ここが曖昧な backend は UI 設計を壊す

### V8. H.264 / AAC 書き出し

- `consumer_avformat` で `vcodec=libx264 acodec=aac`。1080p60 / 60 秒
- 出力を ffprobe で検証: codec、profile、pix_fmt、fps、duration、音声サンプル数
- VLC と Chrome で再生確認（コンテナ / moov atom の健全性）
- **一時ファイル → 完成ファイルの原子的リネーム**を実装。`.mvmtmp` 拡張子で書き、成功時のみ最終名へ `MoveFileEx`。途中でプロセスを kill して、中途半端な最終ファイルが残らないことを確認

### V9. NVENC 書き出し

- `vcodec=h264_nvenc`、および `hevc_nvenc` を試す
- MLT が任意の encoder オプションを pass-through できるか（`preset`、`rc`、`cq`、`b:v`）を確認。**できない場合は所見として記録**（品質制御ができない encoder は実質使えない）
- x264 との比較: エンコード時間、ファイルサイズ、VMAF スコア（`ffmpeg -lavfi libvmaf`）
- NVENC セッション数制限、および他アプリと同時実行時の失敗挙動を確認

### V10. 日本語を含む Windows パスの処理

段階的に検証する。どこで壊れるかの特定が目的:

1. 素材ファイルパスに日本語（`jp_path/`）→ `producer_avformat` で開けるか
2. 出力先パスに日本語 → consumer が書けるか
3. **フォルダ名だけでなくファイル名にも**日本語・全角スペース・絵文字・`＆` を含める
4. ユーザープロファイル配下（`C:\Users\<日本語名>\`）を junction で模擬
5. MLT XML ダンプに日本語パスを書き出し、読み戻せるか（デバッグ用途として）
6. mvm 自身のログ・エラーメッセージで文字化けしないか

**壊れた場合の対処候補**（工数順）: (a) アプリマニフェストで `activeCodePage=UTF-8` を指定 → (b) MLT へ渡す直前に `GetShortPathNameW` で 8.3 名へ変換（ボリュームが 8.3 名生成を無効化している可能性に注意） → (c) ASCII 作業ディレクトリへの junction を自動生成 → (d) MLT にパッチ。**(d) が必要なら MLT 採用の大きな減点**とする。

### V11. clean Windows 環境への配置と起動

- `stage.ps1` が `staging/` を構築: `mvm_spike.exe`、Qt DLL、QML モジュールディレクトリ、`libmlt-7.dll`、**MLT モジュール群 (`lib/mlt-7/`)、MLT の presets/profiles/data**、FFmpeg DLL、SDL2、gcc ランタイム (`libstdc++-6.dll`, `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`)、`ffprobe.exe`
- `MLT_REPOSITORY` / `MLT_DATA` / `MLT_PROFILES_PATH` を **実行ファイル相対で自プロセス内から設定**する（環境変数に依存させない）
- Hyper-V のクリーン Win11 VM へ `staging/` のみコピー。**日本語を含むパス**（例: `D:\動画編集\mvm 検証\`）から実行
- VM 上で `verify-staging.ps1` を実行: 起動 → 日本語パスの素材読み込み → 数秒の CPU export → 終了コード検証
- 失敗時は VM 上で Dependencies / `Process Monitor` により欠落 DLL・欠落リソースを特定し、`stage.ps1` へ反映して再試行。**VM を汚さないため毎回スナップショットへ戻す**

### V12. preview と final render の見た目一致

Phase 0 で最も見落とされやすく、最も重要な検証。

1. 同一タイムラインを (a) preview 設定でフレームダンプ、(b) final render 設定でエンコード後にデコードしてフレームダンプ
2. 同一フレーム番号同士を `ffmpeg -lavfi ssim` / `psnr` で比較
3. **一致を壊しうる要因を個別に潰す**: `mlt_profile` の解像度・fps・SAR・`progressive`、consumer の `rescale`（preview で `nearest`、render で `bicubic` になっていないか）、`mlt_image_format`（rgba vs yuv422）、color range（full/limited）、`deinterlace_method`、`real_time` によるフレーム欠落
4. **幾何は完全一致を要求する**（transform の位置が数ピクセルずれるのは許容しない）。色は圧縮由来の差を許容
5. A/V 同期: 焼き込みタイムコードとトーンの位相から、preview と final のオフセット差を測る

---

## 6. 計測する性能指標

全て `bench/results/*.csv` に記録し、判定は数値で行う。

### スループット
- preview 実効 fps（1080p60 / 5 トラック、`real_time` 各設定、null consumer / Qt 経路）
- ドロップフレーム率（%）
- 再生開始から fps が安定するまでの時間（s）
- render 速度（realtime 比）: x264 / NVENC、1080p60 / 4K
- proxy 生成速度（realtime 比）

### レイテンシ
- seek 完了時間 p50 / p95 / max（ms）— GOP 位置別に分類
- scrub 更新レート（updates/sec）と要求→表示レイテンシ p95
- アプリ起動から最初のフレーム表示まで（ms）
- タイムライン変更（クリップ移動）から preview 反映まで（ms）

### UI 応答性（Qt 経路のみ）
- UI スレッドのフレーム時間 p95 / max（ms）— **max > 100ms のイベントは即座に記録し原因を特定**
- 再生中の UI スレッドブロック回数（16.7ms 超過フレーム数）

### リソース
- ピーク RSS（MB）、1080p60 preview 時 / 4K proxy preview 時
- VRAM 使用量（`nvidia-smi` サンプリング）
- CPU 使用率平均、スレッド数
- 30 分連続再生でのメモリ増加量（**リーク検出**。MLT のフレームキャッシュ設定の妥当性確認）

### 品質
- preview vs final の SSIM / PSNR（フレーム単位の最小値と平均）
- NVENC vs x264 の VMAF（同ビットレート）
- A/V 同期オフセット（ms）

### 開発コスト（判定の材料として明示的に記録する）
- clean 環境からのビルド所要時間
- 各検証項目に実際に費やした時間、および回避策 (workaround) を要した項目数
- **MLT にパッチが必要になった箇所の数**

---

## 7. 自動化するテストと手動確認項目

### 自動テスト（CI / ローカルで毎回）

| 種別 | 内容 |
| --- | --- |
| 単体 | `core` / `project` の時間演算、Rational fps、フレーム↔タイムコード変換、Project Model の JSON round-trip |
| 単体 | `MltTimelineBuilder` が Timeline → MLT 構造へ正しく変換すること（MLT XML をダンプして構造を assert。**XML は検証用途にのみ使う**） |
| **アーキテクチャ検査** | `src/media/mlt/` 以外のファイルに `mlt_` / `#include <mlt` / `Mlt::` が出現しないことを検査。違反でビルド失敗 |
| **アーキテクチャ検査** | `src/core` / `src/project` が Qt にも MLT にもリンクしていないことを CMake のターゲット依存から検査 |
| **アーキテクチャ検査** | `src/app` / `src/ui` に `fopen` / `std::ifstream` / `QFile` の直接使用がないこと（I/O は media 層経由） |
| 統合 | `mvm_bench` による V2 / V3 / V7 のピクセル assert（黄金画像との SSIM ≥ 0.995） |
| 統合 | V12 の preview / final 一致検査を短尺（5 秒）で自動実行 |
| 統合 | V10 の日本語パス読み書き（短尺） |
| 統合 | export 中断時に完成ファイルが生成されないこと（原子的リネームの検証） |
| 性能回帰 | `mvm_bench` の主要指標を CSV 出力し、閾値を下回ったら警告（Phase 0 では fail にしない） |
| 実行時 assert | UI スレッド上で decode/encode/ファイル I/O を呼んだら abort する debug ガード（thread id チェック） |
| メモリ | 5 分再生での RSS 増加が閾値以下（ASAN は mingw では制約があるため RSS 監視で代替） |

### 手動確認（チェックリスト化して `docs/phase0-report.md` に記録）

- 再生の**体感**の滑らかさ（数値上のfpsと体感が乖離することがある）
- scrub の追従感 — Premiere Pro と並べて比較する
- 音声のプチノイズ、再生開始/停止時のクリック音
- 日本語文字レイヤの字形・行間・縦組みの可否
- 4K proxy と original を切り替えたときの視覚的整合
- VM 上での初回起動時間、ウイルス対策ソフトによる警告の有無
- エラー時のメッセージが原因究明に足るか（MLT のエラーが握り潰されないか）

---

## 8. Windows 配布上のリスク

| リスク | 影響 | Phase 0 での検証・緩和 |
| --- | --- | --- |
| **MLT モジュールの動的ロード失敗** | 起動はするが producer/filter が解決できず、原因が分かりにくい形で機能欠落 | `MLT_REPOSITORY` を実行ファイル相対で設定。起動時に `mlt_repository` の登録済みモジュール一覧をログ出力し、期待リストと突合する自動検査を入れる |
| **Qt6 QML モジュールの staging 漏れ** | VM で起動失敗・空白 UI | mingw Qt では `windeployqt` を信用せず、`ldd` + QML import ツリーの両方から収集。VM で毎回検証 |
| **mingw ランタイム DLL 漏れ** | 起動時に DLL エラー | `libstdc++-6` / `libwinpthread-1` / `libgcc_s_seh-1` を明示的に staging。VM で検証 |
| **MSYS2 rolling による再現不能** | 数か月後にビルドできない | `third_party/pkgs/` へパッケージ凍結 + `deps-lock.txt` 記録（§3） |
| **ライセンス（GPL）** | MLT 本体は LGPL だが、frei0r / x264 / 一部モジュールは GPL。同梱すると配布物全体が GPL に | Phase 0 で **同梱する各コンポーネントのライセンス一覧を `third_party/LICENSES/` に作成**。個人利用のみなら実害はないが、将来の配布方針を縛るため ADR に記録。x264 を外して NVENC のみにする選択肢も評価 |
| **NVENC ドライバ依存** | 古いドライバ / 非 NVIDIA GPU で export 不能 | 起動時に NVENC 可用性を検出し、x264 へ自動フォールバックする方針を設計に含める（実装は Phase 1） |
| **ウイルス対策ソフトの誤検知** | 署名なし exe が隔離される | VM で発生有無を確認。発生時は Phase 1 で署名を検討事項に上げる |
| **日本語パス** | §V10。ユーザー自身の環境で確実に発生する | V10 で段階検証。パッチが必要なら MLT 採用の減点 |
| **長いパス（260 文字超）** | 深い階層で読み書き失敗 | `\\?\` プレフィクスまたは manifest の `longPathAware` を検証項目に追加 |
| **VM に GPU が無いため NVENC 検証不可** | VM で export 検証が CPU に限定 | 決定済み。VM は依存関係検証専用、性能・NVENC はホストで測る |

---

## 9. MLT 不採用時の代替案

Phase 0 の判定が「不採用」になった場合に、次に何を検討するかを事前に順位付けする。**Phase 0 中に代替案の実装は行わない**が、MLT の各所見が代替案のどれを支持するかを判定書に記録する。

| 順位 | 案 | 適する状況 | 主な懸念 |
| --- | --- | --- | --- |
| 1 | **libavcodec / libavfilter ベースの自作エンジン + GPU 合成** | MLT のアーキテクチャ的制約（seek 精度、preview/final 不一致、抽象化の困難）が理由で落ちた場合 | 実装量が最大。ただしトラック数が少なく解説動画に特化する mvm では、必要な機能集合が小さく現実的。preview/final の一致は自作なら構造的に保証できる |
| 2 | **hybrid: 自作の軽量 preview エンジン + final render は FFmpeg `filter_complex`** | preview 性能が理由で落ちた場合 | preview と final で経路が分かれるため §V12 の一致保証が自前の責務になる。ただし render の正しさは FFmpeg に委ねられる |
| 3 | **GStreamer + GES (GStreamer Editing Services)** | Windows のビルド・配布（ABI / MSVC）が主因で落ちた場合 | GStreamer は **公式 MSVC ビルドを配布**しており、Qt6 MSVC と揃えられるのが最大の利点。GES 自体の保守状況とドキュメント量が MLT より薄いのが懸念 |
| 4 | MLT を fork して vendoring + パッチ | 欠陥が局所的で、他は良好な場合 | 上流追従コストを恒久的に背負う |

**代替案の評価に必要な情報も Phase 0 で収集する**: 検証素材、`mvm_bench` の計測手法、`IMediaEngine` 抽象化、staging スクリプトはいずれも backend 非依存に作る。これらは MLT 不採用でもそのまま再利用でき、Phase 0 が無駄にならない構造にする。

---

## 10. 採用・不採用を決める exit criteria

判定は **MUST の全充足** で行う。1 つでも欠ければ「不採用」または「条件付き採用（回避策付き）」とする。回避策の総工数が **5 人日**を超える場合は不採用とする。

### MUST（全て満たすこと）

| # | 基準 | 閾値 |
| --- | --- | --- |
| M1 | 再現ビルド | 凍結パッケージからクリーン環境で **2 回連続成功**。手順は scripts 化済み。所要 60 分以内 |
| M2 | 素材読み込み | H.264 / HEVC / PNG(alpha) / WAV の全てを読み、**ffprobe と duration・frame count・fps が一致** |
| M3 | 合成 | 動画 2 + 音声 2 + 文字 1 が意図通り合成される。**日本語文字が正しく描画される**（豆腐なし） |
| M4 | seek 精度 | 要求フレームと取得フレームが **100% 一致**（ずれる backend は不可） |
| M5 | seek 速度 | p95 ≤ 150 ms、max ≤ 400 ms |
| M6 | scrub | 連続スクラブで **≥ 15 updates/sec**、要求→表示 p95 ≤ 200 ms |
| M7 | 1080p60 preview | 5 トラック構成で **実効 ≥ 50 fps**、ドロップ率 ≤ 5% |
| M8 | 4K proxy preview | proxy 経由で **≥ 50 fps**、proxy 生成が realtime 比 ≥ 2x |
| M9 | エフェクト | transform / scale / crop / opacity / fade / audio gain の 6 種が全て適用でき、**座標系と値域が文書化できる**。時間アニメーションが可能 |
| M10 | 書き出し | H.264/AAC で出力し、ffprobe 検証と VLC/Chrome 再生に成功。**原子的リネームが機能** |
| M11 | preview/final 一致 | 幾何は**完全一致**。SSIM 平均 ≥ 0.99、最小 ≥ 0.97。A/V 同期差 ≤ 20 ms |
| M12 | 日本語パス | 読み込み・書き出し・ログの全てで正常動作。**MLT へのパッチ不要**（マニフェスト / short path / junction による回避は許容） |
| M13 | clean VM 起動 | Hyper-V クリーン Win11 で staging のみから起動し、日本語パスの素材読み込みと短尺 CPU export に成功 |
| M14 | UI 応答性 | 再生・スクラブ中に UI スレッドのフレーム時間 max ≤ 100 ms。UI スレッド I/O ガードが 1 度も発火しない |
| M15 | 抽象化可能性 | `IMediaEngine` 越しに MLT を隠せている。**`NullEngine` に差し替えて `mvm_spike` が起動する**。アーキテクチャ検査が全て通る |
| M16 | 安定性 | 30 分連続再生 + 20 回の seek/export でクラッシュなし。RSS 増加 ≤ 200 MB |

### SHOULD（欠けても不採用にしないが、判定書に記録する）

- S1: NVENC による書き出しが動作し、encoder オプションを pass-through できる
- S2: NVENC が x264 比で同 VMAF・**≥ 3x 高速**
- S3: HEVC 10bit 4K がデコードできる
- S4: ハードウェアデコードが利用できる
- S5: `real_time` を切り替えずに 1080p60 が 60 fps に到達する

### 判定の出し方

`docs/phase0-report.md` に以下を記載して完了とする:

1. MUST / SHOULD の充足表（実測値付き）
2. 発見した回避策とその工数見積り
3. MLT の設計上の制約が mvm の将来機能（自動字幕、数式アニメーション、キーフレーム編集）に与える影響の評価
4. **判定**: 採用 / 条件付き採用 / 不採用、および不採用の場合の推奨代替案（§9 の順位を所見に基づき更新）
5. ADR `docs/adr/0001-media-backend-choice.md` として決定を確定

---

## 11. 想定する変更ファイル一覧

Phase 0 は新規作成のみ。既存コードは存在しない。

### ビルド・スクリプト
```
CMakeLists.txt, CMakePresets.json
cmake/FindMLT.cmake, cmake/mvm_warnings.cmake
cmake/mvm_toolchain_guard.cmake      # compiler/Qt/pkg-config が ucrt64 配下かを検証
.gitignore, .gitattributes, .editorconfig, .clang-format
.vscode/launch.json, .vscode/settings.json
scripts/bootstrap-msys2.ps1
scripts/freeze-deps.ps1
scripts/build-mlt.ps1
scripts/make-testmedia.ps1
scripts/stage.ps1
scripts/verify-staging.ps1
```

### core / project（Qt・MLT 非依存）
```
src/core/Time.h, Rational.h, Result.h, Log.h/.cpp, Uuid.h
src/project/Project.h, Track.h, Clip.h, Effect.h, ProjectJson.h/.cpp
```

### media 抽象化
```
src/media/IMediaEngine.h, ITimelinePlayer.h, IRenderJob.h, IMediaProbe.h
src/media/MediaTypes.h
src/media/NullEngine.h/.cpp
src/media/probe/FfprobeMediaProbe.h/.cpp
```

### MLT adapter（mlt_*.h を include してよい唯一の場所）
```
src/media/mlt/MltEngine.h/.cpp
src/media/mlt/MltTimelineBuilder.h/.cpp
src/media/mlt/MltFrameSource.h/.cpp
src/media/mlt/MltRenderJob.h/.cpp
src/media/mlt/MltProps.h/.cpp
src/media/mlt/MltInit.h/.cpp
```

### platform / app / ui
```
src/platform/WinPaths.h/.cpp
src/app/main.cpp, SpikeController.h/.cpp, PreviewItem.h/.cpp, FrameQueue.h/.cpp
src/ui/qml/Main.qml, PreviewPane.qml, ScrubBar.qml
```

### テスト・計測
```
tests/unit/test_time.cpp, test_project_json.cpp, test_timeline_builder.cpp
tests/integration/test_decode.cpp, test_composite.cpp, test_effects.cpp
tests/integration/test_jp_paths.cpp, test_preview_final_match.cpp, test_export_atomic.cpp
tests/guard/check_no_mlt_leak.cmake, check_layering.cmake
tests/harness/mvm_bench.cpp, BenchScenario.h/.cpp, BenchReport.cpp
bench/scenarios/*.json
```

### ドキュメント（最終成果物）
```
docs/research/mlt-notes.md
docs/deps-lock.txt
docs/phase0-report.md          ← 判定書
docs/adr/0001-media-backend-choice.md
third_party/LICENSES/
README.md
```

---

## 12. Phase 0 では実装しない項目

明示的に範囲外とし、着手しかけたら止める。

**UI / 編集機能**: タイムライン UI ウィジェット、クリップのドラッグ編集、トリム、リップル / ロール、undo/redo、コピー&ペースト、マーカー、ショートカット体系、テーマ、設定画面、多言語化。

**プロジェクト**: 本番用 Project Model、保存形式の確定、autosave、プロジェクト移行、メディア再リンク、複数プロジェクト。

**メディア機能**: 自動字幕（Whisper 等）、数式アニメーション（Manim 等）、Python worker、音声波形表示、サムネイル生成、シーン検出、カラーコレクション、カラーマネジメント、LUT、ノイズ除去、モーショントラッキング、キーフレーム編集 UI、トランジションライブラリ、テキストテンプレート、タイトルエディタ。

**エンジン**: レンダーキャッシュ / バックグラウンドレンダリング、GPU フィルタパイプライン、ハードウェアデコード最適化、マルチパスエンコード、バッチ書き出し、書き出しプリセット体系、backend 実装の 2 つ目、MSVC ABI 接続、エンジン別プロセス化。

**配布**: インストーラ、コード署名、自動更新、クラッシュレポーター、テレメトリ。

**その他**: CI サーバ構築（ローカル実行で足りる）、パフォーマンス最適化（計測はするが最適化はしない）、リファクタリング。

---

## 実行順序（issue 分割の単位）

| # | Issue | 依存 | 目安 | 出す答え |
| --- | --- | --- | --- | --- |
| S0 | リポジトリ初期化、ディレクトリ構成、CMake/Presets、clang-format、gitignore | — | 0.5d | — |
| S1 | MSYS2 UCRT64 依存導入 + version 記録 + パッケージ凍結 + prebuilt MLT で hello world | S0 | 1d | **V1 一次** |
| S2 | `make-testmedia.ps1` と検証素材生成 | S0 | 0.5d | — |
| S3 | 資料調査（§1）と `mlt-notes.md` 作成 | 並行 | 1d | 型対応表 |
| S4 | `mvm_bench` 骨格 + probe + 単層再生 + null/sdl2 consumer | S1,S2 | 1d | **V2** |
| S5 | 5 トラック合成（日本語文字含む）+ ピクセル assert | S4,S3 | 1d | **V3** |
| S6 | seek / scrub 計測（coalescing 込み） | S4 | 1d | **V4, M4-M6** |
| S7 | 性能計測（1080p60）+ proxy 生成と 4K preview | S5 | 1d | **V5, V6** |
| S8 | エフェクト 6 種 + 時間アニメーション + 座標系文書化 | S5 | 1d | **V7** |
| S9 | export（x264 / NVENC）+ 原子的リネーム + preview/final 一致検証 | S5,S7 | 1.5d | **V8, V9, V12** |
| S10 | 日本語パス段階検証 | S4 | 0.5d | **V10** |
| S11 | `IMediaEngine` 抽象化 + `NullEngine` + アーキテクチャ検査 | S5 | 1d | **M15** |
| S12 | Qt/QML スパイクシェル + PreviewItem + UI スレッドガード | S11 | 1.5d | **M14, V5 の Qt 経路** |
| S13 | MLT from source ビルド + 再現性 2 回検証 | S1 | 1d | **M1** |
| S14 | staging スクリプト + Hyper-V VM 検証 | S12,S13 | 1d | **V11, M13** |
| S15 | 安定性テスト（30 分連続 + リーク） | S12 | 0.5d | **M16** |
| S16 | `phase0-report.md` 作成と ADR 確定 | 全て | 1d | **判定** |

S6 / S7 / S12 のいずれかが早期に MUST を大きく割った場合、残りを打ち切って S16 に進む判断を許容する。**Phase 0 の目的は完走ではなく判定であり、早期の「不採用」も成功した Phase 0 である。**

### 現在の実施範囲（ユーザー指示）

**S0 と S1 のみを実装する。S2 以降には着手しない。**

- S0: リポジトリ初期化、ディレクトリ構成、CMake / CMakePresets、ツールチェーン検証、各種 dotfile
- S1: MSYS2 UCRT64 依存導入、version 記録（`docs/deps-lock.txt`）、パッケージ凍結、prebuilt MLT による hello world

---

## Verification

Phase 0 の完了は、以下が全て揃った時点とする:

1. `scripts/bootstrap-msys2.ps1` → `cmake --preset ucrt64-release` → `ninja` が、クリーン環境で 2 回連続成功する
2. `ctest` で単体 / 統合 / アーキテクチャ検査が全て通る
3. `mvm_bench --all --out bench/results/` が全シナリオを完走し、CSV を出力する
4. `scripts/stage.ps1` の成果物が Hyper-V クリーン Win11 の日本語パスから起動し、`verify-staging.ps1` が成功する
5. `docs/phase0-report.md` に MUST / SHOULD の実測値と判定が記載され、`docs/adr/0001-media-backend-choice.md` が確定している
