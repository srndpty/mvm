// 画像素材の判定 (media_stream_facts) と静止画 decoder。
// 素材は scripts/make-testmedia.ps1 -Mode Smoke が _import/ へ作る。
// 期待値は実装の式を呼ばずに手で書く (向きは EXIF の定義から導いた表)。

#include "media/still_image/media_stream_facts.h"
#include "media/still_image/static_image.h"
#include "media/still_image/still_image_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mvm::media::StillImage;
using mvm::media::StillImageLimits;

int failures = 0;
int checks = 0;

fs::path utf8Path(const char* text) {
    return fs::path(reinterpret_cast<const char8_t*>(text));
}

void check(bool condition, const std::string& message) {
    ++checks;
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
}

struct Rgba {
    int r = 0, g = 0, b = 0, a = 0;
};

Rgba pixel(const StillImage& image, int x, int y) {
    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x)) *
                        4U;
    return {image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2],
            image.rgba[offset + 3]};
}

bool isRed(const Rgba& p) {
    return p.r >= 200 && p.g <= 60 && p.b <= 60 && p.a == 255;
}

bool isBlue(const Rgba& p) {
    return p.b >= 200 && p.r <= 60 && p.g <= 60 && p.a == 255;
}

bool near(int value, int expected, int tolerance) {
    return std::abs(value - expected) <= tolerance;
}

// ---------------------------------------------------------------------------
// 判定

void testFacts(const fs::path& dir) {
    using mvm::media::probeMediaStreamFacts;

    for (const char* name :
         {"png_rgb24.png", "jpg_quadrant.jpg", "tga_24.tga", "bmp_24.bmp", "qoi_rgb.qoi",
          "tiff_rgb48.tif", "webp_static.webp", "gif_static.gif", "png_pal8_alpha.png"}) {
        const auto facts = probeMediaStreamFacts(dir / name);
        check(facts.ok && facts.stillImageCodec && facts.imagePacketCountUpTo2 == 1 &&
                  !facts.webpAnimationFlag && facts.audioStreamCount == 0,
              std::string("静止画として判定できません: ") + name + " (" + facts.formatName + " / " +
                  facts.videoCodecName + " / " + facts.error + ")");
    }

    for (const char* name : {"gif_animated.gif", "apng_animated.png"}) {
        const auto facts = probeMediaStreamFacts(dir / name);
        check(facts.ok && facts.stillImageCodec && facts.imagePacketCountUpTo2 == 2,
              std::string("アニメーションの packet を 2 まで数えていません: ") + name);
    }
    // FFmpeg 8.1 はアニメーション WebP を 1 packet として返す。header の flag で判別する。
    const auto animatedWebp = probeMediaStreamFacts(dir / "webp_animated.webp");
    check(animatedWebp.webpAnimationFlag, "アニメーション WebP の flag を読めません");
    check(!probeMediaStreamFacts(dir / "webp_static.webp").webpAnimationFlag,
          "静止画 WebP にアニメーション flag を立てました");

    const auto exr = probeMediaStreamFacts(dir / "exr_float.exr");
    check(exr.ok && exr.hdrImageCodec && !exr.stillImageCodec,
          "EXR を HDR 画像として判定できません");

    for (const char* name : {"mp3_cover.mp3", "m4a_cover.m4a", "flac_cover.flac"}) {
        const auto facts = probeMediaStreamFacts(dir / name);
        check(facts.ok && facts.videoStreamCount == 0 && facts.attachedPictureCount == 1 &&
                  facts.audioStreamCount == 1 && !facts.stillImageCodec,
              std::string("カバーアートを映像 stream と区別できません: ") + name);
    }
    for (const char* name : {"opus_48k.opus", "vorbis_48k.ogg"}) {
        const auto facts = probeMediaStreamFacts(dir / name);
        check(facts.ok && facts.videoStreamCount == 0 && facts.attachedPictureCount == 0 &&
                  facts.audioStreamCount == 1,
              std::string("音声だけの素材として判定できません: ") + name);
    }

    // 対照: 映像 + カバーアートの mp4 では本物の映像を 1 本数える。
    const auto coverVideo = probeMediaStreamFacts(dir / "mp4_h264_with_cover.mp4");
    check(coverVideo.ok && coverVideo.videoStreamCount == 1 &&
              coverVideo.attachedPictureCount == 1 && coverVideo.videoCodecName == "h264",
          "映像 + カバーアートの mp4 で映像 stream を取り違えました");
    // 対照: codec が JPEG でも動画の器 (avi) なら静止画にしない。
    const auto motionJpeg = probeMediaStreamFacts(dir / "mjpeg_av.avi");
    check(motionJpeg.ok && motionJpeg.videoCodecName == "mjpeg" && !motionJpeg.stillImageCodec &&
              motionJpeg.audioStreamCount == 1,
          "Motion JPEG の動画を静止画として判定しました");

    check(!probeMediaStreamFacts(dir / "does_not_exist.png").ok, "存在しない素材を受理しました");

    // PNG は stream 情報の取得で decode される。上限がそこへ渡っていれば decode されず、
    // 寸法は分からないまま (0) になる。渡っていなければ 4200 が見えている = 確保済み。
    const auto limited =
        probeMediaStreamFacts(dir / "png_gray_4200.png", StillImageLimits{16384, 16'000'000});
    check(limited.ok && limited.stillImageCodec && limited.width == 0,
          "stream 情報の取得で画素数の上限を超える PNG を decode しました (width=" +
              std::to_string(limited.width) + ")");
    const auto unlimited =
        probeMediaStreamFacts(dir / "png_gray_4200.png", StillImageLimits{16384, 32'000'000});
    check(unlimited.ok && unlimited.width == 4200,
          "上限内の PNG の寸法を stream 情報から取得できません");
}

