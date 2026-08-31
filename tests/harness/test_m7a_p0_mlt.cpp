// M7a-P0: 製品exportを変更せず、MLT crop/affineの実画素意味論を固定する。

#include "core/clip_fade.h"
#include "media/mlt/mvm_mlt_effects_p0.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kDuration = 60;
constexpr int kSourceIn = 10;

struct FrameMetrics {
    int frame = -1;
    int foregroundPixels = 0;
    int minX = kWidth;
    int minY = kHeight;
    int maxX = -1;
    int maxY = -1;
    double meanRgb = 0.0;
    int redMarkerPixels = 0;
    int greenMarkerPixels = 0;
    int blueMarkerPixels = 0;
    int magentaMarkerPixels = 0;
    long long magentaXSum = 0;
    long long magentaYSum = 0;
};

struct CaseResult {
    std::string name;
    std::string output;
    bool affineFirst = false;
    int cropLeft = 0;
    int cropTop = 0;
    int cropRight = 0;
    int cropBottom = 0;
    double rectX = 0.0;
    double rectY = 0.0;
    double rectWidth = kWidth;
    double rectHeight = kHeight;
    double rotation = 0.0;
    bool bAlpha = false;
    bool cropOnParent = false;
    std::vector<MvmM7aP0OpacityKeyframe> keys;
    std::vector<FrameMetrics> samples;
    MvmM7aP0RenderResult render{};
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

bool generateFixture(const std::filesystem::path& ffmpeg, const std::filesystem::path& output) {
    const wchar_t* filter = L"drawbox=x=0:y=0:w=iw:h=20:color=red:t=fill,"
                            L"drawbox=x=0:y=ih-20:w=iw:h=20:color=blue:t=fill,"
                            L"drawbox=x=0:y=20:w=20:h=ih-40:color=green:t=fill,"
                            L"drawbox=x=iw-20:y=20:w=20:h=ih-40:color=yellow:t=fill,"
                            L"drawbox=x=150:y=110:w=20:h=20:color=magenta:t=fill,"
                            L"drawbox=x=45:y=35:w=14:h=14:color=cyan:t=fill";
    const intptr_t exitCode =
        _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                 L"lavfi", L"-i", L"color=c=gray:s=320x240:r=60:d=2", L"-vf", filter, L"-c:v",
                 L"libx264", L"-preset", L"ultrafast", L"-crf", L"8", L"-pix_fmt", L"yuv420p",
                 output.c_str(), static_cast<wchar_t*>(nullptr));
    return exitCode == 0 && std::filesystem::is_regular_file(output);
}

FrameMetrics measureFrame(const std::filesystem::path& path, int frame) {
    FrameMetrics metrics;
    metrics.frame = frame;
    MvmMltImage image{};
    char error[512] = {};
    if (mvm_mlt_decode_frame(toUtf8(path).c_str(), frame, &image, error, sizeof(error)) != 0 ||
        !image.rgba || image.width != kWidth || image.height != kHeight) {
        std::fprintf(stderr, "NG: %s frame %dをdecodeできません: %s\n", toUtf8(path).c_str(), frame,
                     error);
        ++failures;
        return metrics;
    }
    double sum = 0.0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                 static_cast<std::size_t>(x)) *
                4;
            const int red = image.rgba[offset];
            const int green = image.rgba[offset + 1];
            const int blue = image.rgba[offset + 2];
            sum += red + green + blue;
            if (red + green + blue > 45) {
                ++metrics.foregroundPixels;
                metrics.minX = std::min(metrics.minX, x);
                metrics.minY = std::min(metrics.minY, y);
                metrics.maxX = std::max(metrics.maxX, x);
                metrics.maxY = std::max(metrics.maxY, y);
            }
            if (red > 140 && red > green * 3 / 2 && red > blue * 3 / 2)
                ++metrics.redMarkerPixels;
            if (green > 90 && green > red * 3 / 2 && green > blue * 3 / 2)
                ++metrics.greenMarkerPixels;
            if (blue > 120 && blue > red * 3 / 2 && blue > green * 3 / 2)
                ++metrics.blueMarkerPixels;
            if (red > 110 && blue > 110 && green < 100) {
                ++metrics.magentaMarkerPixels;
                metrics.magentaXSum += x;
                metrics.magentaYSum += y;
            }
        }
    }
    metrics.meanRgb = sum / static_cast<double>(image.width * image.height * 3);
    mvm_mlt_image_free(&image);
    return metrics;
}

bool samplesPresent(const std::vector<FrameMetrics>& samples) {
    return !samples.empty() && std::all_of(samples.begin(), samples.end(), [](const auto& sample) {
        return sample.frame >= 0 && sample.foregroundPixels >= 0;
    });
}

std::vector<MvmM7aP0OpacityKeyframe> constantKeys(double opacity) {
    return {{0, opacity}, {kDuration - 1, opacity}};
}

