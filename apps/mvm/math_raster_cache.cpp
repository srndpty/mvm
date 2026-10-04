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
// (provenance)。描いた PNG は write/<key>.partial-<ticket>/ へ置いてから <key>/ へ rename し、
// provenance を最後に atomic に書く。provenance が無い・合わないものは使わず消す
// (途中で止まった結果を引かない)。

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

// 連番の mask を memory に持てるか。持てなければ描けても使わない (preview は全 frame を持つ)。
std::optional<SequenceOutcome> overBudget(std::size_t count, int width, int height,
                                          std::size_t budget) {
    const auto bytes = static_cast<unsigned long long>(count) *
                       static_cast<unsigned long long>(width) *
                       static_cast<unsigned long long>(height);
    if (bytes <= budget)
        return std::nullopt;
    return sequenceFailed(math::MathRenderStatus::Failed,
                          "Write の連番が memory の上限 (" + std::to_string(budget >> 20) +
                              " MB) を超えます (" + std::to_string(bytes >> 20) +
                              " MB)。Write を短くするか、文字サイズを下げてください");
}

// sequence directory の frame を読み、mask と artifact にする。どれか 1 枚でも読めない・
// 大きさが違えば失敗 (呼び出し側が消す)。
bool readSequenceFrames(const std::filesystem::path& sequence, std::size_t count, int width,
                        int height, const std::atomic<bool>* cancel, SequenceOutcome& outcome) {
    auto coverage = std::make_shared<MathCoverageSequence>();
    coverage->width = width;
    coverage->height = height;
    coverage->frames.reserve(count);
    outcome.artifact = {};
    outcome.artifact.width = width;
    outcome.artifact.height = height;
    for (std::size_t index = 0; index < count; ++index) {
        if (cancel && cancel->load())
            return false;
        const auto frame = sequenceFramePath(sequence, index);
        auto decoded = decodeCoverage(frame, width, height);
        if (decoded.empty())
            return false;
        coverage->frames.push_back(std::move(decoded));
        outcome.artifact.frames.push_back(frame);
    }
    outcome.entry.state = State::Ready;
    outcome.entry.status = math::MathRenderStatus::Ok;
    outcome.entry.frames = std::move(coverage);
    return true;
}

std::optional<SequenceOutcome> loadSequenceArtifact(const std::filesystem::path& directory,
                                                    const QString& key,
                                                    const math::MathRenderBackend& backend,
                                                    std::int64_t expectedFrames,
                                                    std::size_t memoryBudget,
                                                    const std::atomic<bool>* cancel) {
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
    // 正しい artifact でも持てない大きさなら使わない (消さない: 上限を変えれば使える)。
    if (valid)
        if (auto rejected = overBudget(sizes.size(), width, height, memoryBudget))
            return rejected;
    SequenceOutcome outcome;
    if (valid && readSequenceFrames(sequence, sizes.size(), width, height, cancel, outcome))
        return outcome;
    if (cancel && cancel->load()) {
        outcome = {};
        outcome.cancelled = true;
        return outcome;
    }
    removeSequenceArtifact(directory, key);
    return std::nullopt;
}

SequenceOutcome renderSequenceJob(const std::filesystem::path& directory,
                                  const std::filesystem::path& jobs, const QString& key,
                                  const math::MathSequenceSpec& spec,
                                  const math::MathRenderBackend& backend,
                                  std::chrono::milliseconds timeout, std::size_t memoryBudget,
                                  std::uint64_t ticket, const std::atomic<bool>* cancel) {
    if (auto loaded =
            loadSequenceArtifact(directory, key, backend, spec.frames, memoryBudget, cancel))
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
    const auto staging = sequenceRoot(directory) /
                         (key.toStdWString() + L".partial-" + std::to_wstring(ticket));
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(request.jobDirectory, ignored);
        std::filesystem::remove_all(staging, ignored);
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

    // 作業 directory の PNG を staging へ写してから、まとめて <key>/ へ移す。
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging, error);
    std::vector<std::uintmax_t> sizes;
    for (std::size_t index = 0; !error && index < rendered.frames.size(); ++index) {
        std::filesystem::copy_file(rendered.frames[index], sequenceFramePath(staging, index),
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (!error)
            sizes.push_back(
                std::filesystem::file_size(sequenceFramePath(staging, index), error));
        if (!error && sizes.back() == 0)
            error = std::make_error_code(std::errc::io_error);
    }
    const auto sequence = sequenceDirectory(directory, key);
    if (!error) {
        removeSequenceArtifact(directory, key);
        std::filesystem::rename(staging, sequence, error);
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
    if (auto rejected = overBudget(sizes.size(), rendered.width, rendered.height, memoryBudget)) {
        rejected->entry.log = QString::fromStdString(rendered.log);
        return std::move(*rejected);
    }
    SequenceOutcome outcome;
    if (!readSequenceFrames(sequence, sizes.size(), rendered.width, rendered.height, cancel,
                            outcome)) {
        if (cancel->load()) {
            outcome = {};
            outcome.cancelled = true;
            return outcome;
        }
        removeSequenceArtifact(directory, key);
        return sequenceFailed(math::MathRenderStatus::Failed,
                              "描いた Write の連番を読めないか、大きさが報告と違います",
                              rendered.log);
    }
    outcome.entry.log = QString::fromStdString(rendered.log);
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
    for (const auto& record : std::as_const(sequences_)) {
        if (record.cancel)
            record.cancel->store(true);
    }
}

void MathRasterCache::clearRecords() {
    records_.clear();
    sequences_.clear();
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
        // 連番の staging の残り (provenance を書く前に止まったもの) も同じ理由で消す。
        for (std::filesystem::directory_iterator it(sequenceRoot(directory), error), end;
             !error && it != end; it.increment(error)) {
            if (it->path().filename().wstring().find(L".partial-") != std::wstring::npos) {
                std::error_code ignored;
                std::filesystem::remove_all(it->path(), ignored);
            }
        }
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
}

void MathRasterCache::forgetFailures() {
    forgetFailedRecords(records_);
    forgetFailedRecords(sequences_);
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
    record.ticket = nextTicket_++;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    sequences_.insert(key, record);
    const auto timeout = renderTimeout_ + sequenceTimeoutPerFrame_ * spec.frames;
    pool_.start([this, key, spec, ticket = record.ticket, cancel = record.cancel,
                 backend = backend_, directory = cacheDirectory_, jobs = jobsDirectory(),
                 timeout, budget = sequenceMemoryBudget_] {
        if (cancel->load())
            return;
        SequenceOutcome outcome = renderSequenceJob(directory, jobs, key, spec, backend, timeout,
                                                    budget, ticket, cancel.get());
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
