#include "math_raster_cache.h"

#include "media/still_image/static_image.h"
#include "util/mvm_atomic_write.h"

#include <QMetaObject>

#include <cwchar>
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

// ---- Write の連番 ----
//
// disk の形: cacheDirectory/write/<key>/00000.png ... と cacheDirectory/write/<key>.txt
// (provenance)。古い provenance を消してから PNG を <key>/ へ写し、provenance を最後に atomic に
// 書く。provenance が無い・合わないものは使わず消す (途中で止まった結果を引かない)。

using SequenceEntry = MathRasterCache::SequenceEntry;

std::filesystem::path sequenceRoot(const std::filesystem::path& directory) {
    return directory / L"write";
}

std::filesystem::path sequenceDirectory(const std::filesystem::path& directory, const QString& key) {
    return sequenceRoot(directory) / key.toStdWString();
}

std::filesystem::path sequenceProvenancePath(const std::filesystem::path& directory,
                                             const QString& key) {
    return sequenceRoot(directory) / (key.toStdWString() + L".txt");
}

std::filesystem::path sequenceFramePath(const std::filesystem::path& sequence, std::size_t index) {
    wchar_t name[32] = {};
    std::swprintf(name, std::size(name), L"%05zu.png", index);
    return sequence / name;
}

std::string sequenceProvenanceText(const QString& key, int width, int height,
                                   const std::vector<std::uintmax_t>& sizes,
                                   const std::string& sequenceTemplate,
                                   const math::MathToolchainFingerprint& toolchain) {
    std::string text = std::string(MathRasterCache::kSequenceArtifactFormat) +
                       "\nkey=" + key.toStdString() + "\nwidth=" + std::to_string(width) +
                       "\nheight=" + std::to_string(height) +
                       "\nframes=" + std::to_string(sizes.size()) + "\nsizes=";
    for (std::size_t index = 0; index < sizes.size(); ++index)
        text += (index == 0 ? "" : ",") + std::to_string(sizes[index]);
    text += "\nsequence_template=" + sequenceTemplate + "\ntoolchain:\n" + toolchain.canonical;
    return text;
}

void removeSequenceArtifact(const std::filesystem::path& directory, const QString& key) {
    std::error_code error;
    std::filesystem::remove(sequenceProvenancePath(directory, key), error);
    std::filesystem::remove_all(sequenceDirectory(directory, key), error);
}

// PNG の alpha だけを取り出す (glyph の被覆)。大きさが違えば空。
std::vector<std::uint8_t> decodeCoverage(const std::filesystem::path& png, int width, int height) {
    auto decoded = media::loadStaticImage(png);
    if (!decoded.success || decoded.image.width != width || decoded.image.height != height)
        return {};
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height));
    for (std::size_t index = 0; index < coverage.size(); ++index)
        coverage[index] = decoded.image.rgba[index * 4U + 3U];
    return coverage;
}

struct SequenceOutcome {
    SequenceEntry entry;
    MathSequenceArtifact artifact;
    bool cancelled = false;
};

SequenceOutcome sequenceFailed(math::MathRenderStatus status, const std::string& message,
                               const std::string& log = {}) {
    SequenceOutcome outcome;
    outcome.entry.state = status == math::MathRenderStatus::BackendUnavailable
                              ? State::Unavailable
                              : State::Failed;
    outcome.entry.status = status;
    outcome.entry.message = QString::fromStdString(message);
    outcome.entry.log = QString::fromStdString(log);
    return outcome;
}

// disk に揃った連番 (書き出しに使える状態)。frame は decode しない: preview 用の mask は
// residentSequence が必要になったときに読む (全 clip の連番を memory に置かない)。
SequenceOutcome readySequenceOutcome(const std::filesystem::path& sequence, std::size_t count,
                                     int width, int height) {
    SequenceOutcome outcome;
    outcome.artifact.width = width;
    outcome.artifact.height = height;
    for (std::size_t index = 0; index < count; ++index)
        outcome.artifact.frames.push_back(sequenceFramePath(sequence, index));
    outcome.entry.state = State::Ready;
    outcome.entry.status = math::MathRenderStatus::Ok;
    outcome.entry.width = width;
    outcome.entry.height = height;
    return outcome;
}

