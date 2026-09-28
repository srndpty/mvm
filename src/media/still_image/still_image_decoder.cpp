#include "media/still_image/still_image_decoder.h"

#include "media/still_image/ffmpeg_input.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/exif.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace mvm::media {
namespace {

using detail::ffError;

struct CodecCloser {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

struct FrameCloser {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketCloser {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct ScalerCloser {
    void operator()(SwsContext* context) const { sws_free_context(&context); }
};

struct DictCloser {
    void operator()(AVDictionary* dict) const { av_dict_free(&dict); }
};

using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;

// tight な RGBA8 での画素 (x, y) の byte 位置。呼び出し側は 0 <= x < width, 0 <= y を保証する。
std::size_t pixelOffset(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x)) *
           4U;
}

bool withinLimits(int width, int height, const StillImageLimits& limits, std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "画像の寸法が不正です";
        return false;
    }
    if (width > limits.maxDimension || height > limits.maxDimension ||
        static_cast<std::int64_t>(width) * height > limits.maxPixels) {
        error =
            "画像が大きすぎます (" + std::to_string(width) + " x " + std::to_string(height) + ")";
        return false;
    }
    return true;
}

std::uint32_t readBe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) << 24 | static_cast<std::uint32_t>(bytes[1]) << 16 |
           static_cast<std::uint32_t>(bytes[2]) << 8 | static_cast<std::uint32_t>(bytes[3]);
}

std::uint16_t readBe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]);
}

// s15Fixed16Number (ICC.1 §4.6)。
double readS15Fixed16(const std::uint8_t* bytes) {
    return static_cast<double>(static_cast<std::int32_t>(readBe32(bytes))) / 65536.0;
}

