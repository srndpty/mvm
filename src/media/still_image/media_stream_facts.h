#ifndef MVM_MEDIA_STILL_IMAGE_MEDIA_STREAM_FACTS_H
#define MVM_MEDIA_STILL_IMAGE_MEDIA_STREAM_FACTS_H

#include "media/still_image/still_image_limits.h"

#include <filesystem>
#include <string>

namespace mvm::media {

// 素材の stream 構成を FFmpeg で直接調べた事実。
//
// MLT の probe (avformat producer) は次の点で素材種別の判定に使えない (実測):
//   - 静止画 GIF を 4 frame / 100fps、静止画 WebP を 15000 frame / 600 秒の動画として返す
//   - アニメーション PNG を静止画と同じ無限尺で返す
//   - カバーアート (attached_pic) 付きの mp3 を映像ありとして返す
// そのため「画像か、時間を持つ素材か」「本物の映像 stream があるか」はここで決める。
// 尺や fps など MLT が扱う値は引き続き MLT の probe から取る。
struct MediaStreamFacts {
    bool ok = false;
    std::string error;
    std::string formatName;       // AVInputFormat::name (例: "png_pipe", "mov,mp4,m4a,3gp,3g2,mj2")
    int videoStreamCount = 0;     // attached_pic (カバーアート) を除く
    int attachedPictureCount = 0; // カバーアート
    int audioStreamCount = 0;
    std::string videoCodecName; // attached_pic を除いた先頭の video stream
    // 先頭の video stream が透過を持ちうる形式か (画素形式に alpha がある、または VP8 / VP9 の
    // webm が alpha_mode=1 で alpha を別に持つ)。値が実際に透過しているかは見ない。
    bool videoAlphaCapable = false;
    // 単一画像を格納する形式の静止画 codec (png / mjpeg を jpeg_pipe で開いた場合など)。
    // mjpeg を avi で開いた場合のように、同じ codec でも動画の器なら false。
    bool stillImageCodec = false;
    // scene-linear の HDR 画像 (exr / hdr)。表示には tone map が要るので扱わない。
    bool hdrImageCodec = false;
    // stillImageCodec のときだけ数える video packet 数。2 以上はアニメーション。
    // 数えるのは 2 までで打ち切る。
    int imagePacketCountUpTo2 = 0;
    // WebP の VP8X chunk の animation flag。FFmpeg 8.1 はアニメーション WebP を
    // 1 packet として demux し decode もできないため (実測)、header を直接読む。
    bool webpAnimationFlag = false;
    int width = 0;
    int height = 0;
};

// 画像の寸法が limits を超える場合、width / height は 0 のまま返る (stream 情報の取得で
// decoder が拒否するため)。寸法の拒否は decodeStillImage が理由を添えて行う。
MediaStreamFacts probeMediaStreamFacts(const std::filesystem::path& path,
                                       const StillImageLimits& limits = {});

} // namespace mvm::media

#endif
