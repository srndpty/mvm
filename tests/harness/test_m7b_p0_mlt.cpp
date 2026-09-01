// M7b-P0: 製品exportを変更せず、MLT tractor/affine overlayの実画素意味論を固定する。

#include "core/clip_fade.h"
#include "media/mlt/mvm_mlt_overlay_p0.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <process.h>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kTotalDuration = 90;
constexpr int kTimelineStart = 20;
constexpr int kSourceIn = 10;
constexpr int kOverlayDuration = 40;

struct Region {
    int x = 0;
    int y = 0;
    int width = kWidth;
    int height = kHeight;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

struct DifferenceBounds {
    int pixels = 0;
    int minX = kWidth;
    int minY = kHeight;
    int maxX = -1;
    int maxY = -1;
};

struct ClipSpec {
    int timelineStart = kTimelineStart;
    int sourceIn = kSourceIn;
    int duration = kOverlayDuration;
    int cropLeft = 0;
    int cropTop = 0;
    int cropRight = 0;
    int cropBottom = 0;
    double rectX = 80.0;
    double rectY = 60.0;
    double rectWidth = 160.0;
    double rectHeight = 120.0;
    double rotation = 0.0;
    int transitionIn = kTimelineStart;
    int transitionOut = kTimelineStart + kOverlayDuration - 1;
    std::vector<MvmM7bP0OpacityKeyframe> keys{{0, 1.0}, {kOverlayDuration - 1, 1.0}};
};

struct RenderCase {
    std::string name;
    std::vector<ClipSpec> clips;
    std::filesystem::path output;
    MvmM7bP0RenderResult result{};
};

struct DomainEvidence {
    double absoluteTransitionError = 0.0;
    double localTransitionError = 0.0;
    double localKeyError = 0.0;
    double absoluteKeyError = 0.0;
    std::string transitionDomain;
    std::string keyframeDomain;
};

int failures = 0;

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string toUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

void check(bool condition, const std::string& message) {
    if (condition)
        return;
    std::fprintf(stderr, "NG: %s\n", message.c_str());
    ++failures;
}

bool spawnFixture(const std::filesystem::path& ffmpeg, const std::filesystem::path& output,
                  const wchar_t* color, const wchar_t* filter) {
    const intptr_t exitCode = _wspawnl(
        _P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f", L"lavfi",
        L"-i", color, L"-vf", filter, L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-crf", L"8",
        L"-pix_fmt", L"yuv420p", output.c_str(), static_cast<wchar_t*>(nullptr));
    return exitCode == 0 && std::filesystem::is_regular_file(output);
}

bool generateFixtures(const std::filesystem::path& ffmpeg, const std::filesystem::path& v1,
                      const std::filesystem::path& v2) {
    const wchar_t* v1Filter = L"drawgrid=w=40:h=30:t=2:c=white@0.35,"
                              L"drawbox=x=8:y=8:w=24:h=24:color=yellow:t=fill,"
                              L"drawbox=x=280:y=200:w=24:h=24:color=cyan:t=fill";
    const wchar_t* v2Filter = L"drawbox=x=0:y=0:w=iw:h=ih:color=lime:t=fill:enable='gte(n,10)',"
                              L"drawbox=x=0:y=0:w=iw:h=12:color=white:t=fill,"
                              L"drawbox=x=0:y=12:w=12:h=ih-24:color=yellow:t=fill,"
                              L"drawbox=x=0:y=ih-12:w=iw:h=12:color=cyan:t=fill,"
                              L"drawbox=x=iw-12:y=12:w=12:h=ih-24:color=blue:t=fill,"
                              L"drawbox=x=150:y=110:w=20:h=20:color=magenta:t=fill,"
                              L"drawbox=x=42:y=34:w=18:h=18:color=black:t=fill";
    return spawnFixture(ffmpeg, v1, L"color=c=0x204080:s=320x240:r=60:d=2", v1Filter) &&
           spawnFixture(ffmpeg, v2, L"color=c=red:s=320x240:r=60:d=2", v2Filter);
}

Image decodeFrame(const std::filesystem::path& path, int frame) {
    Image result;
    MvmMltImage decoded{};
    char error[1024] = {};
    if (mvm_mlt_decode_frame(toUtf8(path).c_str(), frame, &decoded, error, sizeof(error)) != 0 ||
        !decoded.rgba || decoded.width != kWidth || decoded.height != kHeight) {
        std::fprintf(stderr, "NG: %s frame %dをdecodeできません: %s\n", toUtf8(path).c_str(), frame,
                     error);
        ++failures;
        return result;
    }
    result.width = decoded.width;
    result.height = decoded.height;
    result.rgba.assign(decoded.rgba,
                       decoded.rgba + static_cast<std::size_t>(decoded.width * decoded.height * 4));
    mvm_mlt_image_free(&decoded);
    return result;
}

bool validImage(const Image& image) {
    return image.width == kWidth && image.height == kHeight &&
           image.rgba.size() == static_cast<std::size_t>(kWidth * kHeight * 4);
}

Region clipped(Region region) {
    region.x = std::clamp(region.x, 0, kWidth);
    region.y = std::clamp(region.y, 0, kHeight);
    region.width = std::clamp(region.width, 0, kWidth - region.x);
    region.height = std::clamp(region.height, 0, kHeight - region.y);
    return region;
}

double meanDifference(const Image& a, const Image& b, Region region = {}) {
    if (!validImage(a) || !validImage(b))
        return 1e9;
    region = clipped(region);
    if (region.width <= 0 || region.height <= 0)
        return 1e9;
    double sum = 0.0;
    for (int y = region.y; y < region.y + region.height; ++y) {
        for (int x = region.x; x < region.x + region.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * kWidth + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
                sum +=
                    std::abs(static_cast<int>(a.rgba[offset + static_cast<std::size_t>(channel)]) -
                             static_cast<int>(b.rgba[offset + static_cast<std::size_t>(channel)]));
        }
    }
    return sum / static_cast<double>(region.width * region.height * 3);
}

double meanBrightness(const Image& image, Region region = {}) {
    if (!validImage(image))
        return 0.0;
    region = clipped(region);
    double sum = 0.0;
    for (int y = region.y; y < region.y + region.height; ++y) {
        for (int x = region.x; x < region.x + region.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * kWidth + x) * 4;
            sum += image.rgba[offset] + image.rgba[offset + 1] + image.rgba[offset + 2];
        }
    }
    return sum / static_cast<double>(region.width * region.height * 3);
}

