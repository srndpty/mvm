// lint の negative test 用フィクスチャ。**ビルド対象ではない。**
//
// src/media/gpu_preview/ は Qt を一切 include してはいけない
// (docs/phase1-plan.md §7)。この規約が機械的に強制されていることを、
// 「違反すれば落ちる」ことで示すためのファイルである。
//
// scripts/lint.ps1 -Path <このディレクトリ> -AsLayer gpu_preview
// が exit 1 になることを CTest が検査する。

#include <rhi/qrhi.h>

#include <QObject>

void mvm_lint_fixture_qt_in_gpu_preview(void);
