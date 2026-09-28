#ifndef MVM_APP_TEXT_RASTER_H
#define MVM_APP_TEXT_RASTER_H

#include "project/project.h"

#include <QImage>
#include <QString>

namespace mvm::app {

// Project の文字データから preview と書き出しに共通の RGBA 画像を作る。
QImage renderTextRaster(const project::TextClipData& data, int width, int height, QString& error);

} // namespace mvm::app

#endif
