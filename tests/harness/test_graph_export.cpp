#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mlt_rgba_oracle.h"

#include <fstream>
#include <iostream>
#include <mutex>

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

project::Project fixture() {
    auto p = project::createDefaultProject();
    p.outputWidth = 64;
    p.outputHeight = 36;
    p.timelineFpsNum = 30;
    p.timelineFpsDen = 1;
    check(project::addGraph(p, "graph", {"function"}, "グラフ", {project::TrackKind::Video, 0}, 0)
              .success,
          "Graph を作成する");
    auto& c = p.timelineClips.front();
    c.sourceFrameCount = c.sourceOutFrame = 12;
    c.graph.axes = {false, false, "", ""};
    c.graph.functions[0].expression = "0";
    c.graph.intro = {project::GraphIntroKind::Draw, 10};
    return p;
}

graph::RenderResult render(const graph::RenderRequest& request, const std::atomic<bool>*) {
    for (std::int64_t i = -1; i < request.spec.drawFrames; ++i) {
        graph::Raster raster{
            request.spec.width, request.spec.height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(request.spec.width) *
                                      static_cast<std::size_t>(request.spec.height) * 4)};
        if (i != 0) {
            raster.rgba[0] = 200;
            raster.rgba[1] = i < 0 ? 99 : static_cast<std::uint8_t>(i);
            raster.rgba[2] = 31;
            raster.rgba[3] = 128;
        }
        if (!graph::writeRgba(request.job /
                                  (i < 0 ? "static.png" : "frame-" + std::to_string(i) + ".png"),
                              raster))
            return graph::Error{graph::Failure::RendererFailure, 0, "試験 PNG を書けません"};
    }
    return std::monostate{};
}

