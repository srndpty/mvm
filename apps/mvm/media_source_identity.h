#ifndef MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H
#define MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H

#include "util/mvm_file_identity.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace mvm::app {

// 素材から作った cache (波形・画像の raster) が、素材の差し替えを見逃さないための同一性。
// WaveformCache と ImageRasterCache が共有する。
struct MediaSourceIdentity {
    bool exists = false;
    qint64 size = -1;
    // FILETIME (100ns)。ms へ丸めると同じ size の素早い差し替えを見逃す。
    qint64 lastWriteTime = -1;
    bool operator==(const MediaSourceIdentity&) const = default;
};

struct MediaSourceProbe {
    QString key;
    MediaSourceIdentity identity;
};

// 実在する file は実体 (volume + file ID) で区別する。hard link / junction 経由の別 path は
// 同じ素材、case-sensitive directory の別名 file は別素材になる。実体が取れない (存在しない等)
// ときだけ path の字面で区別する。大文字小文字は畳まない (区別するかは directory ごとに違う)。
inline MediaSourceProbe probeMediaSource(const QString& mediaPath) {
    MediaSourceProbe result;
    if (mediaPath.isEmpty())
        return result;
    MvmFileIdentity identity{};
    const std::wstring widePath = mediaPath.toStdWString();
    if (mvm_file_identity_query(widePath.c_str(), &identity) == 0) {
        const QByteArray fileId(reinterpret_cast<const char*>(identity.file_id),
                                sizeof(identity.file_id));
        result.key = QStringLiteral("file:%1:%2")
                         .arg(identity.volume_serial, 16, 16, QLatin1Char('0'))
                         .arg(QString::fromLatin1(fileId.toHex()));
        result.identity = {true, identity.size, identity.last_write_time};
        return result;
    }
    result.key = QStringLiteral("path:") + QDir::cleanPath(QFileInfo(mediaPath).absoluteFilePath());
    return result;
}

// 内容の fingerprint。size と更新時刻が一致したまま中身だけ差し替えられた素材を見つける。
// file を読むので GUI thread では呼ばない。取れなければ nullopt。
inline std::optional<std::uint64_t> mediaContentFingerprint(const QString& mediaPath) {
    unsigned long long fingerprint = 0;
    const std::wstring widePath = mediaPath.toStdWString();
    if (mvm_file_content_fingerprint(widePath.c_str(), &fingerprint) != 0)
        return std::nullopt;
    return fingerprint;
}

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H
