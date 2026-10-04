#include "audio_adjustment_job.h"

#include "clip_sample_reader.h"
#include "core/checked_output_timebase.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string_view>
#include <thread>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QString>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mvm::app {
namespace {
std::atomic<bool>* openGate = nullptr;
std::atomic<int> openGateWaiters{0};
std::atomic<int> contentHashJobsStarted{0};
std::atomic<int> fullHashesStarted{0};
std::atomic<int> fileLocksOpened{0};
std::atomic<int> projectionHashes{0};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;

    ~FileHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

using FileLocks = std::vector<std::unique_ptr<FileHandle>>;

std::shared_ptr<FileLocks> lockAudioFiles(const std::vector<std::filesystem::path>& paths) {
    auto locks = std::make_shared<FileLocks>();
    for (const auto& path : paths) {
        auto file = std::make_unique<FileHandle>();
        fileLocksOpened.fetch_add(1);
        file->value = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->value == INVALID_HANDLE_VALUE)
            return {};
        locks->push_back(std::move(file));
    }
    return locks;
}

bool identitiesMatch(const std::vector<AudioFileIdentity>& start,
                     const std::vector<AudioFileIdentity>& end) {
    if (start.size() != end.size())
        return false;
    for (std::size_t index = 0; index < start.size(); ++index)
        if (start[index].key != end[index].key || start[index].size != end[index].size ||
            start[index].mtime100ns != end[index].mtime100ns ||
            start[index].contentSha256 != end[index].contentSha256)
            return false;
    return true;
}

// 解析対象 track の有効な音声 clip が参照する素材。同じ素材を何 clip が参照しても 1 件にする。
std::vector<std::filesystem::path> targetAudioFiles(const project::Project& project,
                                                    const project::AudioAdjustmentSettings& settings) {
    std::vector<std::filesystem::path> paths;
    for (const auto& clip : project.timelineClips)
        if (clip.enabled && clip.kind == project::TimelineClipKind::Audio &&
            project::audioAdjustmentTargetsTrack(settings, clip.track.index))
            paths.push_back(clip.mediaPath);
    return uniqueAudioFiles(std::move(paths));
}

bool collectAudioIdentities(const std::vector<std::filesystem::path>& paths,
                            std::vector<AudioFileIdentity>& files, const std::atomic<bool>& running,
                            std::string& error) {
    files.clear();
    for (const auto& path : paths) {
        if (!running) {
            error.clear();
            return false;
        }
        AudioFileIdentity identity;
        if (!inspectAudioFile(path, identity, true, &running, error))
            return false;
        files.push_back(std::move(identity));
    }
    return true;
}
} // namespace

std::string audioFileKey(const std::filesystem::path& path) {
    return QFileInfo(QString::fromStdWString(path.wstring())).absoluteFilePath().toUtf8().toStdString();
}

std::vector<std::filesystem::path> uniqueAudioFiles(std::vector<std::filesystem::path> paths) {
    // 照合・保護の単位は clip ではなく素材。表記の違う同じ path も identity の key で 1 件にする。
    std::map<std::string, std::filesystem::path> unique;
    for (auto& path : paths) {
        auto key = audioFileKey(path);
        unique.try_emplace(std::move(key), std::move(path));
    }
    paths.clear();
    for (auto& [key, path] : unique) {
        (void)key;
        paths.push_back(std::move(path));
    }
    return paths;
}

bool inspectAudioFile(const std::filesystem::path& path, AudioFileIdentity& identity,
                      bool hashContent, const std::atomic<bool>* running, std::string& error) {
    identity = {};
    identity.key = audioFileKey(path);
    const auto absolute = QString::fromUtf8(identity.key).toStdWString();
    if (identity.key.find('\n') != std::string::npos ||
        identity.key.find('\r') != std::string::npos) {
        error = "解析する素材の path を識別できません";
        return false;
    }
    FileHandle file;
    file.value = CreateFileW(absolute.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        error = "解析する素材を開けません";
        return false;
    }
    LARGE_INTEGER size{};
    FILETIME written{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        !GetFileTime(file.value, nullptr, nullptr, &written)) {
        error = "解析する素材の状態を確認できません";
        return false;
    }
    ULARGE_INTEGER mtime{};
    mtime.LowPart = written.dwLowDateTime;
    mtime.HighPart = written.dwHighDateTime;
    identity.exists = true;
    identity.size = static_cast<std::uint64_t>(size.QuadPart);
    identity.mtime100ns = mtime.QuadPart;
    if (!hashContent)
        return true;
    fullHashesStarted.fetch_add(1);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<char> buffer(1024 * 1024);
    for (;;) {
        if (running && !running->load(std::memory_order_acquire)) {
            error.clear();
            return false;
        }
        DWORD read = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr)) {
            error = "解析する素材の内容を確認できません";
            return false;
        }
        if (read == 0)
            break;
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(read)));
    }
    identity.contentSha256 = hash.result().toHex().toStdString();
    identity.hashed = true;
    return true;
}