void run(const std::filesystem::path& root) {
    using E = app::GraphExportFailure;
    auto p = fixture();
    std::atomic<bool> cancel{false};
    int preparations = 0, renders = 0;
    app::GraphExportEnvironment env;
    env.cache = root / "cache";
    env.toolchain = "独立試験の固定 identity";
    env.cancel = &cancel;
    env.preflight =
        [&](const std::filesystem::path&,
            const std::atomic<bool>*) -> std::variant<manim::GraphBackend, graph::Error> {
        ++preparations;
        return manim::GraphBackend{env.toolchain, [&](const auto& request, const auto* flag) {
                                       ++renders;
                                       return render(request, flag);
                                   }};
    };
    auto plan = app::prepareGraphExport(p, 64, 36, 0, 12, env);
    check(plan.readiness.ready() && plan.packages.size() == 1 && renders == 1,
          "混合範囲は一つの Draw package だけを生成する");
    if (!plan.readiness.ready()) {
        std::cerr << plan.readiness.detail << '\n';
        return;
    }
    {
        std::ofstream ledger(root / "dependency-ledger.tsv", std::ios::binary);
        ledger << "clipId\tpackageKey\toutputFrame\tsourceFrame\tartifactFrame\n";
        for (const auto& [id, dependency] : plan.clips)
            for (const auto& frame : dependency.frames)
                ledger << id << '\t' << dependency.packageKey << '\t' << frame.outputFrame << '\t'
                       << frame.sourceFrame << '\t' << frame.artifactFrame << '\n';
    }
    for (int i = 0; i < 12; ++i) {
        const auto frame = app::loadGraphExportFrame(plan, "graph", i);
        check(frame.readiness.ready() && frame.sourceFrame == i, "exact source frame を取得する");
        if (frame.raster)
            check(frame.raster->rgba[1] == (i == 0   ? 0
                                            : i < 10 ? i
                                                     : 99) &&
                      frame.raster->rgba[3] == (i == 0 ? 0 : 128),
                  "Draw／端点を混同せず straight alpha を保持する");
    }
    auto noBackend = env;
    noBackend.preflight = {};
    auto drawOnly = app::prepareGraphExport(p, 64, 36, 0, 10, noBackend);
    check(drawOnly.readiness.ready() && drawOnly.packages.size() == 1 && preparations == 1,
          "Draw のみは独立 static package と backend を要求しない");
    auto staticOnly = app::prepareGraphExport(p, 64, 36, 10, 12, noBackend);
    check(staticOnly.readiness.ready() && staticOnly.packages.size() == 1 &&
              staticOnly.clips.at("graph").packageKey == plan.clips.at("graph").packageKey,
          "静止のみは検証済み Draw endpoint を使う");
    auto trimmed = p;
    trimmed.timelineClips[0].sourceInFrame = 4;
    auto right = app::prepareGraphExport(trimmed, 64, 36, 0, 8, noBackend);
    const auto rightFrame = app::loadGraphExportFrame(right, "graph", 0);
    check(right.readiness.ready() && right.clips.at("graph").frames.front().sourceFrame == 4 &&
              rightFrame.raster && rightFrame.raster->rgba[1] == 4,
          "split 右側の source 原点を再開しない");
    auto one = app::prepareGraphExport(p, 64, 36, 4, 5, noBackend);
    check(one.readiness.ready() && one.clips.at("graph").frames.size() == 1 &&
              one.clips.at("graph").frames[0].artifactFrame == 4,
          "一 frame の半開区間");
    auto invalid = p;
    invalid.timelineClips[0].graph.functions[0].expression = "sin(";
    check(app::prepareGraphExport(invalid, 64, 36, 12, 13, noBackend).readiness.ready(),
          "画面外の不正式を compile しない");
    check(app::prepareGraphExport(invalid, 64, 36, 0, 1, noBackend).readiness.failure ==
              E::InvalidExpression,
          "必要な不正式は拒否する");
    invalid.timelineClips[0].enabled = false;
    check(app::prepareGraphExport(invalid, 64, 36, 0, 1, noBackend).clips.empty(),
          "無効 clip を除外する");
    invalid.timelineClips[0].enabled = true;
    invalid.videoTracks[0].muted = true;
    check(app::prepareGraphExport(invalid, 64, 36, 0, 1, noBackend).clips.empty(),
          "非出力 track を除外する");
    auto shared = p;
    auto copy = shared.timelineClips[0];
    copy.id = "other";
    copy.graph.functions[0].id = {"other-function"};
    copy.timelineStartFrame = 12;
    copy.sourceInFrame = 4;
    shared.timelineClips.push_back(copy);
    auto twins = app::prepareGraphExport(shared, 64, 36, 0, 20, noBackend);
    check(twins.readiness.ready() && twins.packages.size() == 1 && twins.clips.size() == 2 &&
              twins.clips.at("other").frames.front().sourceFrame == 4,
          "key 共有でも所有と時間を共有しない");
    auto staticFirst = shared;
    staticFirst.timelineClips[0].id = "a-static";
    staticFirst.timelineClips[0].sourceInFrame = 10;
    auto grouped = env;
    grouped.cache = root / "grouped-cache";
    const auto minimal = app::prepareGraphExport(staticFirst, 64, 36, 0, 20, grouped);
    check(minimal.readiness.ready() && minimal.packages.size() == 1 &&
              minimal.clips.at("a-static").packageKey == minimal.clips.at("other").packageKey,
          "静止所有者が先でも共通 Draw package 一つだけを要求する");
    auto rational = p;
    rational.timelineClips[0].sourceFpsNum = 24000;
    rational.timelineClips[0].sourceFpsDen = 1001;
    auto fractions = app::prepareGraphExport(rational, 64, 36, 0, 10, noBackend);
    check(fractions.readiness.ready(), "有理数 FPS を受理する");
    if (fractions.readiness.ready())
        for (const auto& frame : fractions.clips.at("graph").frames) {
            std::string error;
            const auto expected = project::evaluateGraphClip(rational.timelineClips[0], {30, 1},
                                                             frame.outputFrame, error);
            check(expected && expected->sourceFrame == frame.sourceFrame,
                  "P4-1 の有理数 mapping と一致する");
        }
    auto changed = p;
    changed.timelineClips[0].graph.functions[0].expression = "1";
    check(app::prepareGraphExport(changed, 64, 36, 0, 1, noBackend).readiness.failure ==
              E::BackendUnavailable,
          "古い key の package を現在の式へ流用しない");
    cancel.store(true);
    check(app::prepareGraphExport(p, 64, 36, 0, 1, env).readiness.failure == E::Cancelled,
          "依存準備の取消を検出する");
    check(app::loadGraphExportFrame(plan, "graph", 1, &cancel).readiness.failure == E::Cancelled,
          "decode 前の取消を検出する");
    cancel.store(false);
    check(app::loadGraphExportFrame(plan, "graph", 12).readiness.failure == E::FrameMissing,
          "ledger にない frame を代用しない");
    auto excessive = p;
    excessive.timelineClips[0].sourceFrameCount = excessive.timelineClips[0].sourceOutFrame =
        1000001;
    check(app::prepareGraphExport(excessive, 64, 36, 0, 1000001, noBackend).readiness.failure ==
              E::ResourceLimit,
          "上限を超える台帳は frame 列の確保前に拒否する");
    const auto directory = plan.packages.begin()->second->artifact.directory;
    auto decoded = graph::readRgba(directory / "frame-4.png", 64, 36);
    auto bytes = std::get<graph::Raster>(std::move(decoded));
    bytes.rgba[0] ^= 1;
    check(graph::writeRgba(directory / "frame-4.png", bytes), "画素破損の負例を作る");
    check(app::loadGraphExportFrame(plan, "graph", 4).readiness.failure == E::ArtifactCorrupt,
          "検証後の画素変更を SHA で拒否する");
    check(app::prepareGraphExport(p, 64, 36, 12, 13, noBackend).readiness.ready(),
          "画面外の破損 package を検証しない");
    check(app::prepareGraphExport(p, 64, 36, 0, 1, noBackend).readiness.failure ==
              E::ArtifactCorrupt,
          "必要な package は全体検証で破損を拒否する");
    std::filesystem::remove(directory / "frame-5.png");
    check(app::loadGraphExportFrame(plan, "graph", 5).readiness.failure == E::FrameMissing,
          "Draw 欠損を static で代用しない");
    app::TimelineExportRequest request;
    request.width = 64;
    request.height = 36;
    request.fpsNum = 30;
    request.fpsDen = 1;
    request.graphEnvironment = env;
    request.graphEnvironment.cache = root / "mapping-cache";
    const auto mapped = app::mapTimelineExportPlan(p, request);
    check(mapped.success && mapped.clips.size() == 1 && mapped.clips[0].graph &&
              mapped.clips[0].videoTrackIndex == 1 && mapped.clips[0].producerInFrame == 0 &&
              mapped.clips[0].producerOutFrame == 12,
          "通常 overlay mapping に接続する");
    // 静止範囲に Draw package が無ければ static 一つだけを生成する。
    env.cache = root / "static-cache";
    const auto endpoint = app::prepareGraphExport(p, 64, 36, 10, 12, env);
    check(endpoint.readiness.ready() && endpoint.packages.size() == 1 &&
              endpoint.packages.begin()->second->spec.drawFrames == 0,
          "非可視 Draw の独立 package を新規要求しない");
}