double meanChannel(const Image& image, Region region, int channel) {
    if (!validImage(image))
        return 0.0;
    region = clipped(region);
    double sum = 0.0;
    for (int y = region.y; y < region.y + region.height; ++y)
        for (int x = region.x; x < region.x + region.width; ++x)
            sum += image.rgba[static_cast<std::size_t>(y * kWidth + x) * 4 +
                              static_cast<std::size_t>(channel)];
    return sum / static_cast<double>(region.width * region.height);
}

double blendError(const Image& actual, const Image& bottom, const Image& fullTop, double opacity,
                  Region region = {}) {
    if (!validImage(actual) || !validImage(bottom) || !validImage(fullTop))
        return 1e9;
    region = clipped(region);
    double sum = 0.0;
    for (int y = region.y; y < region.y + region.height; ++y) {
        for (int x = region.x; x < region.x + region.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * kWidth + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const std::size_t channelOffset = offset + static_cast<std::size_t>(channel);
                const double expected = bottom.rgba[channelOffset] * (1.0 - opacity) +
                                        fullTop.rgba[channelOffset] * opacity;
                sum += std::abs(actual.rgba[channelOffset] - expected);
            }
        }
    }
    return sum / static_cast<double>(region.width * region.height * 3);
}

DifferenceBounds differenceBounds(const Image& actual, const Image& baseline) {
    DifferenceBounds bounds;
    if (!validImage(actual) || !validImage(baseline))
        return bounds;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * kWidth + x) * 4;
            int difference = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const std::size_t channelOffset = offset + static_cast<std::size_t>(channel);
                difference += std::abs(static_cast<int>(actual.rgba[channelOffset]) -
                                       static_cast<int>(baseline.rgba[channelOffset]));
            }
            if (difference <= 45)
                continue;
            ++bounds.pixels;
            bounds.minX = std::min(bounds.minX, x);
            bounds.minY = std::min(bounds.minY, y);
            bounds.maxX = std::max(bounds.maxX, x);
            bounds.maxY = std::max(bounds.maxY, y);
        }
    }
    return bounds;
}

