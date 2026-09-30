// preview の frame 問い合わせの費用を、描画区間 (TimelinePreviewPlan) を使い回す場合と
// 毎回作り直す場合で比べる (label performance。通常の試験には含めない)。
//
// 長い timeline (V1 に 2000 clip、各 cut にクロスディゾルブ) で、再生のように frame を順に
// 問い合わせる。合否は速度では決めない (計測値は環境で変わる)。両方の結果が同じであることだけを
// 検査し、1 frame あたりの時間を出力する。性能の判断は release で測った値で行うこと (AGENTS.md)。
#include "app/timeline_preview_mapping.h"
#include "project/timeline_edit.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

constexpr int kClips = 2000;
constexpr std::int64_t kClipFrames = 60;
constexpr std::int64_t kHalfTransition = 10;
constexpr std::int64_t kQueries = 2000;

mvm::project::Project longTimeline() {
    auto project = mvm::project::createDefaultProject();
    for (int index = 0; index < kClips; ++index) {
        mvm::project::TimelineClip clip;
        clip.kind = mvm::project::TimelineClipKind::Video;
        clip.id = "clip-" + std::to_string(index);
        clip.name = clip.id;
        clip.mediaPath = clip.id + ".mp4";
        clip.sourceFpsNum = 60;
        clip.sourceFpsDen = 1;
        // 前後に余白を持たせる (トランジションで延ばす)。
        clip.sourceFrameCount = kClipFrames + 2 * kHalfTransition;
        clip.sourceInFrame = kHalfTransition;
        clip.sourceOutFrame = kHalfTransition + kClipFrames;
        clip.timelineStartFrame = index * kClipFrames;
        project.timelineClips.push_back(std::move(clip));
        if (index > 0)
            project.timelineTransitions.push_back({"t-" + std::to_string(index),
                                                   "clip-" + std::to_string(index - 1),
                                                   "clip-" + std::to_string(index),
                                                   kHalfTransition, kHalfTransition});
    }
    return project;
}

bool sameLayers(const mvm::app::TimelinePreviewFrameMapping& a,
                const mvm::app::TimelinePreviewFrameMapping& b) {
    if (a.success != b.success || a.layers.size() != b.layers.size())
        return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        if (a.layers[i].clipId != b.layers[i].clipId || a.layers[i].slot != b.layers[i].slot ||
            a.layers[i].sourceFrameNumber != b.layers[i].sourceFrameNumber ||
            a.layers[i].transitionOpacity != b.layers[i].transitionOpacity)
            return false;
    }
    return true;
}

} // namespace

int main() {
    const auto project = longTimeline();
    const auto valid = mvm::project::validateTimeline(project);
    if (!valid.success) {
        std::fprintf(stderr, "FAIL: 計測用の timeline が不正です: %s\n", valid.error.c_str());
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto buildStart = Clock::now();
    const auto plan = mvm::app::buildTimelinePreviewPlan(project);
    const auto buildEnd = Clock::now();
    if (!plan.success) {
        std::fprintf(stderr, "FAIL: 描画区間を作れません: %s\n", plan.error.c_str());
        return 1;
    }

    const auto cachedStart = Clock::now();
    std::size_t cachedLayers = 0;
    for (std::int64_t frame = 0; frame < kQueries; ++frame)
        cachedLayers += mvm::app::mapTimelinePreviewFrame(project, plan, frame).layers.size();
    const auto cachedEnd = Clock::now();

    const auto rebuiltStart = Clock::now();
    std::size_t rebuiltLayers = 0;
    bool same = true;
    for (std::int64_t frame = 0; frame < kQueries; ++frame) {
        const auto rebuilt = mvm::app::mapTimelinePreviewFrame(project, frame);
        rebuiltLayers += rebuilt.layers.size();
        same = same && sameLayers(rebuilt, mvm::app::mapTimelinePreviewFrame(project, plan, frame));
    }
    const auto rebuiltEnd = Clock::now();
    if (!same || cachedLayers != rebuiltLayers || cachedLayers == 0) {
        std::fprintf(stderr, "FAIL: 区間を使い回した結果が作り直した結果と一致しません\n");
        return 1;
    }
    const auto micros = [](Clock::time_point begin, Clock::time_point end) {
        return std::chrono::duration<double, std::micro>(end - begin).count();
    };
    std::printf("{\"clips\": %d, \"queries\": %lld, \"plan_build_us\": %.1f, "
                "\"cached_us_per_frame\": %.3f, \"rebuilt_us_per_frame\": %.3f}\n",
                kClips, static_cast<long long>(kQueries), micros(buildStart, buildEnd),
                micros(cachedStart, cachedEnd) / kQueries,
                micros(rebuiltStart, rebuiltEnd) / kQueries);
    return 0;
}
