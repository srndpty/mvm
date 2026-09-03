# mvm

Windows 向けノンリニア動画編集ソフト。YouTube 向けの技術・数学解説動画を主対象とする。

**Phase 0（技術スパイク）は S0〜S7.1 および早期 S16（採否判定）まで完了。
Phase 0 は「MLT を採用しない」という不採用判定で終了した。
製品コードは未着手である。**

Phase 0 の目的は、MLT 7 を mvm の編集・プレビュー・書き出しエンジンとして
採用できるかを判定することのみ。

> **判定:「MLT 7.36.1 / MSYS2 UCRT64 の現行 CPU・RGBA 経路を、
> mvm の統合編集・リアルタイム preview engine として採用しない。」**
>
> 1080p60 / 5 トラックの連続 preview が最良 19.85 fps（基準 50）で、
> 全 video を proxy 化しても 19.67 fps だったため。
> 判定の射程と、**測っていない項目**（書き出し、オフラインレンダ、
> エフェクト、clean VM 起動など）は
> [docs/phase0-decision.md](docs/phase0-decision.md) に明記している。

**Phase 1～3 の GPU preview / compositor / A/V sync spike は closure 済みで、
現在は Phase 4 の実装前契約をfreeze済みである。**
製品用 NLE・タイムライン UI・Project Model は引き続き未着手。

| ドキュメント | 内容 |
| --- | --- |
| **[docs/phase1-plan.md](docs/phase1-plan.md)** | **Phase 1 の計画と exit criteria** |
| **[docs/phase1-findings.md](docs/phase1-findings.md)** | **Phase 1 の実測所見** |
| **[docs/phase2-plan.md](docs/phase2-plan.md)** | **Phase 2 の計画と exit criteria** |
| **[docs/phase2-findings.md](docs/phase2-findings.md)** | **Phase 2 の実測所見** |
| **[docs/phase3-a-plan.md](docs/phase3-a-plan.md)** | **Phase 3 / A の実装前契約** |
| **[docs/phase3-c-contract.md](docs/phase3-c-contract.md)** | **Phase 3 / C-1 formal contract** |
| **[docs/phase3-c2-contract.md](docs/phase3-c2-contract.md)** | **Phase 3 / C-2 display-target formal contract** |
| **[docs/phase3-findings.md](docs/phase3-findings.md)** | **Phase 3 closure と historical result** |
| **[docs/phase4-plan.md](docs/phase4-plan.md)** | **Phase 4 のfreeze済み実装前契約** |
| **[docs/adr/0002-preview-backend-spike.md](docs/adr/0002-preview-backend-spike.md)** | **ADR: 内製 GPU preview backend の検証（Proposed）** |
| **[docs/phase0-decision.md](docs/phase0-decision.md)** | **採否判定書（S16）** |
| **[docs/adr/0001-mlt-adoption.md](docs/adr/0001-mlt-adoption.md)** | **ADR: MLT 採否の決定** |
| [docs/phase0-plan.md](docs/phase0-plan.md) | 計画全体と exit criteria |
| [docs/phase0-findings.md](docs/phase0-findings.md) | 実測結果。事実 / 推測 / 未検証を区別して記録 |
| [docs/research/mlt-notes.md](docs/research/mlt-notes.md) | MLT の実装メモ（実際に動かして確かめた範囲） |
| [docs/research/preview-performance-notes.md](docs/research/preview-performance-notes.md) | preview（consumer 経路）の実測と `real_time` の意味 |
| [docs/research/proxy-notes.md](docs/research/proxy-notes.md) | proxy の生成・path resolver・frame mapping |
| [docs/research/seek-scrub-notes.md](docs/research/seek-scrub-notes.md) | seek / scrub の実測と表示契約 |
| [docs/research/composition-notes.md](docs/research/composition-notes.md) | 5 トラック合成の実測 |
| [docs/research/memory-notes.md](docs/research/memory-notes.md) | メモリ増加の切り分け（診断であり合否ではない） |
| [docs/research/mlt-ownership.md](docs/research/mlt-ownership.md) | MLT の参照所有権 |
| [docs/research/test-media-format.md](docs/research/test-media-format.md) | 検証素材とフレーム固有マーカーの仕様 |

## ツールチェーン

Phase 0 は **MSYS2 UCRT64 に統一**する。Qt / MLT / FFmpeg / アプリ本体で
CRT および C++ ABI を混在させない。

| | |
| --- | --- |
| 環境 | `C:\msys64\ucrt64` |
| compiler | gcc 16.1.0 (UCRT64) |
| Qt | 6.11.1 (UCRT64 / pacman 版) |
| MLT | 7.36.1 (prebuilt) |
| FFmpeg | 8.1.2 (UCRT64) |
| build | CMake 4.4.2 + Ninja（いずれも UCRT64 版） |

> **この開発機には他プロジェクト用の Qt 6.8.3 (MSVC ビルド) が
> `C:\Users\lambe\sdk\Qt\6.8.3` にある。mvm はこれを一切参照しない。
> 削除・変更・移動もしない。**
> 誤って拾うと MSVC ABI と mingw ABI が混在し、リンクは通るのに実行時に
> 不可解な形で壊れる。これを防ぐため、configure 時に compiler・Qt6::Core の実体・
> pkg-config・libmlt が全て `C:/msys64/ucrt64` 配下であることを検証し、
> 違えば失敗する（[cmake/mvm_toolchain_guard.cmake](cmake/mvm_toolchain_guard.cmake)）。

