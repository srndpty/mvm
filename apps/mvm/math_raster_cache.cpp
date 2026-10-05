#include "math_raster_cache.h"

#include "app/math_clip_render.h"
#include "media/still_image/static_image.h"
#include "util/mvm_atomic_write.h"
#include "util/mvm_sha256.h"

#include <QMetaObject>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
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
             const math::MathToolchainFingerprint& toolchain, bool removeInvalid = true) {
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
        if (removeInvalid)
            removeArtifact(directory, key);
        return nullptr;
    }
    auto mask = decodeMask(artifactPath(directory, key), width, height);
    if (!mask && removeInvalid)
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

// ---- 式から式への変形 ----
//
// disk の形: cacheDirectory/transform/<key>/00000.a8 ... と cacheDirectory/transform/<key>.txt
// (provenance)。frame は backend の一時的な canvas から artifact の矩形だけを切り出した被覆
// (1 画素 1 byte の生の byte 列)。PNG にしないのは、この層に PNG の encoder が無く (画素の decode は
// 静止画 decoder だけを通す)、被覆を byte 単位でそのまま残せるため。中身は SHA-256 で照合する。
// 公開の手順は Write と同じ: 古い provenance を消し、frame を書き、provenance を最後に書く
// (provenance があり、正確に合うことが確定の印。directory の rename は使わない)。

using TransformEntry = MathRasterCache::TransformEntry;

std::filesystem::path transformRoot(const std::filesystem::path& directory) {
    return directory / L"transform";
}

std::filesystem::path transformDirectory(const std::filesystem::path& directory, const QString& key) {
    return transformRoot(directory) / key.toStdWString();
}

std::filesystem::path transformProvenancePath(const std::filesystem::path& directory,
                                              const QString& key) {
    return transformRoot(directory) / (key.toStdWString() + L".txt");
}

std::string transformFrameName(std::size_t index) {
    char name[32] = {};
    std::snprintf(name, sizeof name, "%05zu.a8", index);
    return name;
}

void removeTransformArtifact(const std::filesystem::path& directory, const QString& key) {
    std::error_code error;
    std::filesystem::remove(transformProvenancePath(directory, key), error);
    std::filesystem::remove_all(transformDirectory(directory, key), error);
}

std::string sha256Hex(const void* data, std::size_t size) {
    char hex[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(data, size, hex) != 0)
        return {};
    return hex;
}

// provenance の中身。identity (key・静止の key・script・規則の版・toolchain) は読んだ値ではなく
// 期待する値で正準形を組み直し、file と byte 単位で比べる。
struct TransformProvenance {
    std::string key;
    int width = 0; // 切り出した frame の大きさ
    int height = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    math::MathRect artifact; // canvas の座標
    int sourceX = 0;         // 切り出した座標での端点の左上
    int sourceY = 0;
    int targetX = 0;
    int targetY = 0;
    int sourceWidth = 0; // 端点の静止の大きさ
    int sourceHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    std::string sourceStaticKey;
    std::string targetStaticKey;
    std::vector<std::uintmax_t> frameBytes;
    std::vector<std::string> frameSha256;
    std::string transformTemplate;
    std::string segmenter;
    std::string matching;
    std::string toolchain;
};

std::string transformProvenanceText(const TransformProvenance& p) {
    const auto pair = [](int a, char separator, int b) {
        return std::to_string(a) + separator + std::to_string(b);
    };
    std::string text = std::string(MathRasterCache::kTransformArtifactFormat) + "\n";
    text += "key=" + p.key + "\n";
    text += "frames=" + std::to_string(p.frameSha256.size()) + "\n";
    text += "width=" + std::to_string(p.width) + "\n";
    text += "height=" + std::to_string(p.height) + "\n";
    text += "canvas=" + pair(p.canvasWidth, 'x', p.canvasHeight) + "\n";
    text += "artifact_rect=" + pair(p.artifact.x, ',', p.artifact.y) + "," +
            pair(p.artifact.width, ',', p.artifact.height) + "\n";
    text += "source_offset=" + pair(p.sourceX, ',', p.sourceY) + "\n";
    text += "target_offset=" + pair(p.targetX, ',', p.targetY) + "\n";
    text += "source_static=" + pair(p.sourceWidth, 'x', p.sourceHeight) + "\n";
    text += "target_static=" + pair(p.targetWidth, 'x', p.targetHeight) + "\n";
    text += "source_static_key=" + p.sourceStaticKey + "\n";
    text += "target_static_key=" + p.targetStaticKey + "\n";
    for (std::size_t index = 0; index < p.frameSha256.size(); ++index)
        text += "frame=" + transformFrameName(index) + " " +
                std::to_string(index < p.frameBytes.size() ? p.frameBytes[index] : 0) + " " +
                p.frameSha256[index] + "\n";
    text += "transform_template=" + p.transformTemplate + "\n";
    text += "segmenter=" + p.segmenter + "\n";
    text += "matching=" + p.matching + "\n";
    text += "toolchain:\n" + p.toolchain;
    return text;
}