// ---------------------------------------------------------------------------
// decoder

void testDecodeOrientation(const fs::path& dir) {
    using mvm::media::decodeStillImage;

    // 元画像は 64x32、左上 32x16 が赤で残りが青。
    const auto upright = decodeStillImage(dir / "jpg_exif_orient1.jpg");
    check(upright.success && upright.orientation == 1 && upright.image.width == 64 &&
              upright.image.height == 32,
          "orientation 1 の JPEG を decode できません: " + upright.error);
    if (upright.success) {
        check(isRed(pixel(upright.image, 8, 4)), "orientation 1 で左上が赤ではありません");
        check(isBlue(pixel(upright.image, 48, 24)), "orientation 1 で右下が青ではありません");
    }

    // orientation 6 は時計回り 90 度。32x64 になり、赤は右上 (x 16..31, y 0..31) へ移る。
    for (const char* name : {"jpg_exif_orient6.jpg", "写真　縦向き＆EXIF.jpg"}) {
        const auto rotated = decodeStillImage(dir / utf8Path(name));
        check(rotated.success && rotated.orientation == 6 && rotated.image.width == 32 &&
                  rotated.image.height == 64,
              std::string("orientation 6 を反映できません: ") + name + " " + rotated.error);
        if (!rotated.success)
            continue;
        check(isRed(pixel(rotated.image, 24, 8)), "orientation 6 で右上が赤ではありません");
        check(isBlue(pixel(rotated.image, 8, 8)), "orientation 6 で左上が青ではありません");
        check(isBlue(pixel(rotated.image, 24, 48)), "orientation 6 で右下が青ではありません");
    }
}