struct IccTag {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// tag table から signature の tag を探す。table か tag が profile の外を指していたら false。
// 見つからなければ tag.data は nullptr のまま true。size は 132 以上であること。
bool findIccTag(const std::uint8_t* data, std::size_t size, const char* signature, IccTag& tag) {
    tag = {};
    const std::uint32_t tagCount = readBe32(data + 128);
    if (tagCount > (size - 132) / 12)
        return false;
    for (std::uint32_t index = 0; index < tagCount; ++index) {
        const std::uint8_t* entry = data + 132 + static_cast<std::size_t>(index) * 12;
        if (std::memcmp(entry, signature, 4) != 0)
            continue;
        const std::size_t offset = readBe32(entry + 4);
        const std::size_t length = readBe32(entry + 8);
        if (offset > size || length > size - offset || length < 8)
            return false;
        tag = {data + offset, length};
        return true;
    }
    return true;
}

// ICC profile の説明 ('desc' tag)。v2 の 'desc' 型 (ASCII) と v4 の 'mluc' 型 (UTF-16BE の
// 先頭 record、ASCII の範囲だけ) を読む。読めなければ空。エラー文言に添えるためだけに使い、
// sRGB かどうかの判定には使わない (説明は名乗りでしかない)。
std::string iccDescription(const std::uint8_t* data, std::size_t size) {
    IccTag found;
    if (!findIccTag(data, size, "desc", found) || !found.data || found.size < 12)
        return {};
    const std::uint8_t* tag = found.data;
    const std::size_t length = found.size;
    std::string text;
    if (std::memcmp(tag, "desc", 4) == 0) {
        const std::size_t count = readBe32(tag + 8);
        for (std::size_t i = 0; i < count && 12 + i < length && tag[12 + i] != 0; ++i)
            text.push_back(static_cast<char>(tag[12 + i]));
    } else if (std::memcmp(tag, "mluc", 4) == 0 && length >= 28 && readBe32(tag + 8) > 0) {
        const std::size_t bytes = readBe32(tag + 20);
        const std::size_t start = readBe32(tag + 24);
        for (std::size_t i = 0; i + 1 < bytes && start + i + 1 < length; i += 2) {
            const unsigned high = tag[start + i];
            const unsigned low = tag[start + i + 1];
            if (high == 0 && low != 0 && low < 0x80)
                text.push_back(static_cast<char>(low));
        }
    }
    return text;
}

// sRGB (IEC 61966-2-1) の matrix/TRC profile の値。原色は D50 へ Bradford 変換した XYZ
// (ICC の PCS は D50)。Windows 同梱の "sRGB IEC61966-2.1" profile と同じ値。
constexpr double kSrgbColorants[3][3] = {
    {0.436066, 0.222488, 0.013916}, {0.385147, 0.716873, 0.097076}, {0.143066, 0.060608, 0.714096}};
constexpr double kWhiteD50[3] = {0.964203, 1.0, 0.824905};
constexpr double kWhiteD65[3] = {0.950455, 1.0, 1.089050};
// 原色・白色点の許容差。s15Fixed16 の丸めと、profile ごとの Bradford 行列の桁の違いを
// 吸収する大きさ。Display P3 の赤は X が 0.079 違うので、広色域の profile はこれで弾ける。
constexpr double kXyzTolerance = 0.003;
// トーンカーブの許容差 (0..1 の出力に対して)。gamma 2.2 の曲線は sRGB と最大 0.0085 違う。
constexpr double kCurveTolerance = 0.002;
constexpr int kCurveSamples = 1024;

double srgbToLinear(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

bool readIccXyz(const IccTag& tag, double (&xyz)[3]) {
    if (!tag.data || tag.size < 20 || std::memcmp(tag.data, "XYZ ", 4) != 0)
        return false;
    for (int i = 0; i < 3; ++i)
        xyz[i] = readS15Fixed16(tag.data + 8 + i * 4);
    return true;
}

bool nearXyz(const double (&value)[3], const double (&expected)[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!(std::abs(value[i] - expected[i]) <= kXyzTolerance))
            return false;
    }
    return true;
}

// parametricCurveType (ICC.1 §10.18) の関数型 0..4 を式のとおりに評価する。
double evaluateParametricCurve(int type, const double (&p)[7], double x) {
    const auto power = [&](double base) { return base > 0.0 ? std::pow(base, p[0]) : 0.0; };
    switch (type) {
    case 0:
        return power(x);
    case 1:
        return x >= -p[2] / p[1] ? power(p[1] * x + p[2]) : 0.0;
    case 2:
        return x >= -p[2] / p[1] ? power(p[1] * x + p[2]) + p[3] : p[3];
    case 3:
        return x >= p[4] ? power(p[1] * x + p[2]) : p[3] * x;
    default:
        return x >= p[4] ? power(p[1] * x + p[2]) + p[5] : p[3] * x + p[6];
    }
}

// TRC tag ('curv' か 'para') が sRGB の曲線か。標本点ごとに sRGB の式と比べる。
bool curveIsSrgb(const IccTag& tag) {
    if (!tag.data || tag.size < 12)
        return false;
    if (std::memcmp(tag.data, "curv", 4) == 0) {
        const std::size_t count = readBe32(tag.data + 8);
        // 0 点は恒等、1 点は単純な gamma。どちらも sRGB ではない。
        if (count < 2 || count > (tag.size - 12) / 2)
            return false;
        for (std::size_t i = 0; i < count; ++i) {
            const double x = static_cast<double>(i) / static_cast<double>(count - 1);
            const double y = readBe16(tag.data + 12 + i * 2) / 65535.0;
            if (!(std::abs(y - srgbToLinear(x)) <= kCurveTolerance))
                return false;
        }
        return true;
    }
    if (std::memcmp(tag.data, "para", 4) == 0) {
        constexpr int kParameterCounts[5] = {1, 3, 4, 5, 7};
        const int type = readBe16(tag.data + 8);
        if (type > 4)
            return false;
        const int count = kParameterCounts[type];
        if (tag.size < 12 + static_cast<std::size_t>(count) * 4)
            return false;
        double p[7] = {};
        for (int i = 0; i < count; ++i)
            p[i] = readS15Fixed16(tag.data + 12 + i * 4);
        if ((type == 1 || type == 2) && p[1] == 0.0)
            return false;
        for (int i = 0; i < kCurveSamples; ++i) {
            const double x = static_cast<double>(i) / (kCurveSamples - 1);
            if (!(std::abs(evaluateParametricCurve(type, p, x) - srgbToLinear(x)) <=
                  kCurveTolerance))
                return false;
        }
        return true;
    }
    return false;
}

// EXIF side data に残っている orientation (0x112)。無ければ 0。
bool exifOrientationTag(const AVFrameSideData& sideData, int& orientation, std::string& error) {
    orientation = 0;
    AVExifMetadata ifd{};
    const int result =
        av_exif_parse_buffer(nullptr, sideData.data, sideData.size, &ifd, AV_EXIF_TIFF_HEADER);
    if (result < 0) {
        error = "EXIF を解析できません: " + ffError(result);
        return false;
    }
    AVDictionary* rawDict = nullptr;
    const int converted = av_exif_ifd_to_dict(nullptr, &ifd, &rawDict);
    av_exif_free(&ifd);
    std::unique_ptr<AVDictionary, DictCloser> dict(rawDict);
    if (converted < 0) {
        error = "EXIF を解析できません: " + ffError(converted);
        return false;
    }
    const AVDictionaryEntry* entry = av_dict_get(dict.get(), "Orientation", nullptr, 0);
    if (!entry)
        return true;
    char* end = nullptr;
    const long value = std::strtol(entry->value, &end, 10);
    if (end == entry->value || *end != '\0' || value < 1 || value > 8) {
        error = std::string("EXIF の orientation が不正です: ") + entry->value;
        return false;
    }
    orientation = static_cast<int>(value);
    return true;
}

// FFmpeg 8.1 は JPEG の EXIF orientation を display matrix へ移し、EXIF side data からは
// 取り除く (実測)。両方に値があれば一致を要求する。どちらを信じるかを選ばない。
bool frameOrientation(const AVFrame& frame, int& orientation, std::string& error) {
    int fromMatrix = 0;
    if (const AVFrameSideData* matrix =
            av_frame_get_side_data(&frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
        if (matrix->size < sizeof(std::int32_t) * 9) {
            error = "display matrix の大きさが不正です";
            return false;
        }
        fromMatrix =
            av_exif_matrix_to_orientation(reinterpret_cast<const std::int32_t*>(matrix->data));
        if (fromMatrix < 1 || fromMatrix > 8) {
            error = "display matrix を向きへ換算できません";
            return false;
        }
    }
    int fromExif = 0;
    if (const AVFrameSideData* exif = av_frame_get_side_data(&frame, AV_FRAME_DATA_EXIF)) {
        if (!exifOrientationTag(*exif, fromExif, error))
            return false;
    }
    if (fromMatrix != 0 && fromExif != 0 && fromMatrix != fromExif) {
        error = "display matrix と EXIF の向きが一致しません (" + std::to_string(fromMatrix) +
                " / " + std::to_string(fromExif) + ")";
        return false;
    }
    orientation = fromMatrix != 0 ? fromMatrix : (fromExif != 0 ? fromExif : 1);
    return true;
}

// FFmpeg は寸法の上限超過を EINVAL としか返さない。上限の値を添えて、利用者が
// 原因に辿り着けるようにする (上限超過だと断定はしない)。
std::string decodeError(int code, const StillImageLimits& limits) {
    return "画像を decode できません (画素数の上限 " + std::to_string(limits.maxPixels) +
           "): " + ffError(code);
}

bool decodeFirstFrame(const std::filesystem::path& path, const StillImageLimits& limits,
                      FramePtr& output, std::string& error) {
    detail::FormatPtr format;
    if (!detail::openMediaInput(path, limits, format, error))
        return false;
    int result = 0;
    int streamIndex = -1;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        if (streamIndex < 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
            streamIndex = static_cast<int>(index);
        else
            format->streams[index]->discard = AVDISCARD_ALL;
    }
    if (streamIndex < 0) {
        error = "画像の stream がありません";
        return false;
    }
    const AVCodecParameters* codecpar = format->streams[streamIndex]->codecpar;
    // 寸法が header から分かる形式は、decoder が確保する前にここで拒否する。
    if ((codecpar->width != 0 || codecpar->height != 0) &&
        !withinLimits(codecpar->width, codecpar->height, limits, error))
        return false;
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        error = std::string("画像の decoder がありません: ") + avcodec_get_name(codecpar->codec_id);
        return false;
    }
    std::unique_ptr<AVCodecContext, CodecCloser> codec(avcodec_alloc_context3(decoder));
    if (!codec) {
        error = "decoder context を確保できません";
        return false;
    }
    if ((result = avcodec_parameters_to_context(codec.get(), codecpar)) < 0) {
        error = "decoder を設定できません: " + ffError(result);
        return false;
    }
    // 寸法が header から分からない形式でも、decoder が確保する前に拒否させる。
    // 上限を超えた PNG は stream 情報の取得でも decode されず、codecpar の寸法は 0 のまま
    // ここへ来る。decoder は寸法を読んだ時点で EINVAL を返す。
    codec->max_pixels = limits.maxPixels;
    if ((result = avcodec_open2(codec.get(), decoder, nullptr)) < 0) {
        error = "decoder を初期化できません: " + ffError(result);
        return false;
    }

    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        error = "decode 用の領域を確保できません";
        return false;
    }
    bool flushing = false;
    for (;;) {
        result = avcodec_receive_frame(codec.get(), frame.get());
        if (result >= 0) {
            output = std::move(frame);
            return true;
        }
        if (result == AVERROR_EOF) {
            error = "画像に frame がありません";
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            error = decodeError(result, limits);
            return false;
        }
        if (flushing) {
            error = "画像に frame がありません";
            return false;
        }
        result = av_read_frame(format.get(), packet.get());
        if (result == AVERROR_EOF) {
            flushing = true;
            result = avcodec_send_packet(codec.get(), nullptr);
        } else if (result < 0) {
            error = "画像を読めません: " + ffError(result);
            return false;
        } else {
            if (packet->stream_index != streamIndex) {
                av_packet_unref(packet.get());
                continue;
            }
            result = avcodec_send_packet(codec.get(), packet.get());
            av_packet_unref(packet.get());
        }
        if (result < 0 && result != AVERROR_EOF) {
            error = decodeError(result, limits);
            return false;
        }
    }
}