// "a<sep>b<sep>..." の整数を count 個読む。形の厳密さは正準形の組み直しで確かめる。
bool readInts(const std::string& text, char separator, std::size_t count, std::vector<long long>& out) {
    out.clear();
    std::istringstream values(text);
    std::string value;
    while (std::getline(values, value, separator)) {
        try {
            std::size_t used = 0;
            out.push_back(std::stoll(value, &used));
            if (used != value.size())
                return false;
        } catch (...) {
            return false;
        }
    }
    return out.size() == count;
}

bool fitsInt(long long value) {
    return value >= 0 && value <= (1LL << 30);
}

// provenance を読み、数値の field と各 frame の行を取り出す。形が違えば false。
bool parseTransformProvenance(const std::string& text, TransformProvenance& p) {
    std::istringstream lines(text);
    std::string line;
    const auto field = [&](const char* prefix, std::string& value) {
        if (!std::getline(lines, line) || line.rfind(prefix, 0) != 0)
            return false;
        value = line.substr(std::strlen(prefix));
        return true;
    };
    std::string value;
    std::vector<long long> numbers;
    if (!std::getline(lines, line) || line != MathRasterCache::kTransformArtifactFormat)
        return false;
    if (!field("key=", p.key) || !field("frames=", value) || !readInts(value, ',', 1, numbers) ||
        numbers[0] < 1 || numbers[0] > 1000000)
        return false;
    const auto frames = static_cast<std::size_t>(numbers[0]);
    const auto ints = [&](const char* prefix, char separator, std::initializer_list<int*> out) {
        if (!field(prefix, value) || !readInts(value, separator, out.size(), numbers))
            return false;
        std::size_t at = 0;
        for (int* target : out) {
            if (!fitsInt(numbers[at]))
                return false;
            *target = static_cast<int>(numbers[at++]);
        }
        return true;
    };
    if (!ints("width=", ',', {&p.width}) || !ints("height=", ',', {&p.height}) ||
        !ints("canvas=", 'x', {&p.canvasWidth, &p.canvasHeight}) ||
        !ints("artifact_rect=", ',',
              {&p.artifact.x, &p.artifact.y, &p.artifact.width, &p.artifact.height}) ||
        !ints("source_offset=", ',', {&p.sourceX, &p.sourceY}) ||
        !ints("target_offset=", ',', {&p.targetX, &p.targetY}) ||
        !ints("source_static=", 'x', {&p.sourceWidth, &p.sourceHeight}) ||
        !ints("target_static=", 'x', {&p.targetWidth, &p.targetHeight}) ||
        !field("source_static_key=", p.sourceStaticKey) ||
        !field("target_static_key=", p.targetStaticKey))
        return false;
    for (std::size_t index = 0; index < frames; ++index) {
        if (!field("frame=", value))
            return false;
        std::istringstream parts(value);
        std::string name;
        std::string bytes;
        std::string sha;
        if (!(parts >> name >> bytes >> sha) || name != transformFrameName(index) ||
            !readInts(bytes, ',', 1, numbers) || numbers[0] < 0)
            return false;
        p.frameBytes.push_back(static_cast<std::uintmax_t>(numbers[0]));
        p.frameSha256.push_back(sha);
    }
    return true;
}

// 変形の要求から決まる、provenance に期待する値。
struct TransformExpectation {
    QString key;
    std::int64_t frames = 0;
    std::string sourceStaticKey;
    std::string targetStaticKey;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    std::string transformTemplate;
    math::MathTransformAlgorithms algorithms;
    std::string toolchain;
};

bool containsRect(int width, int height, int x, int y, int innerWidth, int innerHeight) {
    return x >= 0 && y >= 0 && innerWidth > 0 && innerHeight > 0 &&
           static_cast<long long>(x) + innerWidth <= width &&
           static_cast<long long>(y) + innerHeight <= height;
}

// provenance の数値どうしが矛盾しないか (切り出しの矩形・端点が収まり、frame の byte 数が合う)。
// 端点の位置は、両端の静止と canvas の大きさから P2-2 の配置 (mathTransformPlacement) で決まる
// 値を、切り出した座標へ移したものでなければならない。
bool transformGeometryValid(const TransformProvenance& p) {
    math::MathTransformPlacement placement;
    if (!math::mathTransformPlacement(p.sourceWidth, p.sourceHeight, p.targetWidth, p.targetHeight,
                                      p.canvasWidth, p.canvasHeight, placement) ||
        p.sourceX != placement.source.left - p.artifact.x ||
        p.sourceY != placement.source.top - p.artifact.y ||
        p.targetX != placement.target.left - p.artifact.x ||
        p.targetY != placement.target.top - p.artifact.y)
        return false;
    if (p.width <= 0 || p.height <= 0 || p.width != p.artifact.width ||
        p.height != p.artifact.height ||
        !containsRect(p.canvasWidth, p.canvasHeight, p.artifact.x, p.artifact.y, p.artifact.width,
                      p.artifact.height) ||
        !containsRect(p.width, p.height, p.sourceX, p.sourceY, p.sourceWidth, p.sourceHeight) ||
        !containsRect(p.width, p.height, p.targetX, p.targetY, p.targetWidth, p.targetHeight))
        return false;
    const auto frameBytes = static_cast<std::uintmax_t>(p.width) * static_cast<std::uintmax_t>(p.height);
    return std::all_of(p.frameBytes.begin(), p.frameBytes.end(),
                       [&](std::uintmax_t bytes) { return bytes == frameBytes; });
}

