#ifndef MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H
#define MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H

#include "util/mvm_file_identity.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>

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

// 内容の標本 fingerprint (先頭・末尾・size)。size と更新時刻が一致したまま中身だけ
// 差し替えられた素材を、file 全体を読まずに見つける。中央部だけの変更は取りこぼす
// ので、検出は保証しない。file を読むので GUI thread では呼ばない。取れなければ nullopt。
inline std::optional<std::uint64_t> mediaContentFingerprint(const QString& mediaPath) {
    unsigned long long fingerprint = 0;
    const std::wstring widePath = mediaPath.toStdWString();
    if (mvm_file_content_fingerprint(widePath.c_str(), &fingerprint) != 0)
        return std::nullopt;
    return fingerprint;
}

// 内容全体の hash。どの位置の変更も値に反映されるので、差し替えを必ず検出したい cache が
// 使う。file 全体を読むので GUI thread では呼ばない。取れなければ nullopt。
inline std::optional<std::uint64_t> mediaContentHash(const QString& mediaPath) {
    unsigned long long hash = 0;
    const std::wstring widePath = mediaPath.toStdWString();
    if (mvm_file_content_hash(widePath.c_str(), &hash) != 0)
        return std::nullopt;
    return hash;
}

// 途中で止められる内容全体の hash。shouldStop は 1 MiB を読むたびに、読む前に呼ぶ。
// 止めたこと (Cancelled) と読めなかったこと (Unavailable) を区別する。
struct MediaContentHashResult {
    enum class Status { Ok, Cancelled, Unavailable } status = Status::Unavailable;
    std::uint64_t hash = 0;
};

inline MediaContentHashResult mediaContentHash(const QString& mediaPath,
                                               const std::function<bool()>& shouldStop) {
    MediaContentHashResult result;
    unsigned long long hash = 0;
    const std::wstring widePath = mediaPath.toStdWString();
    const auto status = mvm_file_content_hash_cancellable(
        widePath.c_str(), &hash,
        [](void* opaque) -> int {
            const auto& stop = *static_cast<const std::function<bool()>*>(opaque);
            return stop && stop() ? 1 : 0;
        },
        const_cast<std::function<bool()>*>(&shouldStop));
    result.status = status == MVM_FILE_HASH_OK ? MediaContentHashResult::Status::Ok
                    : status == MVM_FILE_HASH_CANCELLED
                        ? MediaContentHashResult::Status::Cancelled
                        : MediaContentHashResult::Status::Unavailable;
    result.hash = hash;
    return result;
}

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_SOURCE_IDENTITY_H
