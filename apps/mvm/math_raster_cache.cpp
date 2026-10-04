#include "math_raster_cache.h"

#include "media/still_image/static_image.h"
#include "util/mvm_atomic_write.h"

#include <QMetaObject>

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace mvm::app {
namespace {

using Entry = MathRasterCache::Entry;
using State = MathRasterCache::State;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool writeAtomically(const std::filesystem::path& path, const std::string& bytes,
                     std::string& error) {
    char buffer[512] = {};
    const int code = mvm_atomic_write_file(path.c_str(), bytes.data(), bytes.size(), buffer,
                                           sizeof(buffer));
    error = buffer;
    return code == 0;
}

std::filesystem::path artifactPath(const std::filesystem::path& directory, const QString& key) {
    return directory / (key.toStdWString() + L".png");
}

std::filesystem::path provenancePath(const std::filesystem::path& directory, const QString& key) {
    return directory / (key.toStdWString() + L".txt");
}

std::string provenanceText(const QString& key, int width, int height,
                           const math::MathToolchainFingerprint& toolchain) {
    return std::string(MathRasterCache::kArtifactFormat) + "\nkey=" + key.toStdString() +
           "\nwidth=" + std::to_string(width) + "\nheight=" + std::to_string(height) +
           "\ntoolchain:\n" + toolchain.canonical;
}

void removeArtifact(const std::filesystem::path& directory, const QString& key) {
    std::error_code error;
    std::filesystem::remove(provenancePath(directory, key), error);
    std::filesystem::remove(artifactPath(directory, key), error);
}

std::shared_ptr<const media::StillImage> decodeMask(const std::filesystem::path& png, int width,
                                                    int height) {
    auto decoded = media::loadStaticImage(png);
    if (!decoded.success || decoded.image.width != width || decoded.image.height != height)
        return nullptr;
    return std::make_shared<const media::StillImage>(std::move(decoded.image));
}

// 前回までに描いた結果を読む。provenance (版・key・大きさ・toolchain) が合わない、または画像が
// 読めない・大きさが違うものは消して描き直させる。
std::shared_ptr<const media::StillImage>
loadArtifact(const std::filesystem::path& directory, const QString& key,
             const math::MathToolchainFingerprint& toolchain) {
    const std::string provenance = readFile(provenancePath(directory, key));
    if (provenance.empty())
        return nullptr;
    std::istringstream lines(provenance);
    std::string format;
    std::string keyLine;
    std::string widthLine;
    std::string heightLine;
    std::getline(lines, format);
    std::getline(lines, keyLine);
    std::getline(lines, widthLine);
    std::getline(lines, heightLine);
    int width = 0;
    int height = 0;
    try {
        if (widthLine.rfind("width=", 0) == 0)
            width = std::stoi(widthLine.substr(6));
        if (heightLine.rfind("height=", 0) == 0)
            height = std::stoi(heightLine.substr(7));
    } catch (...) {
        width = 0;
    }
    if (format != MathRasterCache::kArtifactFormat || keyLine != "key=" + key.toStdString() ||
        width <= 0 || height <= 0 || provenance != provenanceText(key, width, height, toolchain)) {
        removeArtifact(directory, key);
        return nullptr;
    }
    auto mask = decodeMask(artifactPath(directory, key), width, height);
    if (!mask)
        removeArtifact(directory, key);
    return mask;
}

struct Outcome {
    Entry entry;
    std::filesystem::path artifact;
    bool cancelled = false;
};

Outcome cancelledOutcome() {
    Outcome outcome;
    outcome.cancelled = true;
    return outcome;
}

Outcome failed(math::MathRenderStatus status, const std::string& message,
               const std::string& log = {}) {
    Outcome outcome;
    outcome.entry.state = status == math::MathRenderStatus::BackendUnavailable
                              ? State::Unavailable
                              : State::Failed;
    outcome.entry.status = status;
    outcome.entry.message = QString::fromStdString(message);
    outcome.entry.log = QString::fromStdString(log);
    return outcome;
}

Outcome renderJob(const std::filesystem::path& directory, const std::filesystem::path& jobs,
                  const QString& key, const math::MathRenderSpec& spec,
                  const math::MathRenderBackend& backend, std::chrono::milliseconds timeout,
                  std::uint64_t ticket, const std::atomic<bool>* cancel) {
    if (auto mask = loadArtifact(directory, key, backend.fingerprint)) {
        Outcome outcome;
        outcome.entry.state = State::Ready;
        outcome.entry.status = math::MathRenderStatus::Ok;
        outcome.entry.mask = std::move(mask);
        outcome.artifact = artifactPath(directory, key);
        return outcome;
    }
    if (cancel->load())
        return cancelledOutcome();

    math::MathStaticRenderRequest request;
    request.spec = spec;
    request.timeout = timeout;
    request.jobDirectory = jobs / (key.toStdWString() + L"-" + std::to_wstring(ticket));
    std::error_code error;
    std::filesystem::remove_all(request.jobDirectory, error);
    std::filesystem::create_directories(request.jobDirectory, error);
    if (error)
        return failed(math::MathRenderStatus::Failed,
                      "数式の作業 directory を作成できません: " + error.message());
    const auto rendered = backend.render(request, cancel);
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(request.jobDirectory, ignored);
    };
    if (rendered.status == math::MathRenderStatus::Cancelled || cancel->load()) {
        cleanup();
        return cancelledOutcome();
    }
    if (rendered.status != math::MathRenderStatus::Ok) {
        cleanup();
        return failed(rendered.status, rendered.message, rendered.log);
    }

    // 作業 directory の PNG を cache へ置く。PNG を先に、provenance を後に書く。読む側は
    // provenance が無ければ使わないので、途中で止まっても半端な結果を引かない。
    const std::string bytes = readFile(rendered.png);
    std::string writeError;
    const auto artifact = artifactPath(directory, key);
    if (bytes.empty() || !writeAtomically(artifact, bytes, writeError) ||
        !writeAtomically(provenancePath(directory, key),
                         provenanceText(key, rendered.width, rendered.height, backend.fingerprint),
                         writeError)) {
        cleanup();
        removeArtifact(directory, key);
        return failed(math::MathRenderStatus::Failed,
                      "数式の画像を cache へ保存できません: " + writeError, rendered.log);
    }
    cleanup();
    auto mask = decodeMask(artifact, rendered.width, rendered.height);
    if (!mask) {
        removeArtifact(directory, key);
        return failed(math::MathRenderStatus::Failed,
                      "描いた数式の画像を読めないか、大きさが報告と違います", rendered.log);
    }
    Outcome outcome;
    outcome.entry.state = State::Ready;
    outcome.entry.status = math::MathRenderStatus::Ok;
    outcome.entry.mask = std::move(mask);
    outcome.entry.log = QString::fromStdString(rendered.log);
    outcome.artifact = artifact;
    return outcome;
}

} // namespace

