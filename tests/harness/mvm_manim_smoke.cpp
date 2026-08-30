#include "media/manim/manim_renderer.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string toUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
    return result;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 4) {
        std::fprintf(stderr, "使い方: mvm_manim_smoke <manim.exe> <sample.py> <output-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    mvm::manim::ManimRenderRequest request{
        .manimExecutablePath = std::filesystem::absolute(fromUtf8(argv[1])),
        .scriptPath = std::filesystem::absolute(fromUtf8(argv[2])),
        .sceneName = "MvmM0Scene",
        .outputDirectory = std::filesystem::absolute(fromUtf8(argv[3])),
        .width = 320,
        .height = 180,
        .fps = 15,
    };
    const auto render = mvm::manim::renderManim(request);
    if (!render.success) {
        std::fprintf(stderr, "Manim render に失敗しました (exit=%d)\n%s\n%s", render.exitCode,
                     render.stdoutText.c_str(), render.stderrText.c_str());
        mvm_win_free_utf8_args(argv, argc);
        return 1;
    }

    const std::string videoPath = toUtf8(render.outputVideoPath);
    std::printf("MANIM_OUTPUT=%s\n", videoPath.c_str());
    std::printf("MANIM_EXIT=%d\n", render.exitCode);

    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT runtime を初期化できませんでした\n");
        mvm_win_free_utf8_args(argv, argc);
        return 1;
    }

    MvmMltProbeResult probe{};
    const int probeCode = mvm_mlt_probe_file(videoPath.c_str(), &probe);
    if (probeCode != 0 || !probe.ok || !probe.has_video || probe.width != 320 ||
        probe.height != 180 || probe.fps_num != 15 || probe.fps_den != 1) {
        std::fprintf(
            stderr, "MLT probe が期待値と一致しません: error=%s video=%d size=%dx%d fps=%d/%d\n",
            probe.error, probe.has_video, probe.width, probe.height, probe.fps_num, probe.fps_den);
        mvm_mlt_runtime_shutdown();
        mvm_win_free_utf8_args(argv, argc);
        return 3;
    }
    std::printf("MLT_PROBE=video,320x180,15/1\n");

    MvmMltImage frame{};
    char decodeError[512]{};
    if (mvm_mlt_decode_frame(videoPath.c_str(), 0, &frame, decodeError, sizeof(decodeError)) != 0) {
        std::fprintf(stderr, "frame 0 を decode できません: %s\n", decodeError);
        mvm_mlt_runtime_shutdown();
        mvm_win_free_utf8_args(argv, argc);
        return 3;
    }
    std::printf("MLT_DECODE_FRAME0=ok,%dx%d\n", frame.width, frame.height);
    mvm_mlt_image_free(&frame);
    mvm_mlt_runtime_shutdown();
    mvm_win_free_utf8_args(argv, argc);
    return 0;
}
