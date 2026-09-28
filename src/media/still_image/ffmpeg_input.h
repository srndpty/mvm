#ifndef MVM_MEDIA_STILL_IMAGE_FFMPEG_INPUT_H
#define MVM_MEDIA_STILL_IMAGE_FFMPEG_INPUT_H

// still_image module の内部用。公開 API ではない。

#include "media/still_image/still_image_limits.h"

#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace mvm::media::detail {

struct FormatCloser {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

std::string ffError(int code);

// 素材を開いて stream 情報を取得する。stream 情報の取得で decoder が動く形式にも
// limits.maxPixels を渡し、上限を超える画像の画素を確保させない。
bool openMediaInput(const std::filesystem::path& path, const StillImageLimits& limits,
                    FormatPtr& format, std::string& error);

} // namespace mvm::media::detail

#endif