std::vector<MvmM7bP0OpacityKeyframe> fadeKeys(int positionOffset) {
    constexpr int fadeFrames = 8;
    const int positions[] = {0, fadeFrames - 1, kOverlayDuration - fadeFrames,
                             kOverlayDuration - 1};
    std::vector<MvmM7bP0OpacityKeyframe> keys;
    for (const int local : positions) {
        keys.push_back(
            {positionOffset + local,
             0.8 * mvm::core::clipFadeFactor(local, kOverlayDuration, fadeFrames, fadeFrames)});
    }
    return keys;
}

bool renderCase(const std::filesystem::path& v1, const std::filesystem::path& v2,
                const std::filesystem::path& directory, RenderCase& item) {
    item.output = directory / fromUtf8((item.name + ".mp4").c_str());
    std::vector<MvmM7bP0OverlayClip> raw;
    raw.reserve(item.clips.size());
    for (const auto& clip : item.clips) {
        raw.push_back({clip.timelineStart, clip.sourceIn, clip.duration, clip.cropLeft,
                       clip.cropTop, clip.cropRight, clip.cropBottom, clip.rectX, clip.rectY,
                       clip.rectWidth, clip.rectHeight, clip.rotation, clip.transitionIn,
                       clip.transitionOut, clip.keys.data(), static_cast<int>(clip.keys.size())});
    }
    const std::string v1Text = toUtf8(v1);
    const std::string v2Text = toUtf8(v2);
    const std::string outputText = toUtf8(item.output);
    MvmM7bP0RenderRequest request{};
    request.v1_source_path = v1Text.c_str();
    request.v2_source_path = v2Text.c_str();
    request.output_path = outputText.c_str();
    request.width = kWidth;
    request.height = kHeight;
    request.fps_num = 60;
    request.fps_den = 1;
    request.total_duration = kTotalDuration;
    request.v2_clips = raw.data();
    request.v2_clip_count = static_cast<int>(raw.size());
    request.timeout_ms = 120000;
    char error[1024] = {};
    if (mvm_m7b_p0_render(&request, &item.result, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "NG: case %sをrenderできません: %s\n", item.name.c_str(), error);
        ++failures;
        return false;
    }
    check(item.result.frame_count >= kTotalDuration, item.name + "の出力frameが不足しています");
    check(item.result.playlist_count == 2, item.name + "がV1/V2の2 playlistではありません");
    check(item.result.opaque_black_affine_filter_count == 0,
          item.name + "がopaque-black affine filterを使用しています");
    return true;
}

RenderCase makeSingle(std::string name) {
    RenderCase result;
    result.name = std::move(name);
    result.clips.push_back({});
    return result;
}

RenderCase& findCase(std::vector<RenderCase>& cases, const std::string& name) {
    const auto found = std::find_if(cases.begin(), cases.end(),
                                    [&](const auto& item) { return item.name == name; });
    if (found == cases.end()) {
        std::fprintf(stderr, "NG: caseがありません: %s\n", name.c_str());
        ++failures;
        return cases.front();
    }
    return *found;
}