MathTransformArtifact transformArtifactFrom(const std::filesystem::path& directory,
                                            const QString& key, const TransformProvenance& p) {
    MathTransformArtifact artifact;
    const auto frames = transformDirectory(directory, key);
    for (std::size_t index = 0; index < p.frameSha256.size(); ++index)
        artifact.frames.push_back(frames / transformFrameName(index));
    artifact.frameSha256 = p.frameSha256;
    artifact.width = p.width;
    artifact.height = p.height;
    artifact.sourceX = p.sourceX;
    artifact.sourceY = p.sourceY;
    artifact.targetX = p.targetX;
    artifact.targetY = p.targetY;
    return artifact;
}

// file の中身を読む。大きさが expected でなければ false。
bool readExactly(const std::filesystem::path& path, std::uintmax_t expected,
                 std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != expected)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.resize(static_cast<std::size_t>(expected));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::uintmax_t>(input.gcount()) == expected;
}

struct TransformOutcome {
    TransformEntry entry;
    MathTransformArtifact artifact;
    bool cancelled = false;
};

TransformOutcome transformCancelled() {
    TransformOutcome outcome;
    outcome.cancelled = true;
    return outcome;
}

TransformOutcome transformFailed(math::MathRenderStatus status, const std::string& message,
                                 const std::string& log = {}) {
    TransformOutcome outcome;
    outcome.entry.state = status == math::MathRenderStatus::BackendUnavailable
                              ? State::Unavailable
                              : State::Failed;
    outcome.entry.status = status;
    outcome.entry.message = QString::fromStdString(message);
    outcome.entry.log = QString::fromStdString(log);
    return outcome;
}

TransformOutcome transformReady(MathTransformArtifact artifact) {
    TransformOutcome outcome;
    outcome.entry.state = State::Ready;
    outcome.entry.status = math::MathRenderStatus::Ok;
    outcome.entry.width = artifact.width;
    outcome.entry.height = artifact.height;
    outcome.artifact = std::move(artifact);
    return outcome;
}

// worker が 1 件の変形に使う値 (起動の時点で固定する)。
struct TransformJob {
    std::filesystem::path directory;
    std::filesystem::path jobs;
    QString key;
    math::MathTransformSpec spec;
    math::MathRenderBackend backend;
    std::chrono::milliseconds timeout{120000};
    std::uint64_t ticket = 0;
    // 両端の今の静止の mask (同じ key の静止の artifact を decode したもの)。
    math::MathCoverage sourceStatic;
    math::MathCoverage targetStatic;
    std::string sourceStaticKey;
    std::string targetStaticKey;
    std::shared_ptr<std::mutex> publishGate;
    std::function<void(const std::filesystem::path&)> beforePublish;
};

TransformExpectation expectationFor(const TransformJob& job) {
    TransformExpectation expect;
    expect.key = job.key;
    expect.frames = job.spec.frames;
    expect.sourceStaticKey = job.sourceStaticKey;
    expect.targetStaticKey = job.targetStaticKey;
    expect.sourceWidth = job.sourceStatic.width;
    expect.sourceHeight = job.sourceStatic.height;
    expect.targetWidth = job.targetStatic.width;
    expect.targetHeight = job.targetStatic.height;
    expect.transformTemplate = job.backend.transformTemplate;
    expect.toolchain = job.backend.fingerprint.canonical;
    return expect;
}

// cache directory を変える操作 (消す・確定する) を、取消と排他に行う。取り消されていれば
// 何もせず false。
bool underGate(const TransformJob& job, const std::atomic<bool>* cancel,
               const std::function<void()>& action) {
    std::lock_guard lock(*job.publishGate);
    if (cancel->load())
        return false;
    action();
    return true;
}

enum class DiskLoad { Ready, Missing, Cancelled };