std::vector<MvmM7aP0OpacityKeyframe> fadeKeys(double baseOpacity, int fadeIn, int fadeOut) {
    const std::vector<int> positions = {0, fadeIn - 1, kDuration - fadeOut, kDuration - 1};
    std::vector<MvmM7aP0OpacityKeyframe> keys;
    for (const int position : positions) {
        if (position < 0 || position >= kDuration ||
            (!keys.empty() && keys.back().local_frame == position))
            continue;
        keys.push_back({position, baseOpacity * mvm::core::clipFadeFactor(position, kDuration,
                                                                          fadeIn, fadeOut)});
    }
    return keys;
}

CaseResult makeCase(std::string name) {
    CaseResult result;
    result.name = std::move(name);
    result.keys = constantKeys(1.0);
    return result;
}

bool renderCase(const std::filesystem::path& source, const std::filesystem::path& directory,
                CaseResult& item, const std::vector<int>& sampleFrames) {
    const auto output = directory / fromUtf8((item.name + ".mp4").c_str());
    item.output = toUtf8(output);
    MvmM7aP0RenderRequest request{};
    const std::string sourceText = toUtf8(source);
    const std::string outputText = toUtf8(output);
    request.source_path = sourceText.c_str();
    request.output_path = outputText.c_str();
    request.width = kWidth;
    request.height = kHeight;
    request.fps_num = 60;
    request.fps_den = 1;
    request.source_in = kSourceIn;
    request.duration = kDuration;
    request.crop_left = item.cropLeft;
    request.crop_top = item.cropTop;
    request.crop_right = item.cropRight;
    request.crop_bottom = item.cropBottom;
    request.rect_x = item.rectX;
    request.rect_y = item.rectY;
    request.rect_width = item.rectWidth;
    request.rect_height = item.rectHeight;
    request.rotation_degrees = item.rotation;
    request.b_alpha = item.bAlpha ? 1 : 0;
    request.crop_on_parent = item.cropOnParent ? 1 : 0;
    request.attach_affine_first = item.affineFirst ? 1 : 0;
    request.opacity_keyframes = item.keys.data();
    request.opacity_keyframe_count = static_cast<int>(item.keys.size());
    request.timeout_ms = 120000;
    char error[1024] = {};
    if (mvm_m7a_p0_render(&request, &item.render, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "NG: case %sをrenderできません: %s\n", item.name.c_str(), error);
        ++failures;
        return false;
    }
    for (const int frame : sampleFrames)
        item.samples.push_back(measureFrame(output, frame));
    check(samplesPresent(item.samples), item.name + "の対象frameが0件です");
    return true;
}

