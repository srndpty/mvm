#ifndef MVM_APPS_MVM_MATH_RASTER_CACHE_H
#define MVM_APPS_MVM_MATH_RASTER_CACHE_H

#include "media/math/math_backend.h"
#include "media/math/math_render.h"
#include "media/math/math_transform.h"
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

// Write と変形の preview が memory に置く mask (A8 の被覆の中身) の総量の上限 (全 clip・全
// トランジションの合計。Write と変形で別の上限を持たない)。
// process 全体・GPU・decode の memory は数えない。
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

// 変形の disk の artifact (frame 0 から順)。各 frame は backend の一時的な canvas から artifact の
// 矩形だけを切り出した被覆 (1 画素 1 byte、行間の余白なし、width * height byte) の生の byte 列。
// 端点の位置は切り出した座標で、端点の静止の mask の左上。
struct MathTransformArtifact {
    std::vector<std::filesystem::path> frames;
    // 各 frame の中身の SHA-256 (provenance に書いた値)。読むたびに照合する。
    std::vector<std::string> frameSha256;
    int width = 0;
    int height = 0;
    int sourceX = 0;
    int sourceY = 0;
    int targetX = 0;
    int targetY = 0;
};

// 変形の artifact の frame index を読む (provenance の大きさと SHA-256 を照合する)。
// 合わなければ false と error。preview の memory への読み込みと、書き出し (P2-6) が使う。
bool loadMathTransformFrame(const MathTransformArtifact& artifact, std::size_t index,
                            std::vector<std::uint8_t>& coverage, std::string& error);

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

    // 変形の disk の artifact。Ready は検証済みの artifact が disk にある (preview・書き出しが
    // 後で使える) ことだけを表す。preview 用の mask が memory にあるかは residentTransform が扱う。
    struct TransformEntry {
        State state = State::Pending;
        int width = 0; // Ready のときの切り出した frame の大きさ
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
    // 描き終えていない連番と変形の要求を取り消して忘れる (描画中なら process ごと止め、
    // 待ち行列の仕事は始めずに捨てる)。worker は 1 本なので、Write や変形の描画が入力中の式の
    // 静止の描画を待たせないために使う。disk の Ready の artifact・memory の mask は消さない。
    // 取り消した連番・変形は次の requestSequence / requestTransform で要求し直される。
    void cancelPendingAnimations();

    // 式から式への変形 (P2-4)。静止・Write と同じ worker・権限・世代で扱い、key は
    // math::mathTransformKey (mvm-math-transform/1)。disk は cacheDirectory/transform/<key>/ と
    // <key>.txt (provenance、mvm-math-transform-artifact/1)。
    //
    // - 描く前に、両端の式の今の静止 (request と同じ key) が Ready であることを待ち、その mask を
    //   そのまま backend へ渡す。前に描けた別の式の静止で代用しない。どちらかの静止が Failed /
    //   Unavailable なら変形も描かずに同じ状態にする
    // - backend の能力を超える枚数は Project の誤りではなく、この描画環境の未対応として失敗させる
    // - preview の表示・書き出しはまだ変形を使わない (P2-5 / P2-6)
    QString transformKeyFor(const math::MathTransformSpec& spec) const;
    TransformEntry requestTransform(const math::MathTransformSpec& spec);
    std::optional<MathTransformArtifact> readyTransform(const math::MathTransformSpec& spec) const;
    // 書き出し開始前に現在の端点と disk provenance を再検査する。常駐 mask は要求しない。
    std::optional<MathTransformArtifact>
    readyTransformForExport(const math::MathTransformSpec& spec) const;
    // preview 用の mask。Write と同じ全体の上限・予約・LRU を共有する (別の上限を持たない)。
    // 上限に収まらなくても disk の artifact は Ready のまま。追い出した mask は disk から読み直す。
    ResidentSequence residentTransform(const math::MathTransformSpec& spec);
    ResidentSequence transformResidencyOf(const math::MathTransformSpec& spec) const;
    int transformRecordCount() const { return static_cast<int>(transforms_.size()); }
    // 試験用: 変形の provenance を書く直前に (worker の thread で) 呼ぶ。引数は provenance の path。
    void setBeforeTransformPublishForTest(
        std::function<void(const std::filesystem::path& provenance)> hook) {
        beforeTransformPublish_ = std::move(hook);
    }

    // keys に無い record (静止・連番・変形) を捨てる (描画中なら止める)。
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
    // 試験用: true の間は、読み終えた mask を Loading のまま留め、false にしたときに届ける。
    // 再生中に mask が届く時刻を試験が決めるために使う (読み込みそのものは止めない)。
    void holdResidentLoadsForTest(bool hold);
    int heldResidentLoadCountForTest() const { return static_cast<int>(heldResident_.size()); }
    const std::filesystem::path& cacheDirectory() const { return cacheDirectory_; }
    // この instance の作業 directory (cacheDirectory/jobs/<session>)。
    std::filesystem::path jobsDirectory() const;
    bool authorized() const { return authorized_; }
    int recordCount() const { return static_cast<int>(records_.size()); }
    int sequenceRecordCount() const { return static_cast<int>(sequences_.size()); }

    // provenance file の 1 行目。形を変えたら上げる。
    static constexpr char kArtifactFormat[] = "mvm-math-artifact/1";
    static constexpr char kSequenceArtifactFormat[] = "mvm-math-sequence-artifact/1";
    static constexpr char kTransformArtifactFormat[] = "mvm-math-transform-artifact/1";

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

    struct TransformRecord {
        TransformEntry entry;
        math::MathTransformSpec spec;
        // 描画を始めた (両端の静止が揃った) か。始める前は両端の静止を待っている。
        bool launched = false;
        MathTransformArtifact artifact;
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    void finishPreflight(std::uint64_t generation, math::MathPreflightResult result);
    void finishRender(const QString& key, std::uint64_t ticket, Entry entry,
                      std::filesystem::path artifact);
    void finishSequence(const QString& key, std::uint64_t ticket, SequenceEntry entry,
                        MathSequenceArtifact artifact);
    // 両端の静止が揃っていれば変形の描画を始める。状態が変わったら true。
    bool advanceTransform(const QString& key);
    // 静止の key の結果が出た: その静止を待つ変形を進める。
    void advanceTransformsWaitingOn(const QString& staticKey);
    void finishTransform(const QString& key, std::uint64_t ticket, TransformEntry entry,
                         MathTransformArtifact artifact);
    // memory に置いた mask と、読んでいる途中の要求。Write と変形は key の名前空間が別なので
    // 同じ表に置き、同じ上限・LRU で扱う。
    enum class ResidentKind { Write, Transform };
    struct ResidentRecord {
        std::shared_ptr<const MathCoverageSequence> frames;
        std::uint64_t lastUse = 0;
    };
    struct LoadingRecord {
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        ResidentKind kind = ResidentKind::Write;
    };
    // 読み込み (取消を見て、読めなければ nullptr。取消なら cancelled を立てる)。
    using ResidentDecoder = std::function<std::shared_ptr<MathCoverageSequence>(
        const std::atomic<bool>* cancel, bool& cancelled)>;
    ResidentSequence acquireResident(const QString& key, ResidentKind kind, std::size_t bytes,
                                     ResidentDecoder decode);
    ResidentSequence residencyFor(const QString& key) const;
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
    QHash<QString, TransformRecord> transforms_;
    // 変形の provenance (確定の印) を書くことと、取消 (権限・世代の変更、要求の取り下げ) を
    // 排他にする。worker は取消を見てから書くまでをこの lock の中で行うので、取り消した後に
    // 古い世代の結果が確定することはない。
    std::shared_ptr<std::mutex> publishGate_ = std::make_shared<std::mutex>();
    std::function<void(const std::filesystem::path&)> beforeTransformPublish_;
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
    struct HeldResident {
        QString key;
        std::uint64_t ticket = 0;
        std::shared_ptr<const MathCoverageSequence> frames;
        QString error;
    };
    bool holdResident_ = false;
    std::vector<HeldResident> heldResident_;
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
