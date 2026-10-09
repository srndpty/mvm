#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_rgba_diagnostic.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mlt_rgba_oracle.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>

#include <QGuiApplication>

using namespace mvm;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "失敗: " << message << '\n';
    }
}

graph::Raster raster(test::Pixel pixel) {
    graph::Raster result{64, 36, std::vector<std::uint8_t>(64 * 36 * 4)};
    for (std::size_t i = 0; i < result.rgba.size(); i += 4)
        std::copy(pixel.begin(), pixel.end(), result.rgba.begin() + static_cast<std::ptrdiff_t>(i));
    return result;
}

std::string hash(const std::vector<std::uint8_t>& bytes) {
    return graph::digest(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string fileHash(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return graph::digest(std::string(std::istreambuf_iterator<char>(file), {}));
}

struct Evidence {
    std::filesystem::path root;
    std::ofstream rows;
    std::string name;
    std::vector<std::vector<std::uint8_t>> stages;
    std::size_t width = 64;

    void record(const std::string& stage, int layer, const std::vector<std::uint8_t>& bytes,
                const std::vector<std::uint8_t>& expected) {
        std::size_t first = bytes.size(), mismatches = 0;
        if (bytes.size() != expected.size())
            ++mismatches;
        else
            for (std::size_t i = 0; i < bytes.size(); ++i)
                if (bytes[i] != expected[i]) {
                    first = std::min(first, i);
                    ++mismatches;
                }
        std::ofstream raw(root / (name + "-" + stage + "-" + std::to_string(layer) + ".rgba"),
                          std::ios::binary);
        raw.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        rows << name << '\t' << stage << '\t' << layer << '\t' << hash(bytes) << '\t'
             << hash(expected) << '\t';
        for (int i = 0; i < 4; ++i)
            rows << int(bytes.at(static_cast<std::size_t>(i))) << (i == 3 ? '\t' : ',');
        if (first == bytes.size())
            rows << "-1\t-1\t-1";
        else
            rows << (first / 4) % width << '\t' << (first / 4) / width << '\t' << first % 4;
        rows << '\t' << mismatches << '\n';
        check(mismatches == 0, "各境界の全画素が独立期待値に完全一致する");
    }
};

struct Probe {
    Evidence* evidence;
    std::vector<test::Pixel> sources;
    test::Pixel background;
    double opacity;
    std::vector<std::uint8_t> output;
};

void probeCallback(int stage, int layer, const unsigned char* bytes, int w, int h, void* opaque) {
    auto& p = *static_cast<Probe*>(opaque);
    auto expected = p.background;
    if (stage == 1 || stage == 3)
        expected = p.sources.at(static_cast<std::size_t>(layer));
    else
        for (int i = 0; i < layer + (stage == 4 ? 1 : 0); ++i)
            expected =
                test::mltSourceOver(expected, p.sources.at(static_cast<std::size_t>(i)), p.opacity);
    const std::vector<std::uint8_t> actual(bytes, bytes + w * h * 4);
    const char* names[] = {"background", "producer", "pre-destination", "pre-source", "post"};
    p.evidence->record(names[stage], layer, actual, raster(expected).rgba);
    if (stage == 4)
        p.output = actual;
}

project::Project projectFor(const std::vector<test::Pixel>& sources, bool draw, double opacity) {
    auto p = project::createDefaultProject();
    p.outputWidth = 64;
    p.outputHeight = 36;
    p.timelineFpsNum = 30;
    p.timelineFpsDen = 1;
    // 出力 track の初期構成に依存せず、必要な最上位まで明示する。
    for (std::size_t i = p.videoTracks.size(); i <= sources.size(); ++i) {
        auto track = p.videoTracks.front();
        track.name = "合成 track " + std::to_string(i);
        p.videoTracks.push_back(track);
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto id = "graph-" + std::to_string(i);
        check(project::addGraph(p, id, {"function-" + std::to_string(i)}, "合成試験",
                                {project::TrackKind::Video, static_cast<int>(i + 1)}, 0)
                  .success,
              "Graph 試験を作成する");
        auto& c = p.timelineClips.back();
        c.sourceFrameCount = c.sourceOutFrame = 3;
        c.graph.functions[0].expression = std::to_string(i);
        c.graph.functions[0].color = i == 0 ? "#FF000000" : i == 1 ? "#FF000001" : "#FF000002";
        c.graph.axes = {false, false, "", ""};
        if (draw)
            c.graph.intro = {project::GraphIntroKind::Draw, 2};
        else
            c.graph.intro = {project::GraphIntroKind::None, 0};
        c.effects.opacityPercent = opacity * 100;
    }
    return p;
}

void matrix(Evidence& e, const std::string& name, std::vector<test::Pixel> sources, test::Pixel bg,
            const char* bgColor, bool draw = false, double opacity = 1, bool product = true) {
    e.name = name;
    std::vector<std::string> pathStrings;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        auto path = e.root / (name + "-source-" + std::to_string(i) + ".png");
        check(graph::writeRgba(path, raster(sources[i])), "独立 straight PNG を保存する");
        auto decoded = graph::readRgba(path, 64, 36);
        check(std::holds_alternative<graph::Raster>(decoded), "PNG を decode する");
        if (std::holds_alternative<graph::Raster>(decoded))
            e.record("decoded", static_cast<int>(i), std::get<graph::Raster>(decoded).rgba,
                     raster(sources[i]).rgba);
        e.rows << name << "\tpng-sha\t" << i << '\t' << fileHash(path) << '\n';
        pathStrings.push_back(path.string());
    }
    std::vector<const char*> paths;
    for (auto& path : pathStrings)
        paths.push_back(path.c_str());
    Probe probe{&e, sources, bg, opacity, {}};
    check(mvm_mlt_rgba_diagnostic(paths.data(), static_cast<int>(paths.size()), bgColor, 64, 36,
                                  opacity, probeCallback, &probe) == 0,
          "実 DLL の qimage/color/affine を段階観測する");
    // 製品は黒の不透明背景を使う。その他は MLT service の controlled 診断に限定する。
    if (!product || bg[3] != 255)
        return;
    auto p = projectFor(sources, draw, opacity);
    if (bg != test::Pixel{0, 0, 0, 255}) {
        auto base = p.timelineClips.front();
        base.id = "background";
        base.kind = project::TimelineClipKind::Image;
        base.graph = {};
        base.effects = {};
        base.track.index = 0;
        base.mediaPath = e.root / (name + "-background.png");
        check(graph::writeRgba(base.mediaPath, raster(bg)), "色付きの共通背景を保存する");
        p.timelineClips.push_back(base);
    }
    app::TimelineExportRequest request;
    request.width = 64;
    request.height = 36;
    request.fpsNum = 30;
    request.fpsDen = 1;
    request.renderThreads = 1;
    request.graphEnvironment.cache = e.root / (name + "-cache");
    request.graphEnvironment.toolchain = "P4-5.1 固定試験 backend";
    request.graphEnvironment.preflight =
        [sources, draw](const auto&,
                        const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{
            "P4-5.1 固定試験 backend",
            [sources, draw](const graph::RenderRequest& r,
                            const std::atomic<bool>*) -> graph::RenderResult {
                const auto index = static_cast<std::size_t>(r.spec.curves.front().argb & 255u);
                for (std::int64_t i = -1; i < r.spec.drawFrames; ++i) {
                    auto pixel = sources.at(index);
                    if (draw && i == 0)
                        pixel = {};
                    else if (draw && i == 1)
                        pixel[1] = static_cast<std::uint8_t>(pixel[1] + 2);
                    if (!graph::writeRgba(
                            r.job / (i < 0 ? "static.png" : "frame-" + std::to_string(i) + ".png"),
                            raster(pixel)))
                        return graph::Error{graph::Failure::RendererFailure, 0,
                                            "試験 PNG の保存失敗"};
                }
                return std::monostate{};
            }};
    };
    std::mutex mutex;
    std::map<std::int64_t, std::vector<std::uint8_t>> graphFrames;
    request.graphFrameObserver = [&](const std::string& id, auto frame, const auto& actual) {
        auto i = std::stoul(id.substr(6));
        auto pixel = sources.at(i);
        if (draw && frame == 0)
            pixel = {};
        else if (draw && frame == 1)
            pixel[1] = static_cast<std::uint8_t>(pixel[1] + 2);
        e.record("artifact-" + std::to_string(frame), static_cast<int>(i), actual.rgba,
                 raster(pixel).rgba);
    };
    request.graphStagingObserver = [&](const std::string& id, auto frame, const auto& path) {
        auto i = std::stoul(id.substr(6));
        auto pixel = sources.at(i);
        if (draw && frame == 0)
            pixel = {};
        else if (draw && frame == 1)
            pixel[1] = static_cast<std::uint8_t>(pixel[1] + 2);
        const auto decoded = graph::readRgba(path, 64, 36);
        check(std::holds_alternative<graph::Raster>(decoded), "実 staging PNG を decode する");
        e.rows << name << "\tstaging-png-sha\t" << frame << '\t' << fileHash(path) << '\n';
        if (std::holds_alternative<graph::Raster>(decoded))
            e.record("staging-" + std::to_string(frame), static_cast<int>(i),
                     std::get<graph::Raster>(decoded).rgba, raster(pixel).rgba);
        std::filesystem::copy_file(path, e.root / (name + "-staged-" + std::to_string(i) + "-" +
                                                   std::to_string(frame) + ".png"));
    };
    request.encoderFrameValidator = [&](auto frame, const auto* bytes, int w, int h) {
        std::lock_guard lock(mutex);
        auto pixel = bg;
        for (auto source : sources) {
            if (draw && frame == 0)
                source = {};
            else if (draw && frame == 1)
                source[1] = static_cast<std::uint8_t>(source[1] + 2);
            pixel = test::mltSourceOver(pixel, source, opacity);
        }
        const std::vector<std::uint8_t> actual(bytes, bytes + w * h * 4);
        graphFrames[frame] = actual;
        e.record("pre-yuv-graph", static_cast<int>(frame), actual, raster(pixel).rgba);
        return actual == raster(pixel).rgba;
    };
    request.outputPath = e.root / (name + "-graph.mp4");
    const auto exported = app::exportTimeline(p, request);
    check(exported.success && graphFrames.size() == 3, "Graph の全 frame exact MLT oracle");
    if (!exported.success)
        std::cerr << exported.error << '\n';
    for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::string> stagedPaths;
        auto frameSources = sources;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            stagedPaths.push_back((e.root / (name + "-staged-" + std::to_string(i) + "-" +
                                             std::to_string(frame) + ".png"))
                                      .string());
            if (draw && frame == 0)
                frameSources[i] = {};
            else if (draw && frame == 1)
                frameSources[i][1] = static_cast<std::uint8_t>(frameSources[i][1] + 2);
        }
        std::vector<const char*> stagedPointers;
        for (auto& path : stagedPaths)
            stagedPointers.push_back(path.c_str());
        e.name = name + "-actual-staging-" + std::to_string(frame);
        Probe actualProbe{&e, frameSources, bg, opacity, {}};
        check(mvm_mlt_rgba_diagnostic(stagedPointers.data(),
                                      static_cast<int>(stagedPointers.size()), bgColor, 64, 36,
                                      opacity, probeCallback, &actualProbe) == 0 &&
                  actualProbe.output == graphFrames[frame],
              "実 staging の producer／合成直後と製品 pre-YUV は全画素一致する");
    }
    e.name = name;
    // Graph が実際に stage した endpoint PNG の同じ bytes を通常 Image へ渡す。
    for (std::size_t i = 0; i < p.timelineClips.size(); ++i) {
        auto& c = p.timelineClips[i];
        if (c.kind != project::TimelineClipKind::Graph)
            continue;
        c.kind = project::TimelineClipKind::Image;
        c.graph = {};
        c.mediaPath = e.root / (name + "-staged-" + std::to_string(i) + "-2.png");
        check(std::filesystem::exists(c.mediaPath), "Graph と Image は同じ PNG bytes を入力する");
    }
    request.graphFrameObserver = {};
    request.graphStagingObserver = {};
    std::set<std::int64_t> imageFrames;
    auto endpoint = bg;
    for (auto source : sources)
        endpoint = test::mltSourceOver(endpoint, source, opacity);
    request.encoderFrameValidator = [&](auto frame, const auto* bytes, int w, int h) {
        std::lock_guard lock(mutex);
        imageFrames.insert(frame);
        const std::vector<std::uint8_t> actual(bytes, bytes + w * h * 4);
        e.record("pre-yuv-image", static_cast<int>(frame), actual, raster(endpoint).rgba);
        return actual == raster(endpoint).rgba && graphFrames.at(2) == actual;
    };
    request.outputPath = e.root / (name + "-image.mp4");
    const auto image = app::exportTimeline(p, request);
    check(image.success && imageFrames.size() == 3,
          "通常 Image と Graph endpoint は全画素一致する");
}