// 任意の pixel format の frame を tight な RGBA8 (full range) へ変換する。
// YUV の range と行列は frame の属性から swscale が決める (dynamic mode)。
bool convertToRgba(const AVFrame& source, int width, int height, unsigned flags, StillImage& image,
                   std::string& error) {
    std::unique_ptr<SwsContext, ScalerCloser> scaler(sws_alloc_context());
    FramePtr destination(av_frame_alloc());
    if (!scaler || !destination) {
        error = "画素変換の領域を確保できません";
        return false;
    }
    scaler->flags = flags;
    destination->format = AV_PIX_FMT_RGBA;
    destination->width = width;
    destination->height = height;
    destination->color_range = AVCOL_RANGE_JPEG;
    destination->colorspace = AVCOL_SPC_RGB;
    const int result = sws_scale_frame(scaler.get(), destination.get(), &source);
    if (result < 0) {
        const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(source.format));
        error = std::string("画素を RGBA へ変換できません (") + (name ? name : "unknown") +
                "): " + ffError(result);
        return false;
    }
    image.width = width;
    image.height = height;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
    image.rgba.resize(rowBytes * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        std::memcpy(image.rgba.data() + rowBytes * static_cast<std::size_t>(row),
                    destination->data[0] +
                        static_cast<std::ptrdiff_t>(destination->linesize[0]) * row,
                    rowBytes);
    }
    return true;
}

