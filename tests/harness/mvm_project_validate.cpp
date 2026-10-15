// 外部の教材作成ツールも、製品と同じProject検証を使う。検査条件を複製しない。
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
std::filesystem::path fromUtf8(const char* value) {
    wchar_t* wide = mvm_utf8_to_wide(value);
    const std::filesystem::path path = wide ? wide : L"";
    mvm_str_free(wide);
    return path;
}
} // namespace

int main() {
    mvm_enable_utf8_console();
    int count = 0;
    char** args = mvm_win_get_utf8_args(&count);
    if (!args || (count != 2 && count != 4) ||
        (count == 4 && std::string(args[2]) != "--roundtrip")) {
        std::fprintf(stderr, "使い方: mvm_project_validate <Project> [--roundtrip <保存先>]\n");
        mvm_win_free_utf8_args(args, count);
        return 2;
    }
    const auto source = fromUtf8(args[1]);
    const auto destination = count == 4 ? fromUtf8(args[3]) : std::filesystem::path{};
    mvm_win_free_utf8_args(args, count);
    auto loaded = mvm::project::loadProjectJson(source);
    if (!loaded.success) {
        std::fprintf(stderr, "Projectを読み込めません: %s\n", loaded.error.c_str());
        return 1;
    }
    const auto valid = mvm::project::validateTimeline(loaded.project);
    if (!valid.success) {
        std::fprintf(stderr, "Projectの検証に失敗しました: %s\n", valid.error.c_str());
        return 1;
    }
    if (!destination.empty()) {
        std::error_code error;
        if (std::filesystem::equivalent(source, destination, error)) {
            std::fprintf(stderr, "再読込の検査で入力Projectを上書きできません\n");
            return 2;
        }
        const auto saved = mvm::project::saveProjectJson(loaded.project, destination);
        if (!saved.success) {
            std::fprintf(stderr, "検証用のProjectを保存できません: %s\n", saved.error.c_str());
            return 1;
        }
        const auto again = mvm::project::loadProjectJson(destination);
        if (!again.success || !(again.project == loaded.project)) {
            std::fprintf(stderr, "保存・再読込の内容が一致しません\n");
            return 1;
        }
    }
    std::printf("Projectの検証を通過しました: clip=%zu、字幕=%zu、停止点=%zu\n",
                loaded.project.timelineClips.size(),
                loaded.project.subtitles ? loaded.project.subtitles->cues.size() : 0,
                loaded.project.timelineMarkers.size());
    return 0;
}
