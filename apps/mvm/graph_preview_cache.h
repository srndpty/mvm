#ifndef MVM_APPS_GRAPH_PREVIEW_CACHE_H
#define MVM_APPS_GRAPH_PREVIEW_CACHE_H

#include "graph_preview_animation.h"
#include "media/manim/manim_graph.h"
#include "preview_engine/preview_types.h"

#include <map>
#include <optional>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QThreadPool>

namespace mvm::app {
// 状態の照会は GUI thread だけ。renderer・検証・decode は専用 worker だけで行う。
class GraphPreviewCache final : public QObject {
    Q_OBJECT
public:
    enum class Job { Unrequested, Checking, Rendering, Ready, Failed, Cancelled, Superseded };
    enum class ArtifactState { Unknown, Missing, Validated, Corrupt };
    enum class Residency { Missing, Loading, Resident, OverBudget, Corrupt, Cancelled };
    enum class Reason {
        None,
        InvalidGraph,
        InvalidExpression,
        UnsupportedExpression,
        NoFiniteSamples,
        BackendUnavailable,
        Rendering,
        ArtifactMissing,
        ArtifactCorrupt,
        ProvenanceMismatch,
        FrameMissing,
        OverBudget,
        Cancelled,
        Superseded,
        UnsupportedTransition,
        RendererFailure,
        LabelFailure,
        PublicationFailure,
        ResourceLimit,
        Loading,
        DisabledClip,
        HiddenTrack
    };

    struct Status {
        std::string clipId;
        QString key;
        Job job = Job::Unrequested;
        ArtifactState artifact = ArtifactState::Unknown;
        Residency residency = Residency::Missing;
        Reason reason = Reason::ArtifactMissing;
        QString message;
        std::int64_t sourceFrame = 0;
        std::uint64_t generation = 0;
        bool identityEstablished = false;
        bool backendAvailable = false;
        // 検証時点の証拠であり、照会時点の disk の不変性を保証しない。
        bool validatedSnapshot = false;
        bool compileValid = true;
        bool frameAvailable = false;
        bool transparentFallback = true;
        bool backendCheckInProgress = false;
        std::uint64_t projectGeneration = 0, projectRevision = 0;
        double artifactReadyMs = 0, decodeMs = 0;
    };

    using Preflight = std::function<std::variant<manim::GraphBackend, graph::Error>(
        const std::filesystem::path&, const std::atomic<bool>*)>;
    explicit GraphPreviewCache(Preflight, QObject* parent = nullptr);
    ~GraphPreviewCache() override;
    void setAuthority(std::filesystem::path, bool authorized);
    void retainOnly(const QSet<QString>&);
    QString keyFor(const graph::GraphRenderSpec&) const;
    void request(const graph::GraphRenderSpec&);
    void requestFrame(const graph::GraphRenderSpec&, std::int64_t sourceFrame);
    Status status(const graph::GraphRenderSpec&, std::int64_t sourceFrame) const;
    std::map<std::int64_t, std::shared_ptr<const preview::PreviewStillImage>>
    frames(const graph::GraphRenderSpec&) const;
    std::shared_ptr<const GraphPresentation> presentation(const graph::GraphRenderSpec&);
    void shutdown();
    void resetSession();
    void refreshBackend();
    void setBudgetForTest(std::size_t bytes);
    std::size_t residentBytes() const;
    std::size_t peakBytes() const;

    int renderCount() const { return renderCount_; }
    // GUI thread で値を取得する。export はこの identity から disk を独立に再検証する。
    std::string exportToolchain() const { return backend_ ? backend_->toolchain : std::string{}; }

    std::size_t pendingDecodeCount() const;
    static Reason reasonFor(graph::Failure);
Q_SIGNALS:
    void changed();

private:
    struct Record {
        graph::GraphRenderSpec spec;
        Status status;
        std::optional<graph::Artifact> artifact;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::uint64_t ticket = 0, priority = 0;
        std::chrono::steady_clock::time_point requested;
    };

    struct Budget {
        std::atomic<std::size_t> live{0}, peak{0};
        std::size_t limit = 64 * 1024 * 1024;
    };

    struct Frame {
        QString owner;
        std::int64_t index = -1;
        Residency state = Residency::Loading;
        Reason reason = Reason::None;
        std::shared_ptr<const preview::PreviewStillImage> image;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::uint64_t tick = 0;
        double decodeMs = 0;
    };

    static std::int64_t frameIndex(const graph::GraphRenderSpec&, std::int64_t);
    static QString frameKey(const QString&, std::int64_t);
    void startPreflight();
    void pump();
    void invalidate();
    void publishPresentations();
    void requestDecoded(const graph::GraphRenderSpec&, std::int64_t, bool prefetch);
    Preflight preflight_;
    std::optional<manim::GraphBackend> backend_;
    bool available_ = false, checking_ = false, authorized_ = false, closed_ = false;
    bool preflightAttempted_ = false;
    int activeRenders_ = 0, activeDecodes_ = 0;
    std::filesystem::path directory_;
    std::shared_ptr<graph::PublicationAuthority> authority_;
    std::shared_ptr<std::atomic<bool>> preflightCancel_;
    QThreadPool renderPool_, decodePool_;
    QHash<QString, Record> records_;
    QHash<QString, graph::GraphRenderSpec> waitingIdentity_;
    QHash<QString, Frame> frames_;
    QHash<QString, std::shared_ptr<GraphPresentation>> presentations_;
    std::shared_ptr<Budget> budget_ = std::make_shared<Budget>();
    std::uint64_t generation_ = 0, ticket_ = 0, tick_ = 0;
    std::uint64_t publicationGeneration_ = 0;
    int renderCount_ = 0;
};
} // namespace mvm::app
#endif