// disk の変形を確かめる: provenance の正準形・期待する identity・数値の整合、各 frame の
// 大きさと SHA-256。どれかが合わなければ消して Missing (描き直させる)。
DiskLoad loadTransformArtifact(const TransformJob& job, const std::atomic<bool>* cancel,
                               MathTransformArtifact& artifact, bool removeInvalid = true) {
    const auto provenancePath = transformProvenancePath(job.directory, job.key);
    const std::string text = readFile(provenancePath);
    if (text.empty())
        return DiskLoad::Missing; // 確定の印が無い: 途中で止まった (または未作成)
    const auto expect = expectationFor(job);
    TransformProvenance parsed;
    bool valid = parseTransformProvenance(text, parsed);
    if (valid) {
        TransformProvenance canonical = parsed;
        canonical.key = expect.key.toStdString();
        canonical.sourceStaticKey = expect.sourceStaticKey;
        canonical.targetStaticKey = expect.targetStaticKey;
        canonical.transformTemplate = expect.transformTemplate;
        canonical.segmenter = expect.algorithms.segmenter;
        canonical.matching = expect.algorithms.matching;
        canonical.toolchain = expect.toolchain;
        valid = transformProvenanceText(canonical) == text &&
                static_cast<std::int64_t>(parsed.frameSha256.size()) == expect.frames &&
                parsed.sourceWidth == expect.sourceWidth &&
                parsed.sourceHeight == expect.sourceHeight &&
                parsed.targetWidth == expect.targetWidth &&
                parsed.targetHeight == expect.targetHeight && transformGeometryValid(parsed);
    }
    if (valid) {
        artifact = transformArtifactFrom(job.directory, job.key, parsed);
        std::vector<std::uint8_t> bytes;
        for (std::size_t index = 0; valid && index < artifact.frames.size(); ++index) {
            if (cancel->load())
                return DiskLoad::Cancelled;
            valid = readExactly(artifact.frames[index], parsed.frameBytes[index], bytes) &&
                    sha256Hex(bytes.data(), bytes.size()) == artifact.frameSha256[index];
            // frame 0 は今の変形前の静止と、記録した端点の位置で全画素一致する。
            if (valid && index == 0)
                valid = math::mathEndpointDifference(
                            {artifact.width, artifact.height, bytes}, job.sourceStatic,
                            artifact.sourceX, artifact.sourceY) == 0;
        }
    }
    if (valid)
        return DiskLoad::Ready;
    if (removeInvalid &&
        !underGate(job, cancel, [&] { removeTransformArtifact(job.directory, job.key); }))
        return DiskLoad::Cancelled;
    return DiskLoad::Missing;
}

TransformOutcome renderTransformJob(const TransformJob& job, const std::atomic<bool>* cancel) {
    {
        MathTransformArtifact artifact;
        switch (loadTransformArtifact(job, cancel, artifact)) {
        case DiskLoad::Ready:
            return transformReady(std::move(artifact));
        case DiskLoad::Cancelled:
            return transformCancelled();
        case DiskLoad::Missing:
            break;
        }
    }
    if (cancel->load())
        return transformCancelled();

    math::MathTransformRenderRequest request;
    request.spec = job.spec;
    request.sourceStatic = job.sourceStatic;
    request.targetStatic = job.targetStatic;
    request.timeout = job.timeout;
    request.jobDirectory =
        job.jobs / (job.key.toStdWString() + L"-transform-" + std::to_wstring(job.ticket));
    std::error_code error;
    std::filesystem::remove_all(request.jobDirectory, error);
    std::filesystem::create_directories(request.jobDirectory, error);
    if (error)
        return transformFailed(math::MathRenderStatus::Failed,
                               "数式の作業 directory を作成できません: " + error.message());
    // backend の PNG は静止の mask と同じ decoder (loadMathCoverage) で読む。
    const auto rendered = job.backend.renderTransform(request, loadMathCoverage, cancel);
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(request.jobDirectory, ignored);
    };
    if (rendered.status == math::MathRenderStatus::Cancelled || cancel->load()) {
        cleanup();
        return transformCancelled();
    }
    if (rendered.status != math::MathRenderStatus::Ok) {
        cleanup();
        return transformFailed(rendered.status, rendered.message, rendered.log);
    }

    // 切り出す矩形と、切り出した座標での端点の位置。
    const math::MathRect& rect = rendered.artifact;
    TransformProvenance p;
    p.key = job.key.toStdString();
    p.width = rect.width;
    p.height = rect.height;
    p.canvasWidth = rendered.canvasWidth;
    p.canvasHeight = rendered.canvasHeight;
    p.artifact = rect;
    p.sourceX = rendered.placement.source.left - rect.x;
    p.sourceY = rendered.placement.source.top - rect.y;
    p.targetX = rendered.placement.target.left - rect.x;
    p.targetY = rendered.placement.target.top - rect.y;
    p.sourceWidth = job.sourceStatic.width;
    p.sourceHeight = job.sourceStatic.height;
    p.targetWidth = job.targetStatic.width;
    p.targetHeight = job.targetStatic.height;
    p.sourceStaticKey = job.sourceStaticKey;
    p.targetStaticKey = job.targetStaticKey;
    p.transformTemplate = job.backend.transformTemplate;
    p.segmenter = math::MathTransformAlgorithms{}.segmenter;
    p.matching = math::MathTransformAlgorithms{}.matching;
    p.toolchain = job.backend.fingerprint.canonical;
    const auto frameBytes = static_cast<std::uintmax_t>(rect.width > 0 ? rect.width : 0) *
                            static_cast<std::uintmax_t>(rect.height > 0 ? rect.height : 0);
    p.frameBytes.assign(rendered.frames.size(), frameBytes);
    p.frameSha256.assign(rendered.frames.size(), std::string(64, '0'));
    if (static_cast<std::int64_t>(rendered.frames.size()) != job.spec.frames ||
        rendered.canvasWidth <= 0 || rendered.canvasHeight <= 0 || !transformGeometryValid(p)) {
        cleanup();
        return transformFailed(math::MathRenderStatus::Failed,
                               "変形の連番の枚数・artifact の矩形・端点の配置が要求と合いません",
                               rendered.log);
    }

    // 古い provenance を消し、frame の directory を空にしてから書く。
    const auto frames = transformDirectory(job.directory, job.key);
    const auto provenancePath = transformProvenancePath(job.directory, job.key);
    if (!underGate(job, cancel, [&] {
            std::filesystem::remove(provenancePath, error);
            std::filesystem::remove_all(frames, error);
            if (!error)
                std::filesystem::create_directories(frames, error);
        })) {
        cleanup();
        return transformCancelled();
    }
    const auto fail = [&](const std::string& message) {
        cleanup();
        underGate(job, cancel, [&] { removeTransformArtifact(job.directory, job.key); });
        return transformFailed(math::MathRenderStatus::Failed, message, rendered.log);
    };
    if (error)
        return fail("変形の連番を cache へ保存できません: " + error.message());

    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(frameBytes));
    for (std::size_t index = 0; index < rendered.frames.size(); ++index) {
        if (cancel->load()) {
            // provenance が無いので、書きかけの frame は使われない (権限を失った後は消さない)。
            cleanup();
            return transformCancelled();
        }
        const std::string name = "変形の frame " + std::to_string(index);
        math::MathCoverage canvas;
        std::string loadError;
        if (!loadMathCoverage(rendered.frames[index], canvas, loadError))
            return fail(name + " を読めません: " + loadError);
        if (canvas.width != rendered.canvasWidth || canvas.height != rendered.canvasHeight)
            return fail(name + " の大きさが canvas と違います");
        // artifact の矩形の外に被覆があれば、切り出すと画素を失う。黙って切らない。
        const math::MathRect bounds = math::mathCoverageBounds(canvas);
        if (!bounds.empty() && math::mathRectUnion(bounds, rect) != rect)
            return fail(name + " の式が artifact の矩形の外にあります");
        for (int y = 0; y < rect.height; ++y)
            std::copy_n(canvas.alpha.begin() +
                            static_cast<std::ptrdiff_t>(rect.y + y) * canvas.width + rect.x,
                        rect.width,
                        cropped.begin() + static_cast<std::ptrdiff_t>(y) * rect.width);
        // 切り出した frame 0 は、変形前の式の静止と (切り出した座標の端点の位置で) 全画素一致する。
        if (index == 0) {
            const math::MathCoverage first{rect.width, rect.height, cropped};
            const std::int64_t different =
                math::mathEndpointDifference(first, job.sourceStatic, p.sourceX, p.sourceY);
            if (different != 0)
                return fail("切り出した変形の最初の frame が変形前の式の静止と一致しません "
                            "(違う画素 " +
                            std::to_string(different) + ")");
        }
        std::string writeError;
        if (!writeAtomically(frames / transformFrameName(index),
                             std::string(cropped.begin(), cropped.end()), writeError))
            return fail("変形の連番を cache へ保存できません: " + writeError);
        p.frameSha256[index] = sha256Hex(cropped.data(), cropped.size());
        if (p.frameSha256[index].empty())
            return fail("変形の frame の SHA-256 を計算できません");
    }

    if (job.beforePublish)
        job.beforePublish(provenancePath);
    std::string writeError;
    bool written = false;
    if (!underGate(job, cancel, [&] {
            written =
                writeAtomically(provenancePath, transformProvenanceText(p), writeError);
        })) {
        cleanup();
        return transformCancelled();
    }
    if (!written)
        return fail("変形の provenance を cache へ保存できません: " + writeError);
    cleanup();
    auto outcome = transformReady(transformArtifactFrom(job.directory, job.key, p));
    outcome.entry.log = QString::fromStdString(rendered.log);
    return outcome;
}