void writeJson(const std::filesystem::path& path, const std::filesystem::path& fixture,
               const std::vector<CaseResult>& cases) {
    std::ofstream json(path, std::ios::binary | std::ios::trunc);
    json << "{\n  \"schema\": \"m7a-p0-v1\",\n"
         << "  \"fade_authority\": \"clip_local_frame\",\n"
         << "  \"fixture\": \"" << toUtf8(fixture) << "\",\n"
         << "  \"source_in_frame\": " << kSourceIn << ",\n"
         << "  \"duration_frames\": " << kDuration << ",\n"
         << "  \"cases\": [\n";
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        json << "    {\"name\":\"" << item.name << "\",\"attach_order\":\""
             << (item.affineFirst ? "affine_then_crop" : "crop_then_affine") << "\",\"crop\":["
             << item.cropLeft << ',' << item.cropTop << ',' << item.cropRight << ','
             << item.cropBottom << "],\"rect\":[" << item.rectX << ',' << item.rectY << ','
             << item.rectWidth << ',' << item.rectHeight
             << "],\"rotation_degrees\":" << item.rotation
             << ",\"b_alpha\":" << (item.bAlpha ? "true" : "false") << ",\"crop_target\":\""
             << (item.cropOnParent ? "parent" : "cut")
             << "\",\"keyframes_verified\":" << item.render.keyframes_verified
             << ",\"output_frames\":" << item.render.frame_count << ",\"samples\":[";
        for (std::size_t sampleIndex = 0; sampleIndex < item.samples.size(); ++sampleIndex) {
            const auto& sample = item.samples[sampleIndex];
            if (sampleIndex)
                json << ',';
            json << "{\"local_frame\":" << sample.frame << ",\"expected_opacity\":";
            double expectedOpacity = 0.0;
            if (item.name == "fade")
                expectedOpacity = 0.8 * mvm::core::clipFadeFactor(sample.frame, kDuration, 12, 12);
            else
                expectedOpacity = item.keys.front().opacity;
            json << expectedOpacity << ",\"foreground_pixels\":" << sample.foregroundPixels
                 << ",\"bbox\":[" << sample.minX << ',' << sample.minY << ',' << sample.maxX << ','
                 << sample.maxY << "],\"mean_rgb\":" << sample.meanRgb
                 << ",\"marker_pixels\":{\"red\":" << sample.redMarkerPixels
                 << ",\"green\":" << sample.greenMarkerPixels
                 << ",\"blue\":" << sample.blueMarkerPixels
                 << ",\"magenta\":" << sample.magentaMarkerPixels << "},\"magenta_centroid\":[";
            if (sample.magentaMarkerPixels > 0) {
                json << static_cast<double>(sample.magentaXSum) / sample.magentaMarkerPixels << ','
                     << static_cast<double>(sample.magentaYSum) / sample.magentaMarkerPixels;
            } else {
                json << "null,null";
            }
            json << "]}";
        }
        json << "]}" << (index + 1 == cases.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    check(json.good(), "M7a-P0 MLT生JSONを書き込めません");
}

void testFadeHelper() {
    using mvm::core::clipFadeFactor;
    check(clipFadeFactor(0, 10, 0, 0) == 1.0, "fadeなしの先頭が1ではありません");
    check(clipFadeFactor(0, 10, 1, 0) == 0.0, "1-frame fade-inの先頭が0ではありません");
    check(clipFadeFactor(1, 10, 1, 0) == 1.0, "1-frame fade-in後が1ではありません");
    check(std::abs(clipFadeFactor(2, 10, 4, 0) - 2.0 / 3.0) < 1e-12,
          "複数frame fade-inの線形補間が違います");
    check(clipFadeFactor(9, 10, 0, 4) == 0.0, "fade-out末尾が0ではありません");
    check(clipFadeFactor(-1, 10, 0, 0) == 0.0, "clip外の負frameを拒否しません");
    check(clipFadeFactor(10, 10, 0, 0) == 0.0, "clip終端外を拒否しません");
    check(clipFadeFactor(0, 10, 6, 5) == 0.0, "重複fadeを拒否しません");

    const long long previewOutputOrdinal = 2;
    const long long decodedSourceFrame = 110;
    const long long sourceInFrame = 100;
    const double explicitLocal = clipFadeFactor(decodedSourceFrame - sourceInFrame, 20, 12, 0);
    const double wrongOrdinal = clipFadeFactor(previewOutputOrdinal, 20, 12, 0);
    check(explicitLocal != wrongOrdinal,
          "source frameとoutput ordinalを区別できないfade authority testです");
}

} // namespace