## セットアップ

```powershell
# 1. 依存導入 + version 記録 (docs/deps-lock.txt)
pwsh scripts/bootstrap-msys2.ps1

# 2. パッケージ実体を退避 (MSYS2 は rolling repo のため)
pwsh scripts/freeze-deps.ps1

# 3. ビルド
pwsh scripts/build.ps1
```

`scripts/build.ps1` は PATH に `C:\msys64\ucrt64\bin` を先頭で加えてから cmake を呼ぶ。
UCRT64 の gcc は依存 DLL を PATH から解決するため、これが無いと
**エラーを出さずに失敗**し、CMake からは「compiler is broken」としか見えない。

## 開発用コマンド

リポジトリルートの `dev.ps1` を、通常の開発作業の短い入口として使う。
これは既存の正式スクリプトを置き換えず、そのまま呼び出す薄い front-end である。

```powershell
.\dev.ps1 build   # scripts/build.ps1
.\dev.ps1 gui     # release ビルド済みの mvm GUI
.\dev.ps1 test    # scripts/test.ps1（release/debug の通常 CTest）
.\dev.ps1 lint    # scripts/lint.ps1
.\dev.ps1 help
```

`gui` は既定で `build/ucrt64-release/m6a-gui/project.mvm` を開き、
ユーザープロファイル下の `.local/bin/manim.exe` を使用する。場所が異なる場合は
`-Ucrt64` または `-ManimExecutable` で明示する。

MSYS2 UCRT64 シェル、または VSCode の統合ターミナル「MSYS2 UCRT64」からなら
`cmake --preset ucrt64-release` を直接呼んでもよい。

## 検証素材の生成

自動テストは検証素材を必要とする。生成には UCRT64 版 FFmpeg を直接使う
（ホストの `C:\tools` 版や winget 版へはフォールバックしない）。

```powershell
pwsh scripts/make-testmedia.ps1 -Mode Smoke       # 5 秒。自動検査用
pwsh scripts/make-testmedia.ps1 -Mode Benchmark   # 60 秒。S7 以降の性能計測用
```

生成物は `tests/assets/<mode>/`（git 管理外）。
映像にはフレーム固有マーカーが焼き込まれ、`mvm_bench decode --expect-marker` が
要求フレームとの一致を機械判定する（OCR は使わない）。
日本語・半角空白・全角空白・全角記号を含むパスへの複製も作られる（V10）。

## テスト

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cd build\ucrt64-release
ctest --output-on-failure
```

素材が未生成のテストは実行されず、実行すべきコマンドが案内される。

## 動作確認

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cd build\ucrt64-release\bin

.\mvm_mlt_hello.exe                       # MLT モジュール解決の確認 (exit 0 が成功)
.\mvm_qt_probe.exe                        # Qt が UCRT64 版であることの確認

.\mvm_bench.exe doctor                    # MLT ランタイムの健全性検査
.\mvm_bench.exe probe <素材>              # MLT と ffprobe の解析結果を比較
.\mvm_bench.exe decode <素材> --frame 137 --expect-marker
.\mvm_bench.exe verify-media <manifest>
```

`mvm_bench` の終了コード: `0`=成功 `1`=実行時エラー `2`=使い方の誤り `3`=検証不一致。

## 再現性の検証 (R0)

`third_party/pkgs` の凍結パッケージだけで環境を組み直せることを、
**クリーンな MSYS2 ベース**（新規取得した base tarball）で検証する。
既存環境のコピーでは凍結物の不足を検出できないため意味がない。

```powershell
pwsh scripts/verify-frozen-restore.ps1
```

既存の `C:\msys64` と `C:\Users\lambe\sdk\Qt\6.8.3` は変更しない。
スクリプトはこれらを検証先に指定できないよう拒否する。

## 設計上の制約（Phase 0 全体で守る）

- mvm 独自の Project Model を MLT の型や property 名に依存させない
- MLT XML をプロジェクトの正本にしない
- media backend は交換可能な interface の背後に置く
- **MLT のヘッダを include してよいのは `src/media/mlt/` のみ。Mlt++ は使わず C API のみ**
- UI スレッドで decode / encode / ファイル I/O を行わない
- Python へ生の動画フレームを渡さない
- 一時出力を完成ファイルとして扱わない
- Phase 0 で本番機能を先回り実装しない

## ディレクトリ

| | |
| --- | --- |
| `cmake/` | ツールチェーン検証、MLT 探索 |
| `scripts/` | bootstrap / freeze / build / 素材生成 / 復元検証 |
| `src/util/` | UTF-8・wide 変換ヘルパ（Phase 0 スパイク用。製品 platform 層ではない） |
| `src/media/mlt/` | MLT adapter（MLT ヘッダを include できる唯一の場所） |
| `src/app/` | Qt スパイクシェル |
| `tests/harness/` | `mvm_bench`（Qt 非依存の検証 CLI） |
| `tests/assets/` | 生成された検証素材（git 管理外） |
| `docs/` | 計画・所見・依存 lock |
| `third_party/pkgs/` | 凍結 MSYS2 パッケージと署名（git 管理外） |