void writeJson(const std::filesystem::path& path, const std::filesystem::path& v1,
               const std::filesystem::path& v2, const std::vector<RenderCase>& cases,
               const DomainEvidence& domains, const std::map<std::string, double>& metrics) {
    std::ofstream json(path, std::ios::binary | std::ios::trunc);
    json << "{\n  \"schema\": \"m7b-p0-v1\",\n"
         << "  \"graph\": \"tractor:v1_playlist+v2_playlist->affine_transition\",\n"
         << "  \"v2_affine_filter\": false,\n"
         << "  \"affine_properties\": {\"fill\":1,\"distort\":1,\"b_alpha\":0,"
            "\"repeat_off\":1,\"mirror_off\":1,\"keyed\":0,"
            "\"halign\":\"center\",\"valign\":\"middle\","
            "\"rotation_property\":\"fix_rotate_x\","
            "\"opacity_property\":\"rect.o\"},\n"
         << "  \"crop_properties\": {\"parameter_filter\":{\"active\":0,"
            "\"use_profile\":1},\"active_filter\":{\"active\":1}},\n"
         << "  \"fixtures\": {\"v1\":\"" << toUtf8(v1) << "\",\"v2\":\"" << toUtf8(v2) << "\"},\n"
         << "  \"domains\": {\"transition_in_out\":\"" << domains.transitionDomain
         << "\",\"animation_keyframes\":\"" << domains.keyframeDomain
         << "\",\"absolute_transition_error\":" << domains.absoluteTransitionError
         << ",\"local_transition_error\":" << domains.localTransitionError
         << ",\"local_key_error\":" << domains.localKeyError
         << ",\"absolute_key_error\":" << domains.absoluteKeyError << "},\n"
         << "  \"metrics\": {";
    std::size_t metricIndex = 0;
    for (const auto& [name, value] : metrics) {
        if (metricIndex++)
            json << ',';
        json << "\"" << name << "\":" << value;
    }
    json << "},\n  \"cases\": [\n";
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        json << "    {\"name\":\"" << item.name << "\",\"output\":\"" << toUtf8(item.output)
             << "\",\"output_frames\":" << item.result.frame_count
             << ",\"playlist_count\":" << item.result.playlist_count
             << ",\"transition_count\":" << item.result.transition_count
             << ",\"crop_pair_count\":" << item.result.crop_pair_count
             << ",\"keyframes_verified\":" << item.result.keyframes_verified
             << ",\"opaque_black_affine_filter_count\":"
             << item.result.opaque_black_affine_filter_count << ",\"clips\":[";
        for (std::size_t clipIndex = 0; clipIndex < item.clips.size(); ++clipIndex) {
            const auto& clip = item.clips[clipIndex];
            json << "{\"timeline_start\":" << clip.timelineStart
                 << ",\"source_in\":" << clip.sourceIn << ",\"duration\":" << clip.duration
                 << ",\"crop\":[" << clip.cropLeft << ',' << clip.cropTop << ',' << clip.cropRight
                 << ',' << clip.cropBottom << "],\"rect\":[" << clip.rectX << ',' << clip.rectY
                 << ',' << clip.rectWidth << ',' << clip.rectHeight
                 << "],\"rotation_degrees\":" << clip.rotation
                 << ",\"transition_in\":" << clip.transitionIn
                 << ",\"transition_out\":" << clip.transitionOut << ",\"opacity_keys\":[";
            for (std::size_t keyIndex = 0; keyIndex < clip.keys.size(); ++keyIndex) {
                const auto& key = clip.keys[keyIndex];
                json << "{\"position\":" << key.position << ",\"opacity\":" << key.opacity << '}'
                     << (keyIndex + 1 == clip.keys.size() ? "" : ",");
            }
            json << "]}" << (clipIndex + 1 == item.clips.size() ? "" : ",");
        }
        json << "]}" << (index + 1 == cases.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"evaluated_case_count\": " << cases.size()
         << ",\n  \"failure_count\": " << failures << "\n}\n";
    check(json.good(), "M7b-P0生JSONを書き込めません");
}

} // namespace