void testDecodeColor(const fs::path& dir) {
    using mvm::media::decodeStillImage;

    // JPEG は full range。limited range として展開すると 128 は約 130、逆なら約 126 になる。
    for (const char* name : {"jpg_gray128.jpg", "png_gray16.png"}) {
        const auto gray = decodeStillImage(dir / name);
        check(gray.success,
              std::string("灰色の画像を decode できません: ") + name + " " + gray.error);
        if (!gray.success)
            continue;
        bool allGray = true;
        for (int y = 0; y < gray.image.height; ++y) {
            for (int x = 0; x < gray.image.width; ++x) {
                const Rgba p = pixel(gray.image, x, y);
                allGray = allGray && near(p.r, 128, 1) && near(p.g, 128, 1) && near(p.b, 128, 1) &&
                          p.a == 255;
            }
        }
        check(allGray, std::string("灰色 128 が ±1 に収まりません: ") + name + " (" +
                           gray.sourcePixelFormat + ")");
    }

    // palette の alpha が残ること。透明な部分を不透明な黒にしない。
    const auto palette = decodeStillImage(dir / "png_pal8_alpha.png");
    check(palette.success && palette.sourcePixelFormat == "pal8",
          "pal8 の PNG を decode できません: " + palette.error);
    if (palette.success) {
        check(isRed(pixel(palette.image, 8, 4)), "pal8 の不透明な赤が失われました");
        check(pixel(palette.image, 48, 24).a == 0, "pal8 の透明な画素が不透明になりました");
    }

    for (const char* name : {"png_rgb24.png", "tiff_rgb48.tif", "bmp_24.bmp", "tga_24.tga",
                             "qoi_rgb.qoi", "webp_static.webp", "gif_static.gif"}) {
        const auto decoded = decodeStillImage(dir / name);
        check(decoded.success && decoded.image.width == 64 && decoded.image.height == 48 &&
                  decoded.image.rgba.size() == 64U * 48U * 4U,
              std::string("64x48 の画像として decode できません: ") + name + " " + decoded.error);
    }
}

// ICC profile を持つ画像。色の変換はしないので、sRGB 以外は拒否し sRGB は受理する (対照)。
void testDecodeIcc(const fs::path& dir) {
    using mvm::media::decodeStillImage;
    const auto p3 = decodeStillImage(dir / "png_icc_display_p3.png");
    check(!p3.success && p3.error.find("ICC") != std::string::npos &&
              p3.error.find("Display P3") != std::string::npos,
          "Display P3 の ICC profile を持つ画像を拒否しません: " + p3.error);
    // 説明は sRGB を名乗るが原色は Display P3。説明ではなく中身で判定していること。
    const auto fake = decodeStillImage(dir / "png_icc_fake_srgb.png");
    check(!fake.success && fake.error.find("rXYZ") != std::string::npos,
          "sRGB を名乗る Display P3 の ICC profile を持つ画像を拒否しません: " + fake.error);
    const auto srgb = decodeStillImage(dir / "png_icc_srgb.png");
    check(srgb.success && srgb.image.width == 64,
          "対照: sRGB の ICC profile を持つ画像を decode できません: " + srgb.error);
}

// 原色は D50 へ Bradford 変換した XYZ。
constexpr double kSrgb[3][3] = {
    {0.436066, 0.222488, 0.013916}, {0.385147, 0.716873, 0.097076}, {0.143066, 0.060608, 0.714096}};
constexpr double kDisplayP3[3][3] = {{0.515121, 0.241196, -0.001053},
                                     {0.291977, 0.692245, 0.041885},
                                     {0.157104, 0.066574, 0.784073}};

enum class Curve { SrgbParametric, SrgbTable, Gamma22Table, Gamma22Single };

struct IccSpec {
    const char* deviceClass = "mntr";
    const char* colourSpace = "RGB ";
    std::string description = "sRGB IEC61966-2.1";
    bool v4 = false;
    const double (*colorants)[3] = kSrgb;
    double white[3] = {0.964203, 1.0, 0.824905};
    Curve curve = Curve::SrgbParametric;
    bool lut = false;      // A2B0 tag を持つ
    bool omitBlue = false; // bXYZ tag が無い
};

