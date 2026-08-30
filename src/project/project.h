#ifndef MVM_PROJECT_PROJECT_H
#define MVM_PROJECT_PROJECT_H

#include <filesystem>
#include <string>
#include <vector>

namespace mvm::project {

enum class ManimGenerationState { NotGenerated, Ready, SourceChanged, GenerationFailed };

struct ManimAsset {
    std::filesystem::path scriptPath;
    std::string sceneName;
    std::filesystem::path generatedVideoPath;
    ManimGenerationState generationState = ManimGenerationState::NotGenerated;
    std::string sourceFingerprint;
};

struct Project {
    int schemaVersion = 1;
    std::vector<ManimAsset> manimAssets;
};

struct ManimAssetResult {
    bool success = false;
    ManimAsset asset;
    std::string error;
};

ManimAssetResult createReadyManimAsset(std::filesystem::path scriptPath, std::string sceneName,
                                       std::filesystem::path generatedVideoPath,
                                       std::string sourceFingerprint);

// Ready / SourceChanged の asset を現在の fingerprint で再評価する。
// NotGenerated / GenerationFailed は render 操作まで現在の state を維持する。
bool refreshManimGenerationState(ManimAsset& asset, const std::string& currentFingerprint,
                                 std::string& error);

const char* manimGenerationStateName(ManimGenerationState state);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_H