int main(int argc, char** argv) {
    mvm_enable_utf8_console();
    if (argc == 2 && std::string(argv[1]) == "--negative-empty")
        return validImage({}) ? 1 : 7;
    if (argc != 4) {
        std::fprintf(stderr, "使い方: mvm_test_m7b_p0_mlt <出力dir> <ffmpeg.exe> <raw.json>\n");
        return 2;
    }
    const auto outputDirectory = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto ffmpeg = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto jsonPath = std::filesystem::absolute(fromUtf8(argv[3]));
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(ffmpeg)) {
        std::fprintf(stderr, "fixtureを生成できません。UCRT64 ffmpegを確認してください\n");
        return 2;
    }

    const auto v1 = outputDirectory / L"m7b-p0-v1.mp4";
    const auto v2 = outputDirectory / L"m7b-p0-v2.mp4";
    check(generateFixtures(ffmpeg, v1, v2), "M7b-P0 fixtureを生成できません");
    if (failures)
        return 1;
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません\n");
        return 1;
    }

    std::vector<RenderCase> cases;
    cases.push_back({"v1_only_baseline", {}, {}, {}});
    cases.push_back(makeSingle("centered_scaled_v2"));

    auto opacity = makeSingle("opacity_50_reveals_v1");
    opacity.clips[0].keys = {{0, 0.5}, {kOverlayDuration - 1, 0.5}};
    cases.push_back(opacity);

    auto crop = makeSingle("asymmetric_crop_reveals_v1");
    crop.clips[0].cropLeft = 64;
    crop.clips[0].cropTop = 24;
    crop.clips[0].cropRight = 32;
    crop.clips[0].cropBottom = 48;
    crop.clips[0].rectX = 64;
    crop.clips[0].rectY = 24;
    crop.clips[0].rectWidth = 224;
    crop.clips[0].rectHeight = 168;
    cases.push_back(crop);

    auto rotated = makeSingle("position_rotation");
    rotated.clips[0].rectX = 160;
    rotated.clips[0].rectY = 45;
    rotated.clips[0].rectWidth = 120;
    rotated.clips[0].rectHeight = 90;
    rotated.clips[0].rotation = 30;
    cases.push_back(rotated);

    auto fade = makeSingle("fade_reveals_v1");
    fade.clips[0].keys = fadeKeys(0);
    cases.push_back(fade);

    auto sourceZero = makeSingle("source_in_zero_control");
    sourceZero.clips[0].sourceIn = 0;
    cases.push_back(sourceZero);
    cases.push_back(makeSingle("nonzero_source_in"));
    cases.push_back(makeSingle("nonzero_timeline_start_and_blanks"));

    RenderCase twoClips{"two_nonoverlapping_v2_clips", {}, {}, {}};
    ClipSpec first;
    first.timelineStart = 10;
    first.sourceIn = 10;
    first.duration = 20;
    first.rectX = 20;
    first.rectY = 30;
    first.rectWidth = 120;
    first.rectHeight = 90;
    first.transitionIn = 10;
    first.transitionOut = 29;
    first.keys = {{0, 1.0}, {19, 1.0}};
    ClipSpec second;
    second.timelineStart = 55;
    second.sourceIn = 20;
    second.duration = 20;
    second.rectX = 180;
    second.rectY = 120;
    second.rectWidth = 120;
    second.rectHeight = 90;
    second.rotation = -20;
    second.transitionIn = 55;
    second.transitionOut = 74;
    second.keys = {{0, 1.0}, {19, 1.0}};
    twoClips.clips = {first, second};
    cases.push_back(twoClips);

    auto absoluteTransition = makeSingle("domain_absolute_transition_local_keys");
    absoluteTransition.clips[0].keys = fadeKeys(0);
    cases.push_back(absoluteTransition);

    auto localTransition = absoluteTransition;
    localTransition.name = "domain_local_transition_local_keys";
    localTransition.clips[0].transitionIn = 0;
    localTransition.clips[0].transitionOut = kOverlayDuration - 1;
    cases.push_back(localTransition);

    auto absoluteKeys = absoluteTransition;
    absoluteKeys.name = "domain_absolute_transition_absolute_keys";
    absoluteKeys.clips[0].keys = fadeKeys(kTimelineStart);
    cases.push_back(absoluteKeys);

    for (auto& item : cases)
        renderCase(v1, v2, outputDirectory, item);

    auto& baselineCase = findCase(cases, "v1_only_baseline");
    auto& centeredCase = findCase(cases, "centered_scaled_v2");
    auto& opacityCase = findCase(cases, "opacity_50_reveals_v1");
    auto& cropCase = findCase(cases, "asymmetric_crop_reveals_v1");
    auto& rotationCase = findCase(cases, "position_rotation");
    auto& fadeCase = findCase(cases, "fade_reveals_v1");
    auto& sourceZeroCase = findCase(cases, "source_in_zero_control");
    auto& sourceInCase = findCase(cases, "nonzero_source_in");
    auto& blankCase = findCase(cases, "nonzero_timeline_start_and_blanks");
    auto& twoCase = findCase(cases, "two_nonoverlapping_v2_clips");
    auto& absoluteTransitionCase = findCase(cases, "domain_absolute_transition_local_keys");
    auto& localTransitionCase = findCase(cases, "domain_local_transition_local_keys");
    auto& absoluteKeysCase = findCase(cases, "domain_absolute_transition_absolute_keys");

    std::map<int, Image> baselineFrames;
    for (const int frame : {0, 9, 19, 20, 24, 27, 30, 40, 45, 52, 54, 55, 59, 60, 74, 75, 89})
        baselineFrames.emplace(frame, decodeFrame(baselineCase.output, frame));

    std::map<std::string, double> metrics;
    const Region centeredInside{105, 85, 100, 70};
    const Region outside{0, 170, 55, 55};
    const Image centered30 = decodeFrame(centeredCase.output, 30);
    metrics["centered_inside_difference"] =
        meanDifference(centered30, baselineFrames.at(30), centeredInside);
    metrics["centered_outside_difference"] =
        meanDifference(centered30, baselineFrames.at(30), outside);
    check(metrics["centered_inside_difference"] > 25.0,
          "centered/scaled V2が内側へ描画されていません");
    check(metrics["centered_outside_difference"] < 6.0, "centered/scaled V2がrect外へ漏れています");

    const Image opacity30 = decodeFrame(opacityCase.output, 30);
    metrics["opacity_blend_error"] =
        blendError(opacity30, baselineFrames.at(30), centered30, 0.5, centeredInside);
    metrics["opacity_bottom_difference"] =
        meanDifference(opacity30, baselineFrames.at(30), centeredInside);
    check(metrics["opacity_blend_error"] < 12.0 && metrics["opacity_bottom_difference"] > 10.0,
          "V2 opacity 50%がV1との実画素blendになっていません");

    const Image crop30 = decodeFrame(cropCase.output, 30);
    metrics["crop_outside_difference"] =
        meanDifference(crop30, baselineFrames.at(30), {0, 0, 55, kHeight});
    metrics["crop_inside_difference"] =
        meanDifference(crop30, baselineFrames.at(30), {100, 60, 120, 100});
    metrics["crop_outside_brightness"] = meanBrightness(crop30, {0, 0, 55, kHeight});
    check(metrics["crop_outside_difference"] < 6.0 && metrics["crop_inside_difference"] > 20.0 &&
              metrics["crop_outside_brightness"] > 25.0,
          "asymmetric crop外側がV1を露出していません");

    const Image rotated30 = decodeFrame(rotationCase.output, 30);
    const DifferenceBounds rotatedBounds = differenceBounds(rotated30, baselineFrames.at(30));
    metrics["rotation_bbox_min_x"] = rotatedBounds.minX;
    metrics["rotation_bbox_min_y"] = rotatedBounds.minY;
    metrics["rotation_bbox_max_x"] = rotatedBounds.maxX;
    metrics["rotation_bbox_max_y"] = rotatedBounds.maxY;
    metrics["rotation_center_difference"] =
        meanDifference(rotated30, baselineFrames.at(30), {195, 70, 55, 40});
    metrics["rotation_wing_difference"] =
        meanDifference(rotated30, baselineFrames.at(30), {181, 22, 16, 15});
    check(metrics["rotation_center_difference"] > 25.0 && metrics["rotation_wing_difference"] > 8.0,
          "position + rotationの中心または回転wingが実画素にありません");
    check(meanDifference(rotated30, baselineFrames.at(30), {0, 170, 80, 60}) < 6.0,
          "rotation rect外でV1が露出していません");

    const Image fadeStart = decodeFrame(fadeCase.output, 20);
    const Image fadeMiddle = decodeFrame(fadeCase.output, 40);
    const Image fadeEnd = decodeFrame(fadeCase.output, 59);
    metrics["fade_start_bottom_difference"] =
        meanDifference(fadeStart, baselineFrames.at(20), centeredInside);
    metrics["fade_middle_bottom_difference"] =
        meanDifference(fadeMiddle, baselineFrames.at(40), centeredInside);
    metrics["fade_end_bottom_difference"] =
        meanDifference(fadeEnd, baselineFrames.at(59), centeredInside);
    metrics["fade_start_brightness"] = meanBrightness(fadeStart, centeredInside);
    metrics["fade_end_brightness"] = meanBrightness(fadeEnd, centeredInside);
    check(metrics["fade_start_bottom_difference"] < 6.0 &&
              metrics["fade_end_bottom_difference"] < 6.0 &&
              metrics["fade_middle_bottom_difference"] > 15.0 &&
              metrics["fade_start_brightness"] > 25.0 && metrics["fade_end_brightness"] > 25.0,
          "fade端が黒ではなくV1を露出することを確認できません");

    const Region sourceProbe{120, 95, 55, 35};
    const Image sourceZeroFrame = decodeFrame(sourceZeroCase.output, 20);
    const Image sourceIn = decodeFrame(sourceInCase.output, 20);
    metrics["source_zero_red"] = meanChannel(sourceZeroFrame, sourceProbe, 0);
    metrics["source_zero_green"] = meanChannel(sourceZeroFrame, sourceProbe, 1);
    metrics["source_ten_red"] = meanChannel(sourceIn, sourceProbe, 0);
    metrics["source_ten_green"] = meanChannel(sourceIn, sourceProbe, 1);
    check(metrics["source_zero_red"] > metrics["source_zero_green"] + 40.0 &&
              metrics["source_ten_green"] > metrics["source_ten_red"] + 40.0,
          "非ゼロV2 source-inが実際のsource frame選択へ反映されていません");

    double blankMaxDifference = 0.0;
    for (const int frame : {0, 19, 60, 89}) {
        blankMaxDifference =
            std::max(blankMaxDifference, meanDifference(decodeFrame(blankCase.output, frame),
                                                        baselineFrames.at(frame)));
    }
    metrics["blank_max_difference"] = blankMaxDifference;
    check(blankMaxDifference < 6.0, "V2前後blankでtransitionまたはeffectが漏れています");

    double twoGapMaxDifference = 0.0;
    for (const int frame : {0, 9, 30, 54, 75, 89}) {
        twoGapMaxDifference =
            std::max(twoGapMaxDifference,
                     meanDifference(decodeFrame(twoCase.output, frame), baselineFrames.at(frame)));
    }
    const Image twoFirst = decodeFrame(twoCase.output, 20);
    const Image twoSecond = decodeFrame(twoCase.output, 60);
    metrics["two_clip_gap_max_difference"] = twoGapMaxDifference;
    metrics["two_clip_first_left_difference"] =
        meanDifference(twoFirst, baselineFrames.at(20), {45, 50, 70, 50});
    metrics["two_clip_first_right_difference"] =
        meanDifference(twoFirst, baselineFrames.at(20), {210, 145, 60, 40});
    metrics["two_clip_second_left_difference"] =
        meanDifference(twoSecond, baselineFrames.at(60), {45, 50, 70, 50});
    metrics["two_clip_second_right_difference"] =
        meanDifference(twoSecond, baselineFrames.at(60), {210, 145, 60, 40});
    check(twoGapMaxDifference < 6.0 && metrics["two_clip_first_left_difference"] > 20.0 &&
              metrics["two_clip_first_right_difference"] < 6.0 &&
              metrics["two_clip_second_left_difference"] < 6.0 &&
              metrics["two_clip_second_right_difference"] > 20.0,
          "2本の非重複V2 clip間でtransition/effectが漏れています");

    DomainEvidence domains;
    const std::vector<int> activeFrames{27, 40, 45, 52};
    for (const int frame : activeFrames) {
        const Image bottom = baselineFrames.at(frame);
        const Image full = decodeFrame(centeredCase.output, frame);
        domains.absoluteTransitionError += blendError(
            decodeFrame(absoluteTransitionCase.output, frame), bottom, full, 0.8, centeredInside);
        domains.localTransitionError += blendError(decodeFrame(localTransitionCase.output, frame),
                                                   bottom, full, 0.8, centeredInside);
    }
    domains.transitionDomain = domains.absoluteTransitionError + 12.0 < domains.localTransitionError
                                   ? "timeline_absolute"
                                   : "unresolved";
    check(domains.transitionDomain == "timeline_absolute",
          "transition in/outのtimeline-absolute authorityを実画素で分離できません");

    const std::vector<int> fadeFrames{20, 24, 27, 40, 52, 55, 59};
    for (const int frame : fadeFrames) {
        const int local = frame - kTimelineStart;
        const double expectedOpacity =
            0.8 * mvm::core::clipFadeFactor(local, kOverlayDuration, 8, 8);
        const Image bottom = baselineFrames.at(frame);
        const Image full = decodeFrame(centeredCase.output, frame);
        domains.localKeyError += blendError(decodeFrame(absoluteTransitionCase.output, frame),
                                            bottom, full, expectedOpacity, centeredInside);
        domains.absoluteKeyError += blendError(decodeFrame(absoluteKeysCase.output, frame), bottom,
                                               full, expectedOpacity, centeredInside);
    }
    domains.keyframeDomain =
        domains.localKeyError + 12.0 < domains.absoluteKeyError ? "transition_local" : "unresolved";
    check(domains.keyframeDomain == "transition_local",
          "animation keyframeのtransition-local authorityを実画素で分離できません");

    check(cases.size() >= 10, "必須実画素caseが10件未満です");
    writeJson(jsonPath, v1, v2, cases, domains, metrics);
    mvm_mlt_runtime_shutdown();

    if (failures) {
        std::fprintf(stderr, "M7b-P0 MLT overlay primitive: FAIL (%d件)\n", failures);
        return 1;
    }
    std::puts("M7b-P0 MLT overlay primitive: PASS");
    return 0;
}