double srgbCurve(double x) {
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

// matrix/TRC 型の ICC profile を組む。v2 は 'desc' 型、v4 は 'mluc' 型の説明を持つ。
std::vector<std::uint8_t> iccProfile(const IccSpec& spec) {
    using Bytes = std::vector<std::uint8_t>;
    const auto be16 = [](Bytes& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    };
    const auto be32 = [](Bytes& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto fixed = [&](Bytes& out, double value) {
        be32(out,
             static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value * 65536))));
    };
    const auto text = [](Bytes& out, const std::string& value) {
        out.insert(out.end(), value.begin(), value.end());
    };
    const auto xyz = [&](const double* value) {
        Bytes tag;
        text(tag, "XYZ ");
        be32(tag, 0);
        for (int i = 0; i < 3; ++i)
            fixed(tag, value[i]);
        return tag;
    };

    Bytes desc;
    if (spec.v4) {
        text(desc, "mluc");
        be32(desc, 0);
        be32(desc, 1);  // record 数
        be32(desc, 12); // record の大きさ
        text(desc, "enUS");
        be32(desc, static_cast<std::uint32_t>(spec.description.size() * 2));
        be32(desc, 28); // 文字列の位置 (tag の先頭から)
        for (const char c : spec.description) {
            desc.push_back(0);
            desc.push_back(static_cast<std::uint8_t>(c));
        }
    } else {
        text(desc, "desc");
        be32(desc, 0);
        be32(desc, static_cast<std::uint32_t>(spec.description.size() + 1));
        text(desc, spec.description);
        desc.push_back(0);
    }
    Bytes trc;
    switch (spec.curve) {
    case Curve::SrgbParametric:
        text(trc, "para");
        be32(trc, 0);
        be16(trc, 3);
        be16(trc, 0);
        for (const double value : {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045})
            fixed(trc, value);
        break;
    case Curve::SrgbTable:
    case Curve::Gamma22Table:
        text(trc, "curv");
        be32(trc, 0);
        be32(trc, 1024);
        for (int i = 0; i < 1024; ++i) {
            const double x = i / 1023.0;
            const double y = spec.curve == Curve::SrgbTable ? srgbCurve(x) : std::pow(x, 2.2);
            be16(trc, static_cast<std::uint32_t>(std::lround(y * 65535)));
        }
        break;
    case Curve::Gamma22Single:
        text(trc, "curv");
        be32(trc, 0);
        be32(trc, 1);
        be16(trc, 0x0233); // u8Fixed8 の 2.2
        break;
    }

    std::vector<std::pair<std::string, Bytes>> tags = {
        {"desc", desc},
        {"wtpt", xyz(spec.white)},
        {"rXYZ", xyz(spec.colorants[0])},
        {"gXYZ", xyz(spec.colorants[1])},
        {"rTRC", trc},
        {"gTRC", trc},
        {"bTRC", trc},
    };
    if (!spec.omitBlue)
        tags.push_back({"bXYZ", xyz(spec.colorants[2])});
    if (spec.lut) {
        Bytes lut;
        text(lut, "mft2");
        lut.resize(52, 0);
        tags.push_back({"A2B0", lut});
    }

    const std::size_t tableEnd = 128 + 4 + tags.size() * 12;
    Bytes table;
    Bytes body;
    be32(table, static_cast<std::uint32_t>(tags.size()));
    for (const auto& [signature, data] : tags) {
        text(table, signature);
        be32(table, static_cast<std::uint32_t>(tableEnd + body.size()));
        be32(table, static_cast<std::uint32_t>(data.size()));
        body.insert(body.end(), data.begin(), data.end());
        while (body.size() % 4 != 0)
            body.push_back(0);
    }
    Bytes profile;
    be32(profile, static_cast<std::uint32_t>(tableEnd + body.size()));
    profile.resize(12, 0);
    text(profile, spec.deviceClass);
    text(profile, spec.colourSpace);
    text(profile, "XYZ ");
    profile.resize(128, 0);
    profile.insert(profile.end(), table.begin(), table.end());
    profile.insert(profile.end(), body.begin(), body.end());
    return profile;
}