std::string audioProjectionHash(const project::Project& project) {
    const auto text = project::audioAdjustmentInputProjection(project);
    return QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

std::string audioProjectionHash(const project::Project& project,
                                const project::AudioAdjustmentSettings& settings) {
    projectionHashes.fetch_add(1);
    return QCryptographicHash::hash(QByteArray::fromStdString(
                                        project::audioAdjustmentInputProjection(project, settings)),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

std::string formatAudioInputFingerprint(const std::string& projectionHash,
                                        const std::vector<AudioFileIdentity>& files) {
    std::string text = "v2\n" + projectionHash + "\n";
    for (const auto& file : files)
        text += std::to_string(file.size) + " " + std::to_string(file.mtime100ns) + " " +
                file.contentSha256 + " " + file.key + "\n";
    return text;
}

bool parseAudioInputFingerprint(const std::string& text, std::string& projectionHash,
                                std::vector<AudioFileIdentity>& files) {
    constexpr std::string_view prefix = "v2\n";
    if (text.size() < prefix.size() || text.compare(0, prefix.size(), prefix) != 0)
        return false;
    const auto hashEnd = text.find('\n', prefix.size());
    if (hashEnd == std::string::npos)
        return false;
    projectionHash = text.substr(prefix.size(), hashEnd - prefix.size());
    if (projectionHash.size() != 64)
        return false;
    files.clear();
    std::size_t line = hashEnd + 1;
    while (line < text.size()) {
        const auto end = text.find('\n', line);
        const auto row =
            text.substr(line, end == std::string::npos ? std::string::npos : end - line);
        line = end == std::string::npos ? text.size() : end + 1;
        if (row.empty())
            continue;
        const auto sizeEnd = row.find(' ');
        const auto mtimeEnd =
            sizeEnd == std::string::npos ? std::string::npos : row.find(' ', sizeEnd + 1);
        const auto shaEnd =
            mtimeEnd == std::string::npos ? std::string::npos : row.find(' ', mtimeEnd + 1);
        if (sizeEnd == std::string::npos || mtimeEnd == std::string::npos ||
            shaEnd == std::string::npos)
            return false;
        AudioFileIdentity file;
        char* sizeStop = nullptr;
        char* mtimeStop = nullptr;
        file.size = std::strtoull(row.c_str(), &sizeStop, 10);
        file.mtime100ns = std::strtoull(row.c_str() + sizeEnd + 1, &mtimeStop, 10);
        file.contentSha256 = row.substr(mtimeEnd + 1, shaEnd - (mtimeEnd + 1));
        file.key = row.substr(shaEnd + 1);
        file.exists = true;
        file.hashed = true;
        if (sizeStop != row.c_str() + sizeEnd || mtimeStop != row.c_str() + mtimeEnd ||
            file.contentSha256.size() != 64 || file.key.empty())
            return false;
        files.push_back(std::move(file));
    }
    return true;
}

void setAudioAdjustmentOpenGateForTest(std::atomic<bool>* gate) {
    openGate = gate;
}

int audioAdjustmentOpenGateWaitersForTest() {
    return openGateWaiters.load(std::memory_order_acquire);
}

int audioContentHashJobsStartedForTest() {
    return contentHashJobsStarted.load();
}

int audioFullHashesStartedForTest() {
    return fullHashesStarted.load();
}

int audioFileLocksOpenedForTest() {
    return fileLocksOpened.load();
}

int audioProjectionHashesForTest() {
    return projectionHashes.load();
}

AudioContentHashJob::AudioContentHashJob(std::vector<std::filesystem::path> paths, bool lockFiles) {
    contentHashJobsStarted.fetch_add(1);
    paths = uniqueAudioFiles(std::move(paths));
    future_ = std::async(std::launch::async, [this, paths = std::move(paths), lockFiles] {
        AudioContentHashResult result;
        if (lockFiles) {
            result.locks = lockAudioFiles(paths);
            if (!result.locks) {
                result.status = AudioContentHashResult::Status::ReadError;
                for (const auto& path : paths) {
                    AudioFileIdentity identity;
                    std::string error;
                    inspectAudioFile(path, identity, false, nullptr, error);
                    result.files.push_back(std::move(identity));
                }
                return result;
            }
        }
        for (const auto& path : paths) {
            if (!running_.load(std::memory_order_acquire))
                return result;
            AudioFileIdentity identity;
            std::string error;
            if (!inspectAudioFile(path, identity, true, &running_, error)) {
                if (!running_)
                    return result;
                const auto attributes = GetFileAttributesW(path.wstring().c_str());
                const auto code = GetLastError();
                result.status =
                    attributes == INVALID_FILE_ATTRIBUTES &&
                            (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                        ? AudioContentHashResult::Status::Missing
                        : AudioContentHashResult::Status::ReadError;
                identity.hashed = false;
                result.files.push_back(std::move(identity));
                continue;
            }
            result.files.push_back(std::move(identity));
        }
        if (!running_.load(std::memory_order_acquire))
            return result;
        if (result.status == AudioContentHashResult::Status::Cancelled)
            result.status = AudioContentHashResult::Status::Complete;
        return result;
    });
}

AudioContentHashJob::~AudioContentHashJob() {
    cancel();
    if (future_.valid())
        future_.wait();
}

bool AudioContentHashJob::ready() const {
    return future_.valid() &&
           future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

AudioContentHashResult AudioContentHashJob::take() {
    return future_.get();
}

AudioAdjustmentResult analyzeAudioAdjustment(project::Project source,
                                             const project::AudioAdjustmentSettings& settings,
                                             const std::atomic<bool>& running,
                                             std::atomic<int>& progress) {
    AudioAdjustmentResult combined;
    if (!project::validateAudioAdjustmentSettings(
            settings, static_cast<int>(source.audioTracks.size()), combined.error))
        return combined;
    combined.candidate = source;
    combined.settings = settings;
    combined.projectionHash = audioProjectionHash(combined.candidate, settings);
    // 自動調整は effects だけを変えるので、開始時と終了時の照合対象は同じ素材の集合になる。
    const auto paths = targetAudioFiles(source, settings);
    const auto locks = lockAudioFiles(paths);
    if (!locks) {
        combined.error = "解析する素材を読み取り専用で保護できません";
        return combined;
    }
    if (!collectAudioIdentities(paths, combined.files, running, combined.error)) {
        combined.cancelled = !running && combined.error.empty();
        if (combined.error.empty() && !combined.cancelled)
            combined.error = "解析する素材の内容を確認できません";
        return combined;
    }
    const auto filesAtStart = combined.files;
    // 解析対象は明示指定。mute / solo と前回の自動調整を測定入力から除く。
    for (auto& track : source.audioTracks) {
        track.muted = false;
        track.solo = false;
    }
    for (auto& clip : source.timelineClips) {
        clip.effects.normalizationGainDb = 0;
        clip.effects.duckingDb = 0;
        clip.effects.duckingKeys.clear();
    }
    ShuttleAudioPlan plan;
    if (!planShuttleAudio(source, 1, 0, plan, combined.error))
        return combined;
    std::erase_if(plan.clips, [&](const auto& clip) {
        const int track = clip.clip.track.index;
        return track != settings.bgmTrack &&
               std::find(settings.voiceTracks.begin(), settings.voiceTracks.end(), track) ==
                   settings.voiceTracks.end();
    });
    if (plan.clips.empty() ||
        std::none_of(plan.clips.begin(), plan.clips.end(), [&](const auto& clip) {
            return clip.clip.track.index == settings.bgmTrack;
        })) {
        combined.error = "BGM トラックに解析できる音声クリップがありません";
        return combined;
    }
    if (settings.duck && std::none_of(plan.clips.begin(), plan.clips.end(), [&](const auto& clip) {
            return clip.clip.track.index != settings.bgmTrack;
        })) {
        combined.error = "声トラックに解析できる音声クリップがありません";
        return combined;
    }
    const auto timebase =
        core::CheckedOutputTimebase::create(plan.timelineFpsNum, plan.timelineFpsDen, 48000);
    if (!timebase) {
        combined.error = "解析する音声の時間軸が不正です";
        return combined;
    }
    std::int64_t total = 0;
    std::atomic<std::int64_t> completed{0};
    for (const auto& clip : plan.clips)
        total += clip.timelineEndSample - clip.timelineStartSample;
    auto* candidate = &combined.candidate;
    std::atomic<bool> failed{false};
    const auto analyzeClip = [&](std::size_t index) {
        AudioAdjustmentResult result;
        // 各 worker は自分の decoder・測定器・PCM だけを保持する。
        ClipSampleReaders reader;
        reader.reset(1, 48000);
        const auto& clip = plan.clips[index];
        auto target = std::find_if(candidate->timelineClips.begin(), candidate->timelineClips.end(),
                                   [&](const auto& item) { return item.id == clip.clip.id; });
        if (target == candidate->timelineClips.end()) {
            result.error = "解析したクリップがありません";
            return result;
        }
        audio::LoudnessMeter meter;
        if (!meter.start(result.error))
            return result;

        struct Window {
            std::int64_t start;
            std::int64_t end;
            double rms;
        };

        std::vector<Window> windows;
        const bool voice = clip.clip.track.index != settings.bgmTrack;
        const auto pan = project::audioMixGains(0, clip.segment.mixerPan);
        std::vector<float> pcm;
        // open / decode が戻らない素材でも、取消が GUI で待たされないことを試験する。
        // 待っている間は running を見ない。
        if (openGate != nullptr) {
            openGateWaiters.fetch_add(1, std::memory_order_release);
            while (openGate->load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            openGateWaiters.fetch_sub(1, std::memory_order_release);
            if (!running) {
                result.cancelled = true;
                return result;
            }
        }
        // PCM は 200 ms ずつ渡し、検出窓は従来どおり 20 ms に分ける。
        constexpr std::int64_t blockSamples = 9600;
        for (auto start = clip.timelineStartSample; start < clip.timelineEndSample;
             start += blockSamples) {
            if (!running || failed) {
                result.cancelled = !running;
                return result;
            }
            const auto count = std::min(blockSamples, clip.timelineEndSample - start);
            if (!reader.read(clip, 0, start + clip.sourceOffset, count, pcm, running,
                             result.error)) {
                result.cancelled = !running;
                if (result.error.empty() && !result.cancelled)
                    result.error = "解析する音声を読み出せません";
                return result;
            }
            // 同一 frame のゲインは一定。共通評価を frame 境界でだけ呼び出す。
            for (std::int64_t sample = 0; sample < count;) {
                const auto frame = timebase.value().schedulerOutputFrame(start + sample);
                const auto gain =
                    frame ? project::renderSegmentGain(clip.segment, plan.timelineFpsNum,
                                                       plan.timelineFpsDen, frame.value())
                          : std::nullopt;
                if (!gain) {
                    result.error = "解析する音声のゲインを評価できません";
                    return result;
                }
                const auto boundary = timebase.value().firstAudioSample(frame.value() + 1);
                if (!boundary || boundary.value() <= start + sample) {
                    result.error = "解析する音声の frame 境界を評価できません";
                    return result;
                }
                const auto end = std::min(count, boundary.value() - start);
                const float left = static_cast<float>(*gain * pan.first);
                const float right = static_cast<float>(*gain * pan.second);
                for (; sample < end; ++sample) {
                    const auto at = static_cast<std::size_t>(sample) * 2;
                    pcm[at] *= left;
                    pcm[at + 1] *= right;
                }
            }
            if (!meter.push(pcm.data(), static_cast<std::size_t>(count), result.error))
                return result;
            if (voice && settings.duck)
                for (std::int64_t first = 0; first < count; first += 960) {
                    const auto end = std::min(first + 960, count);
                    double squaresLeft = 0, squaresRight = 0;
                    for (auto sample = first; sample < end; ++sample) {
                        const auto at = static_cast<std::size_t>(sample) * 2;
                        squaresLeft += static_cast<double>(pcm[at]) * static_cast<double>(pcm[at]);
                        squaresRight +=
                            static_cast<double>(pcm[at + 1]) * static_cast<double>(pcm[at + 1]);
                    }
                    windows.push_back({start + first, start + end,
                                       std::sqrt(std::max(squaresLeft, squaresRight) /
                                                 static_cast<double>(end - first))});
                }
            const auto done = completed.fetch_add(count, std::memory_order_relaxed) + count;
            const auto percent =
                static_cast<int>(static_cast<double>(done) / static_cast<double>(total) * 100);
            auto previous = progress.load(std::memory_order_relaxed);
            while (previous < percent &&
                   !progress.compare_exchange_weak(previous, percent, std::memory_order_relaxed)) {
            }
        }
        AudioAdjustmentClipResult measured;
        measured.clipId = target->id;
        measured.name = target->name;
        if (!meter.finish(measured.measurement, result.error))
            return result;
        measured.correctionDb = target->effects.normalizationGainDb;
        if (settings.normalize && measured.measurement.measurable) {
            const double wanted = (voice ? settings.voiceLufs : settings.bgmLufs) -
                                  measured.measurement.integratedLufs;
            const double ceiling = -1 - measured.measurement.truePeakDb;
            measured.peakLimited = wanted > ceiling;
            const double correction = std::min(wanted, ceiling);
            measured.correctionDb = std::clamp(correction, -96.0, 60.0);
            measured.gainLimited = correction != measured.correctionDb;
            target->effects.normalizationGainDb = measured.correctionDb;
        }
        if (settings.duck && voice) {
            const double normalized = std::pow(10.0, target->effects.normalizationGainDb / 20);
            const double threshold = std::pow(10.0, settings.thresholdDb / 20);
            for (const auto& window : windows)
                if (window.rms * normalized >= threshold)
                    result.ranges.push_back({window.start, window.end});
        }
        result.clips.push_back(measured);
        result.success = true;
        return result;
    };
    // スレッド数と常駐メモリを制限し、結果は元の clip 順に取りまとめる。
    const auto concurrency =
        std::min(plan.clips.size(),
                 static_cast<std::size_t>(std::clamp(std::thread::hardware_concurrency(), 1U, 4U)));
    std::vector<AudioAdjustmentResult> outputs(plan.clips.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::future<void>> workers;
    for (std::size_t worker = 0; worker < concurrency; ++worker)
        workers.push_back(std::async(std::launch::async, [&] {
            try {
                while (running && !failed) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= plan.clips.size())
                        break;
                    outputs[index] = analyzeClip(index);
                    if (!outputs[index].success)
                        failed = true;
                }
            } catch (...) {
                failed = true;
                throw;
            }
        }));
    for (auto& worker : workers)
        worker.get();
    if (failed) {
        for (const auto& output : outputs)
            if (!output.error.empty()) {
                combined.error = output.error;
                break;
            }
        combined.cancelled = !running;
        if (combined.error.empty() && !combined.cancelled)
            combined.error = "音声解析を完了できません";
        return combined;
    }
    if (!running) {
        combined.cancelled = true;
        return combined;
    }
    for (auto& output : outputs) {
        combined.clips.insert(combined.clips.end(), output.clips.begin(), output.clips.end());
        combined.ranges.insert(combined.ranges.end(), output.ranges.begin(), output.ranges.end());
    }
    combined.ranges = project::mergeAudioDetectedRanges(std::move(combined.ranges));
    if (settings.duck)
        for (auto& clip : combined.candidate.timelineClips)
            if (clip.enabled && clip.kind == project::TimelineClipKind::Audio &&
                clip.track.index == settings.bgmTrack) {
                const auto duration = project::timelineClipDuration(combined.candidate, clip);
                if (!duration.success) {
                    combined.error = duration.error;
                    return combined;
                }
                clip.effects.duckingDb = 0;
                clip.effects.duckingKeys = project::makeDuckingKeys(
                    combined.ranges, settings, clip.timelineStartFrame, duration.frame,
                    plan.timelineFpsNum, plan.timelineFpsDen);
            }
    const auto valid = project::validateTimeline(combined.candidate);
    if (!valid.success) {
        combined.error = valid.error;
        return combined;
    }
    std::vector<AudioFileIdentity> filesAtEnd;
    if (!collectAudioIdentities(paths, filesAtEnd, running, combined.error)) {
        combined.cancelled = !running && combined.error.empty();
        if (combined.error.empty() && !combined.cancelled)
            combined.error = "解析する素材の内容を確認できません";
        return combined;
    }
    if (!identitiesMatch(filesAtStart, filesAtEnd)) {
        combined.error = "解析中に素材の内容が変わりました。再解析してください";
        combined.files.clear();
        return combined;
    }
    combined.fingerprintText = formatAudioInputFingerprint(combined.projectionHash, combined.files);
    combined.success = true;
    progress = 100;
    return combined;
}

AudioAdjustmentJob::AudioAdjustmentJob(project::Project source,
                                       project::AudioAdjustmentSettings settings) {
    future_ = std::async(std::launch::async, [this, source = std::move(source),
                                              settings = std::move(settings)]() mutable {
        try {
            return analyzeAudioAdjustment(std::move(source), settings, running_, progress_);
        } catch (const std::exception&) {
            AudioAdjustmentResult result;
            result.error = "音声解析中に処理を継続できなくなりました";
            return result;
        }
    });
}

AudioAdjustmentJob::~AudioAdjustmentJob() {
    cancel();
    if (future_.valid())
        future_.wait();
}

bool AudioAdjustmentJob::ready() const {
    return future_.valid() &&
           future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

AudioAdjustmentResult AudioAdjustmentJob::take() {
    return future_.get();
}
} // namespace mvm::app