math::MathCoverage coverageOf(const media::StillImage& mask) {
    math::MathCoverage coverage;
    coverage.width = mask.width;
    coverage.height = mask.height;
    coverage.alpha.resize(mask.rgba.size() / 4U);
    for (std::size_t index = 0; index < coverage.alpha.size(); ++index)
        coverage.alpha[index] = mask.rgba[index * 4U + 3U];
    return coverage;
}

} // namespace

bool loadMathTransformFrame(const MathTransformArtifact& artifact, std::size_t index,
                            std::vector<std::uint8_t>& coverage, std::string& error) {
    if (index >= artifact.frames.size() || index >= artifact.frameSha256.size() ||
        artifact.width <= 0 || artifact.height <= 0) {
        error = "変形の frame の番号または大きさが不正です";
        return false;
    }
    const auto bytes =
        static_cast<std::uintmax_t>(artifact.width) * static_cast<std::uintmax_t>(artifact.height);
    std::vector<std::uint8_t> read;
    if (!readExactly(artifact.frames[index], bytes, read)) {
        error = "変形の frame を読めないか、大きさが provenance と違います";
        return false;
    }
    if (sha256Hex(read.data(), read.size()) != artifact.frameSha256[index]) {
        error = "変形の frame の中身が provenance と違います";
        return false;
    }
    coverage = std::move(read);
    return true;
}

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
    // 変形の provenance を書いている worker と排他にする (取り消した後に確定させない)。
    std::lock_guard gate(*publishGate_);
    for (const auto& record : std::as_const(transforms_)) {
        if (record.cancel)
            record.cancel->store(true);
    }
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
    transforms_.clear();
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
    advanceTransformsWaitingOn(key);
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
    {
        std::lock_guard gate(*publishGate_);
        retainKeys(transforms_, keys);
    }
    retainKeys(records_, keys);
    retainKeys(sequences_, keys);
    // 連番・変形が要求されなくなれば、preview 用の mask も手放す (読んでいる途中なら止める)。
    for (const auto& key : resident_.keys() + loading_.keys() + overBudget_.keys())
        if (!keys.contains(key))
            dropResident(key);
}