// artifact の frame を読み、preview 用の mask にする。どれか 1 枚でも読めない・大きさが違えば
// nullptr (呼び出し側が artifact を消して描き直させる)。取消なら nullptr で cancelled を立てる。
std::shared_ptr<MathCoverageSequence> decodeSequence(const MathSequenceArtifact& artifact,
                                                     const std::atomic<bool>* cancel,
                                                     bool& cancelled) {
    cancelled = false;
    auto coverage = std::make_shared<MathCoverageSequence>();
    coverage->width = artifact.width;
    coverage->height = artifact.height;
    coverage->frames.reserve(artifact.frames.size());
    for (const auto& frame : artifact.frames) {
        if (cancel && cancel->load()) {
            cancelled = true;
            return nullptr;
        }
        auto decoded = decodeCoverage(frame, artifact.width, artifact.height);
        if (decoded.empty())
            return nullptr;
        coverage->frames.push_back(std::move(decoded));
    }
    return coverage;
}

// disk の連番を確かめる (provenance の全体と各 frame の byte 数)。合わなければ消して nullopt。
std::optional<SequenceOutcome> loadSequenceArtifact(const std::filesystem::path& directory,
                                                    const QString& key,
                                                    const math::MathRenderBackend& backend,
                                                    std::int64_t expectedFrames) {
    const std::string provenance = readFile(sequenceProvenancePath(directory, key));
    if (provenance.empty())
        return std::nullopt;
    std::istringstream lines(provenance);
    std::string format, keyLine, widthLine, heightLine, framesLine, sizesLine;
    std::getline(lines, format);
    std::getline(lines, keyLine);
    std::getline(lines, widthLine);
    std::getline(lines, heightLine);
    std::getline(lines, framesLine);
    std::getline(lines, sizesLine);
    int width = 0;
    int height = 0;
    std::vector<std::uintmax_t> sizes;
    try {
        if (widthLine.rfind("width=", 0) == 0)
            width = std::stoi(widthLine.substr(6));
        if (heightLine.rfind("height=", 0) == 0)
            height = std::stoi(heightLine.substr(7));
        if (sizesLine.rfind("sizes=", 0) == 0) {
            std::istringstream values(sizesLine.substr(6));
            std::string value;
            while (std::getline(values, value, ','))
                sizes.push_back(std::stoull(value));
        }
    } catch (...) {
        width = 0;
    }
    const auto sequence = sequenceDirectory(directory, key);
    // provenance 全体を期待する正準形と比べる (版・key・大きさ・枚数・script・toolchain)。
    bool valid = format == MathRasterCache::kSequenceArtifactFormat && width > 0 && height > 0 &&
                 static_cast<std::int64_t>(sizes.size()) == expectedFrames &&
                 provenance == sequenceProvenanceText(key, width, height, sizes,
                                                      backend.sequenceTemplate,
                                                      backend.fingerprint);
    for (std::size_t index = 0; valid && index < sizes.size(); ++index) {
        std::error_code error;
        const auto size = std::filesystem::file_size(sequenceFramePath(sequence, index), error);
        valid = !error && size == sizes[index];
    }
    if (valid)
        return readySequenceOutcome(sequence, sizes.size(), width, height);
    removeSequenceArtifact(directory, key);
    return std::nullopt;
}

