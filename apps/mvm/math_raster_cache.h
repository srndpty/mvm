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
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mvm::app {

// Write の preview が memory に置く連番の mask の総量の上限 (全 clip の合計)。
// 予約してから decode し、mask が破棄される (どこからも参照されなくなる) ときに返す。
// cache が手放しても preview engine が持っている間は返らないので、memory に実際にある量を数える。
class MathResidencyBudget {
public:
    // released は予約が返るたびに (どの thread からでも) 呼ばれる。待ちをしないこと。
    explicit MathResidencyBudget(std::size_t limit, std::function<void()> released = {})
        : limit_(limit), released_(std::move(released)) {}
    // bytes を足しても上限を超えなければ予約して true。
    bool tryReserve(std::size_t bytes) {
        std::size_t current = live_.load();
        do {
            if (bytes > limit_ || current > limit_ - bytes)
                return false;
        } while (!live_.compare_exchange_weak(current, current + bytes));
        return true;
    }
    void release(std::size_t bytes) {
        live_.fetch_sub(bytes);
        if (released_)
            released_();
    }
    std::size_t live() const { return live_.load(); }
    std::size_t limit() const { return limit_; }

private:
    std::size_t limit_;
    std::function<void()> released_;
    std::atomic<std::size_t> live_{0};
};

// 予約 1 件。破棄すると予約を返す。
class MathResidencyReservation {
public:
    MathResidencyReservation(std::shared_ptr<MathResidencyBudget> budget, std::size_t bytes)
        : budget_(std::move(budget)), bytes_(bytes) {}
    ~MathResidencyReservation() { budget_->release(bytes_); }
    MathResidencyReservation(const MathResidencyReservation&) = delete;
    MathResidencyReservation& operator=(const MathResidencyReservation&) = delete;

private:
    std::shared_ptr<MathResidencyBudget> budget_;
    std::size_t bytes_;
};

// Write の連番の mask。各 frame は 1 画素 1 byte の被覆 (PNG の alpha)、width * height byte。
// preview は RGBA ではなくこの形で持つ (出力全面の RGBA を frame ごとに持たない)。
// reservation は全体の上限 (MathResidencyBudget) の予約で、この mask と一緒に破棄される。
struct MathCoverageSequence {
    int width = 0;
    int height = 0;
    std::vector<std::vector<std::uint8_t>> frames;
    std::shared_ptr<const MathResidencyReservation> reservation;
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

    // Write の連番の disk の artifact。状態の意味は Entry と同じで、Ready は disk に揃っている
    // (書き出しに使える) ことだけを表す。preview 用に memory へ置くかは residentSequence が別に扱う。
    struct SequenceEntry {
        State state = State::Pending;
        int width = 0; // Ready のときの frame の大きさ
        int height = 0;
        math::MathRenderStatus status = math::MathRenderStatus::Failed;
        QString message;
        QString log;
    };

    // Write の連番の preview 用の mask が memory にあるか。
    //   NotReady   disk の連番が Ready でない (描画中・失敗など。SequenceEntry を見る)
    //   Loading    disk から読んでいる
    //   Resident   frames が使える
    //   OverBudget 全体の上限 (setResidentMemoryBudget) に収まらない。preview は静止で見せる
    //              (書き出しは disk の連番を使うので影響しない)
    //   Failed     disk の連番を読めない (artifact を消し、連番を Failed にして描き直させる)
    enum class Residency { NotReady, Loading, Resident, OverBudget, Failed };
    struct ResidentSequence {
        Residency state = Residency::NotReady;
        std::shared_ptr<const MathCoverageSequence> frames;
        QString message;
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
    // preview 用の mask。disk の連番が Ready で memory に無ければ、全体の上限の中で予約して
    // 別の worker で読み始める (読めたら entryChanged)。上限に足りなければ、最も長く使って
    // いない mask を cache から外してから予約し直す (preview engine が使っている mask は外しても
    // memory に残るので、その分は上限に数えたまま)。
    ResidentSequence residentSequence(const math::MathSequenceSpec& spec);
    // residentSequence と同じ状態を、読み始めずに返す (inspector の表示用)。
    ResidentSequence residencyOf(const math::MathSequenceSpec& spec) const;
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
    // preview 用の連番の mask (1 画素 1 byte x 枚数) を memory に置ける全体の上限。
    // 変えると memory に置いた mask を cache から外す (使用中の mask は参照が無くなると返る)。
    void setResidentMemoryBudget(std::size_t bytes);
    static constexpr std::size_t kDefaultResidentMemoryBudget = std::size_t{256} << 20;
    // memory に実際にある preview 用の mask の総量 (preview engine が持つ分も含む)。
    std::size_t residentBytes() const { return residency_->live(); }
    std::size_t residentMemoryBudget() const { return residency_->limit(); }
    // disk から mask を読んだ回数 (試験が追い出しと読み直しを確かめる)。
    int residentLoadCount() const { return residentLoads_; }
    int residentSequenceCount() const { return static_cast<int>(resident_.size()); }
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
    // memory に置いた mask と、読んでいる途中の要求。
    struct ResidentRecord {
        std::shared_ptr<const MathCoverageSequence> frames;
        std::uint64_t lastUse = 0;
    };
    struct LoadingRecord {
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    void finishResident(const QString& key, std::uint64_t ticket,
                        std::shared_ptr<const MathCoverageSequence> frames, QString error);
    void dropResident(const QString& key);
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
    // preview 用の mask の読み込みは描画 (Manim) と別の worker で行う (長い描画を待たない)。
    QThreadPool residentPool_;
    QHash<QString, ResidentRecord> resident_;
    QHash<QString, LoadingRecord> loading_;
    // 最後に上限に収まらなかった key と理由 (residencyOf が返す)。
    QHash<QString, QString> overBudget_;
    // 予約が返ったことを (どの thread からでも) この cache へ知らせる。cache の破棄後は知らせない。
    struct ResidencyNotifier {
        std::mutex mutex;
        MathRasterCache* target = nullptr;
    };
    std::shared_ptr<ResidencyNotifier> notifier_ = std::make_shared<ResidencyNotifier>();
    std::shared_ptr<MathResidencyBudget> makeResidencyBudget(std::size_t bytes) const;
    // 予約が返った: 上限に収まらなかった key をもう一度試させる (entryChanged)。
    void residencyReleased();
    std::shared_ptr<MathResidencyBudget> residency_;
    std::uint64_t useTick_ = 0;
    int residentLoads_ = 0;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t preflightGeneration_ = 0;
    std::shared_ptr<std::atomic<bool>> preflightCancel_;
    BackendState backendState_ = BackendState::Unavailable;
    QString backendMessage_ = QStringLiteral("数式の cache が設定されていません");
    math::MathRenderBackend backend_;
    std::chrono::milliseconds renderTimeout_{60000};
    std::chrono::milliseconds sequenceTimeoutPerFrame_{500};
    bool shutDown_ = false;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MATH_RASTER_CACHE_H