bool isSrgb(const IccSpec& spec, std::string& description, std::string& reason) {
    const auto profile = iccProfile(spec);
    return mvm::media::iccProfileIsSrgb(profile.data(), profile.size(), description, reason);
}

void testIccClassification() {
    using mvm::media::iccProfileIsSrgb;
    std::string description;
    std::string reason;
    for (const bool v4 : {false, true}) {
        const std::string version = v4 ? " (v4 mluc)" : " (v2 desc)";
        IccSpec srgb;
        srgb.v4 = v4;
        check(isSrgb(srgb, description, reason) && description == "sRGB IEC61966-2.1",
              "sRGB の profile を sRGB と判定しません" + version + ": " + reason);
        // 説明は判定に使わない。説明の無い sRGB も sRGB (対照)。
        IccSpec unnamed = srgb;
        unnamed.description = "";
        check(isSrgb(unnamed, description, reason),
              "説明の無い sRGB の profile を sRGB と判定しません" + version + ": " + reason);
        IccSpec p3 = srgb;
        p3.description = "Display P3";
        p3.colorants = kDisplayP3;
        check(!isSrgb(p3, description, reason) && description == "Display P3" &&
                  reason.find("rXYZ") != std::string::npos,
              "Display P3 を sRGB と判定しました" + version);
        // 説明は sRGB を名乗るが中身は Display P3。
        IccSpec fake = p3;
        fake.description = "sRGB compatible profile";
        check(!isSrgb(fake, description, reason) && description == "sRGB compatible profile",
              "sRGB を名乗る Display P3 の profile を sRGB と判定しました" + version);
        IccSpec gray = srgb;
        gray.colourSpace = "GRAY";
        check(!isSrgb(gray, description, reason),
              "RGB でない profile を sRGB と判定しました" + version);
    }

    const IccSpec base;
    // トーンカーブ。sRGB の表 (1024 点) は通し、gamma 2.2 (表 / 1 点) は通さない。
    IccSpec table = base;
    table.curve = Curve::SrgbTable;
    check(isSrgb(table, description, reason),
          "sRGB の曲線を表で持つ profile を sRGB と判定しません: " + reason);
    for (const Curve curve : {Curve::Gamma22Table, Curve::Gamma22Single}) {
        IccSpec gamma = base;
        gamma.curve = curve;
        check(!isSrgb(gamma, description, reason) && reason.find("TRC") != std::string::npos,
              "原色が sRGB で gamma 2.2 の profile を sRGB と判定しました");
    }
    // 白色点。v2 の sRGB profile が書く D65 は通し、それ以外は通さない。
    IccSpec d65 = base;
    d65.white[0] = 0.950455;
    d65.white[2] = 1.089050;
    check(isSrgb(d65, description, reason), "白色点 D65 の sRGB profile を拒否しました: " + reason);
    IccSpec warm = base;
    warm.white[0] = 1.0985; // A 光源
    warm.white[2] = 0.3558;
    check(!isSrgb(warm, description, reason) && reason.find("wtpt") != std::string::npos,
          "白色点が D50 / D65 でない profile を sRGB と判定しました");
    // 中身を検証できない LUT 型、原色の欠けた profile、表示装置用でない profile。
    IccSpec lut = base;
    lut.lut = true;
    check(!isSrgb(lut, description, reason) && reason.find("A2B0") != std::string::npos,
          "LUT 型の tag を持つ profile を sRGB と判定しました");
    IccSpec missing = base;
    missing.omitBlue = true;
    check(!isSrgb(missing, description, reason) && reason.find("bXYZ") != std::string::npos,
          "bXYZ の無い profile を sRGB と判定しました");
    IccSpec scanner = base;
    scanner.deviceClass = "scnr";
    check(!isSrgb(scanner, description, reason), "表示装置用でない profile を sRGB と判定しました");

    // 壊れた profile。header に満たない / 宣言した大きさが buffer を超える / tag が外を指す。
    const auto good = iccProfile(base);
    check(isSrgb(base, description, reason), "前提: 基準の profile が sRGB ではありません");
    check(!iccProfileIsSrgb(good.data(), 100, description, reason),
          "header に満たない profile を sRGB と判定しました");
    check(!iccProfileIsSrgb(good.data(), good.size() - 4, description, reason),
          "宣言より短い profile を sRGB と判定しました");
    auto outside = good;
    outside[132 + 12 * 2 + 4] = 0x7F; // rXYZ の offset を profile の外へ
    check(!iccProfileIsSrgb(outside.data(), outside.size(), description, reason),
          "外を指す tag を持つ profile を sRGB と判定しました");

    // 実物: Windows 同梱の sRGB profile (HP の v2、curv 1024 点) は sRGB。
    const fs::path windowsSrgb =
        "C:/Windows/System32/spool/drivers/color/sRGB Color Space Profile.icm";
    std::ifstream file(windowsSrgb, std::ios::binary);
    const std::vector<std::uint8_t> real((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
    check(!real.empty() && iccProfileIsSrgb(real.data(), real.size(), description, reason),
          "Windows 同梱の sRGB profile を sRGB と判定しません: " + reason);
}

// 静止画の authority。取り込み・preview・書き出しが共通に通す。静止画だけを decode し、
// decodeStillImage なら先頭 frame を読めてしまう動画・アニメーション画像・HDR 画像を拒否する。
void testLoadStaticImage(const fs::path& dir) {
    using mvm::media::decodeStillImage;
    using mvm::media::loadStaticImage;
    const auto still = loadStaticImage(dir / "png_rgb24.png");
    check(still.success && still.image.width == 64, "対照: 静止画を読めません: " + still.error);

    struct Rejected {
        const char* file;
        const char* reason;
    };

    for (const Rejected& rejected :
         {Rejected{"gif_animated.gif", "アニメーション"},
          Rejected{"apng_animated.png", "アニメーション"},
          Rejected{"webp_animated.webp", "アニメーション"},
          Rejected{"mp4_h264_with_cover.mp4", "静止画ではありません"},
          Rejected{"mjpeg_av.avi", "静止画ではありません"}, Rejected{"exr_float.exr", "HDR"}}) {
        const auto result = loadStaticImage(dir / rejected.file);
        check(!result.success && result.error.find(rejected.reason) != std::string::npos,
              std::string("静止画でない素材を静止画として読みました: ") + rejected.file + " (" +
                  result.error + ")");
    }
    // 対照: decoder 単体は animated GIF の先頭 frame を読める。authority を通さないと
    // 受理してしまうことが、上の拒否の意味。
    check(decodeStillImage(dir / "gif_animated.gif").success,
          "前提: decoder 単体が animated GIF の先頭 frame を読めません");
}

void testDecodeLimits(const fs::path& dir) {
    using mvm::media::decodeStillImage;

    // 4200x4200 = 17,640,000 画素。上限 16M では拒否し、32M では通す (対照)。
    const StillImageLimits small{16384, 16'000'000};
    const auto rejected = decodeStillImage(dir / "png_gray_4200.png", small);
    check(!rejected.success && rejected.error.find("16000000") != std::string::npos,
          "画素数の上限を超える PNG を拒否しません: " + rejected.error);
    const StillImageLimits large{16384, 32'000'000};
    const auto accepted = decodeStillImage(dir / "png_gray_4200.png", large);
    check(accepted.success && accepted.image.width == 4200 && accepted.image.height == 4200,
          "上限内の 4200x4200 PNG を decode できません: " + accepted.error);

    // 1 辺の上限。header から寸法が分かるので decoder を開く前に拒否する。
    const auto narrow = decodeStillImage(dir / "png_rgb24.png", StillImageLimits{32, 1'000'000});
    check(!narrow.success && narrow.error.find("大きすぎ") != std::string::npos,
          "1 辺の上限を超える画像を拒否しません: " + narrow.error);
    check(decodeStillImage(dir / "png_rgb24.png", StillImageLimits{64, 1'000'000}).success,
          "1 辺がちょうど上限の画像を拒否しました");

    check(!decodeStillImage(dir / "does_not_exist.png").success, "存在しない画像を受理しました");
}

// ---------------------------------------------------------------------------
// 純粋関数

StillImage numbered3x2() {
    // 画素 (x, y) の R に 10y + x + 1 を入れる。
    //   1  2  3
    //  11 12 13
    StillImage image;
    image.width = 3;
    image.height = 2;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            image.rgba.push_back(static_cast<std::uint8_t>(10 * y + x + 1));
            image.rgba.push_back(0);
            image.rgba.push_back(0);
            image.rgba.push_back(255);
        }
    }
    return image;
}

