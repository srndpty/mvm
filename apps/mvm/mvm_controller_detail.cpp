#include "mvm_controller_detail.h"

#include <cmath>
#include <QUuid>

namespace mvm::app::detail {

double linearToDb(float linear, double silenceDb) {
    if (!(linear > 0.0F))
        return silenceDb;
    const double db = 20.0 * std::log10(static_cast<double>(linear));
    return db < silenceDb ? silenceDb : db;
}

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString previewErrorText(const preview::PreviewError& error) {
    return QString::fromStdString(error.detail);
}

// 素材の種別は probeMediaFile (media_import) だけが決める。ここでは用途に合うかを見る。
ProbedMedia videoFacts(const MediaImportResult& probed) {
    ProbedMedia result;
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    if (probed.item.kind != project::MediaKind::Video) {
        result.error = probed.item.kind == project::MediaKind::Image
                           ? QStringLiteral("静止画は動画として追加できません")
                           : QStringLiteral("映像の無い素材は動画として追加できません");
        return result;
    }
    result.width = probed.item.width;
    result.height = probed.item.height;
    result.fpsNum = probed.item.fpsNum;
    result.fpsDen = probed.item.fpsDen;
    result.sarNum = probed.sarNum;
    result.sarDen = probed.sarDen;
    result.frameCount = probed.item.frameCount;
    result.hasAudio = probed.hasAudio;
    result.success = true;
    return result;
}

ProbedMedia probeMedia(const std::filesystem::path& path) {
    return videoFacts(probeMediaFile(path));
}

std::string newClipId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

int indexOfClipId(const std::vector<project::TimelineClip>& clips, const std::string& clipId) {
    for (std::size_t index = 0; index < clips.size(); ++index)
        if (clips[index].id == clipId)
            return static_cast<int>(index);
    return -1;
}

} // namespace mvm::app::detail
