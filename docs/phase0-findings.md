# Phase 0 所見（S0–S1 時点）

最終更新: 2026-08-04 / 対象: S0（リポジトリ初期化）、S1（UCRT64 依存導入 + prebuilt MLT hello world）

判定そのものは S16 で [phase0-report.md](phase0-report.md) にまとめる。
本書は途中で得た事実を、忘れないうちに記録するためのもの。

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

計画時点では「MLT 7 が FFmpeg 8 に対応していない可能性」をリスクとして挙げていたが、
**MSYS2 の MLT 7.36.1 は FFmpeg 8.1.2 に対してビルドされており、この懸念は構成上解消している。**
モジュール `libmltavformat.dll` は正常にロードでき、producer / consumer とも登録された。

ただし実際に H.264 / HEVC がデコードできるかは S4（V2）で確認する。ここで分かったのは
「リンクとロードが成立している」ところまでである。

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

## 検証済みの項目

### V1（再現ビルド）— 部分的に達成

- `bootstrap-msys2.ps1` → `build.ps1` で clean build が成功する
- 184 パッケージの version を `deps-lock.txt` に記録済み
- パッケージ実体 369 MB を凍結済み
- **未達**: 凍結パッケージからの復元（`-FromFrozen`）は未検証。
  MLT の from-source ビルドと 2 回連続の再現確認は S13 で行う

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

## 次にやること（S2 以降。未着手）

S2（検証素材生成）以降には着手していない。優先度の高い順:

1. **S13 に含めていた「凍結パッケージからの復元」を前倒しで一度試す。** 所見 3 のように
   optional 依存の欠落が機能欠落として現れるため、復元経路が壊れていないかは早めに知りたい
2. S2: 検証素材の生成（タイムコード焼き込み必須）
3. S4: `mvm_bench` 骨格と V2（読み込み）
4. S5: V3（5 トラック合成 + 日本語文字）

## 判定への影響（暫定）

現時点で MLT 不採用に傾く材料は無い。ただし所見 1〜3 はいずれも
**「失敗しているのに失敗と分からない」**という共通の性質を持つ。
MLT はエラーを握り潰して縮退する設計であり、これは V12（preview と final の一致）や
V11（clean 環境）の検証を、通常より慎重に組む必要があることを意味する。

exit criteria の M15（抽象化可能性）を評価する際、
「mvm の adapter 層が MLT の静かな失敗を検知して上位に伝えられるか」を
明示的な観点として加えること。