MathRasterCache::MathRasterCache(std::string sessionId, PreflightFunction preflight,
                                 QObject* parent)
    : QObject(parent), sessionId_(std::move(sessionId)), preflight_(std::move(preflight)) {
    // Manim / LaTeX を同時に走らせない。preflight と描画もこの順に並ぶ。
    pool_.setMaxThreadCount(1);
}

MathRasterCache::~MathRasterCache() {
    shutdown();
}

void MathRasterCache::shutdown() {
    shutDown_ = true;
    cancelAll();
    pool_.clear();
    pool_.waitForDone();
}

void MathRasterCache::cancelAll() {
    if (preflightCancel_)
        preflightCancel_->store(true);
    for (const auto& record : std::as_const(records_)) {
        if (record.cancel)
            record.cancel->store(true);
    }
}

void MathRasterCache::setPreflight(PreflightFunction preflight) {
    preflight_ = std::move(preflight);
}

std::filesystem::path MathRasterCache::jobsDirectory() const {
    return cacheDirectory_ / L"jobs" / QString::fromStdString(sessionId_).toStdWString();
}

void MathRasterCache::becomeUnavailable(QString reason) {
    // 世代を進めて、進行中の確認の結果を捨てる。
    ++preflightGeneration_;
    cancelAll();
    records_.clear();
    backend_ = {};
    backendState_ = BackendState::Unavailable;
    backendMessage_ = std::move(reason);
    Q_EMIT entryChanged(QString());
}

void MathRasterCache::setAuthority(std::filesystem::path cacheDirectory, bool authorized,
                                   QString reason) {
    if (shutDown_)
        return;
    // 置き場所や権限の変更は世代の変更として扱う。進行中の確認を捨てるだけにせず、
    // 許可があれば必ず次の確認を始める (Checking のまま止まらない)。
    cacheDirectory_ = std::move(cacheDirectory);
    authorized_ = authorized && !cacheDirectory_.empty();
    if (!authorized_) {
        becomeUnavailable(reason.isEmpty()
                              ? QStringLiteral("数式の cache を変更する権限がありません")
                              : std::move(reason));
        return;
    }
    startPreflight();
}