void encode(const std::filesystem::path& root) {
    auto p = fixture();
    app::TimelineExportRequest request;
    request.width = 64;
    request.height = 36;
    request.fpsNum = 30;
    request.fpsDen = 1;
    request.renderThreads = 1;
    request.outputPath = root / "graph.mp4";
    request.graphEnvironment.cache = root / "cache";
    request.graphEnvironment.toolchain = "独立 encoder 試験";
    request.graphEnvironment.preflight =
        [](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{"独立 encoder 試験", render};
    };
    std::mutex mutex;
    std::set<std::int64_t> observed;
    request.encoderFrameValidator = [&](std::int64_t t, const std::uint8_t* rgba, int w, int h) {
        std::lock_guard lock(mutex);
        observed.insert(t);
        {
            std::ofstream raw(root / ("encoder-" + std::to_string(t) + ".rgba"), std::ios::binary);
            raw.write(reinterpret_cast<const char*>(rgba), static_cast<std::streamsize>(w * h * 4));
        }
        bool ok = t >= 0 && t < 12 && w == 64 && h == 36;
        // artifact の整数契約とは別の、MLT binary32・切り捨て契約を照合する。
        const test::Pixel source =
            t == 0 ? test::Pixel{}
                   : test::Pixel{200, static_cast<std::uint8_t>(t < 10 ? t : 99), 31, 128};
        const auto pixel = test::mltSourceOver({0, 0, 0, 255}, source);
        for (int channel = 0; channel < 4; ++channel) {
            const int expected = pixel[static_cast<std::size_t>(channel)];
            if (rgba[channel] != expected) {
                std::cerr << "frame " << t << " channel " << channel << ": " << int(rgba[channel])
                          << " 期待 " << expected << '\n';
                ok = false;
            }
        }
        for (std::size_t i = 4; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
             ++i)
            ok = ok && rgba[i] == (i % 4 == 3 ? 255 : 0);
        return ok;
    };
    const auto exported = app::exportTimeline(p, request);
    check(exported.success && exported.frameCount == 12 && observed.size() == 12,
          "H.264 と encoder 直前の全 frame／全画素 oracle");
    if (!exported.success)
        std::cerr << exported.error << '\n';
    // 因果対照: 同じ straight PNG を通常 Image として出す。Graph の decode／timing は通さない。
    auto ordinary = p;
    auto& imageClip = ordinary.timelineClips[0];
    imageClip.kind = project::TimelineClipKind::Image;
    imageClip.graph = {};
    imageClip.track.index = 1;
    graph::Raster reference{64, 36, std::vector<std::uint8_t>(64 * 36 * 4)};
    reference.rgba[0] = 200;
    reference.rgba[1] = 99;
    reference.rgba[2] = 31;
    reference.rgba[3] = 128;
    imageClip.mediaPath = root / "ordinary-image.png";
    check(graph::writeRgba(imageClip.mediaPath, reference), "通常画像の同一 RGBA 対照を作成する");
    std::ifstream endpointFile(root / "encoder-10.rgba", std::ios::binary);
    const std::vector<std::uint8_t> endpointBytes{std::istreambuf_iterator<char>(endpointFile), {}};
    request.outputPath = root / "ordinary-image.mp4";
    std::set<std::int64_t> controlFrames;
    request.encoderFrameValidator = [&](auto t, const auto* rgba, int w, int h) {
        std::lock_guard lock(mutex);
        controlFrames.insert(t);
        return endpointBytes.size() == static_cast<std::size_t>(w * h * 4) &&
               std::equal(endpointBytes.begin(), endpointBytes.end(), rgba);
    };
    const auto control = app::exportTimeline(ordinary, request);
    check(control.success && control.frameCount == 12 && controlFrames.size() == 12,
          "通常 Image も Graph と全画素同一であり丸め差は既存 MLT 境界にある");
    if (!control.success)
        std::cerr << control.error << '\n';
    // oracle の不一致を成功として公開しない。
    request.outputPath = root / "oracle-failure.mp4";
    request.encoderFrameValidator = [](auto, const auto*, auto, auto) { return false; };
    const auto rejected = app::exportTimeline(p, request);
    check(!rejected.success && !std::filesystem::exists(request.outputPath),
          "oracle 失敗で最終公開しない");
    request.outputPath = root / "existing.mp4";
    std::error_code copyError;
    std::filesystem::copy_file(root / "ordinary-image.mp4", request.outputPath, copyError);
    check(!copyError, "実際に検証済みの MP4 を置換失敗の対照にする");
    std::ifstream original(request.outputPath, std::ios::binary);
    const std::string originalBytes{std::istreambuf_iterator<char>(original), {}};
    const auto oldHash = graph::digest(originalBytes);
    original.close();
    const auto preserved = app::exportTimeline(p, request);
    std::ifstream old(request.outputPath, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(old), {}};
    check(!preserved.success && graph::digest(bytes) == oldHash, "失敗時に既存出力を保持する");
    request.outputPath = root / "cancelled.mp4";
    request.encoderFrameValidator = {};
    request.progress = [](auto completed, auto) { return completed > 0; };
    const auto cancelled = app::exportTimeline(p, request);
    check(cancelled.cancelled && !std::filesystem::exists(request.outputPath),
          "encode の取消で最終公開しない");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication gui(argc, argv);
    if (argc != 2 && argc != 3) {
        std::cerr << "新規の証拠 directory を指定してください\n";
        return 2;
    }
    const bool encoding = argc == 3 && std::string(argv[1]) == "--encode";
    const std::filesystem::path root(argv[encoding ? 2 : 1]);
    if (std::filesystem::exists(root)) {
        std::cerr << "既存の証拠を上書きしません\n";
        return 2;
    }
    std::filesystem::create_directories(root);
    if (encoding) {
        if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0)
            return 1;
        encode(root);
        mvm_mlt_runtime_shutdown();
    } else {
        run(root);
    }
    std::cout << "検査 " << checks << " 件、失敗 " << failures << " 件\n";
    return failures ? 1 : 0;
}
