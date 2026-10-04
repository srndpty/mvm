#ifndef MVM_APPS_MVM_MATH_RASTER_CACHE_H
#define MVM_APPS_MVM_MATH_RASTER_CACHE_H

#include "media/math/math_render.h"
#include "media/still_image/still_image_decoder.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mvm::app {

// Write の連番の mask。各 frame は 1 画素 1 byte の被覆 (PNG の alpha)、width * height byte。
// preview は RGBA ではなくこの形で持つ (出力全面の RGBA を frame ごとに持たない)。
struct MathCoverageSequence {
    int width = 0;
    int height = 0;
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t bytes() const {
        return frames.size() * static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

// 書き出しが読む連番の artifact (cache の PNG、frame 0 から順)。
struct MathSequenceArtifact {
    std::vector<std::filesystem::path> frames;
    int width = 0;
    int height = 0;
};

// 数式 clip の描画結果 (白い glyph の mask) を key 単位で持つ (docs/math-clips.md)。
//
// - key は描画に効く値 (MathRenderSpec) と backend の toolchain fingerprint の SHA-256。
//   key 単位なので、古い要求の結果が後から届いても、その key の正しい結果として残るだけで、
//   新しい編集の結果を上書きしない
// - 描いた PNG は cacheDirectory/<key>.png と provenance の <key>.txt に置く。Project には書かない。
//   次に開いたときは描かずに読む (provenance と画像の大きさを照合し、合わなければ描き直す)
// - preflight (backend の確認) と描画は worker 1 本で順に行う。Manim / LaTeX は重く、
//   同時に走らせない。要求されなくなった key の描画は process ごと止める
// - 失敗した key は覚えておき、自動では描き直さない (forgetFailures で解除する)
// - **権限:** cache directory の変更 (job の掃除・PNG の書き込み) と外部 renderer の起動は、
//   setAuthority で許可されている間だけ行う。controller は Project lock を持つときだけ許可する。
//   許可が無い間は何も始めず、Unavailable (理由付き) を返す
// - 作業 directory は cacheDirectory/jobs/<session>/。確認の前に jobs/ の残り (強制終了など) を
//   消すのは、許可がある (= この cache directory を他の instance が使っていない) ときだけ
class MathRasterCache : public QObject {
    Q_OBJECT

public:
    enum class BackendState { Checking, Available, Unavailable };
    enum class State { Pending, Ready, Failed, Unavailable };

    struct Entry {
        State state = State::Pending;
        std::shared_ptr<const media::StillImage> mask;
        math::MathRenderStatus status = math::MathRenderStatus::Failed;
        QString message;
        QString log;
    };

    // Write の連番。状態の意味は Entry と同じ。
    struct SequenceEntry {
        State state = State::Pending;
        std::shared_ptr<const MathCoverageSequence> frames;
        math::MathRenderStatus status = math::MathRenderStatus::Failed;
        QString message;
        QString log;
    };

    // backend を確かめ、使えるなら render 関数を束ねて返す。workDirectory は作業用 (cache が所有)。
    using PreflightFunction = std::function<math::MathPreflightResult(
        const std::filesystem::path& workDirectory, const std::atomic<bool>* cancel)>;

    // 作った時点では権限が無く、何もしない (Unavailable)。setAuthority で始める。
    // sessionId は作業 directory の名前に使う (instance ごとに違う値)。
    MathRasterCache(std::string sessionId, PreflightFunction preflight, QObject* parent = nullptr);
    // 描画中の process を止め、worker が終わるまで待つ。
    ~MathRasterCache() override;

    // cache の置き場所と、そこを変更してよいか (Project lock を持つか) を設定する。
    // 呼ぶたびに世代が変わる: 持っている結果・進行中の確認と描画を捨て、許可があれば
    // 確認をやり直す (必ず Available / Unavailable に着く)。許可が無ければ reason で Unavailable。
    void setAuthority(std::filesystem::path cacheDirectory, bool authorized, QString reason = {});
    // backend を確かめ直し、覚えている失敗を忘れる (許可がある場合だけ)。描けている式の
    // disk の結果は使い続ける (強制の描き直しではない)。
    void startPreflight();
    void setPreflight(PreflightFunction preflight);

    BackendState backendState() const { return backendState_; }
    QString backendMessage() const { return backendMessage_; }
    // backend の toolchain の識別 (Available のときだけ)。
    QString toolchainText() const;

    // backend が Available でなければ空。
    QString keyFor(const math::MathRenderSpec& spec) const;
    // 未要求なら disk の確認と描画を始める。backend の確認中は Pending、使えなければ Unavailable。
    Entry request(const math::MathRenderSpec& spec);
    // Ready の key の PNG。書き出しはこれを読む (Manim を起動しない)。
    std::optional<std::filesystem::path> readyArtifact(const math::MathRenderSpec& spec) const;

    // Write の連番。静止と同じ worker・権限・世代で扱い、key は別の名前空間
    // (math::mathSequenceKey)。disk は cacheDirectory/write/<key>/ と <key>.txt (provenance)。
    QString sequenceKeyFor(const math::MathSequenceSpec& spec) const;
    SequenceEntry requestSequence(const math::MathSequenceSpec& spec);
    std::optional<MathSequenceArtifact> readySequence(const math::MathSequenceSpec& spec) const;
    // 描き終えていない連番の要求を取り消して忘れる (描画中なら process ごと止める)。
    // worker は 1 本なので、長い連番が入力中の式の静止の描画を待たせないために使う。
    // 取り消した連番は次の requestSequence で要求し直される。
    void cancelPendingSequences();

    // keys に無い record (静止・連番) を捨てる (描画中なら止める)。
    void retainOnly(const QSet<QString>& keys);
    void forgetFailures();
    void shutdown();

    void setRenderTimeout(std::chrono::milliseconds timeout) { renderTimeout_ = timeout; }
    // 連番の timeout は「静止の timeout + frame ごとの追加」。
    void setSequenceTimeoutPerFrame(std::chrono::milliseconds perFrame) {
        sequenceTimeoutPerFrame_ = perFrame;
    }
    // 連番の mask (1 画素 1 byte x 枚数) を memory に持てる上限。超える連番は Failed。
    void setSequenceMemoryBudget(std::size_t bytes) { sequenceMemoryBudget_ = bytes; }
    static constexpr std::size_t kDefaultSequenceMemoryBudget = std::size_t{256} << 20;
    const std::filesystem::path& cacheDirectory() const { return cacheDirectory_; }
    // この instance の作業 directory (cacheDirectory/jobs/<session>)。
    std::filesystem::path jobsDirectory() const;
    bool authorized() const { return authorized_; }
    int recordCount() const { return static_cast<int>(records_.size()); }
    int sequenceRecordCount() const { return static_cast<int>(sequences_.size()); }

    // provenance file の 1 行目。形を変えたら上げる。
    static constexpr char kArtifactFormat[] = "mvm-math-artifact/1";
    static constexpr char kSequenceArtifactFormat[] = "mvm-math-sequence-artifact/1";

Q_SIGNALS:
    // key の結果が出た。空なら全体 (backend の状態が変わった)。
    void entryChanged(const QString& key);

private:
    struct Record {
        Entry entry;
        std::filesystem::path artifact;
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    struct SequenceRecord {
        SequenceEntry entry;
        MathSequenceArtifact artifact;
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    void finishPreflight(std::uint64_t generation, math::MathPreflightResult result);
    void finishRender(const QString& key, std::uint64_t ticket, Entry entry,
                      std::filesystem::path artifact);
    void finishSequence(const QString& key, std::uint64_t ticket, SequenceEntry entry,
                        MathSequenceArtifact artifact);
    void cancelAll();
    void clearRecords();

    void becomeUnavailable(QString reason);

    std::string sessionId_;
    std::filesystem::path cacheDirectory_;
    bool authorized_ = false;
    PreflightFunction preflight_;
    QThreadPool pool_;
    QHash<QString, Record> records_;
    QHash<QString, SequenceRecord> sequences_;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t preflightGeneration_ = 0;
    std::shared_ptr<std::atomic<bool>> preflightCancel_;
    BackendState backendState_ = BackendState::Unavailable;
    QString backendMessage_ = QStringLiteral("数式の cache が設定されていません");
    math::MathRenderBackend backend_;
    std::chrono::milliseconds renderTimeout_{60000};
    std::chrono::milliseconds sequenceTimeoutPerFrame_{500};
    std::size_t sequenceMemoryBudget_ = kDefaultSequenceMemoryBudget;
    bool shutDown_ = false;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MATH_RASTER_CACHE_H
