#include "media/still_image/static_image.h"

namespace mvm::media {

MediaRouteDecision routeMedia(const MediaStreamFacts& facts) {
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
        decision.error =
            "HDR 画像 (EXR / Radiance HDR) には対応していません: " + facts.videoCodecName;
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

StillImageDecodeResult loadStaticImage(const std::filesystem::path& path,
                                       const StillImageLimits& limits) {
    const auto decision = routeMedia(probeMediaStreamFacts(path, limits));
    if (decision.route == MediaRoute::StillImage)
        return decodeStillImage(path, limits);
    StillImageDecodeResult result;
    result.error = decision.route == MediaRoute::TimeBased
                       ? "静止画ではありません (動画または音声の素材です)"
                       : decision.error;
    return result;
}

} // namespace mvm::media
