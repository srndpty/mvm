#ifndef MVM_PROJECT_PROJECT_JSON_H
#define MVM_PROJECT_PROJECT_JSON_H

#include "project/project.h"

#include <filesystem>
#include <string>

namespace mvm::project {

struct ProjectIoResult {
    bool success = false;
    std::string error;
};

struct ProjectLoadResult {
    bool success = false;
    Project project;
    std::string error;
};

ProjectIoResult saveProjectJson(const Project& project, const std::filesystem::path& projectPath);
// candidate の保存に成功した場合だけ liveProject を差し替える。
// ファイル置換自体を atomic にする API ではない。
ProjectIoResult saveProjectJsonTransaction(Project& liveProject, Project candidate,
                                           const std::filesystem::path& projectPath);
ProjectLoadResult loadProjectJson(const std::filesystem::path& projectPath);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_JSON_H
