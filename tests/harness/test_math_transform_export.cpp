// disk の変形を直接書き出す契約。preview の常駐 mask を使わず、復号した画素も照合する。
#include "app/math_clip_render.h"
#include "app/timeline_export.h"
#include "math_fake_backend.h"
#include "media/mlt/mvm_mlt_runtime.h"

#include <cstdio>
#include <fstream>
#include <iterator>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

namespace {
namespace app = mvm::app;
namespace math = mvm::math;
namespace project = mvm::project;
int checks = 0, failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message.c_str());
    }
}

bool pump(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

constexpr int width = 320, height = 240, clipFrames = 30;

project::TimelineClip endpoint(std::string id, std::string source, int start, std::string color) {
    project::TimelineClip clip;
    clip.id = clip.name = id;
    clip.kind = project::TimelineClipKind::Math;
    clip.track = {project::TrackKind::Video, 0};
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = clip.sourceOutFrame = clipFrames;
    clip.timelineStartFrame = start;
    clip.math.source = source;
    clip.math.color = color;
    clip.math.fontSize = 64;
    return clip;
}

app::TimelineMathTransformArtifact inputFor(const math::MathTransformSpec& spec,
                                            const app::MathTransformArtifact& artifact) {
    app::TimelineMathTransformArtifact input;
    input.spec = spec;
    input.width = artifact.width;
    input.height = artifact.height;
    input.sourceX = artifact.sourceX;
    input.sourceY = artifact.sourceY;
    input.targetX = artifact.targetX;
    input.targetY = artifact.targetY;
    input.frames = static_cast<std::int64_t>(artifact.frames.size());
    input.loadFrame = [artifact](std::size_t i, std::vector<std::uint8_t>& bytes,
                                 std::string& error) {
        return app::loadMathTransformFrame(artifact, i, bytes, error);
    };
    return input;
}

QByteArray decode(const QString& ffmpeg, const std::filesystem::path& path) {
    QProcess process;
    process.start(ffmpeg, {"-v", "error", "-i", QString::fromStdWString(path.wstring()), "-f",
                           "rawvideo", "-pix_fmt", "rgba", "-"});
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return process.readAllStandardOutput();
}

void run(const QString& ffmpeg, const std::filesystem::path& root, bool reverse) {
    mvm::test::FakeMathBackend backend;
    // 半透明の被覆で、最下層でも alpha が捨てられずに合成されることを確かめる。
    const auto preflight = [base = backend.preflight()](const std::filesystem::path& work,
                                                        const std::atomic<bool>* cancel) {
        auto result = base(work, cancel);
        result.backend.fingerprint.canonical += "coverage=128\n";
        result.backend.render =
            [render = result.backend.render](const math::MathStaticRenderRequest& request,
                                             const std::atomic<bool>* stop) {
                auto rendered = render(request, stop);
                if (rendered.status == math::MathRenderStatus::Ok) {
                    QImage image(QString::fromStdWString(rendered.png.wstring()));
                    image.fill(QColor(255, 255, 255, 128));
                    if (!image.save(QString::fromStdWString(rendered.png.wstring()), "PNG"))
                        rendered.status = math::MathRenderStatus::Failed;
                }
                return rendered;
            };
        return result;
    };
    app::MathRasterCache cache(reverse ? "reverse" : "forward", preflight);
    cache.setAuthority(root, true);
    check(
        pump([&] { return cache.backendState() == app::MathRasterCache::BackendState::Available; }),
        "偽 backend が使える");
    auto p = project::createDefaultProject();
    p.outputWidth = width;
    p.outputHeight = height;
    p.timelineClips = {
        endpoint("A", reverse ? "SIZE48x24 a" : "SIZE49x25 a", 0, "#FFFF0000"),
        endpoint("B", reverse ? "SIZE49x25 b" : "SIZE48x24 b", clipFrames, "#FF0000FF")};
    p.timelineTransitions = {{"T", "A", "B", 3, 3, project::TransitionKind::MathTransform}};
    const auto spec = *app::mathTransformSpecFor(p.timelineTransitions[0], p.timelineClips[0],
                                                 p.timelineClips[1]);
    check(pump([&] {
              return cache.requestTransform(spec).state == app::MathRasterCache::State::Ready;
          }),
          "disk の変形が Ready");
    const auto artifact = cache.readyTransformForExport(spec);
    check(artifact.has_value(), "現在の端点と provenance を再検査できる");
    if (!artifact)
        return;
    app::TimelineExportRequest request;
    request.width = width;
    request.height = height;
    request.outputPath = root / "transform.mp4";
    request.videoCrf = 0;
    for (const auto& clip : p.timelineClips)
        request.mathArtifacts.emplace(clip.id,
                                      *cache.readyArtifact(app::mathRenderSpecFor(clip.math)));
    request.mathTransforms.emplace("T", inputFor(spec, *artifact));
    auto loadedIndices = std::make_shared<std::vector<std::size_t>>();
    const auto diskLoader = request.mathTransforms.at("T").loadFrame;
    request.mathTransforms.at("T").loadFrame =
        [loadedIndices, diskLoader](std::size_t i, std::vector<std::uint8_t>& bytes,
                                    std::string& error) {
            loadedIndices->push_back(i);
            return diskLoader(i, bytes, error);
        };
    check(cache.transformResidencyOf(spec).state == app::MathRasterCache::Residency::NotReady,
          "書き出し前の変形 mask は常駐していない");
    const auto plan = app::mapTimelineExportPlan(p, request);
    check(plan.success && plan.clips.size() == 4 && plan.clips[1].mathTransformId == "T" &&
              plan.clips[1].timelineStartFrame == 27 && plan.clips[2].mathTransformId == "T" &&
              plan.clips[2].timelineStartFrame == 30 && plan.clips[3].mathTransformId.empty() &&
              plan.clips[3].timelineStartFrame == 33,
          "cut の前後を同じ変形に割り当て、直後は静止に戻る");
    loadedIndices->clear();
    const auto result = app::exportTimeline(p, request);
    check(result.success && result.frameCount == 60,
          "常駐 mask が無くても動画を書き出す: " + result.error);
    check(*loadedIndices == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5},
          "開始前の全 frame の検査の後、cut 前は 0..2・cut 後は 3..5 を disk から読む");
    const auto frames = decode(ffmpeg, request.outputPath);
    constexpr qsizetype frameBytes = width * height * 4;
    check(frames.size() == frameBytes * 60, "復号した 60 frame を比較する");
    if (frames.size() == frameBytes * 60)
        check(frames.mid(26 * frameBytes, frameBytes) == frames.mid(27 * frameBytes, frameBytes),
              "復号した変形 frame 0 は直前の通常の静止 A と全画素一致する");
    math::MathCoverage a, b;
    std::string error;
    check(app::loadMathCoverage(request.mathArtifacts.at("A"), a, error) &&
              app::loadMathCoverage(request.mathArtifacts.at("B"), b, error),
          "両端の mask を読む");
    math::MathTransformRasterPlacement placement;
    check(math::mathTransformRasterPlacement(artifact->width, artifact->height, artifact->sourceX,
                                             artifact->sourceY, a.width, a.height,
                                             artifact->targetX, artifact->targetY, b.width,
                                             b.height, width, height, placement),
          "偶奇が異なる端点を配置できる");
    check(placement.targetLeft - placement.sourceLeft == (reverse ? -1 : 1) &&
              placement.targetTop - placement.sourceTop == (reverse ? -1 : 1),
          "artifact の原点が各軸で ±1 px 異なる対照");
    int left = 0, top = 0;
    check(math::mathTransformArtifactOriginAt(placement, 5, 6, left, top) &&
              left == placement.targetLeft && top == placement.targetTop,
          "最後の frame は target 側の原点を使う");
    for (int t : {26, 27, 29, 30, 32, 33}) {
        std::vector<std::uint8_t> expected;
        if (t < 27 || t >= 33) {
            const auto& clip = p.timelineClips[t < 27 ? 0 : 1];
            expected = app::composeMathClipFromPng(request.mathArtifacts.at(clip.id), clip.math,
                                                   width, height)
                           .rgba;
        } else {
            const auto i = app::mathTransformFrameAt({27, 6}, t);
            std::vector<std::uint8_t> bytes;
            check(app::loadMathTransformFrame(*artifact, static_cast<std::size_t>(i), bytes, error),
                  "比較対象の disk frame を読む");
            math::MathComposeStyle style;
            math::mathTransformColorAt(0xFFFF0000, 0xFF0000FF, i, 6, style.colorArgb);
            math::mathTransformArtifactOriginAt(placement, i, 6, left, top);
            expected.resize(static_cast<std::size_t>(frameBytes), 0);
            math::composeMathPatchAt(bytes.data(), artifact->width, artifact->height, style,
                                     expected.data(), width, left, top);
            if (i == 0)
                check(expected == app::composeMathClipFromPng(request.mathArtifacts.at("A"),
                                                              p.timelineClips[0].math, width,
                                                              height)
                                      .rgba,
                      "変形 frame 0 は普通の静止 A と全画素一致する");
        }
        if (frames.size() != frameBytes * 60)
            continue;
        const auto* decoded =
            reinterpret_cast<const unsigned char*>(frames.constData() + t * frameBytes);
        std::size_t compared = 0, bad = 0, colorCompared = 0, colorBad = 0;
        double expectedEnergy = 0, actualEnergy = 0, expectedX = 0, expectedY = 0, actualX = 0,
               actualY = 0;
        for (std::size_t at = 0; at < expected.size(); at += 4) {
            // H.264 の 4:2:0 による境界の色差を避け、被覆の有無は全画素で照合する。
            const bool visible = expected[at + 3] > 64;
            const bool observed = decoded[at] + decoded[at + 1] + decoded[at + 2] > 60;
            ++compared;
            bad += visible != observed;
            const auto pixel = at / 4;
            const auto x = pixel % width;
            const auto y = pixel / width;
            const double wantedIntensity =
                (expected[at] + expected[at + 1] + expected[at + 2]) * expected[at + 3] / 255.0;
            const double observedIntensity = decoded[at] + decoded[at + 1] + decoded[at + 2];
            expectedEnergy += wantedIntensity;
            actualEnergy += observedIntensity;
            expectedX += static_cast<double>(x) * wantedIntensity;
            expectedY += static_cast<double>(y) * wantedIntensity;
            actualX += static_cast<double>(x) * observedIntensity;
            actualY += static_cast<double>(y) * observedIntensity;
            if (x > 0 && x + 1 < width && pixel >= width && pixel + width < width * height &&
                expected[at + 3] > 64 && expected[at - 4 + 3] > 64 && expected[at + 4 + 3] > 64 &&
                expected[at - width * 4 + 3] > 64 && expected[at + width * 4 + 3] > 64) {
                ++colorCompared;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    colorBad +=
                        std::abs(static_cast<int>(decoded[at + channel]) -
                                 (expected[at + channel] * expected[at + 3] + 127) / 255) > 30;
            }
        }
        check(compared == width * height && bad <= 80, "復号した frame " + std::to_string(t) +
                                                           " の配置と被覆が一致 (差 " +
                                                           std::to_string(bad) + ")");
        check(colorCompared > 0 && colorBad == 0,
              "復号色は共有の補間に一致する: " + std::to_string(t));
        check(expectedEnergy > 0 && actualEnergy > 0 &&
                  std::abs(expectedX / expectedEnergy - actualX / actualEnergy) < 0.35 &&
                  std::abs(expectedY / expectedEnergy - actualY / actualEnergy) < 0.35,
              "復号画像の重心も一致し、1 px の原点の誤りを許さない: " + std::to_string(t));
    }
    check(pump([&] {
              return cache.residentTransform(spec).state ==
                     app::MathRasterCache::Residency::Resident;
          }),
          "対照として変形 mask を一度常駐させる");
    cache.setResidentMemoryBudget(1024 * 1024);
    check(cache.transformResidencyOf(spec).state != app::MathRasterCache::Residency::Resident &&
              cache.residentBytes() == 0,
          "全常駐 mask を追い出す");
    request.outputPath = root / "evicted.mp4";
    check(app::exportTimeline(p, request).success, "全 mask の追い出し後も disk から書き出す");
    cache.setResidentMemoryBudget(1);
    check(cache.residentTransform(spec).state == app::MathRasterCache::Residency::OverBudget,
          "preview は OverBudget");
    request.outputPath = root / "over-budget.mp4";
    check(app::exportTimeline(p, request).success, "disk Ready + preview OverBudget でも書き出す");
    check(cache.residentBytes() == 0, "書き出しは常駐予算を使わない");
    auto absent = request;
    absent.mathTransforms.clear();
    check(!app::mapTimelineExportPlan(p, absent).success, "必要な変形を hard cut へ代用しない");
    auto stale = request;
    stale.mathTransforms.at("T").spec.target.source += "changed";
    check(!app::mapTimelineExportPlan(p, stale).success, "現在の Project と異なる変形を拒否する");
    auto shortFrames = request;
    --shortFrames.mathTransforms.at("T").frames;
    check(!app::mapTimelineExportPlan(p, shortFrames).success, "区間と異なる枚数を拒否する");
    auto small = request;
    small.width = 50;
    check(!app::mapTimelineExportPlan(p, small).success, "disk Ready の配置不成立を拒否する");
    check(cache.requestTransform(spec).state == app::MathRasterCache::State::Ready,
          "配置不成立でも disk の Ready は変えない");
    auto noStatic = request;
    noStatic.mathArtifacts.erase("B");
    check(!app::mapTimelineExportPlan(p, noStatic).success, "現在の target 静止を要求する");
    auto disabled = p;
    disabled.timelineClips[0].enabled = false;
    check(app::mapTimelineExportPlan(disabled, absent).success, "無効な端点の変形は要求しない");
    auto hidden = p;
    hidden.videoTracks[0].muted = true;
    check(app::mapTimelineExportPlan(hidden, absent).error.find("数式の変形") == std::string::npos,
          "非出力 track の変形は要求しない");
    // 共通エフェクトは両端に同じ値を持つ P2-1 の条件を満たし、mapping だけで一度掛ける。
    auto effects = p;
    for (auto& clip : effects.timelineClips) {
        clip.effects.positionXPercent = 10;
        clip.effects.opacityPercent = 50;
    }
    const auto effectsPlan = app::mapTimelineExportPlan(effects, request);
    check(effectsPlan.success && effectsPlan.clips[1].effectsEnabled &&
              effectsPlan.clips[1].rectX == 32 &&
              effectsPlan.clips[1].opacityKeys[0].opacity == 0.5,
          "ClipEffects の位置・不透明度は通常の mapping に一度だけ掛かる");
    auto effectsRequest = request;
    effectsRequest.outputPath = root / "effects.mp4";
    check(app::exportTimeline(effects, effectsRequest).success,
          "共通エフェクト付きの変形を書き出す");
    const auto effectFrames = decode(ffmpeg, effectsRequest.outputPath);
    if (effectFrames.size() == frameBytes * 60 && frames.size() == frameBytes * 60) {
        const auto moments = [&](const QByteArray& sequence) {
            const auto* pixels =
                reinterpret_cast<const unsigned char*>(sequence.constData() + 27 * frameBytes);
            double sum = 0, xsum = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const auto at = (y * width + x) * 4;
                    const double intensity = pixels[at] + pixels[at + 1] + pixels[at + 2];
                    sum += intensity;
                    xsum += x * intensity;
                }
            return std::pair{sum, xsum / sum};
        };
        const auto plain = moments(frames);
        const auto effected = moments(effectFrames);
        check(plain.first > 1000 && std::abs(effected.first / plain.first - 0.5) < 0.08 &&
                  std::abs(effected.second - plain.second - 32) < 1,
              "復号画素でも位置 +32 px・不透明度 50% が一度だけ掛かる");
    } else {
        check(false, "共通エフェクトの復号画素を比較できる");
    }
    auto write = p;
    write.timelineClips[0].mathAnimation = {project::MathIntroKind::Write, 6};
    const auto writeSpec = *app::mathSequenceSpecFor(write.timelineClips[0]);
    check(pump([&] {
              return cache.requestSequence(writeSpec).state == app::MathRasterCache::State::Ready;
          }),
          "先行 Write の成果物が Ready");
    auto writeRequest = request;
    writeRequest.mathWriteFrames.emplace("A", cache.readySequence(writeSpec)->frames);
    writeRequest.outputPath = root / "write-transform.mp4";
    const auto writePlan = app::mapTimelineExportPlan(write, writeRequest);
    check(writePlan.success && writePlan.clips.size() == 5 && writePlan.clips[0].mathWrite &&
              writePlan.clips[0].mathTransformId.empty() && !writePlan.clips[2].mathWrite &&
              writePlan.clips[2].mathTransformId == "T",
          "Write と変形を重複せずに分ける");
    check(app::exportTimeline(write, writeRequest).success, "Write の後の変形を両方書き出す");
    const auto writeFrames = decode(ffmpeg, writeRequest.outputPath);
    check(writeFrames.size() == frameBytes * 60, "Write と変形を含む動画を復号する");
    if (writeFrames.size() == frameBytes * 60 && frames.size() == frameBytes * 60) {
        const auto* intro = reinterpret_cast<const unsigned char*>(writeFrames.constData());
        long long firstIntensity = 0;
        for (qsizetype at = 0; at < frameBytes; at += 4)
            firstIntensity += intro[at] + intro[at + 1] + intro[at + 2];
        check(firstIntensity == 0, "Write frame 0 は静止や変形で代用せず透明になる");
        check(writeFrames.mid(27 * frameBytes, 6 * frameBytes) ==
                  frames.mid(27 * frameBytes, 6 * frameBytes),
              "先行 Write があっても変形の全 frame は同じ画素になる");
    }
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto writeFile = [](const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    auto provenance = artifact->frames[0].parent_path();
    provenance += L".txt";
    const auto validProvenance = read(provenance);
    check(!validProvenance.empty(), "変形の provenance を実際に検査する");
    std::filesystem::rename(provenance, root / "saved-transform.txt");
    check(!cache.readyTransformForExport(spec), "変形の provenance が無ければ拒否する");
    std::filesystem::rename(root / "saved-transform.txt", provenance);
    writeFile(provenance, "stale\n" + validProvenance);
    check(!cache.readyTransformForExport(spec), "古い変形の provenance を拒否する");
    writeFile(provenance, validProvenance);
    for (const auto& id : {"A", "B"}) {
        const auto staticPath = request.mathArtifacts.at(id);
        auto staticProvenance = staticPath;
        staticProvenance.replace_extension(L".txt");
        const auto validStatic = read(staticProvenance);
        std::filesystem::rename(staticPath, root / "saved-static.png");
        check(!cache.readyTransformForExport(spec),
              std::string(id) + ": 現在の静止が無ければ拒否する");
        std::filesystem::rename(root / "saved-static.png", staticPath);
        writeFile(staticProvenance, "stale\n" + validStatic);
        check(!cache.readyTransformForExport(spec),
              std::string(id) + ": 静止の provenance が古ければ拒否する");
        writeFile(staticProvenance, validStatic);
    }
    check(cache.readyTransformForExport(spec).has_value(), "対照の正しい disk は復元後に通る");
    std::filesystem::rename(artifact->frames[2], root / "saved-frame.a8");
    check(!cache.readyTransformForExport(spec), "変形の frame が無ければ拒否する");
    std::filesystem::rename(root / "saved-frame.a8", artifact->frames[2]);
    const auto validFrame = read(artifact->frames[2]);
    auto alteredFrame = validFrame;
    alteredFrame[0] = static_cast<char>(static_cast<unsigned char>(alteredFrame[0]) ^ 0xFF);
    writeFile(artifact->frames[2], alteredFrame);
    check(!cache.readyTransformForExport(spec) && !app::mapTimelineExportPlan(p, request).success,
          "byte 数が同じ破損も SHA-256 で出力開始前に拒否する");
    writeFile(artifact->frames[2], validFrame);
    std::ofstream corrupt(artifact->frames[2], std::ios::binary | std::ios::trunc);
    corrupt << "broken";
    corrupt.close();
    check(!cache.readyTransformForExport(spec), "Ready でも壊れた disk frame を拒否する");
    check(!app::mapTimelineExportPlan(p, request).success, "破損を出力開始前に拒否する");
    request.outputPath = root / "refused.mp4";
    writeFile(request.outputPath, "既存の出力を保つ");
    check(!app::exportTimeline(p, request).success &&
              read(request.outputPath) == "既存の出力を保つ",
          "必要な変形が壊れた書き出しは既存の出力に触れず失敗する");
    cache.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication gui(argc, argv);
    if (argc != 2)
        return 2;
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT の初期化に失敗\n");
        return 1;
    }
    QTemporaryDir directory;
    const std::filesystem::path root(directory.path().toStdWString());
    run(QString::fromLocal8Bit(argv[1]), root / "forward", false);
    run(QString::fromLocal8Bit(argv[1]), root / "reverse", true);
    mvm_mlt_runtime_shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
