#ifndef MVM_APPS_MVM_MATH_RASTER_CACHE_H
#define MVM_APPS_MVM_MATH_RASTER_CACHE_H

#include "math_equation_sequence_artifact.h"
#include "media/math/equation_sequence_render.h"
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
#include <deque>
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

    // Equation Sequence (P3-3) の disk の artifact。Ready は検証済みの artifact が disk にある
    // ことだけを表す (preview・書き出しへの組み込みはまだ無い)。失敗は backend の構造検証の
    // 理由 (backendFailure) を持つ。
    struct EquationSequenceEntry {
        State state = State::Pending;
        math::MathRenderStatus status = math::MathRenderStatus::Failed;
        math::EquationBackendFailure backendFailure = math::EquationBackendFailure::NotValidated;
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

    // Equation Sequence (P3-3)。静止・Write・変形と同じ worker・権限・世代・publish の gate で
    // 扱い、key は math::equationSequenceRenderKey (mvm-equation-sequence/1)。disk は
    // cacheDirectory/equation-sequence/ (math_equation_sequence_artifact.h)。
    //
    // - 描く前に、全状態の今の静止 (request と同じ key) が Ready であることを待ち、その mask を
    //   backend へ渡す。どれかの静止が Failed / Unavailable なら sequence も同じ状態にする
    // - backend の構造検証 (部分・所有・空の対象) を通らなければ描かずに Failed
    // - 変形・action の枚数が backend の能力を超えれば、この描画環境の未対応として Failed
    QString equationSequenceKeyFor(const math::EquationSequenceRenderSpec& spec) const;
    EquationSequenceEntry requestEquationSequence(const math::EquationSequenceRenderSpec& spec);
    // Ready の artifact を、今の静止と disk の provenance・全 frame で検査し直して返す。
    std::optional<EquationSequenceArtifact>
    readyEquationSequence(const math::EquationSequenceRenderSpec& spec) const;
    int equationSequenceRecordCount() const { return static_cast<int>(equationSequences_.size()); }
    // request せずに今の状態を返す (record が無ければ既定の Pending)。
    EquationSequenceEntry equationSequenceEntryOf(const math::EquationSequenceRenderSpec& spec) const;
    // Ready の record が持つ検証済みの artifact (P3-4 の preview の配置と色)。disk は読まない
    // (provenance と各層は memory へ読むときに 1 枚ずつ照合する)。
    std::optional<EquationSequenceArtifact>
    equationSequenceArtifactOf(const math::EquationSequenceRenderSpec& spec) const;

    // ---- Equation Sequence の preview 用の層 (P3-4) ----
    //
    // 1 枚の A8 の層を、Write・変形と同じ上限 (MathResidencyBudget)・同じ LRU で memory に置く。
    // 別の上限・別の cache を持たない。disk の Ready (書き出しが使える) と memory の Resident は
    // 別の状態で、上限に収まらなくても disk の artifact は Ready のまま。
    enum class EquationLayerRole { TransitionFrame, ActionBase, ActionAccent };
    struct EquationLayerRef {
        EquationLayerRole role = EquationLayerRole::TransitionFrame;
        std::size_t interval = 0; // 変形・action の番号 (spec の順)
        std::int64_t frame = 0;   // TransitionFrame / ActionAccent の区間内の frame
        bool operator==(const EquationLayerRef&) const = default;
    };
    // residency の key (派生の cache の識別で、Project の正ではない):
    //   <sequence の key>/t<変形>/<frame>、/a<action>/base、/a<action>/<frame>
    static QString equationLayerKey(const QString& sequenceKey, const EquationLayerRef& layer);
    struct ResidentEquationLayers {
        Residency state = Residency::NotReady;
        // Resident のとき refs の順の層 (各 1 枚)。
        std::vector<std::shared_ptr<const MathCoverageSequence>> layers;
        QString message;
    };
    // refs の層を 1 つの束として memory に置く。disk の artifact が今の key で Ready でなければ
    // NotReady。束の全層が揃ったときだけ Resident (action の base と accent の片方だけを見せない)。
    // 足りない層の byte をまとめて予約し、収まらなければ束全体を OverBudget にする (1 層だけ
    // 読んで上限を使わない)。
    //   current  今の frame の束。先に読み (優先度)、上限に足りなければ cache だけが持つ mask を
    //            外す (preview が使っている mask は外さない)
    //   先読み   何も外さない (今の frame に要る層を追い出さない)。OverBudget を記録しない
    // 読むときに disk の provenance が検証した時と同じことと、frame の大きさ・SHA-256 を確かめる。
    // 合わなければ artifact を消して sequence を Failed (CorruptFrame) にする。
    ResidentEquationLayers residentEquationLayers(const math::EquationSequenceRenderSpec& spec,
                                                  const std::vector<EquationLayerRef>& refs,
                                                  bool current);
    // 同じ状態を、読み始めずに返す (状態の表示・試験用)。
    ResidentEquationLayers equationLayersResidencyOf(const math::EquationSequenceRenderSpec& spec,
                                                     const std::vector<EquationLayerRef>& refs) const;
    // memory にある (読み終えた) 層の数 (試験用)。
    int residentEquationLayerCount() const;
    // 試験用: true の間、Equation Sequence の層の読み込みの worker は待ち行列から取り出さずに待つ
    // (1 本の worker を止め、待ち行列の順を試験が決めるため)。
    void pauseEquationLayerLoadsForTest(bool paused);
    // 試験用: 層の読み込みを待ち行列から取り出した順の key (取消で読まなかったものも含む)。
    std::vector<QString> equationLayerLoadOrderForTest() const;
    // 今の Ready の静止の状態を、要求せずに返す (record が無ければ既定の Pending。backend が
    // 使えなければ Unavailable)。状態の問い合わせ用で、描画を始めない。
    Entry entryOf(const math::MathRenderSpec& spec) const;
    // 試験用: provenance を書く直前に (worker の thread で) 呼ぶ。
    void setBeforeEquationSequencePublishForTest(
        std::function<void(const std::filesystem::path& provenance)> hook) {
        beforeEquationSequencePublish_ = std::move(hook);
    }

    // keys に無い record (静止・連番・変形・Equation Sequence) を捨てる (描画中なら止める)。
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
    static constexpr const char* kEquationSequenceArtifactFormat =
        app::kEquationSequenceArtifactFormat;

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

    struct EquationSequenceRecord {
        EquationSequenceEntry entry;
        math::EquationSequenceRenderSpec spec;
        bool launched = false; // 全状態の静止が揃って描画を始めたか
        EquationSequenceArtifact artifact;
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
    // 全状態の静止が揃っていれば sequence の描画を始める。状態が変わったら true。
    bool advanceEquationSequence(const QString& key);
    void finishEquationSequence(const QString& key, std::uint64_t ticket,
                                EquationSequenceEntry entry, EquationSequenceArtifact artifact);
    // sequence の job を組む (静止の mask は呼び出し側が渡す)。
    EquationSequenceJob equationSequenceJob(const QString& key,
                                            const math::EquationSequenceRenderSpec& spec) const;
    // memory に置いた mask と、読んでいる途中の要求。Write と変形は key の名前空間が別なので
    // 同じ表に置き、同じ上限・LRU で扱う。
    // Equation Sequence の層は 1 枚ごとの key (owner の sequence の key の下) で同じ表に置く。
    enum class ResidentKind { Write, Transform, EquationLayer };
    struct ResidentRecord {
        std::shared_ptr<const MathCoverageSequence> frames;
        std::uint64_t lastUse = 0;
    };
    struct LoadingRecord {
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        ResidentKind kind = ResidentKind::Write;
    };
    // EquationLayer の key の owner (sequence の key)。それ以外は key そのもの。
    static QString residentOwner(const QString& key);
    // 予約済みの層を読み始める (読めたら finishResident)。
    void startEquationLayerLoad(const QString& key, int width, int height,
                                std::shared_ptr<const MathResidencyReservation> reservation,
                                EquationArtifactFrame frame, std::string sequenceKey,
                                std::string provenance, bool current);
    // Equation Sequence の層の読み込みの待ち行列 (P3-4.1)。worker は 1 本。待ち行列の仕事は
    // 取り出す時点で今の frame の列を先に取る。先読みで待っている層が今の frame になったら、
    // 同じ仕事 (ticket・取消・予約をそのまま) を今の frame の列の末尾へ移す。
    struct EquationLoadJob {
        QString key;
        std::uint64_t ticket = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<const MathResidencyReservation> reservation;
        EquationArtifactFrame frame;
        int width = 0;
        int height = 0;
        std::string sequenceKey;
        std::string provenance;
        std::filesystem::path directory;
    };
    struct EquationLoadQueue {
        std::mutex mutex;
        std::deque<EquationLoadJob> current;
        std::deque<EquationLoadJob> prefetch;
        std::vector<QString> taken; // 取り出した順 (試験用)
        std::atomic<bool> paused{false};
        std::atomic<bool> stopped{false};
    };
    std::shared_ptr<EquationLoadQueue> equationLoads_ = std::make_shared<EquationLoadQueue>();
    // 待ち行列から 1 件取り出して読む仕事を worker へ出す。
    void startEquationLoadDrain(int priority);
    // 先読みで待っている key の仕事を今の frame の列へ移す。移したら true。
    bool promoteEquationLayerLoad(const QString& key);
    // 壊れた・古い層を見つけた: sequence を Failed にし、その層をすべて手放す。
    void failEquationSequence(const QString& sequenceKey, const QString& error);
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
    QHash<QString, EquationSequenceRecord> equationSequences_;
    std::function<void(const std::filesystem::path&)> beforeEquationSequencePublish_;
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