std::vector<int> redChannel(const StillImage& image) {
    std::vector<int> values;
    for (std::size_t index = 0; index < image.rgba.size(); index += 4)
        values.push_back(image.rgba[index]);
    return values;
}

void testOrientationTable() {
    struct Case {
        int orientation;
        int width;
        int height;
        std::vector<int> rows; // 行優先
    };

    // EXIF 2.3 の定義 (反転と回転の組み合わせ) から手で導いた表。
    const std::vector<Case> cases = {
        {1, 3, 2, {1, 2, 3, 11, 12, 13}}, {2, 3, 2, {3, 2, 1, 13, 12, 11}}, // 左右反転
        {3, 3, 2, {13, 12, 11, 3, 2, 1}},                                   // 180 度
        {4, 3, 2, {11, 12, 13, 1, 2, 3}},                                   // 上下反転
        {5, 2, 3, {1, 11, 2, 12, 3, 13}}, // 左右反転 + 反時計回り 90 度
        {6, 2, 3, {11, 1, 12, 2, 13, 3}}, // 時計回り 90 度
        {7, 2, 3, {13, 3, 12, 2, 11, 1}}, // 左右反転 + 時計回り 90 度
        {8, 2, 3, {3, 13, 2, 12, 1, 11}}, // 反時計回り 90 度
    };
    for (const auto& entry : cases) {
        StillImage image = numbered3x2();
        std::string error;
        const bool ok = mvm::media::applyExifOrientation(image, entry.orientation, error);
        check(ok && image.width == entry.width && image.height == entry.height &&
                  redChannel(image) == entry.rows,
              "orientation " + std::to_string(entry.orientation) + " の並びが定義と一致しません");
    }
    for (const int invalid : {0, 9, -1}) {
        StillImage image = numbered3x2();
        std::string error;
        check(!mvm::media::applyExifOrientation(image, invalid, error) &&
                  redChannel(image) == redChannel(numbered3x2()),
              "範囲外の orientation " + std::to_string(invalid) + " を受理しました");
    }
    StillImage broken = numbered3x2();
    broken.rgba.pop_back();
    std::string error;
    check(!mvm::media::applyExifOrientation(broken, 6, error),
          "画素数の足りない画像を受理しました");
}