SequenceOutcome renderSequenceJob(const std::filesystem::path& directory,
                                  const std::filesystem::path& jobs, const QString& key,
                                  const math::MathSequenceSpec& spec,
                                  const math::MathRenderBackend& backend,
                                  std::chrono::milliseconds timeout, std::uint64_t ticket,
                                  const std::atomic<bool>* cancel) {
    if (auto loaded = loadSequenceArtifact(directory, key, backend, spec.frames))
        return std::move(*loaded);
    if (cancel->load()) {
        SequenceOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (!backend.renderSequence)
        return sequenceFailed(math::MathRenderStatus::Failed,
                              "数式の描画 backend は Write の連番を描けません");

    math::MathSequenceRenderRequest request;
    request.spec = spec;
    request.timeout = timeout;
    request.jobDirectory = jobs / (key.toStdWString() + L"-write-" + std::to_wstring(ticket));
    std::error_code error;
    std::filesystem::remove_all(request.jobDirectory, error);
    std::filesystem::create_directories(request.jobDirectory, error);
    if (error)
        return sequenceFailed(math::MathRenderStatus::Failed,
                              "数式の作業 directory を作成できません: " + error.message());
    const auto rendered = backend.renderSequence(request, cancel);
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(request.jobDirectory, ignored);
    };
    if (rendered.status == math::MathRenderStatus::Cancelled || cancel->load()) {
        cleanup();
        SequenceOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (rendered.status != math::MathRenderStatus::Ok) {
        cleanup();
        return sequenceFailed(rendered.status, rendered.message, rendered.log);
    }
    if (static_cast<std::int64_t>(rendered.frames.size()) != spec.frames ||
        rendered.width <= 0 || rendered.height <= 0) {
        cleanup();
        return sequenceFailed(math::MathRenderStatus::Failed,
                              "Write の連番の枚数または大きさが要求と違います", rendered.log);
    }

    // 作業 directory の PNG を <key>/ へ写し、provenance を最後に書く。provenance がこの連番の
    // 確定の印なので、directory の rename で置き換えない。Windows では書いた直後の file を
    // 他の process (anti-virus・索引など) が開いていると directory の rename が失敗する
    // (実測: 試験の 15 回に 1 回 "Permission denied")。先に古い provenance を消すので、
    // 書き終えるまでこの key の連番は使われない。
    const auto sequence = sequenceDirectory(directory, key);
    std::filesystem::remove(sequenceProvenancePath(directory, key), error);
    if (!error)
        std::filesystem::create_directories(sequence, error);
    std::vector<std::uintmax_t> sizes;
    for (std::size_t index = 0; !error && index < rendered.frames.size(); ++index) {
        std::filesystem::copy_file(rendered.frames[index], sequenceFramePath(sequence, index),
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (!error)
            sizes.push_back(
                std::filesystem::file_size(sequenceFramePath(sequence, index), error));
        if (!error && sizes.back() == 0)
            error = std::make_error_code(std::errc::io_error);
    }
    std::string writeError;
    if (error || !writeAtomically(sequenceProvenancePath(directory, key),
                                  sequenceProvenanceText(key, rendered.width, rendered.height,
                                                         sizes, backend.sequenceTemplate,
                                                         backend.fingerprint),
                                  writeError)) {
        cleanup();
        removeSequenceArtifact(directory, key);
        return sequenceFailed(math::MathRenderStatus::Failed,
                              "Write の連番を cache へ保存できません: " +
                                  (error ? error.message() : writeError),
                              rendered.log);
    }
    cleanup();
    auto outcome = readySequenceOutcome(sequence, sizes.size(), rendered.width, rendered.height);
    outcome.entry.log = QString::fromStdString(rendered.log);
    return outcome;
}

} // namespace

MathRasterCache::MathRasterCache(std::string sessionId, PreflightFunction preflight,
                                 QObject* parent)
    : QObject(parent), sessionId_(std::move(sessionId)), preflight_(std::move(preflight)) {
    // Manim / LaTeX を同時に走らせない。preflight と描画もこの順に並ぶ。
    pool_.setMaxThreadCount(1);
    // preview 用の mask の読み込み (PNG の decode だけ) も 1 本で順に行う。
    residentPool_.setMaxThreadCount(1);
    notifier_->target = this;
    residency_ = makeResidencyBudget(kDefaultResidentMemoryBudget);
}

MathRasterCache::~MathRasterCache() {
    {
        // preview engine が後から mask を手放しても、破棄した cache へ知らせない。
        std::lock_guard lock(notifier_->mutex);
        notifier_->target = nullptr;
    }
    shutdown();
}

std::shared_ptr<MathResidencyBudget>
MathRasterCache::makeResidencyBudget(std::size_t bytes) const {
    return std::make_shared<MathResidencyBudget>(bytes, [notifier = notifier_] {
        std::lock_guard lock(notifier->mutex);
        if (notifier->target)
            QMetaObject::invokeMethod(
                notifier->target, [target = notifier->target] { target->residencyReleased(); },
                Qt::QueuedConnection);
    });
}

void MathRasterCache::residencyReleased() {
    if (shutDown_ || overBudget_.isEmpty())
        return;
    const auto keys = overBudget_.keys();
    overBudget_.clear();
    for (const auto& key : keys)
        Q_EMIT entryChanged(key);
}

void MathRasterCache::shutdown() {
    shutDown_ = true;
    cancelAll();
    pool_.clear();
    residentPool_.clear();
    pool_.waitForDone();
    residentPool_.waitForDone();
}

void MathRasterCache::cancelAll() {
    if (preflightCancel_)
        preflightCancel_->store(true);
    for (const auto& record : std::as_const(records_)) {
        if (record.cancel)
            record.cancel->store(true);
    }
    for (const auto& record : std::as_const(sequences_)) {
        if (record.cancel)
            record.cancel->store(true);
    }
    for (const auto& record : std::as_const(loading_))
        record.cancel->store(true);
}

void MathRasterCache::clearRecords() {
    records_.clear();
    sequences_.clear();
    resident_.clear();
    loading_.clear();
    overBudget_.clear();
    heldResident_.clear();
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
    clearRecords();
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
    clearRecords();
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

namespace {

template <typename Records> void retainKeys(Records& records, const QSet<QString>& keys) {
    for (auto it = records.begin(); it != records.end();) {
        if (keys.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = records.erase(it);
    }
}

template <typename Records> void forgetFailedRecords(Records& records) {
    for (auto it = records.begin(); it != records.end();) {
        if (it->entry.state == State::Failed || it->entry.state == State::Unavailable)
            it = records.erase(it);
        else
            ++it;
    }
}

} // namespace

void MathRasterCache::retainOnly(const QSet<QString>& keys) {
    retainKeys(records_, keys);
    retainKeys(sequences_, keys);
    // 連番が要求されなくなれば、preview 用の mask も手放す (読んでいる途中なら止める)。
    for (const auto& key : resident_.keys() + loading_.keys() + overBudget_.keys())
        if (!keys.contains(key))
            dropResident(key);
}

void MathRasterCache::forgetFailures() {
    forgetFailedRecords(records_);
    forgetFailedRecords(sequences_);
    overBudget_.clear();
}

void MathRasterCache::dropResident(const QString& key) {
    resident_.remove(key);
    overBudget_.remove(key);
    if (const auto found = loading_.find(key); found != loading_.end()) {
        found->cancel->store(true);
        loading_.erase(found);
    }
}

void MathRasterCache::setResidentMemoryBudget(std::size_t bytes) {
    // 新しい上限は新しい予約から数える。今ある mask は cache から外し、参照が無くなると
    // 古い予約へ返る (古い予約と新しい予約を混ぜない)。
    for (const auto& key : resident_.keys() + loading_.keys())
        dropResident(key);
    overBudget_.clear();
    residency_ = makeResidencyBudget(bytes);
}

void MathRasterCache::holdResidentLoadsForTest(bool hold) {
    holdResident_ = hold;
    if (hold)
        return;
    auto held = std::move(heldResident_);
    heldResident_.clear();
    for (auto& item : held)
        finishResident(item.key, item.ticket, std::move(item.frames), std::move(item.error));
}

MathRasterCache::ResidentSequence
MathRasterCache::residencyOf(const math::MathSequenceSpec& spec) const {
    ResidentSequence result;
    const QString key = sequenceKeyFor(spec);
    const auto record = sequences_.constFind(key);
    if (key.isEmpty() || record == sequences_.constEnd() || record->entry.state != State::Ready)
        return result;
    if (const auto found = resident_.constFind(key); found != resident_.constEnd()) {
        result.state = Residency::Resident;
        result.frames = found->frames;
    } else if (loading_.contains(key)) {
        result.state = Residency::Loading;
    } else if (const auto refused = overBudget_.constFind(key); refused != overBudget_.constEnd()) {
        result.state = Residency::OverBudget;
        result.message = *refused;
    }
    return result;
}

MathRasterCache::ResidentSequence
MathRasterCache::residentSequence(const math::MathSequenceSpec& spec) {
    ResidentSequence result;
    const QString key = sequenceKeyFor(spec);
    const auto record = sequences_.constFind(key);
    if (key.isEmpty() || record == sequences_.constEnd() || record->entry.state != State::Ready ||
        shutDown_)
        return result;
    if (const auto found = resident_.find(key); found != resident_.end()) {
        found->lastUse = ++useTick_;
        result.state = Residency::Resident;
        result.frames = found->frames;
        return result;
    }
    if (loading_.contains(key)) {
        result.state = Residency::Loading;
        return result;
    }
    const auto& artifact = record->artifact;
    const auto bytes = static_cast<std::size_t>(artifact.frames.size()) *
                       static_cast<std::size_t>(artifact.width) *
                       static_cast<std::size_t>(artifact.height);
    // 予約できるまで、最も長く使っていない mask から cache の参照を外す。外すのは cache だけが
    // 持つ mask に限る。preview (合成中の animation や engine) が使っている mask は外しても
    // memory に残る (予約も返らない) うえ、次の合成で読み直しになり、同じ frame の clip どうしで
    // 追い出し合う。
    auto reserved = residency_->tryReserve(bytes);
    while (!reserved) {
        auto oldest = resident_.end();
        for (auto it = resident_.begin(); it != resident_.end(); ++it)
            if (it->frames.use_count() == 1 &&
                (oldest == resident_.end() || it->lastUse < oldest->lastUse))
                oldest = it;
        if (oldest == resident_.end())
            break;
        resident_.erase(oldest);
        reserved = residency_->tryReserve(bytes);
    }
    if (!reserved) {
        const auto message =
            QStringLiteral("Write の preview 用の mask が memory の上限 (全 clip の合計 %1 MB) に"
                           "収まらないため、書き終えた式で表示します (書き出しには影響しません)。"
                           "必要 %2 MB、使用中 %3 MB")
                .arg(residency_->limit() >> 20)
                .arg(bytes >> 20)
                .arg(residency_->live() >> 20);
        overBudget_.insert(key, message);
        result.state = Residency::OverBudget;
        result.message = message;
        return result;
    }
    overBudget_.remove(key);
    auto reservation = std::make_shared<const MathResidencyReservation>(residency_, bytes);
    LoadingRecord loading;
    loading.ticket = nextTicket_++;
    loading.cancel = std::make_shared<std::atomic<bool>>(false);
    loading_.insert(key, loading);
    ++residentLoads_;
    residentPool_.start([this, key, artifact, reservation, ticket = loading.ticket,
                         cancel = loading.cancel]() mutable {
        if (cancel->load())
            return;
        bool cancelled = false;
        auto decoded = decodeSequence(artifact, cancel.get(), cancelled);
        if (cancelled)
            return;
        if (decoded)
            decoded->reservation = std::move(reservation);
        QString error;
        if (!decoded)
            error = QStringLiteral("Write の連番の PNG を読めないか、大きさが provenance と"
                                   "違います。描き直します");
        QMetaObject::invokeMethod(
            this,
            [this, key, ticket, frames = std::shared_ptr<const MathCoverageSequence>(decoded),
             error]() mutable { finishResident(key, ticket, std::move(frames), error); },
            Qt::QueuedConnection);
    });
    result.state = Residency::Loading;
    return result;
}

void MathRasterCache::finishResident(const QString& key, std::uint64_t ticket,
                                     std::shared_ptr<const MathCoverageSequence> frames,
                                     QString error) {
    const auto found = loading_.find(key);
    // 取り消した (要求されなくなった・上限を変えた) 読み込みの結果は残さない。
    // frames を捨てると予約も返る。
    if (found == loading_.end() || found->ticket != ticket || shutDown_)
        return;
    if (holdResident_) {
        heldResident_.push_back({key, ticket, std::move(frames), std::move(error)});
        return;
    }
    loading_.erase(found);
    if (!frames) {
        // disk の連番が壊れている。消して、連番を Failed にする (forgetFailures・再試行で描き直す)。
        // cache directory の変更は権限がある間だけ行う。
        if (authorized_)
            removeSequenceArtifact(cacheDirectory_, key);
        if (auto record = sequences_.find(key); record != sequences_.end()) {
            record->entry = {};
            record->entry.state = State::Failed;
            record->entry.message = error;
            record->artifact = {};
        }
        qWarning("%s", qUtf8Printable(error));
    } else {
        resident_.insert(key, {std::move(frames), ++useTick_});
    }
    Q_EMIT entryChanged(key);
}

QString MathRasterCache::sequenceKeyFor(const math::MathSequenceSpec& spec) const {
    if (backendState_ != BackendState::Available)
        return {};
    return QString::fromStdString(
        math::mathSequenceKey(spec, backend_.fingerprint, backend_.sequenceTemplate));
}

MathRasterCache::SequenceEntry
MathRasterCache::requestSequence(const math::MathSequenceSpec& spec) {
    if (backendState_ == BackendState::Checking)
        return {};
    if (backendState_ == BackendState::Unavailable) {
        SequenceEntry entry;
        entry.state = State::Unavailable;
        entry.status = math::MathRenderStatus::BackendUnavailable;
        entry.message = backendMessage_;
        return entry;
    }
    const QString key = sequenceKeyFor(spec);
    if (key.isEmpty()) {
        SequenceEntry entry;
        entry.state = State::Failed;
        entry.message = QStringLiteral("数式の連番の cache key を計算できません");
        return entry;
    }
    if (const auto found = sequences_.constFind(key); found != sequences_.constEnd())
        return found->entry;
    if (shutDown_)
        return {};

    SequenceRecord record;
    // backend が描けない長さは描かずに未対応として失敗させる (Project の値は正しいまま)。
    if (!backend_.renderSequence || spec.frames < 1 ||
        spec.frames > backend_.maximumSequenceFrames) {
        record.entry.state = State::Failed;
        record.entry.status = math::MathRenderStatus::Failed;
        record.entry.message =
            backend_.renderSequence
                ? QStringLiteral("この描画環境の Write は %1 frame までです (要求 %2 frame)。"
                                 "Write を短くしてください")
                      .arg(backend_.maximumSequenceFrames)
                      .arg(spec.frames)
                : QStringLiteral("数式の描画 backend は Write の連番を描けません");
        sequences_.insert(key, record);
        return record.entry;
    }
    record.ticket = nextTicket_++;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    sequences_.insert(key, record);
    const auto timeout = renderTimeout_ + sequenceTimeoutPerFrame_ * spec.frames;
    pool_.start([this, key, spec, ticket = record.ticket, cancel = record.cancel,
                 backend = backend_, directory = cacheDirectory_, jobs = jobsDirectory(),
                 timeout] {
        if (cancel->load())
            return;
        SequenceOutcome outcome = renderSequenceJob(directory, jobs, key, spec, backend, timeout,
                                                    ticket, cancel.get());
        if (outcome.cancelled)
            return;
        QMetaObject::invokeMethod(
            this,
            [this, key, ticket, entry = std::move(outcome.entry),
             artifact = std::move(outcome.artifact)]() mutable {
                finishSequence(key, ticket, std::move(entry), std::move(artifact));
            },
            Qt::QueuedConnection);
    });
    return record.entry;
}

void MathRasterCache::finishSequence(const QString& key, std::uint64_t ticket, SequenceEntry entry,
                                     MathSequenceArtifact artifact) {
    auto found = sequences_.find(key);
    // 要求されなくなった (retainOnly・取消で捨てた) key の結果は残さない。
    if (found == sequences_.end() || found->ticket != ticket || shutDown_)
        return;
    if (entry.state != State::Ready)
        qWarning("数式の Write を描けません: %s", qUtf8Printable(entry.message));
    found->entry = std::move(entry);
    found->artifact = std::move(artifact);
    found->cancel.reset();
    Q_EMIT entryChanged(key);
}

std::optional<MathSequenceArtifact>
MathRasterCache::readySequence(const math::MathSequenceSpec& spec) const {
    const auto found = sequences_.constFind(sequenceKeyFor(spec));
    if (found == sequences_.constEnd() || found->entry.state != State::Ready)
        return std::nullopt;
    return found->artifact;
}

void MathRasterCache::cancelPendingSequences() {
    for (auto it = sequences_.begin(); it != sequences_.end();) {
        if (it->entry.state != State::Pending) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = sequences_.erase(it);
    }
}

} // namespace mvm::app