// RGBA8 の画像を AVFrame として参照する (画素は複製しない)。
FramePtr wrapRgba(const StillImage& image, std::string& error) {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        error = "frame を確保できません";
        return frame;
    }
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = image.width;
    frame->height = image.height;
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->colorspace = AVCOL_SPC_RGB;
    frame->data[0] = const_cast<std::uint8_t*>(image.rgba.data());
    frame->linesize[0] = image.width * 4;
    return frame;
}

void premultiply(StillImage& image) {
    for (std::size_t index = 0; index + 3 < image.rgba.size(); index += 4) {
        const unsigned alpha = image.rgba[index + 3];
        for (std::size_t channel = 0; channel < 3; ++channel)
            image.rgba[index + channel] =
                static_cast<std::uint8_t>((image.rgba[index + channel] * alpha + 127U) / 255U);
    }
}

void unpremultiply(StillImage& image) {
    for (std::size_t index = 0; index + 3 < image.rgba.size(); index += 4) {
        const unsigned alpha = image.rgba[index + 3];
        if (alpha == 0) {
            image.rgba[index] = image.rgba[index + 1] = image.rgba[index + 2] = 0;
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const unsigned value = (image.rgba[index + channel] * 255U + alpha / 2U) / alpha;
            image.rgba[index + channel] = static_cast<std::uint8_t>(std::min(value, 255U));
        }
    }
}

} // namespace

