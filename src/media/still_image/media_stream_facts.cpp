#include "media/still_image/media_stream_facts.h"

#include "media/still_image/ffmpeg_input.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace mvm::media {
namespace {

// 静止画として受け付ける codec。器が単一画像形式のときに限る (isImageContainer)。
// 列挙していない codec は時間を持つ素材として MLT 側の判定へ回る。
constexpr std::array<std::string_view, 25> kStillImageCodecs = {
    "png",   "apng", "mjpeg",  "jpegls",      "jpeg2000", "bmp", "gif",    "webp", "tiff",
    "targa", "pcx",  "qoi",    "psd",         "sgi",      "dds", "ppm",    "pgm",  "pgmyuv",
    "pbm",   "pam",  "jpegxl", "jpegxl_anim", "xbm",      "xpm", "sunrast"};

constexpr std::array<std::string_view, 2> kHdrImageCodecs = {"exr", "hdr"};

// 画像 1 枚 (またはアニメーション) を格納する demuxer。
// 同じ codec でも mov / avi に入っていれば動画として扱う (PNG の QuickTime 動画、Motion JPEG)。
constexpr std::array<std::string_view, 4> kImageContainers = {"image2", "gif", "apng",
                                                              "jpegxl_anim"};

template<std::size_t N>
bool contains(const std::array<std::string_view, N>& table, std::string_view value) {
    for (const auto entry : table) {
        if (entry == value)
            return true;
    }
    return false;
}

bool isImageContainer(std::string_view formatName) {
    constexpr std::string_view kPipe = "_pipe";
    if (formatName.size() > kPipe.size() &&
        formatName.substr(formatName.size() - kPipe.size()) == kPipe)
        return true;
    return contains(kImageContainers, formatName);
}

std::uint32_t readLe32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8 |
           static_cast<std::uint32_t>(bytes[2]) << 16 | static_cast<std::uint32_t>(bytes[3]) << 24;
}

// RIFF/WEBP の先頭 chunk が VP8X で、flags の animation bit (0x02) が立っているか。
// 拡張形式の WebP では VP8X が必ず先頭 chunk になる (WebP Container Specification)。
bool readWebpAnimationFlag(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<unsigned char, 21> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()))
        return false;
    const auto tag = [&](std::size_t offset, std::string_view expected) {
        return std::string_view(reinterpret_cast<const char*>(header.data() + offset), 4) ==
               expected;
    };
    if (!tag(0, "RIFF") || !tag(8, "WEBP") || !tag(12, "VP8X"))
        return false;
    // VP8X の payload は 10 byte。flags はその先頭 byte。
    if (readLe32(header.data() + 16) < 10)
        return false;
    return (header[20] & 0x02U) != 0;
}

struct PacketCloser {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

} // namespace

MediaStreamFacts probeMediaStreamFacts(const std::filesystem::path& path,
                                       const StillImageLimits& limits) {
    MediaStreamFacts facts;
    // FFmpeg が開けないアニメーション WebP も判別できるよう、demux より先に読む。
    facts.webpAnimationFlag = readWebpAnimationFlag(path);

    detail::FormatPtr format;
    if (!detail::openMediaInput(path, limits, format, facts.error))
        return facts;
    facts.formatName = format->iformat && format->iformat->name ? format->iformat->name : "";

    int imageStreamIndex = -1;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        const AVCodecParameters* codecpar = stream->codecpar;
        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++facts.audioStreamCount;
            continue;
        }
        if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            ++facts.attachedPictureCount;
            continue;
        }
        if (facts.videoStreamCount++ == 0) {
            facts.videoCodecName = avcodec_get_name(codecpar->codec_id);
            facts.width = codecpar->width;
            facts.height = codecpar->height;
            const AVPixFmtDescriptor* pixel =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(codecpar->format));
            // VP9 の alpha は画素形式 (yuv420p) に出ず、webm の alpha_mode で示される (実測)。
            const AVDictionaryEntry* alphaMode =
                av_dict_get(stream->metadata, "alpha_mode", nullptr, 0);
            facts.videoAlphaCapable = (pixel && (pixel->flags & AV_PIX_FMT_FLAG_ALPHA) != 0) ||
                                      (alphaMode && std::string(alphaMode->value) == "1");
            imageStreamIndex = static_cast<int>(index);
        }
    }

    const bool imageContainer = isImageContainer(facts.formatName);
    facts.stillImageCodec = imageContainer && contains(kStillImageCodecs, facts.videoCodecName);
    facts.hdrImageCodec = contains(kHdrImageCodecs, facts.videoCodecName);
    if (facts.stillImageCodec) {
        for (unsigned index = 0; index < format->nb_streams; ++index) {
            if (static_cast<int>(index) != imageStreamIndex)
                format->streams[index]->discard = AVDISCARD_ALL;
        }
        std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
        if (!packet) {
            facts.error = "packet を確保できません";
            return facts;
        }
        while (facts.imagePacketCountUpTo2 < 2) {
            const int result = av_read_frame(format.get(), packet.get());
            if (result == AVERROR_EOF)
                break;
            if (result < 0) {
                facts.error = "画像を読めません: " + detail::ffError(result);
                return facts;
            }
            if (packet->stream_index == imageStreamIndex)
                ++facts.imagePacketCountUpTo2;
            av_packet_unref(packet.get());
        }
    }
    facts.ok = true;
    return facts;
}

} // namespace mvm::media
