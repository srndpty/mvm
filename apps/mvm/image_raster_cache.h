#ifndef MVM_APPS_MVM_IMAGE_RASTER_CACHE_H
#define MVM_APPS_MVM_IMAGE_RASTER_CACHE_H

#include "media_source_identity.h"
#include "preview_engine/preview_types.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace mvm::app {

// 画像 clip の preview 用 raster (素材を decode し、出力解像度の raster へ配置したもの) を持つ。
// 書き出しと同じ静止画 decoder (media/still_image) を通すので、同じ画素になる。
//
// WaveformCache と同じ規則で作る。
//   - decode と配置は worker thread で行い、GUI thread は完成した raster を受け取るだけにする。
//     大きな画像でも初回表示で GUI が止まらない
//   - key は素材の実体 (volume + file ID) と出力解像度。request のたびに size と更新時刻を
//     照合し、差し替えられていれば作り直す。size と更新時刻が同じまま中身だけ変わったものは
//     revalidateAll() の内容 fingerprint で見つける
//   - retainOnly() で、現在の Project が使わない key (clip が消えた・出力解像度が変わった)
//     を捨てる
//   - byte budget を超えたら、engine も controller も参照していないものから古い順に捨てる。
//     参照中の raster は捨てられないので、参照中のものだけで budget を超えている間は超えた
//     ままになる。参照が外れた時点 (request が返した image の最後の copy が破棄された時点)
//     で予算を再評価するので、参照されていない raster が budget を超えて残ることはない
class ImageRasterCache : public QObject {
    Q_OBJECT

public:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State state = State::Loading;
        std::shared_ptr<const preview::PreviewStillImage> image;
        QString error;
    };

    // path の素材を decode し、width x height の raster へ置く。worker thread で呼ばれる。
    using RasterFunction =
        std::function<Entry(const std::filesystem::path& path, int width, int height)>;

    // 1080p の raster は約 8MiB、4K は約 32MiB。
    static constexpr std::size_t kDefaultBudgetBytes = std::size_t{512} * 1024 * 1024;

    explicit ImageRasterCache(QObject* parent = nullptr);
    // raster を省略すると静止画 decoder を使う。差し替えはテスト用。
    ImageRasterCache(std::size_t budgetBytes, RasterFunction raster, QObject* parent = nullptr);
    // 生成中の job を中断し、終わるまで待つ。job は this を参照するため。
    ~ImageRasterCache() override;

    // 未要求か、前回から素材が差し替えられていれば生成を開始する。現在の状態を返す。
    Entry request(const std::filesystem::path& path, int width, int height);

    // request と同じ key。retainOnly に渡す集合を作るのに使う。
    static QString keyFor(const std::filesystem::path& path, int width, int height);

    // keys に無い record を捨てる (生成中なら中断する)。
    void retainOnly(const QSet<QString>& keys);

    // 生成済みの素材の内容 fingerprint を worker で照合し、中身が変わったものを捨てて
    // entryChanged を出す。size / 更新時刻の変化は次の request で見つかるので、
    // 呼び出し側に request し直させるため entryChanged(空) も出す。
    // アプリが前面へ戻ったときに呼ぶ。
    void revalidateAll();

    // byte budget を超えていれば、参照されていない raster を古い順に捨てる。
    // request が返した image の参照が外れるたびに queued で呼ばれる。
    void trimToBudget();

    // 診断・テスト用。
    std::size_t readyBytes() const;
    int entryCount() const { return static_cast<int>(records_.size()); }

Q_SIGNALS:
    // 生成が終わった (key)、または素材が変わったので request し直してほしい (空なら全部)。
    void entryChanged(const QString& key);

private:
    // request が渡した image の参照が外れたことを cache へ知らせる窓口。image の破棄は
    // engine の render thread でも起こるので、cache の破棄と mutex で排他する。
    struct ReleaseHub {
        std::mutex mutex;
        ImageRasterCache* cache = nullptr;
    };

    struct Record {
        // entry.image は cache が持つ本体。request はこれを直接渡さず、参照が外れたときに
        // trimToBudget を呼ぶ handle で包んで渡す (handOut)。
        Entry entry;
        MediaSourceIdentity identity;
        std::filesystem::path path;
        std::optional<std::uint64_t> fingerprint;
        std::uint64_t ticket = 0;
        std::uint64_t lastUse = 0;
        std::size_t bytes = 0;
        // 一度も request で渡していない raster は、controller が受け取る前なので捨てない。
        bool delivered = false;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    Entry handOut(Record& record);

    void finish(const QString& key, std::uint64_t ticket, std::optional<std::uint64_t> fingerprint,
                Entry entry);
    void dropStale(const QList<QPair<QString, quint64>>& stale);

    std::size_t budgetBytes_;
    RasterFunction raster_;
    QThreadPool pool_;
    QHash<QString, Record> records_;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t useClock_ = 0;
    std::shared_ptr<std::atomic<bool>> shuttingDown_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<ReleaseHub> releaseHub_ = std::make_shared<ReleaseHub>();
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_IMAGE_RASTER_CACHE_H
