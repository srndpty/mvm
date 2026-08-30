#include "project/project.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace mvm::project {
namespace {

bool isSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) || (character >= 'a' && character <= 'f');
           });
}

} // namespace

ManimAssetResult createReadyManimAsset(std::filesystem::path scriptPath, std::string sceneName,
                                       std::filesystem::path generatedVideoPath,
                                       std::string sourceFingerprint) {
    ManimAssetResult result;
    if (scriptPath.empty()) {
        result.error = "Manim script path が空です";
        return result;
    }
    if (sceneName.empty()) {
        result.error = "Manim Scene 名が空です";
        return result;
    }
    if (generatedVideoPath.empty()) {
        result.error = "生成済み Manim video path が空です";
        return result;
    }
    if (!isSha256(sourceFingerprint)) {
        result.error = "Manim source fingerprint が小文字64桁の SHA-256 ではありません";
        return result;
    }

    result.asset = {
        .scriptPath = std::move(scriptPath),
        .sceneName = std::move(sceneName),
        .generatedVideoPath = std::move(generatedVideoPath),
        .generationState = ManimGenerationState::Ready,
        .sourceFingerprint = std::move(sourceFingerprint),
    };
    result.success = true;
    return result;
}

bool refreshManimGenerationState(ManimAsset& asset, const std::string& currentFingerprint,
                                 std::string& error) {
    error.clear();
    if (asset.generationState == ManimGenerationState::NotGenerated ||
        asset.generationState == ManimGenerationState::GenerationFailed) {
        return true;
    }
    if (!isSha256(asset.sourceFingerprint) || !isSha256(currentFingerprint)) {
        error = "比較する Manim source fingerprint が正しい SHA-256 ではありません";
        return false;
    }
    asset.generationState = asset.sourceFingerprint == currentFingerprint
                                ? ManimGenerationState::Ready
                                : ManimGenerationState::SourceChanged;
    return true;
}

const char* timelineClipKindName(TimelineClipKind kind) {
    switch (kind) {
    case TimelineClipKind::Video:
        return "video";
    case TimelineClipKind::Manim:
        return "manim";
    }
    return "";
}

const char* manimGenerationStateName(ManimGenerationState state) {
    switch (state) {
    case ManimGenerationState::NotGenerated:
        return "NotGenerated";
    case ManimGenerationState::Ready:
        return "Ready";
    case ManimGenerationState::SourceChanged:
        return "SourceChanged";
    case ManimGenerationState::GenerationFailed:
        return "GenerationFailed";
    }
    return "";
}

} // namespace mvm::project