void MathRasterCache::forgetFailures() {
    forgetFailedRecords(records_);
    forgetFailedRecords(sequences_);
    forgetFailedRecords(transforms_);
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
    const QString key = sequenceKeyFor(spec);
    const auto record = sequences_.constFind(key);
    if (key.isEmpty() || record == sequences_.constEnd() || record->entry.state != State::Ready)
        return {};
    return residencyFor(key);
}

MathRasterCache::ResidentSequence MathRasterCache::residencyFor(const QString& key) const {
    ResidentSequence result;
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
    const QString key = sequenceKeyFor(spec);
    const auto record = sequences_.constFind(key);
    if (key.isEmpty() || record == sequences_.constEnd() || record->entry.state != State::Ready ||
        shutDown_)
        return {};
    const auto& artifact = record->artifact;
    const auto bytes = static_cast<std::size_t>(artifact.frames.size()) *
                       static_cast<std::size_t>(artifact.width) *
                       static_cast<std::size_t>(artifact.height);
    return acquireResident(
        key, ResidentKind::Write, bytes,
        [artifact](const std::atomic<bool>* cancel,
                   bool& cancelled) -> std::shared_ptr<MathCoverageSequence> {
            return decodeSequence(artifact, cancel, cancelled);
        });
}

MathRasterCache::ResidentSequence
MathRasterCache::transformResidencyOf(const math::MathTransformSpec& spec) const {
    const QString key = transformKeyFor(spec);
    const auto record = transforms_.constFind(key);
    if (key.isEmpty() || record == transforms_.constEnd() || record->entry.state != State::Ready)
        return {};
    return residencyFor(key);
}

MathRasterCache::ResidentSequence
MathRasterCache::residentTransform(const math::MathTransformSpec& spec) {
    const QString key = transformKeyFor(spec);
    const auto record = transforms_.constFind(key);
    if (key.isEmpty() || record == transforms_.constEnd() || record->entry.state != State::Ready ||
        shutDown_)
        return {};
    const auto& artifact = record->artifact;
    const auto bytes = static_cast<std::size_t>(artifact.frames.size()) *
                       static_cast<std::size_t>(artifact.width) *
                       static_cast<std::size_t>(artifact.height);
    return acquireResident(
        key, ResidentKind::Transform, bytes,
        [artifact](const std::atomic<bool>* cancel,
                   bool& cancelled) -> std::shared_ptr<MathCoverageSequence> {
            cancelled = false;
            auto coverage = std::make_shared<MathCoverageSequence>();
            coverage->width = artifact.width;
            coverage->height = artifact.height;
            coverage->frames.reserve(artifact.frames.size());
            for (std::size_t index = 0; index < artifact.frames.size(); ++index) {
                if (cancel && cancel->load()) {
                    cancelled = true;
                    return nullptr;
                }
                std::vector<std::uint8_t> frame;
                std::string error;
                if (!loadMathTransformFrame(artifact, index, frame, error))
                    return nullptr;
                coverage->frames.push_back(std::move(frame));
            }
            return coverage;
        });
}

