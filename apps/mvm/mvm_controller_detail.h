#pragma once

#include "media_import.h"
#include "preview_engine/preview_engine.h"

#include <QString>

// controller の翻訳単位だけで共有する補助関数。状態は所有しない。
namespace mvm::app::detail {

struct ProbedMedia {
    bool success = false;
    int width = 0;
    int height = 0;
    std::int64_t fpsNum = 0;
    std::int64_t fpsDen = 1;
    int sarNum = 1;
    int sarDen = 1;
    std::int64_t frameCount = 0;
    bool hasAudio = false;
    QString error;
};

QString fromPath(const std::filesystem::path& path);
QString previewErrorText(const preview::PreviewError& error);
ProbedMedia videoFacts(const MediaImportResult& probed);
ProbedMedia probeMedia(const std::filesystem::path& path);
std::string newClipId();
double linearToDb(float linear, double silenceDb);
int indexOfClipId(const std::vector<project::TimelineClip>& clips, const std::string& clipId);

} // namespace mvm::app::detail
