# mvm

Windows 向けノンリニア動画編集ソフト。YouTube 向けの技術・数学解説動画を主対象とする。

**現在 Phase 0（技術スパイク）の S0〜S1 まで実装済み。製品コードはまだ存在しない。**

Phase 0 の目的は、MLT 7 を mvm の編集・プレビュー・書き出しエンジンとして
採用できるかを判定することのみ。詳細は [docs/phase0-plan.md](docs/phase0-plan.md)、
S1 までの実測結果は [docs/phase0-findings.md](docs/phase0-findings.md) を参照。

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

MSYS2 UCRT64 シェル、または VSCode の統合ターミナル「MSYS2 UCRT64」からなら
`cmake --preset ucrt64-release` を直接呼んでもよい。

## 動作確認

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build\ucrt64-release\bin\mvm_mlt_hello.exe   # MLT モジュール解決の確認 (exit 0 が成功)
.\build\ucrt64-release\bin\mvm_qt_probe.exe    # Qt が UCRT64 版であることの確認
```

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
| `scripts/` | bootstrap / freeze / build |
| `src/media/mlt/` | MLT adapter（MLT ヘッダを include できる唯一の場所） |
| `src/app/` | Qt スパイクシェル |
| `docs/` | 計画・所見・依存 lock |
| `third_party/pkgs/` | 凍結 MSYS2 パッケージ（git 管理外） |
