#ifndef MVM_APP_TEXT_RASTER_H
#define MVM_APP_TEXT_RASTER_H

#include "project/project.h"

#include <QImage>
#include <QSizeF>
#include <QString>

namespace mvm::app {

// Project の文字データから preview と書き出しに共通の RGBA 画像を作る。
QImage renderTextRaster(const project::TextClipData& data, int width, int height, QString& error);

// 文字の描画範囲 (背景矩形) の大きさ。(x, y) はこの矩形の左上である。
// renderTextRaster と同じ計算で求める。フォントが無ければ空の大きさと error。
QSizeF textBlockSize(const project::TextClipData& data, QString& error);

// テロップの定位置。揃え ("left" / "center" / "right") ごとに決まる位置へ置く。
//
// YouTube のテロップの慣例に合わせ、文字の矩形の下端を画面の下から 15% に置く。
// 左右は画面幅の 5% の余白を取る (center は中央)。値は決め打ちの ad hoc 仕様であり、
// 画面の安全域の規格から導いたものではない。
inline constexpr double kTextPresetBottomRatio = 0.15;
inline constexpr double kTextPresetSideRatio = 0.05;

struct TextPresetPlacement {
    bool success = false;
    int x = 0;
    int y = 0;
    QString error;
};

TextPresetPlacement textPresetPlacement(const project::TextClipData& data, int width, int height,
                                        const std::string& alignment);

} // namespace mvm::app

#endif