int main(int argc, char** argv) {
    mvm_enable_utf8_console();
    if (argc == 2 && std::string(argv[1]) == "--negative-empty") {
        return samplesPresent({}) ? 1 : 7;
    }
    if (argc != 4) {
        std::fprintf(stderr, "使い方: mvm_test_m7a_p0_mlt <出力dir> <ffmpeg.exe> <raw.json>\n");
        return 2;
    }
    const auto outputDirectory = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto ffmpeg = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto jsonPath = std::filesystem::absolute(fromUtf8(argv[3]));
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(ffmpeg)) {
        std::fprintf(stderr,
                     "fixtureを生成できません。UCRT64 ffmpegと出力directoryを確認してください\n");
        return 2;
    }

    testFadeHelper();
    const auto fixture = outputDirectory / L"m7a-p0-fixture.mp4";
    check(generateFixture(ffmpeg, fixture), "色付きM7a-P0 fixtureを生成できません");
    if (failures)
        return 1;
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません\n");
        return 1;
    }

    std::vector<CaseResult> cases;
    cases.push_back(makeCase("baseline"));

    auto crop = makeCase("crop");
    crop.cropLeft = 32;
    crop.cropTop = 24;
    crop.cropRight = 64;
    crop.cropBottom = 12;
    cases.push_back(crop);

    auto cropParent = crop;
    cropParent.name = "crop_on_parent";
    cropParent.cropOnParent = true;
    cases.push_back(cropParent);

    auto transform = makeCase("transform");
    transform.rectX = 80;
    transform.rectY = 36;
    transform.rectWidth = 192;
    transform.rectHeight = 144;
    transform.rotation = 30;
    cases.push_back(transform);

    auto positionScale = transform;
    positionScale.name = "position_scale";
    positionScale.rotation = 0;
    cases.push_back(positionScale);

    auto opacity = transform;
    opacity.name = "opacity";
    opacity.rotation = 0;
    opacity.keys = constantKeys(0.5);
    cases.push_back(opacity);

    auto opacityBAlpha = opacity;
    opacityBAlpha.name = "opacity_b_alpha_1";
    opacityBAlpha.bAlpha = true;
    cases.push_back(opacityBAlpha);

    auto fade = makeCase("fade");
    fade.keys = fadeKeys(0.8, 12, 12);
    cases.push_back(fade);

    auto combinedCropFirst = transform;
    combinedCropFirst.name = "combined_crop_then_affine";
    combinedCropFirst.cropLeft = 32;
    combinedCropFirst.cropTop = 24;
    combinedCropFirst.cropRight = 64;
    combinedCropFirst.cropBottom = 12;
    combinedCropFirst.cropOnParent = true;
    combinedCropFirst.keys = constantKeys(0.5);
    cases.push_back(combinedCropFirst);

    auto combinedAffineFirst = combinedCropFirst;
    combinedAffineFirst.name = "combined_affine_then_crop";
    combinedAffineFirst.affineFirst = true;
    cases.push_back(combinedAffineFirst);

    for (auto& item : cases) {
        const std::vector<int> samples =
            item.name == "fade" ? std::vector<int>{0, 1, 11, 12, 48, 58, 59} : std::vector<int>{20};
        renderCase(fixture, outputDirectory, item, samples);
    }

    const auto fadeIterator = std::find_if(cases.begin(), cases.end(),
                                           [](const auto& item) { return item.name == "fade"; });
    const auto& fadeSamples = fadeIterator->samples;
    if (fadeSamples.size() == 7) {
        check(fadeSamples.front().meanRgb < fadeSamples[3].meanRgb * 0.25,
              "fade-in先頭が中央より十分暗くありません");
        check(fadeSamples.back().meanRgb < fadeSamples[3].meanRgb * 0.25,
              "fade-out末尾が中央より十分暗くありません");
    }
    const auto transformIterator = std::find_if(
        cases.begin(), cases.end(), [](const auto& item) { return item.name == "position_scale"; });
    const auto opacityIterator = std::find_if(
        cases.begin(), cases.end(), [](const auto& item) { return item.name == "opacity"; });
    check(opacityIterator->samples.empty() || transformIterator->samples.empty() ||
              opacityIterator->samples[0].meanRgb < transformIterator->samples[0].meanRgb * 0.75,
          "opacity 50%が実画素へ反映されていません");
    const auto baselineIterator = std::find_if(
        cases.begin(), cases.end(), [](const auto& item) { return item.name == "baseline"; });
    const auto cropIterator = std::find_if(cases.begin(), cases.end(),
                                           [](const auto& item) { return item.name == "crop"; });
    if (!baselineIterator->samples.empty() && !cropIterator->samples.empty()) {
        const auto& baselineSample = baselineIterator->samples[0];
        const auto& cropSample = cropIterator->samples[0];
        check(baselineSample.redMarkerPixels > 0 && baselineSample.greenMarkerPixels > 0 &&
                  baselineSample.blueMarkerPixels > 0,
              "対照fixtureの色付き辺を検出できません");
        check(cropSample.redMarkerPixels == 0 && cropSample.greenMarkerPixels == 0 &&
                  cropSample.blueMarkerPixels == 0 &&
                  cropSample.magentaMarkerPixels > baselineSample.magentaMarkerPixels * 2,
              "非対称cropの実画素またはcrop後fill semanticsを確認できません");
    }
    for (const auto& sample : fadeSamples) {
        const double expectedOpacity =
            0.8 * mvm::core::clipFadeFactor(sample.frame, kDuration, 12, 12);
        const double expectedMean = baselineIterator->samples[0].meanRgb * expectedOpacity;
        check(std::abs(sample.meanRgb - expectedMean) < 5.0,
              "clip-local fadeの実画素が共通helper期待値と一致しません");
    }
    if (!transformIterator->samples.empty()) {
        const auto rotationIterator = std::find_if(
            cases.begin(), cases.end(), [](const auto& item) { return item.name == "transform"; });
        const auto& unrotated = transformIterator->samples[0];
        const auto& rotated = rotationIterator->samples[0];
        const double unrotatedX =
            static_cast<double>(unrotated.magentaXSum) / std::max(1, unrotated.magentaMarkerPixels);
        const double unrotatedY =
            static_cast<double>(unrotated.magentaYSum) / std::max(1, unrotated.magentaMarkerPixels);
        const double rotatedX =
            static_cast<double>(rotated.magentaXSum) / std::max(1, rotated.magentaMarkerPixels);
        const double rotatedY =
            static_cast<double>(rotated.magentaYSum) / std::max(1, rotated.magentaMarkerPixels);
        check(std::abs(unrotatedX - rotatedX) < 2.0 && std::abs(unrotatedY - rotatedY) < 2.0,
              "affine rotationの中心pivotをmarkerで確認できません");
    }

    writeJson(jsonPath, fixture, cases);
    mvm_mlt_runtime_shutdown();

    if (failures) {
        std::fprintf(stderr, "M7a-P0 MLT primitive: FAIL (%d件)\n", failures);
        return 1;
    }
    std::puts("M7a-P0 MLT primitive: PASS");
    return 0;
}
