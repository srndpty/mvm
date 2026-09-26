#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string toUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--full")) {
        std::fprintf(stderr, "使い方: mvm_test_mpg_export <source.mpg> <output.mp4> [--full]\n");
        return 2;
    }
    const std::filesystem::path source = std::filesystem::absolute(argv[1]);
    const std::filesystem::path output = std::filesystem::absolute(argv[2]);
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません\n");
        return 3;
    }

    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(toUtf8(source).c_str(), &probe) != 0 || !probe.ok || !probe.has_video ||
        !probe.has_audio || probe.frame_count <= 0) {
        std::fprintf(stderr, "映像・音声を持つMPGを解析できません: %s\n", probe.error);
        mvm_mlt_runtime_shutdown();
        return 4;
    }

    mvm::project::Project project = mvm::project::createDefaultProject();
    const std::int64_t frameCount =
        argc == 4 ? probe.frame_count : std::min<std::int64_t>(probe.frame_count, 60);
    mvm::project::TimelineClip video;
    video.id = "mpg-video";
    video.name = "mpg-video";
    video.mediaPath = source;
    video.sourceFpsNum = probe.fps_num;
    video.sourceFpsDen = probe.fps_den;
    video.sourceFrameCount = probe.frame_count;
    video.sourceInFrame = 0;
    video.sourceOutFrame = frameCount;
    video.track = {mvm::project::TrackKind::Video, 0};
    video.linkGroupId = "mpg-pair";
    auto audio = video;
    audio.id = "mpg-audio";
    audio.name = "mpg-audio";
    audio.kind = mvm::project::TimelineClipKind::Audio;
    audio.track = {mvm::project::TrackKind::Audio, 0};
    // videoとstart/trimが一致しなくても、独立audio trackとして書き出せることを検査する。
    if (frameCount > 2) {
        audio.sourceInFrame = 1;
        audio.timelineStartFrame = 1;
    }
    project.timelineClips = {video, audio};

    mvm::app::TimelineExportRequest request;
    request.outputPath = output;
    request.width = 320;
    request.height = 240;
    request.fpsNum = static_cast<int>(project.timelineFpsNum);
    request.fpsDen = static_cast<int>(project.timelineFpsDen);
    const auto exported = mvm::app::exportTimeline(project, request);
    if (!exported.success) {
        std::fprintf(stderr, "MPGを書き出せません: %s\n", exported.error.c_str());
        mvm_mlt_runtime_shutdown();
        return 5;
    }

    MvmMltProbeResult outputProbe{};
    const bool validOutput = mvm_mlt_probe_file(toUtf8(output).c_str(), &outputProbe) == 0 &&
                             outputProbe.ok && outputProbe.has_video && outputProbe.has_audio &&
                             outputProbe.frame_count > 0;
    if (!validOutput) {
        mvm_mlt_runtime_shutdown();
        std::fprintf(stderr, "書き出し結果に映像・音声がありません: %s\n", outputProbe.error);
        return 6;
    }

    // linked audio clipを削除したtimelineでは、video producer内蔵の音声を復活させない。
    // sequential fast pathとtractor経路の両方で検査する。
    // Projectはlinked audio削除時に相手側のlinkも外すため、fixtureも同じ状態にする。
    auto unlinkedVideo = video;
    unlinkedVideo.linkGroupId.clear();
    // V1先頭にgapを置くとaudio clipなしでもtractor経路になる。
    auto gappedVideo = unlinkedVideo;
    gappedVideo.timelineStartFrame = 10;
    // MPG(非timeline fps)をV2へ置く。最終素材frameが複数timeline frameに跨っても、
    // opacity端keyがclip末尾へ届いて書き出せることも同時に踏む。
    auto overlay = unlinkedVideo;
    overlay.id = "mpg-overlay";
    overlay.name = "mpg-overlay";
    overlay.track = {mvm::project::TrackKind::Video, 1};
    overlay.effects.fadeOutFrames = 3;

    struct VideoOnlyCase {
        const char* name;
        std::vector<mvm::project::TimelineClip> clips;
        mvm::app::TimelineExportResult::Backend backend;
    };

    const VideoOnlyCase videoOnlyCases[] = {
        {"sequential", {unlinkedVideo}, mvm::app::TimelineExportResult::Backend::Sequential},
        {"tractor", {gappedVideo}, mvm::app::TimelineExportResult::Backend::Tractor},
        {"overlay", {unlinkedVideo, overlay}, mvm::app::TimelineExportResult::Backend::Tractor},
    };
    int result = 0;
    for (const auto& videoOnly : videoOnlyCases) {
        project.timelineClips = videoOnly.clips;
        request.outputPath = output;
        request.outputPath.replace_filename(output.stem().wstring() + L"-video-only-" +
                                            std::filesystem::path(videoOnly.name).wstring() +
                                            L".mp4");
        const auto videoOnlyExported = mvm::app::exportTimeline(project, request);
        MvmMltProbeResult videoOnlyProbe{};
        const bool probed =
            videoOnlyExported.success &&
            mvm_mlt_probe_file(toUtf8(request.outputPath).c_str(), &videoOnlyProbe) == 0 &&
            videoOnlyProbe.ok;
        if (!probed || videoOnlyExported.backend != videoOnly.backend ||
            !videoOnlyProbe.has_video || videoOnlyProbe.has_audio) {
            std::fprintf(stderr,
                         "%s: video-only timelineから素材内蔵音声が出力された、"
                         "または書き出せません: %s\n",
                         videoOnly.name, videoOnlyExported.error.c_str());
            result = 7;
        }
    }
    mvm_mlt_runtime_shutdown();
    return result;
}
