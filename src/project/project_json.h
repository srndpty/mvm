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

struct ProjectSerializationResult {
    bool success = false;
    std::string json;
    std::string error;
};

ProjectSerializationResult serializeProjectJson(const Project& project,
                                                const std::filesystem::path& projectPath);
// 同じdirectoryの一時fileへ書いてから置換する。途中まで書いたcanonicalを残さない。
ProjectIoResult saveProjectJson(const Project& project, const std::filesystem::path& projectPath);
// candidate の保存に成功した場合だけ liveProject を差し替える。
ProjectIoResult saveProjectJsonTransaction(Project& liveProject, Project candidate,
                                           const std::filesystem::path& projectPath);
ProjectLoadResult loadProjectJson(const std::filesystem::path& projectPath);
ProjectLoadResult parseProjectJsonText(const std::string& jsonText,
                                       const std::filesystem::path& projectPath);

// crash recovery。canonicalそのものではなく、autosave時点の照合情報を持つ。
struct ProjectRecoveryLoadResult {
    bool success = false;
    bool legacy = false;
    std::string canonicalSha256;
    std::string savedAt;
    std::string sessionId;
    std::string canonicalPath;
    Project project;
    std::string error;
};

enum class RecoveryDisposition { Stale, Restorable, CanonicalChanged };

ProjectIoResult saveProjectRecovery(const Project& project,
                                    const std::filesystem::path& recoveryPath,
                                    const std::filesystem::path& canonicalPath,
                                    const std::string& canonicalSha256, const std::string& savedAt,
                                    const std::string& sessionId);
ProjectRecoveryLoadResult loadProjectRecovery(const std::filesystem::path& recoveryPath,
                                              const std::filesystem::path& canonicalPath);
// 内容が同一ならStale。記録したhashと現在のcanonicalが一致するときだけRestorable。
// hashが空なのは、canonical file自体が無い場合だけRestorableにする。
RecoveryDisposition classifyRecovery(const Project& recoveryProject,
                                     const Project& canonicalProject,
                                     const std::string& recordedHash,
                                     const std::string& currentHash);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_JSON_H