// 空間 filtering の oracle を一般化せず、同じ実 PNG と同じ通常効果の経路を比較する。
void spatialEffects(Evidence& e) {
    auto input = raster({200, 99, 31, 128});
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < 64; ++x) {
            const auto offset = static_cast<std::size_t>((y * 64 + x) * 4);
            input.rgba[offset] = static_cast<std::uint8_t>(x * 3);
            input.rgba[offset + 1] = static_cast<std::uint8_t>(y * 7);
            input.rgba[offset + 3] = static_cast<std::uint8_t>(32 + (x + y) % 200);
        }
    std::vector<project::ClipEffects> effects(6);
    effects[0].positionXPercent = 13;
    effects[0].positionYPercent = -7;
    effects[1].scaleXPercent = 73;
    effects[1].scaleYPercent = 61;
    effects[2].rotationDegrees = 17;
    effects[3].cropLeftPercent = 11;
    effects[3].cropTopPercent = 9;
    effects[3].cropRightPercent = 13;
    effects[3].cropBottomPercent = 7;
    effects[4] = effects[3];
    effects[4].rotationDegrees = 12;
    effects[4].scaleXPercent = 79;
    effects[4].positionXPercent = 9;
    effects[4].opacityPercent = 50;
    effects[5].positionXKeys = {{0, -13}, {2, 17}};
    effects[5].scaleXKeys = {{0, 61}, {2, 83}};
    effects[5].rotationKeys = {{0, -9}, {2, 21}};
    for (std::size_t i = 0; i < effects.size(); ++i) {
        const auto name = "spatial-" + std::to_string(i);
        auto p = projectFor({{200, 99, 31, 128}}, false, 1);
        p.timelineClips.front().effects = effects[i];
        app::TimelineExportRequest request;
        request.width = 64;
        request.height = 36;
        request.fpsNum = 30;
        request.fpsDen = 1;
        request.renderThreads = 1;
        request.graphEnvironment.cache = e.root / (name + "-cache");
        request.graphEnvironment.toolchain = "空間効果の固定入力";
        request.graphEnvironment.preflight =
            [input](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
            return manim::GraphBackend{"空間効果の固定入力",
                                       [input](const graph::RenderRequest& r,
                                               const std::atomic<bool>*) -> graph::RenderResult {
                                           if (!graph::writeRgba(r.job / "static.png", input))
                                               return graph::Error{graph::Failure::RendererFailure,
                                                                   0, "試験 PNG の保存失敗"};
                                           return std::monostate{};
                                       }};
        };
        const auto samePng = e.root / (name + "-actual.png");
        request.graphStagingObserver = [&](const auto&, auto frame, const auto& path) {
            if (frame == 0)
                std::filesystem::copy_file(path, samePng);
        };
        std::mutex mutex;
        std::map<std::int64_t, std::vector<std::uint8_t>> actualGraph;
        request.encoderFrameValidator = [&](auto frame, const auto* bytes, int w, int h) {
            std::lock_guard lock(mutex);
            actualGraph[frame] = {bytes, bytes + w * h * 4};
            return true;
        };
        request.outputPath = e.root / (name + "-graph.mp4");
        check(app::exportTimeline(p, request).success && actualGraph.size() == 3,
              "空間 ClipEffects の Graph 全 frame を実 encoder 境界で観測する");
        if (actualGraph.size() != 3 || !std::filesystem::exists(samePng))
            continue;
        p.timelineClips.front().kind = project::TimelineClipKind::Image;
        p.timelineClips.front().graph = {};
        p.timelineClips.front().mediaPath = samePng;
        request.graphStagingObserver = {};
        std::set<std::int64_t> compared;
        request.encoderFrameValidator = [&](auto frame, const auto* bytes, int w, int h) {
            std::lock_guard lock(mutex);
            const std::vector<std::uint8_t> image(bytes, bytes + w * h * 4);
            compared.insert(frame);
            e.name = name;
            e.record("image-differential", static_cast<int>(frame), image, actualGraph.at(frame));
            return image == actualGraph.at(frame);
        };
        request.outputPath = e.root / (name + "-image.mp4");
        check(app::exportTimeline(p, request).success && compared.size() == 3,
              "Graph と通常 Image の空間 ClipEffects は全画素で同じ一回の効果になる");
        if (i == 5)
            check(actualGraph.at(0) != actualGraph.at(2), "空間 keyframe は実際に画素を変える");
    }
}

