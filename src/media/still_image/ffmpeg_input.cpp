#include "media/still_image/ffmpeg_input.h"

#include <vector>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace mvm::media::detail {

std::string ffError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

bool openMediaInput(const std::filesystem::path& path, const StillImageLimits& limits,
                    FormatPtr& format, std::string& error) {
    const auto text = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    AVFormatContext* rawFormat = nullptr;
    int result = avformat_open_input(&rawFormat, utf8.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "素材を開けません: " + ffError(result);
        return false;
    }
    format.reset(rawFormat);

    // stream ごとの codec option。stream 情報の取得で動く decoder へ上限を渡す。
    const std::string maxPixels = std::to_string(limits.maxPixels);
    std::vector<AVDictionary*> options(format->nb_streams, nullptr);
    for (auto& option : options)
        av_dict_set(&option, "max_pixels", maxPixels.c_str(), 0);
    result = avformat_find_stream_info(format.get(), options.empty() ? nullptr : options.data());
    for (auto& option : options)
        av_dict_free(&option);
    if (result < 0) {
        error = "stream 情報を取得できません: " + ffError(result);
        return false;
    }
    return true;
}

} // namespace mvm::media::detail