StillImage solid(int width, int height, Rgba color) {
    StillImage image;
    image.width = width;
    image.height = height;
    for (int index = 0; index < width * height; ++index) {
        image.rgba.push_back(static_cast<std::uint8_t>(color.r));
        image.rgba.push_back(static_cast<std::uint8_t>(color.g));
        image.rgba.push_back(static_cast<std::uint8_t>(color.b));
        image.rgba.push_back(static_cast<std::uint8_t>(color.a));
    }
    return image;
}

void testFit() {
    using mvm::media::fitStillImageToRaster;

    // 横長 4x2 を 8x8 へ: 8x4 になり、上下 2 行ずつが透明。
    const auto wide = fitStillImageToRaster(solid(4, 2, {255, 0, 0, 255}), 8, 8);
    check(wide.success && wide.raster.width == 8 && wide.raster.height == 8,
          "横長の画像を raster へ置けません: " + wide.error);
    if (wide.success) {
        bool layout = true;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const Rgba p = pixel(wide.raster, x, y);
                layout = layout && (y >= 2 && y < 6 ? isRed(p) : p.a == 0);
            }
        }
        check(layout, "横長の画像が上下中央に 8x4 で置かれていません");
    }

    // 縦長 2x4 を 8x4 へ: 拡縮なしで左右 3 列ずつが透明。
    const auto tall = fitStillImageToRaster(solid(2, 4, {255, 0, 0, 255}), 8, 4);
    check(tall.success, "縦長の画像を raster へ置けません: " + tall.error);
    if (tall.success) {
        bool layout = true;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) {
                const Rgba p = pixel(tall.raster, x, y);
                layout = layout && (x >= 3 && x < 5 ? isRed(p) : p.a == 0);
            }
        }
        check(layout, "縦長の画像が左右中央に置かれていません");
    }

    // 左半分が不透明な赤、右半分が完全に透明な緑の 16x4 を 4 倍に拡大する。
    // premultiplied で縮拡していれば、透明な画素の色 (緑) は境目の半透明な画素へ混ざらない。
    // straight alpha のまま bicubic で拡大すると、境目に G=0x5a 程度の緑が出る (実測)。
    // 幅 2 のような小さな画像では swscale の filter が縮退して最近傍と同じ結果になり、
    // 境目ができないので検査にならない (実測)。
    StillImage edge;
    edge.width = 16;
    edge.height = 4;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool red = x < 8;
            edge.rgba.insert(edge.rgba.end(), {static_cast<std::uint8_t>(red ? 255 : 0),
                                               static_cast<std::uint8_t>(red ? 0 : 255), 0,
                                               static_cast<std::uint8_t>(red ? 255 : 0)});
        }
    }
    const auto blended = fitStillImageToRaster(edge, 64, 16);
    check(blended.success, "境目の検査用画像を raster へ置けません: " + blended.error);
    if (blended.success) {
        bool noBleed = true;
        int partial = 0;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 64; ++x) {
                const Rgba p = pixel(blended.raster, x, y);
                if (p.a > 0)
                    noBleed = noBleed && p.g <= 2;
                if (p.a > 0 && p.a < 255)
                    ++partial;
            }
        }
        // 半透明な画素が無ければ、この検査は境目を見ていない。
        check(partial > 0, "拡大しても半透明な境目ができません (検査が空振りしています)");
        check(noBleed, "透明な画素の色が境目へにじみました");
    }

    check(!fitStillImageToRaster(solid(4, 2, {0, 0, 0, 255}), 0, 8).success,
          "幅 0 の raster を受理しました");
    StillImage broken = solid(4, 2, {0, 0, 0, 255});
    broken.rgba.resize(4);
    check(!fitStillImageToRaster(broken, 8, 8).success, "画素数の足りない画像を受理しました");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_still_image <_import directory>\n");
        return 2;
    }
    const fs::path dir = utf8Path(argv[1]);
    if (!fs::exists(dir / "jpg_exif_orient6.jpg")) {
        std::fprintf(stderr,
                     "素材がありません: %s\n先に pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
                     "実行してください\n",
                     argv[1]);
        return 3;
    }
    testFacts(dir);
    testDecodeOrientation(dir);
    testDecodeColor(dir);
    testDecodeLimits(dir);
    testDecodeIcc(dir);
    testIccClassification();
    testLoadStaticImage(dir);
    testOrientationTable();
    testFit();
    std::printf("still_image: %d 件の検査、失敗 %d 件\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