void negatives() {
    check(test::mltSourceOver({0, 0, 0, 255}, {255, 255, 255, 112}) ==
              test::Pixel{112, 112, 112, 254},
          "負例: alpha 112 の定数逆数演算を直接除算へ置き換えない");
    const test::Pixel source{200, 99, 31, 128}, bg{0, 0, 0, 255};
    const auto expected = test::mltSourceOver(bg, source);
    check(expected == test::Pixel{100, 49, 15, 254}, "記録された差分を数式から再現する");
    auto wrong = source;
    wrong[1] = 100;
    check(test::mltSourceOver(bg, wrong) != expected, "負例: source frame を取り違える");
    wrong = source;
    wrong[3] = 255;
    check(test::mltSourceOver(bg, wrong) != expected, "負例: Graph alpha を不透明にする");
    check(test::mltSourceOver(bg, source, 0.5) != expected,
          "負例: Graph と Image の合成設定が分岐する");
    wrong = {100, 49, 15, 128};
    check(test::mltSourceOver(bg, wrong) != expected,
          "負例: premultiplied 入力を straight と誤解する");
    check(test::Pixel{100, 50, 16, 255} != expected, "負例: 最近接整数丸めで代用する");
    const test::Pixel second{17, 211, 87, 128};
    check(test::mltSourceOver(test::mltSourceOver(bg, source), second) !=
              test::mltSourceOver(test::mltSourceOver(bg, second), source),
          "負例: layer 順序を逆転する");
    check(source != expected, "負例: audit を合成前へ移動する");
    check(test::mltSourceOver({0, 0, 0, 0}, source) != expected, "負例: 背景 alpha が欠ける");
}