MathRasterCache::ResidentSequence MathRasterCache::acquireResident(const QString& key,
                                                                   ResidentKind kind,
                                                                   std::size_t bytes,
                                                                   ResidentDecoder decode) {
    ResidentSequence result;
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
    // 予約できるまで、最も長く使っていない mask から cache の参照を外す。外すのは cache だけが
    // 持つ mask に限る。preview (合成中の animation や engine) が使っている mask は外しても
    // memory に残る (予約も返らない) うえ、次の合成で読み直しになり、同じ frame の clip どうしで
    // 追い出し合う。Write と変形の mask は同じ上限・同じ LRU で数える。1 件で上限を超える mask の
    // ために他の mask を外すことはしない (外しても収まらない)。
    auto reserved = residency_->tryReserve(bytes);
    while (!reserved && bytes <= residency_->limit()) {
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
            (kind == ResidentKind::Write
                 ? QStringLiteral(
                       "Write の preview 用の mask が memory の上限 (全 clip の合計 %1 MB) に"
                       "収まらないため、書き終えた式で表示します (書き出しには影響しません)。"
                       "必要 %2 MB、使用中 %3 MB")
                 : QStringLiteral("変形の preview 用の mask が memory の上限 (Write と変形の合計 "
                                  "%1 MB) に収まりません (disk の変形は使えます。書き出しには"
                                  "影響しません)。必要 %2 MB、使用中 %3 MB"))
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
    loading.kind = kind;
    loading_.insert(key, loading);
    ++residentLoads_;
    residentPool_.start([this, key, kind, decode = std::move(decode), reservation,
                         ticket = loading.ticket, cancel = loading.cancel]() mutable {
        if (cancel->load())
            return;
        bool cancelled = false;
        auto decoded = decode(cancel.get(), cancelled);
        if (cancelled)
            return;
        if (decoded)
            decoded->reservation = std::move(reservation);
        QString error;
        if (!decoded)
            error = kind == ResidentKind::Write
                        ? QStringLiteral("Write の連番の PNG を読めないか、大きさが provenance と"
                                         "違います。描き直します")
                        : QStringLiteral("変形の frame を読めないか、大きさ・中身が provenance と"
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
    const ResidentKind kind = found->kind;
    loading_.erase(found);
    if (!frames) {
        // disk の連番・変形が壊れている。消して Failed にする (forgetFailures・再試行で描き直す)。
        // cache directory の変更は権限がある間だけ行う。
        if (kind == ResidentKind::Write) {
            if (authorized_)
                removeSequenceArtifact(cacheDirectory_, key);
            if (auto record = sequences_.find(key); record != sequences_.end()) {
                record->entry = {};
                record->entry.state = State::Failed;
                record->entry.message = error;
                record->artifact = {};
            }
        } else {
            if (authorized_)
                removeTransformArtifact(cacheDirectory_, key);
            if (auto record = transforms_.find(key); record != transforms_.end()) {
                record->entry = {};
                record->entry.state = State::Failed;
                record->entry.message = error;
                record->artifact = {};
            }
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

namespace {

// 描き終えていない (Pending の) record を取り消して忘れる。Ready・失敗の record は残す。
template <typename Records> void cancelPendingRecords(Records& records) {
    for (auto it = records.begin(); it != records.end();) {
        if (it->entry.state != State::Pending) {
            ++it;
            continue;
        }
        if (it->cancel)
            it->cancel->store(true);
        it = records.erase(it);
    }
}

} // namespace

void MathRasterCache::cancelPendingAnimations() {
    cancelPendingRecords(sequences_);
    // 変形の取消は provenance の書き込み (確定) と排他にする (retainOnly と同じ)。描きかけの
    // 変形の worker は取消を見てから書くので、取り消した後に確定しない。
    std::lock_guard gate(*publishGate_);
    cancelPendingRecords(transforms_);
}

QString MathRasterCache::transformKeyFor(const math::MathTransformSpec& spec) const {
    if (backendState_ != BackendState::Available)
        return {};
    return QString::fromStdString(
        math::mathTransformKey(spec, backend_.fingerprint, backend_.transformTemplate));
}

MathRasterCache::TransformEntry
MathRasterCache::requestTransform(const math::MathTransformSpec& spec) {
    if (backendState_ == BackendState::Checking)
        return {};
    if (backendState_ == BackendState::Unavailable) {
        TransformEntry entry;
        entry.state = State::Unavailable;
        entry.status = math::MathRenderStatus::BackendUnavailable;
        entry.message = backendMessage_;
        return entry;
    }
    const QString key = transformKeyFor(spec);
    if (key.isEmpty()) {
        TransformEntry entry;
        entry.state = State::Failed;
        entry.message = QStringLiteral("数式の変形の cache key を計算できません");
        return entry;
    }
    if (shutDown_)
        return {};
    if (!transforms_.contains(key)) {
        TransformRecord record;
        record.spec = spec;
        // backend が描けない枚数は描かずに未対応として失敗させる (Project の値は正しいまま)。
        if (!backend_.renderTransform || spec.frames < 1 ||
            spec.frames > backend_.maximumTransformFrames) {
            record.entry.state = State::Failed;
            record.entry.status = math::MathRenderStatus::Failed;
            record.entry.message =
                backend_.renderTransform
                    ? QStringLiteral("この描画環境の変形は %1 frame までです (要求 %2 frame)。"
                                     "トランジションを短くしてください")
                          .arg(backend_.maximumTransformFrames)
                          .arg(spec.frames)
                    : QStringLiteral("数式の描画 backend は式の変形を描けません");
        }
        transforms_.insert(key, record);
    }
    // 両端の静止を待っている間は、要求のたびに静止を要求し直す (取り下げられていても戻す)。
    advanceTransform(key);
    return transforms_.value(key).entry;
}

bool MathRasterCache::advanceTransform(const QString& key) {
    const auto found = transforms_.constFind(key);
    if (found == transforms_.constEnd() || found->launched ||
        found->entry.state != State::Pending || shutDown_ || !authorized_ ||
        backendState_ != BackendState::Available)
        return false;
    const math::MathTransformSpec spec = found->spec;
    // 両端の今の静止 (この spec の key の静止そのもの)。前に描けた別の式の静止で代用しない。
    const Entry source = request(spec.source);
    const Entry target = request(spec.target);
    auto record = transforms_.find(key);
    const auto endpointFailed = [&](const Entry& endpoint, const QString& side) {
        if (endpoint.state != State::Failed && endpoint.state != State::Unavailable)
            return false;
        record->entry = {};
        record->entry.state = endpoint.state;
        record->entry.status = endpoint.status;
        record->entry.message =
            QStringLiteral("%1の式の静止を描けないため、変形を描けません: %2")
                .arg(side, endpoint.message);
        return true;
    };
    if (endpointFailed(source, QStringLiteral("変形前")) ||
        endpointFailed(target, QStringLiteral("変形後")))
        return true;
    if (source.state != State::Ready || target.state != State::Ready)
        return false; // どちらかの静止がまだ描けていない: 待つ (古い静止では描かない)
    if (!source.mask || !target.mask) {
        record->entry = {};
        record->entry.state = State::Failed;
        record->entry.message = QStringLiteral("変形の端点の静止の mask がありません");
        return true;
    }

    TransformJob job;
    job.directory = cacheDirectory_;
    job.jobs = jobsDirectory();
    job.key = key;
    job.spec = spec;
    job.backend = backend_;
    job.timeout = renderTimeout_ + sequenceTimeoutPerFrame_ * (spec.frames + 1);
    job.sourceStatic = coverageOf(*source.mask);
    job.targetStatic = coverageOf(*target.mask);
    job.sourceStaticKey = keyFor(spec.source).toStdString();
    job.targetStaticKey = keyFor(spec.target).toStdString();
    job.publishGate = publishGate_;
    job.beforePublish = beforeTransformPublish_;
    record->launched = true;
    record->ticket = nextTicket_++;
    record->cancel = std::make_shared<std::atomic<bool>>(false);
    job.ticket = record->ticket;
    pool_.start([this, job = std::move(job), cancel = record->cancel] {
        if (cancel->load())
            return;
        TransformOutcome outcome = renderTransformJob(job, cancel.get());
        if (outcome.cancelled)
            return;
        QMetaObject::invokeMethod(
            this,
            [this, key = job.key, ticket = job.ticket, entry = std::move(outcome.entry),
             artifact = std::move(outcome.artifact)]() mutable {
                finishTransform(key, ticket, std::move(entry), std::move(artifact));
            },
            Qt::QueuedConnection);
    });
    return false;
}

void MathRasterCache::advanceTransformsWaitingOn(const QString& staticKey) {
    QStringList waiting;
    for (auto it = transforms_.cbegin(); it != transforms_.cend(); ++it)
        if (!it->launched && it->entry.state == State::Pending &&
            (keyFor(it->spec.source) == staticKey || keyFor(it->spec.target) == staticKey))
            waiting.push_back(it.key());
    for (const auto& key : waiting)
        if (advanceTransform(key))
            Q_EMIT entryChanged(key);
}

void MathRasterCache::finishTransform(const QString& key, std::uint64_t ticket,
                                      TransformEntry entry, MathTransformArtifact artifact) {
    auto found = transforms_.find(key);
    // 要求されなくなった (retainOnly で捨てた・世代が変わった) key の結果は残さない。
    if (found == transforms_.end() || found->ticket != ticket || shutDown_)
        return;
    if (entry.state != State::Ready)
        qWarning("数式の変形を描けません: %s", qUtf8Printable(entry.message));
    found->entry = std::move(entry);
    found->artifact = std::move(artifact);
    found->cancel.reset();
    Q_EMIT entryChanged(key);
}

std::optional<MathTransformArtifact>
MathRasterCache::readyTransform(const math::MathTransformSpec& spec) const {
    const auto found = transforms_.constFind(transformKeyFor(spec));
    if (found == transforms_.constEnd() || found->entry.state != State::Ready)
        return std::nullopt;
    return found->artifact;
}

std::optional<MathTransformArtifact>
MathRasterCache::readyTransformForExport(const math::MathTransformSpec& spec) const {
    if (!readyTransform(spec))
        return std::nullopt;
    const auto source = readyArtifact(spec.source);
    const auto target = readyArtifact(spec.target);
    if (!source || !target)
        return std::nullopt;
    TransformJob job;
    job.directory = cacheDirectory_;
    job.key = transformKeyFor(spec);
    job.spec = spec;
    job.backend = backend_;
    job.sourceStaticKey = keyFor(spec.source).toStdString();
    job.targetStaticKey = keyFor(spec.target).toStdString();
    std::string error;
    if (!loadMathCoverage(*source, job.sourceStatic, error) ||
        !loadMathCoverage(*target, job.targetStatic, error))
        return std::nullopt;
    // 静止の内容も現在の Ready の mask と照合し、disk の差し替えを拒否する。
    const auto sourceRecord = records_.constFind(keyFor(spec.source));
    const auto targetRecord = records_.constFind(keyFor(spec.target));
    const auto sourceDisk =
        loadArtifact(cacheDirectory_, keyFor(spec.source), backend_.fingerprint, false);
    const auto targetDisk =
        loadArtifact(cacheDirectory_, keyFor(spec.target), backend_.fingerprint, false);
    if (!sourceRecord->entry.mask || !targetRecord->entry.mask || !sourceDisk || !targetDisk ||
        job.sourceStatic != coverageOf(*sourceRecord->entry.mask) ||
        job.targetStatic != coverageOf(*targetRecord->entry.mask))
        return std::nullopt;
    MathTransformArtifact artifact;
    const std::atomic<bool> cancel{false};
    if (loadTransformArtifact(job, &cancel, artifact, false) != DiskLoad::Ready)
        return std::nullopt;
    return artifact;
}

} // namespace mvm::app