void MathRasterCache::startPreflight() {
    if (shutDown_)
        return;
    // 権限が無い間は、cache directory の掃除も外部 renderer の起動もしない。状態は
    // setAuthority が理由付きの Unavailable にしてある。
    if (!authorized_)
        return;
    cancelAll();
    records_.clear();
    backendState_ = BackendState::Checking;
    backendMessage_.clear();
    backend_ = {};
    const auto generation = ++preflightGeneration_;
    preflightCancel_ = std::make_shared<std::atomic<bool>>(false);
    Q_EMIT entryChanged(QString());
    if (!preflight_) {
        math::MathPreflightResult result;
        result.message = "数式の描画 backend が設定されていません";
        finishPreflight(generation, std::move(result));
        return;
    }
    pool_.start([this, generation, preflight = preflight_, cancel = preflightCancel_,
                 directory = cacheDirectory_, jobs = jobsDirectory()] {
        if (cancel->load())
            return;
        // 前回の作業 directory の残り (強制終了など) を消す。この cache directory は Project
        // ごとに分けてあり、権限 (Project lock) があるので、他の instance が使っていない。
        std::error_code error;
        std::filesystem::remove_all(directory / L"jobs", error);
        auto result = preflight(jobs / L"preflight", cancel.get());
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)]() mutable {
                finishPreflight(generation, std::move(result));
            },
            Qt::QueuedConnection);
    });
}

void MathRasterCache::finishPreflight(std::uint64_t generation, math::MathPreflightResult result) {
    if (generation != preflightGeneration_ || shutDown_)
        return;
    if (result.status == math::MathPreflightStatus::Available && result.backend.render) {
        backendState_ = BackendState::Available;
        backend_ = std::move(result.backend);
        backendMessage_.clear();
    } else {
        backendState_ = BackendState::Unavailable;
        backendMessage_ = QString::fromStdString(result.message);
        qWarning("数式の描画 backend を使えません: %s", qUtf8Printable(backendMessage_));
    }
    Q_EMIT entryChanged(QString());
}

QString MathRasterCache::toolchainText() const {
    return backendState_ == BackendState::Available
               ? QString::fromStdString(backend_.fingerprint.canonical)
               : QString();
}

QString MathRasterCache::keyFor(const math::MathRenderSpec& spec) const {
    if (backendState_ != BackendState::Available)
        return {};
    return QString::fromStdString(math::mathRenderKey(spec, backend_.fingerprint));
}

MathRasterCache::Entry MathRasterCache::request(const math::MathRenderSpec& spec) {
    if (backendState_ == BackendState::Checking)
        return {};
    if (backendState_ == BackendState::Unavailable) {
        Entry entry;
        entry.state = State::Unavailable;
        entry.status = math::MathRenderStatus::BackendUnavailable;
        entry.message = backendMessage_;
        return entry;
    }
    const QString key = keyFor(spec);
    if (key.isEmpty()) {
        Entry entry;
        entry.state = State::Failed;
        entry.message = QStringLiteral("数式の cache key を計算できません");
        return entry;
    }
    if (const auto found = records_.constFind(key); found != records_.constEnd())
        return found->entry;
    if (shutDown_)
        return {};

    Record record;
    record.ticket = nextTicket_++;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    records_.insert(key, record);
    pool_.start([this, key, spec, ticket = record.ticket, cancel = record.cancel,
                 backend = backend_, directory = cacheDirectory_, jobs = jobsDirectory(),
                 timeout = renderTimeout_] {
        if (cancel->load())
            return;
        Outcome outcome =
            renderJob(directory, jobs, key, spec, backend, timeout, ticket, cancel.get());
        if (outcome.cancelled)
            return;
        // this は destructor が waitForDone するまで生きている。queued call は this の
        // 破棄時に Qt が捨てる。
        QMetaObject::invokeMethod(
            this,
            [this, key, ticket, entry = std::move(outcome.entry),
             artifact = std::move(outcome.artifact)]() mutable {
                finishRender(key, ticket, std::move(entry), std::move(artifact));
            },
            Qt::QueuedConnection);
    });
    return record.entry;
}

void MathRasterCache::finishRender(const QString& key, std::uint64_t ticket, Entry entry,
                                   std::filesystem::path artifact) {
    auto found = records_.find(key);
    // 要求されなくなった (retainOnly で捨てた) key の結果は残さない。
    if (found == records_.end() || found->ticket != ticket || shutDown_)
        return;
    if (entry.state != State::Ready)
        qWarning("数式を描けません: %s", qUtf8Printable(entry.message));
    found->entry = std::move(entry);
    found->artifact = std::move(artifact);
    found->cancel.reset();
    Q_EMIT entryChanged(key);
}

std::optional<std::filesystem::path>
MathRasterCache::readyArtifact(const math::MathRenderSpec& spec) const {
    const auto found = records_.constFind(keyFor(spec));
    if (found == records_.constEnd() || found->entry.state != State::Ready)
        return std::nullopt;
    return found->artifact;
}

void MathRasterCache::retainOnly(const QSet<QString>& keys) {
    for (auto it = records_.begin(); it != records_.end();) {
        if (keys.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = records_.erase(it);
    }
}

void MathRasterCache::forgetFailures() {
    for (auto it = records_.begin(); it != records_.end();) {
        if (it->entry.state == State::Failed || it->entry.state == State::Unavailable)
            it = records_.erase(it);
        else
            ++it;
    }
}

} // namespace mvm::app
