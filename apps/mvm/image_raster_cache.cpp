#include "image_raster_cache.h"

#include "media/still_image/still_image_decoder.h"

#include <QMetaObject>

#include <limits>
#include <vector>

namespace mvm::app {
namespace {

QString pathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

ImageRasterCache::Entry decodeRaster(const std::filesystem::path& path, int width, int height) {
    ImageRasterCache::Entry entry;
    const auto decoded = media::decodeStillImage(path);
    if (!decoded.success) {
        entry.state = ImageRasterCache::State::Failed;
        entry.error = QString::fromStdString(decoded.error);
        return entry;
    }
    auto fitted = media::fitStillImageToRaster(decoded.image, width, height);
    if (!fitted.success) {
        entry.state = ImageRasterCache::State::Failed;
        entry.error = QString::fromStdString(fitted.error);
        return entry;
    }
    auto image = std::make_shared<preview::PreviewStillImage>();
    image->width = fitted.raster.width;
    image->height = fitted.raster.height;
    image->rgba = std::move(fitted.raster.rgba);
    entry.state = ImageRasterCache::State::Ready;
    entry.image = std::move(image);
    return entry;
}

} // namespace

ImageRasterCache::ImageRasterCache(QObject* parent)
    : ImageRasterCache(kDefaultBudgetBytes, {}, parent) {}

ImageRasterCache::ImageRasterCache(std::size_t budgetBytes, RasterFunction raster, QObject* parent)
    : QObject(parent), budgetBytes_(budgetBytes), raster_(std::move(raster)) {
    if (!raster_)
        raster_ = decodeRaster;
    // 画像を一度に大量に置いても、decode が preview の CPU を食い尽くさないようにする。
    pool_.setMaxThreadCount(2);
}

ImageRasterCache::~ImageRasterCache() {
    shuttingDown_->store(true, std::memory_order_relaxed);
    for (const auto& record : std::as_const(records_)) {
        if (record.cancel)
            record.cancel->store(true, std::memory_order_relaxed);
    }
    pool_.clear();
    pool_.waitForDone();
}

QString ImageRasterCache::keyFor(const std::filesystem::path& path, int width, int height) {
    return probeMediaSource(pathText(path)).key + QStringLiteral("|%1x%2").arg(width).arg(height);
}

ImageRasterCache::Entry ImageRasterCache::request(const std::filesystem::path& path, int width,
                                                  int height) {
    if (path.empty() || width <= 0 || height <= 0)
        return {State::Failed, {}, QStringLiteral("画像の path または出力解像度が不正です")};
    const MediaSourceProbe source = probeMediaSource(pathText(path));
    const QString key = source.key + QStringLiteral("|%1x%2").arg(width).arg(height);
    auto found = records_.find(key);
    if (found != records_.end() && found->identity == source.identity) {
        found->lastUse = ++useClock_;
        return found->entry;
    }

    // 前回と素材が違う (差し替えられた)。生成中の job は結果ごと捨てる (ticket で見分ける)。
    if (found != records_.end() && found->cancel)
        found->cancel->store(true, std::memory_order_relaxed);
    Record record;
    record.identity = source.identity;
    record.path = path;
    record.ticket = nextTicket_++;
    record.lastUse = ++useClock_;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    records_.insert(key, record);

    pool_.start([this, key, ticket = record.ticket, cancel = record.cancel, path, width, height] {
        // decode より前に取る。decode 中に差し替えられても、次の再検証で必ず不一致になる。
        const auto fingerprint = mediaContentFingerprint(pathText(path));
        Entry entry = raster_(path, width, height);
        if (cancel->load(std::memory_order_relaxed))
            return;
        // this は destructor が waitForDone するまで生きている。queued event は
        // this の破棄時に Qt が捨てるので、完了通知が破棄後に届くことはない。
        QMetaObject::invokeMethod(
            this,
            [this, key, ticket, fingerprint, entry = std::move(entry)]() mutable {
                finish(key, ticket, fingerprint, std::move(entry));
            },
            Qt::QueuedConnection);
    });
    return record.entry;
}

void ImageRasterCache::finish(const QString& key, std::uint64_t ticket,
                              std::optional<std::uint64_t> fingerprint, Entry entry) {
    auto found = records_.find(key);
    // 生成中に素材が差し替えられた / 捨てられた場合、古い job の結果で上書きしない。
    if (found == records_.end() || found->ticket != ticket)
        return;
    if (entry.state == State::Failed)
        qWarning("画像を読めません: %s: %s", qUtf8Printable(pathText(found->path)),
                 qUtf8Printable(entry.error));
    found->bytes = entry.image ? entry.image->rgba.size() : 0;
    found->entry = std::move(entry);
    found->fingerprint = fingerprint;
    found->cancel.reset();
    evictUnused(key);
    Q_EMIT entryChanged(key);
}

void ImageRasterCache::retainOnly(const QSet<QString>& keys) {
    for (auto it = records_.begin(); it != records_.end();) {
        if (keys.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true, std::memory_order_relaxed);
        it = records_.erase(it);
    }
}

void ImageRasterCache::revalidateAll() {
    Q_EMIT entryChanged(QString());
    struct Target {
        QString key;
        std::uint64_t ticket = 0;
        QString path;
        std::uint64_t fingerprint = 0;
    };
    std::vector<Target> targets;
    for (auto it = records_.cbegin(); it != records_.cend(); ++it) {
        if (!it->cancel && it->fingerprint)
            targets.push_back({it.key(), it->ticket, pathText(it->path), *it->fingerprint});
    }
    if (targets.empty())
        return;
    pool_.start([this, targets = std::move(targets), shuttingDown = shuttingDown_] {
        QList<QPair<QString, quint64>> stale;
        for (const auto& target : targets) {
            if (shuttingDown->load(std::memory_order_relaxed))
                return;
            const auto current = mediaContentFingerprint(target.path);
            if (!current || *current != target.fingerprint)
                stale.append({target.key, target.ticket});
        }
        if (stale.isEmpty())
            return;
        QMetaObject::invokeMethod(
            this, [this, stale] { dropStale(stale); }, Qt::QueuedConnection);
    });
}

void ImageRasterCache::dropStale(const QList<QPair<QString, quint64>>& stale) {
    for (const auto& [key, ticket] : stale) {
        const auto found = records_.find(key);
        // 検査中に作り直しが始まった record は、その結果を待つ。
        if (found == records_.end() || found->ticket != ticket)
            continue;
        records_.erase(found);
        Q_EMIT entryChanged(key);
    }
}

std::size_t ImageRasterCache::readyBytes() const {
    std::size_t total = 0;
    for (const auto& record : records_)
        total += record.bytes;
    return total;
}

void ImageRasterCache::evictUnused(const QString& keep) {
    std::size_t total = readyBytes();
    while (total > budgetBytes_) {
        // cache だけが raster を持っている (engine の composition も controller も参照して
        // いない) ものから古い順に捨てる。完成したばかりの record はまだ誰も受け取っていないので残す。
        auto victim = records_.end();
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            if (it.key() == keep || !it->entry.image || it->entry.image.use_count() != 1)
                continue;
            if (it->lastUse < oldest) {
                oldest = it->lastUse;
                victim = it;
            }
        }
        if (victim == records_.end())
            return;
        total -= victim->bytes;
        records_.erase(victim);
    }
}

} // namespace mvm::app