bool applyExifOrientation(StillImage& image, int orientation, std::string& error) {
    if (orientation < 1 || orientation > 8) {
        error = "EXIF orientation は 1..8 のいずれかでなければなりません: " +
                std::to_string(orientation);
        return false;
    }
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() !=
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U) {
        error = "画像の寸法と画素数が一致しません";
        return false;
    }
    if (orientation == 1)
        return true;
    const int w = image.width;
    const int h = image.height;
    const bool swapsAxes = orientation >= 5;
    StillImage rotated;
    rotated.width = swapsAxes ? h : w;
    rotated.height = swapsAxes ? w : h;
    rotated.rgba.resize(image.rgba.size());
    for (int y = 0; y < rotated.height; ++y) {
        for (int x = 0; x < rotated.width; ++x) {
            // 表示位置 (x, y) に来る元画素 (sx, sy)。EXIF 2.3 の Orientation の定義による。
            int sx = x;
            int sy = y;
            switch (orientation) {
            case 2:
                sx = w - 1 - x;
                sy = y;
                break; // 左右反転
            case 3:
                sx = w - 1 - x;
                sy = h - 1 - y;
                break; // 180 度
            case 4:
                sx = x;
                sy = h - 1 - y;
                break; // 上下反転
            case 5:
                sx = y;
                sy = x;
                break; // 転置
            case 6:
                sx = y;
                sy = h - 1 - x;
                break; // 時計回り 90 度
            case 7:
                sx = w - 1 - y;
                sy = h - 1 - x;
                break; // 反転置
            case 8:
                sx = w - 1 - y;
                sy = x;
                break; // 反時計回り 90 度
            default:
                break;
            }
            std::memcpy(rotated.rgba.data() + pixelOffset(x, y, rotated.width),
                        image.rgba.data() + pixelOffset(sx, sy, w), 4U);
        }
    }
    image = std::move(rotated);
    return true;
}

StillImageDecodeResult decodeStillImage(const std::filesystem::path& path,
                                        const StillImageLimits& limits) {
    StillImageDecodeResult result;
    FramePtr frame;
    if (!decodeFirstFrame(path, limits, frame, result.error))
        return result;
    if (!withinLimits(frame->width, frame->height, limits, result.error))
        return result;
    const char* formatName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    result.sourcePixelFormat = formatName ? formatName : "";
    if (!frameOrientation(*frame, result.orientation, result.error))
        return result;
    // 色の変換 (ICC) はしない。sRGB 以外の profile を持つ画像を sRGB として描くと、
    // decode には成功して色だけが静かに変わる。読み込み時点で理由を添えて拒否する。
    if (const AVFrameSideData* icc =
            av_frame_get_side_data(frame.get(), AV_FRAME_DATA_ICC_PROFILE)) {
        std::string description;
        std::string reason;
        if (!iccProfileIsSrgb(icc->data, icc->size, description, reason)) {
            result.error = "sRGB 以外の ICC profile を持つ画像には対応していません (" +
                           (description.empty() ? std::string("説明なし") : description) + ": " +
                           reason +
                           ")。色が変わるため読み込みません。sRGB へ変換してから読み込んでください";
            return result;
        }
    }
    if (!convertToRgba(*frame, frame->width, frame->height,
                       SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, result.image,
                       result.error))
        return result;
    if (!applyExifOrientation(result.image, result.orientation, result.error))
        return result;
    result.success = true;
    return result;
}