void realPng(Evidence& evidence, const std::filesystem::path& path) {
    evidence.width = 320;
    evidence.name = "real-manim";
    const auto decoded = graph::readRgba(path, 320, 180);
    check(std::holds_alternative<graph::Raster>(decoded), "実 Manim の PNG を独立に decode する");
    if (!std::holds_alternative<graph::Raster>(decoded))
        return;

    struct Input {
        Evidence* evidence;
        std::vector<std::uint8_t> source, background, composed;
    } input{&evidence, std::get<graph::Raster>(decoded).rgba, {}, {}};

    input.background.resize(input.source.size());
    input.composed.resize(input.source.size());
    for (std::size_t at = 0; at < input.source.size(); at += 4) {
        input.background[at + 3] = 255;
        const auto result =
            test::mltSourceOver({0, 0, 0, 255}, {input.source[at], input.source[at + 1],
                                                 input.source[at + 2], input.source[at + 3]});
        std::copy(result.begin(), result.end(),
                  input.composed.begin() + static_cast<std::ptrdiff_t>(at));
    }
    evidence.rows << "real-manim\tpng-sha\t0\t" << fileHash(path) << '\n';
    const auto filename = path.string();
    const char* filenames[] = {filename.c_str()};
    check(mvm_mlt_rgba_diagnostic(
              filenames, 1, "#000000", 320, 180, 1,
              [](int stage, int layer, const unsigned char* bytes, int width, int height,
                 void* opaque) {
                  auto& observed = *static_cast<Input*>(opaque);
                  const char* names[] = {"background", "producer", "pre-destination", "pre-source",
                                         "post"};
                  const auto& expected = stage == 4                 ? observed.composed
                                         : stage == 1 || stage == 3 ? observed.source
                                                                    : observed.background;
                  observed.evidence->record(
                      names[stage], layer,
                      std::vector<std::uint8_t>(bytes, bytes + width * height * 4), expected);
              },
              &input) == 0,
          "実 Manim の非一様 PNG を同じ DLL の全境界で観測する");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc != 3 && argc != 4) {
        std::cerr << "段階名と新規の証拠 directory を指定してください\n";
        return 2;
    }
    const std::filesystem::path root(argv[2]);
    if (std::filesystem::exists(root))
        return 2;
    std::filesystem::create_directories(root);
    Evidence evidence{root, std::ofstream(root / "pixels.tsv"), "", {}};
    evidence.rows
        << "case\tstage\tlayer\tactual-sha\toracle-sha\trgba\tx\ty\tchannel\tmismatches\n";
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR))
        return 1;
    const std::string mode(argv[1]);
    if (mode == "real-png" && argc == 4) {
        realPng(evidence, argv[3]);
    } else if (mode == "alpha-domain") {
        for (int alpha = 0; alpha <= 255; ++alpha) {
            const auto source = alpha == 0
                                    ? test::Pixel{}
                                    : test::Pixel{255, 99, 31, static_cast<std::uint8_t>(alpha)};
            matrix(evidence, "alpha-" + std::to_string(alpha), {source}, {0, 0, 0, 255}, "#000000",
                   false, 1, false);
            matrix(evidence, "colored-alpha-" + std::to_string(alpha), {source}, {23, 71, 143, 255},
                   "#ff17478f", false, 1, false);
        }
    } else if (mode == "diagnostic") {
        matrix(evidence, "recorded", {{200, 99, 31, 128}}, {0, 0, 0, 255}, "#000000");
    } else if (mode == "oracle" || mode == "differential") {
        const bool product = mode == "differential";
        if (product)
            spatialEffects(evidence);
        negatives();
        for (int alpha : {255, 128, 0, 1, 254}) {
            const test::Pixel source =
                alpha ? test::Pixel{200, 99, 31, static_cast<std::uint8_t>(alpha)} : test::Pixel{};
            matrix(evidence, "black-" + std::to_string(alpha), {source}, {0, 0, 0, 255}, "#000000",
                   false, 1, product);
            matrix(evidence, "colored-" + std::to_string(alpha), {source}, {23, 71, 143, 255},
                   "#ff17478f", false, 1, product);
            if (alpha)
                matrix(evidence, "transparent-" + std::to_string(alpha), {source}, {0, 0, 0, 0},
                       "#00000000", false, 1, false);
        }
        matrix(evidence, "two", {{200, 99, 31, 128}, {17, 211, 87, 128}}, {0, 0, 0, 255}, "#000000",
               false, 1, product);
        matrix(evidence, "three", {{200, 99, 31, 128}, {17, 211, 87, 128}, {81, 41, 203, 254}},
               {0, 0, 0, 255}, "#000000", false, 1, product);
        matrix(evidence, "reordered", {{81, 41, 203, 254}, {17, 211, 87, 128}, {200, 99, 31, 128}},
               {0, 0, 0, 255}, "#000000", false, 1, product);
        matrix(evidence, "draw", {{200, 99, 31, 128}}, {0, 0, 0, 255}, "#000000", true, 1, product);
        matrix(evidence, "effects", {{200, 99, 31, 128}}, {0, 0, 0, 255}, "#000000", false, 0.5,
               product);
    } else {
        return 2;
    }
    mvm_mlt_runtime_shutdown();
    std::cout << "検査 " << checks << " 件、失敗 " << failures << " 件\n";
    return failures ? 1 : 0;
}
