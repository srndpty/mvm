#include "media_import.h"

#include <cmath>
#include <numeric>

namespace mvm::app {

MediaImportResult classifyMediaProbe(const MvmMltProbeResult& probe,
                                     const std::filesystem::path& mediaPath) {
    MediaImportResult result;
    if (!probe.ok) {
        result.error = std::string("素材を解析できません: ") + probe.error;
        return result;
    }
    auto& item = result.item;
    item.mediaPath = mediaPath;
    if (probe.has_video) {
        if (probe.width <= 0 || probe.height <= 0) {
            result.error = "映像の解像度を取得できません";
            return result;
        }
        item.width = probe.width;
        item.height = probe.height;
        // PNG は length = INT_MAX、JPEG は image2 demuxer 経由で 1 frame として返る (実測)。
        // 音声を持たない 1 frame の映像には時間方向の長さが無いので、どちらも静止画とする。
        if (probe.is_unbounded_length || (probe.frame_count == 1 && !probe.has_audio)) {
            item.kind = project::MediaKind::Image;
            result.success = true;
            return result;
        }
        if (probe.frame_count <= 0 || probe.fps_num <= 0 || probe.fps_den <= 0) {
            result.error = "有限尺と有効なFPSを持つ動画ではありません";
            return result;
        }
        const auto divisor = std::gcd(probe.fps_num, probe.fps_den);
        item.kind = project::MediaKind::Video;
        item.fpsNum = probe.fps_num / divisor;
        item.fpsDen = probe.fps_den / divisor;
        item.frameCount = probe.frame_count;
        result.success = true;
        return result;
    }
    if (!probe.has_audio) {
        result.error = "映像も音声も持たない素材です";
        return result;
    }
    if (probe.is_unbounded_length || !(probe.duration_sec > 0.0) ||
        !std::isfinite(probe.duration_sec)) {
        result.error = "有限の尺を持つ音声素材ではありません";
        return result;
    }
    if (probe.sample_rate <= 0) {
        result.error = "音声の sample rate を取得できません";
        return result;
    }
    // llround は int64 に収まらない値で未定義になる。2^63 未満であることを先に確かめる。
    const double exactSamples = probe.duration_sec * static_cast<double>(probe.sample_rate);
    if (!(exactSamples < 0x1p63)) {
        result.error = "音声の尺が扱える範囲を超えています";
        return result;
    }
    const auto samples = std::llround(exactSamples);
    if (samples <= 0) {
        result.error = "音声の尺が 1 sample に満たない素材です";
        return result;
    }
    item.kind = project::MediaKind::Audio;
    item.sampleRate = probe.sample_rate;
    item.durationSamples = samples;
    result.success = true;
    return result;
}

MediaImportResult probeMediaForBin(const std::filesystem::path& mediaPath) {
    const auto text = mediaPath.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(utf8.c_str(), &probe) != 0 || !probe.ok) {
        MediaImportResult result;
        result.error = "素材を解析できません: " + std::string(probe.error[0] ? probe.error : utf8);
        return result;
    }
    return classifyMediaProbe(probe, mediaPath);
}

} // namespace mvm::app