bool iccProfileIsSrgb(const std::uint8_t* data, std::size_t size, std::string& description,
                      std::string& reason) {
    description.clear();
    reason.clear();
    if (!data || size < 132) {
        reason = "profile が短すぎます";
        return false;
    }
    // header の profile size が buffer に収まらなければ壊れている。以降はその範囲だけを読む。
    const std::size_t declared = readBe32(data);
    if (declared < 132 || declared > size) {
        reason = "profile の大きさが不正です";
        return false;
    }
    size = declared;
    description = iccDescription(data, size);
    // 説明 ('desc') は名乗りでしかないので判定に使わない。色を決める中身 (原色・白色点・
    // トーンカーブ) が sRGB と一致する matrix/TRC 型の表示装置 profile だけを通す。
    if (std::memcmp(data + 12, "mntr", 4) != 0) {
        reason = "表示装置用 (mntr) の profile ではありません";
        return false;
    }
    if (std::memcmp(data + 16, "RGB ", 4) != 0 || std::memcmp(data + 20, "XYZ ", 4) != 0) {
        reason = "RGB / XYZ の profile ではありません";
        return false;
    }
    // LUT 型の tag があると、色管理はそちらを matrix/TRC より優先して使う。LUT の中身は
    // 検証しないので、あれば拒否する。
    for (const char* lut : {"A2B0", "A2B1", "A2B2", "B2A0", "B2A1", "B2A2", "D2B0", "D2B1", "D2B2",
                            "D2B3", "B2D0", "B2D1", "B2D2", "B2D3"}) {
        IccTag tag;
        if (!findIccTag(data, size, lut, tag) || tag.data) {
            reason = std::string("LUT 型の profile (") + lut + ") は検証できません";
            return false;
        }
    }
    const char* colorantTags[3] = {"rXYZ", "gXYZ", "bXYZ"};
    for (int channel = 0; channel < 3; ++channel) {
        IccTag tag;
        double xyz[3] = {};
        if (!findIccTag(data, size, colorantTags[channel], tag) || !readIccXyz(tag, xyz)) {
            reason = std::string("原色 (") + colorantTags[channel] + ") を読めません";
            return false;
        }
        if (!nearXyz(xyz, kSrgbColorants[channel])) {
            reason = std::string("原色 (") + colorantTags[channel] + ") が sRGB と違います";
            return false;
        }
    }
    {
        IccTag tag;
        double xyz[3] = {};
        if (!findIccTag(data, size, "wtpt", tag) || !readIccXyz(tag, xyz)) {
            reason = "白色点 (wtpt) を読めません";
            return false;
        }
        // v4 は PCS の D50、v2 の sRGB profile は D65 を書く。
        if (!nearXyz(xyz, kWhiteD50) && !nearXyz(xyz, kWhiteD65)) {
            reason = "白色点 (wtpt) が D50 / D65 ではありません";
            return false;
        }
    }
    for (const char* trc : {"rTRC", "gTRC", "bTRC"}) {
        IccTag tag;
        if (!findIccTag(data, size, trc, tag) || !curveIsSrgb(tag)) {
            reason = std::string("トーンカーブ (") + trc + ") が sRGB と違います";
            return false;
        }
    }
    return true;
}

StillRasterResult fitStillImageToRaster(const StillImage& image, int outputWidth,
                                        int outputHeight) {
    StillRasterResult result;
    if (outputWidth <= 0 || outputHeight <= 0) {
        result.error = "出力解像度が不正です";
        return result;
    }
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() !=
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U) {
        result.error = "画像の寸法と画素数が一致しません";
        return result;
    }
    // 収まる最大の大きさ。縦横どちらかが出力と一致する。
    const double scale = std::min(static_cast<double>(outputWidth) / image.width,
                                  static_cast<double>(outputHeight) / image.height);
    const int fittedWidth =
        std::clamp(static_cast<int>(std::lround(image.width * scale)), 1, outputWidth);
    const int fittedHeight =
        std::clamp(static_cast<int>(std::lround(image.height * scale)), 1, outputHeight);

    StillImage scaled;
    if (fittedWidth == image.width && fittedHeight == image.height) {
        scaled = image;
    } else {
        StillImage premultiplied = image;
        premultiply(premultiplied);
        FramePtr source = wrapRgba(premultiplied, result.error);
        if (!source)
            return result;
        const bool shrinking = fittedWidth < image.width || fittedHeight < image.height;
        const unsigned flags = (shrinking ? SWS_AREA : SWS_BICUBIC) | SWS_ACCURATE_RND;
        if (!convertToRgba(*source, fittedWidth, fittedHeight, flags, scaled, result.error))
            return result;
        unpremultiply(scaled);
    }

    result.raster.width = outputWidth;
    result.raster.height = outputHeight;
    result.raster.rgba.assign(
        static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight) * 4U, 0);
    const int left = (outputWidth - fittedWidth) / 2;
    const int top = (outputHeight - fittedHeight) / 2;
    const std::size_t rowBytes = static_cast<std::size_t>(fittedWidth) * 4U;
    for (int row = 0; row < fittedHeight; ++row) {
        std::memcpy(result.raster.rgba.data() + pixelOffset(left, top + row, outputWidth),
                    scaled.rgba.data() + rowBytes * static_cast<std::size_t>(row), rowBytes);
    }
    result.success = true;
    return result;
}

} // namespace mvm::media
