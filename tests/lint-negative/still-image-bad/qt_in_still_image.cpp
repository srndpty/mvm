// lint の negative test 用フィクスチャ。**ビルド対象ではない。**
//
// src/media/still_image/ は Qt を一切 include してはいけない。画像の decode を
// FFmpeg に一本化し、preview と書き出しで同じ画素を使うための規約である。
//
// scripts/lint.ps1 -Path <このディレクトリ> -AsLayer still_image
// が exit 1 になることを CTest が検査する。
// QRhi は含めない。含めると QRhi の規則で落ち、still_image の規則が
// 効いているかどうかを判別できなくなる。

#include <QImage>

void mvm_lint_fixture_qt_in_still_image(void);
