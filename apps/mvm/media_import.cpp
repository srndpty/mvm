#include "media_import.h"

#include <cmath>
#include <numeric>

namespace mvm::app {

MediaRouteDecision routeMedia(const media::MediaStreamFacts& facts) {
    MediaRouteDecision decision;
    // FFmpeg が開けないアニメーション WebP もあるので、開けたかどうかより先に見る。
    if (facts.webpAnimationFlag || (facts.stillImageCodec && facts.imagePacketCountUpTo2 >= 2)) {
        decision.error = "アニメーション画像 (GIF / WebP / APNG) には対応していません。"
                         "動画 (mp4 など) へ変換してから読み込んでください";
        return decision;
    }
    if (!facts.ok) {
        decision.error = "素材を解析できません: " + facts.error;
        return decision;
    }
    if (facts.hdrImageCodec) {
        decision.error = "HDR 画像 (EXR / Radiance HDR) には対応していません: " +
                         facts.videoCodecName;
        return decision;
    }
    if (facts.stillImageCodec) {
        if (facts.imagePacketCountUpTo2 == 0) {
            decision.error = "画像のデータがありません";
            return decision;
        }
        decision.route = MediaRoute::StillImage;
        return decision;
    }
    if (facts.videoStreamCount == 0 && facts.audioStreamCount == 0) {
        decision.error = "映像も音声も持たない素材です";
        return decision;
    }
    decision.route = MediaRoute::TimeBased;
    return decision;
}

MediaImportResult classifyMediaProbe(const MvmMltProbeResult& probe,
                                     const media::MediaStreamFacts& facts,
                                     const std::filesystem::path& mediaPath) {
    MediaImportResult result;
    if (!probe.ok) {
        result.error = std::string("素材を解析できません: ") + probe.error;
        return result;
    }
    auto& item = result.item;
    item.mediaPath = mediaPath;
    result.durationSec = probe.duration_sec;
    if (facts.videoStreamCount > 0) {
        if (!probe.has_video) {
            result.error = "映像 stream を MLT で開けません";
            return result;
        }
        if (probe.width <= 0 || probe.height <= 0) {
            result.error = "映像の解像度を取得できません";
            return result;
        }
        if (probe.is_unbounded_length || probe.frame_count <= 0 || probe.fps_num <= 0 ||
            probe.fps_den <= 0) {
            result.error = "有限尺と有効なFPSを持つ動画ではありません";
            return result;
        }
        if (probe.frame_count == 1) {
            result.error = "1 frame だけの動画は読み込めません";
            return result;
        }
        const auto divisor = std::gcd(probe.fps_num, probe.fps_den);
        item.kind = project::MediaKind::Video;
        item.width = probe.width;
        item.height = probe.height;
        item.fpsNum = probe.fps_num / divisor;
        item.fpsDen = probe.fps_den / divisor;
        item.frameCount = probe.frame_count;
        result.hasAudio = probe.has_audio != 0;
        if (probe.sar_num > 0 && probe.sar_den > 0) {
            const auto sarDivisor = std::gcd(probe.sar_num, probe.sar_den);
            result.sarNum = probe.sar_num / sarDivisor;
            result.sarDen = probe.sar_den / sarDivisor;
        }
        result.success = true;
        return result;
    }
    // 本物の映像が無い。カバーアート付きの mp3 / m4a を MLT は映像ありと返すが (実測)、
    // 音声として扱う。
    if (!probe.has_audio) {
        result.error = "音声を MLT で開けません";
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

MediaImportResult classifyStillImage(const media::StillImageDecodeResult& decoded,
                                     const std::filesystem::path& mediaPath) {
    MediaImportResult result;
    if (!decoded.success) {
        result.error = decoded.error;
        return result;
    }
    if (decoded.image.width <= 0 || decoded.image.height <= 0) {
        result.error = "画像の寸法を取得できません";
        return result;
    }
    result.item.mediaPath = mediaPath;
    result.item.kind = project::MediaKind::Image;
    result.item.width = decoded.image.width;
    result.item.height = decoded.image.height;
    result.success = true;
    return result;
}

MediaImportResult probeMediaFile(const std::filesystem::path& mediaPath) {
    const auto facts = media::probeMediaStreamFacts(mediaPath);
    const auto decision = routeMedia(facts);
    if (decision.route == MediaRoute::Rejected) {
        MediaImportResult result;
        result.error = decision.error;
        return result;
    }
    if (decision.route == MediaRoute::StillImage)
        return classifyStillImage(media::decodeStillImage(mediaPath), mediaPath);

    const auto text = mediaPath.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(utf8.c_str(), &probe) != 0 || !probe.ok) {
        MediaImportResult result;
        result.error = "素材を解析できません: " + std::string(probe.error[0] ? probe.error : utf8);
        return result;
    }
    return classifyMediaProbe(probe, facts, mediaPath);
}

} // namespace mvm::app
