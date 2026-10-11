// 解の公式 Project と、そこから作る固定の fixture を、製品の MvmController が組む要求のまま
// exportTimeline（MLT / H.264）へ渡し、書き出しの進行の観測と parity の照合を行う。
//
// 画素の authority は encoder の YUV 変換前（encoderFrameValidator の RGBA）。
// 復号した H.264 は frame 数・寸法・時刻の照合に使い、画素の一致判定には使わない。
// 進行は出力 file の大きさではなく、各境界の通過回数（MvmExportObserver）で見る。
// 上限時間に達しても進んでいる書き出しを「停止」とは呼ばない（終了コードを分ける）。

#include "app/math_clip_render.h"
#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media/still_image/static_image.h"
#include "media/still_image/still_image_decoder.h"
#include "mlt_rgba_oracle.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <QUrl>

extern "C" {
#include <libavutil/log.h>
}

namespace {

using mvm::project::Project;
using mvm::project::TimelineClip;
using mvm::project::TimelineClipKind;
using mvm::project::TrackKind;

// 終了コード。runner（scripts/math-score-export-diagnose.ps1）が fixture ごとの期待と照合する。
constexpr int kExitPass = 0;
constexpr int kExitProtocol = 2;     // 使い方・入力・fixture の前提が崩れた（判定しない）
constexpr int kExitExportFailed = 6; // 書き出しが失敗を返した
constexpr int kExitParityFailed = 8; // 書き出しは終わったが照合が不成立
constexpr int kExitCapped = 9;       // 上限時間まで進み続けたが終わらなかった
constexpr int kExitStalled = 10;     // 進行が止まった（stack を採ってから取消）
constexpr int kExitCancelled = 11;   // 指示した取消で止まった（negative control）
constexpr int kExitLeak = 12;        // 失敗・取消なのに出力 file が残った

// 事前に固定した fixture。実行時の結果を見て増減・変更しない。
enum class FixtureKind { Synthetic, Region, Full };

struct Fixture {
    const char* name;
    FixtureKind kind;
    int layers;               // Synthetic: 同時に重ねる数式の数
    std::int64_t frames;      // Synthetic・Region: 尺。Full: Project 全体の想定 frame 数
    std::int64_t windowStart; // Region: 元の Project での開始 frame
    std::int64_t representative[3];
    // Synthetic: 数式を画面中央・等倍・fade なしに置き直す。整数画素の配置になり、既存の
    // MLT oracle (source-over) をそのまま適用できる対照になる。
    bool centered = false;
};

constexpr Fixture kFixtures[] = {
    {"background-only", FixtureKind::Synthetic, 0, 120, 0, {0, 60, 119}},
    {"math-1", FixtureKind::Synthetic, 1, 120, 0, {0, 60, 119}},
    {"math-1-integer", FixtureKind::Synthetic, 1, 120, 0, {0, 60, 119}, true},
    {"math-16", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}},
    {"math-48", FixtureKind::Synthetic, 48, 120, 0, {0, 60, 119}},
    // 層数を固定して尺（= layer ごとの keyframe 数）だけを伸ばす。コストが尺に依存するかを見る。
    {"math-16-480f", FixtureKind::Synthetic, 16, 480, 0, {0, 240, 479}},
    {"math-16-1920f", FixtureKind::Synthetic, 16, 1920, 0, {0, 960, 1919}},
    // 実 Project の 89〜92 秒。分数線の伸縮と分子の移動を含む（元の代表 frame 5340・5370・5400）。
    {"v4-region", FixtureKind::Region, 0, 180, 5280, {60, 90, 120}},
    {"v4-full", FixtureKind::Full, 0, 6840, 0, {600, 5370, 6720}},
};

enum class Inject { None, DropFrame, RejectFrame, OracleShift, Cancel };

// 不透明な下地 (alpha 255) へ MLT の affine が重ねた結果の alpha は 255 * (as + ad - as * ad) の
// 切り捨てで、ad >= 254/255 なら 255 - (1 - as) >= 254。これより低い alpha は合成の異常として扱う。
// 代表 frame では alpha も含めた 4 channel を独立期待値と比べる (下の expectedFrame)。
constexpr int kAdmissibleAlpha = 254;

struct Options {
    std::filesystem::path project, manim, output, ffprobe, gdb;
    const Fixture* fixture = nullptr;
    Inject inject = Inject::None;
    int capSeconds = 240;  // 書き出し全体の診断上限
    int stallSeconds = 45; // どの counter も進まない時間
    int stackSamples = 2;
    // 0 なら製品の timeout のまま。0 以外は「製品の条件ではない」と report に残して上書きする。
    // 停止の原因を調べる代わりに使わない。原因と 1 frame のコストが分かった後で、製品の timeout
    // では終わらない長さの Project の全 frame を照合するためだけに使う。
    int requestTimeoutMs = 0;
};

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// --- fixture の構築 ------------------------------------------------------------

std::int64_t clipEnd(const Project& project, const TimelineClip& clip) {
    const auto duration = mvm::project::timelineClipDuration(project, clip);
    return duration.success ? clip.timelineStartFrame + duration.frame : -1;
}

// 尺を frames に切る。key は frames より手前を残し、末尾に切り口の値を置く（値は製品の評価）。
void truncateKeys(TimelineClip& clip, std::int64_t frames) {
    const auto atEnd = mvm::project::evaluateClipEffects(clip.effects, frames - 1);
    for (const auto& channel : mvm::project::effectChannels()) {
        auto& keys = clip.effects.*channel.keys;
        if (keys.empty() || keys.back().frame < frames)
            continue;
        const auto cut = std::find_if(keys.begin(), keys.end(),
                                      [&](const auto& key) { return key.frame >= frames - 1; });
        auto boundary = *cut;
        boundary.frame = frames - 1;
        boundary.value = atEnd.*channel.base;
        keys.erase(cut, keys.end());
        keys.push_back(boundary);
    }
}

bool buildSynthetic(const Project& base, const Fixture& fixture, Project& out, std::string& error) {
    out = base;
    out.timelineClips.clear();
    out.timelineTransitions.clear();
    out.timelineMarkers.clear();
    out.inFrame.reset();
    out.outFrame.reset();
    out.subtitles.reset();
    const TimelineClip* background = nullptr;
    std::vector<const TimelineClip*> math;
    for (const auto& clip : base.timelineClips) {
        if (clip.kind == TimelineClipKind::Image && clip.track.kind == TrackKind::Video &&
            clip.track.index == 0 && !background)
            background = &clip;
        else if (clip.kind == TimelineClipKind::Math && clip.enabled &&
                 static_cast<int>(math.size()) < fixture.layers)
            math.push_back(&clip); // Project の並び順で先頭から選ぶ（結果を見て選ばない）
    }
    if (!background || static_cast<int>(math.size()) != fixture.layers) {
        error = "fixture に必要な背景または数式 clip が元の Project にありません";
        return false;
    }
    auto bottom = *background;
    bottom.timelineStartFrame = 0;
    bottom.sourceInFrame = 0;
    bottom.sourceOutFrame = bottom.sourceFrameCount = fixture.frames;
    bottom.effects = {};
    out.timelineClips.push_back(bottom);
    while (static_cast<int>(out.videoTracks.size()) < fixture.layers + 1) {
        auto track = out.videoTracks.front();
        track.name = "診断 " + std::to_string(out.videoTracks.size());
        track.muted = false;
        out.videoTracks.push_back(track);
    }
    for (auto& track : out.videoTracks)
        track.muted = false;
    for (int layer = 0; layer < fixture.layers; ++layer) {
        auto clip = *math[static_cast<std::size_t>(layer)];
        clip.track = {TrackKind::Video, layer + 1};
        clip.timelineStartFrame = 0;
        clip.sourceInFrame = 0;
        clip.sourceOutFrame = clip.sourceFrameCount = fixture.frames;
        truncateKeys(clip, fixture.frames);
        if (fixture.centered)
            clip.effects = {};
        out.timelineClips.push_back(std::move(clip));
    }
    return true;
}

bool buildRegion(const Project& base, const Fixture& fixture, Project& out, std::string& error) {
    out = base;
    const auto valid = mvm::project::validateTimeline(out);
    if (!valid.success) {
        error = valid.error;
        return false;
    }
    const auto windowEnd = fixture.windowStart + fixture.frames;
    if (windowEnd > valid.totalFrames) {
        error = "fixture の区間が Project の尺を超えています";
        return false;
    }
    // 製品の時間削除（区間をまたぐ clip を分割し、key と字幕も同じ規則で切る）で両側を落とす。
    if (windowEnd < valid.totalFrames) {
        const auto tail =
            mvm::project::editTimelineTime(out, windowEnd, valid.totalFrames - windowEnd, 0);
        if (!tail.success) {
            error = "区間より後ろを削除できません: " + tail.error;
            return false;
        }
    }
    if (fixture.windowStart > 0) {
        const auto head = mvm::project::editTimelineTime(out, 0, fixture.windowStart, 0);
        if (!head.success) {
            error = "区間より前を削除できません: " + head.error;
            return false;
        }
    }
    out.timelineMarkers.clear();
    out.inFrame.reset();
    out.outFrame.reset();
    return true;
}

// --- pre-YUV の観測 --------------------------------------------------------------

struct FrameSeen {
    int calls = 0;
    std::uint64_t hash = 0;
    int minAlpha = 0;
    bool repeatDiffers = false; // 同じ frame の再要求で画素が違った
};

struct Observation {
    std::mutex mutex;
    std::map<std::int64_t, FrameSeen> frames;
    std::map<std::int64_t, std::vector<std::uint8_t>> samples;
    std::string error;
    int width = 0;
    int height = 0;
};

// 8 byte ずつ畳む。alpha は全 frame・全画素の最小値を同じ走査で取る。
void digestFrame(const std::uint8_t* rgba, std::size_t bytes, std::uint64_t& hash, int& minAlpha) {
    std::uint64_t value = 0xcbf29ce484222325ull;
    int alpha = 255;
    std::size_t index = 0;
    for (; index + 8 <= bytes; index += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, rgba + index, 8);
        value = (value ^ word) * 0x100000001b3ull;
        value ^= value >> 29;
        alpha =
            std::min({alpha, static_cast<int>(rgba[index + 3]), static_cast<int>(rgba[index + 7])});
    }
    for (; index < bytes; ++index) {
        value = (value ^ rgba[index]) * 0x100000001b3ull;
        if (index % 4 == 3)
            alpha = std::min(alpha, static_cast<int>(rgba[index]));
    }
    hash = value;
    minAlpha = alpha;
}

// --- FFmpeg の記録の tap（encoder・muxer の境界） -------------------------------------

std::atomic<MvmExportObserver*> gTapObserver{nullptr};
constexpr int kCensusCapacity = 16;
const char* gCensus[kCensusCapacity];
std::atomic<int> gCensusCount{0};

void censusFormat(const char* format) {
    const int count = gCensusCount.load(std::memory_order_acquire);
    for (int index = 0; index < count && index < kCensusCapacity; ++index)
        if (gCensus[index] == format)
            return;
    const int slot = gCensusCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot < kCensusCapacity)
        gCensus[slot] = format; // FFmpeg の静的な書式文字列。寿命は DLL と同じ
}

void logTap(void* context, int level, const char* format, va_list arguments) {
    auto* observer = gTapObserver.load(std::memory_order_acquire);
    if (observer && format) {
        if (std::strncmp(format, "frame=%4d QP=", 13) == 0) {
            // libx264 が 1 frame の符号化を終えるたびに出す debug 記録。
            va_list copy;
            va_copy(copy, arguments);
            const int frame = va_arg(copy, int);
            va_end(copy);
            censusFormat(format);
            mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_ENCODED_PACKET, frame);
        } else if (std::strncmp(format, "%s size:%d dts:%s pts:%s", 24) == 0) {
            // libavformat の FF_FDEBUG_TS。FFmpeg 8.1 では muxer の入口 (write_packet_common) が
            // packet ごとに出す。どの関数の記録だったかは report に残す。
            va_list copy;
            va_copy(copy, arguments);
            const char* function = va_arg(copy, const char*);
            va_end(copy);
            censusFormat(format);
            if (function)
                censusFormat(function);
            mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_MUXED_PACKET, -1);
        }
    }
    if (level <= av_log_get_level())
        av_log_default_callback(context, level, format, arguments);
}

// --- CPU 時間と stack ------------------------------------------------------------

double fileTimeSeconds(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) / 1e7;
}

double processCpuSeconds() {
    FILETIME creation, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user))
        return -1.0;
    return fileTimeSeconds(kernel) + fileTimeSeconds(user);
}

void threadCpuSeconds(std::map<DWORD, double>& out) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self)
            continue;
        const HANDLE thread =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        FILETIME creation, exited, kernel, user;
        if (GetThreadTimes(thread, &creation, &exited, &kernel, &user))
            out[entry.th32ThreadID] = fileTimeSeconds(kernel) + fileTimeSeconds(user);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

// gdb を自分へ attach して全 thread の stack を採る。attach の間は自 process が止まる。
bool captureStacks(const Options& options, const std::filesystem::path& path, QString& detail) {
    if (options.gdb.empty() || !std::filesystem::exists(options.gdb)) {
        detail = QStringLiteral("gdb が指定されていないため stack を採っていません");
        return false;
    }
    QProcess gdb;
    gdb.setStandardOutputFile(toQString(path));
    gdb.setStandardErrorFile(toQString(path) + QStringLiteral(".err"));
    gdb.start(toQString(options.gdb), {QStringLiteral("-p"), QString::number(GetCurrentProcessId()),
                                       QStringLiteral("-batch"), QStringLiteral("-ex"),
                                       QStringLiteral("set pagination off"), QStringLiteral("-ex"),
                                       QStringLiteral("thread apply all bt 40")});
    // 途中で gdb を殺すと attach された自 process も終わる。終わるまで待つ。
    if (!gdb.waitForFinished(900000)) {
        detail = QStringLiteral("gdb が 15 分以内に終わりませんでした");
        return false;
    }
    detail = QStringLiteral("gdb exit=%1").arg(gdb.exitCode());
    return gdb.exitCode() == 0;
}

// --- 独立した期待値 ---------------------------------------------------------------

// 補間の途中でない frame の値だけを返す。製品の補間（ease）の実装を共有しない。
std::optional<double> heldValue(const std::vector<mvm::project::ClipKeyframe>& keys, double base,
                                std::int64_t frame) {
    if (keys.empty())
        return base;
    const mvm::project::ClipKeyframe* previous = nullptr;
    const mvm::project::ClipKeyframe* next = nullptr;
    for (const auto& key : keys) {
        if (key.frame == frame)
            return key.value;
        if (key.frame < frame)
            previous = &key;
        else if (!next)
            next = &key;
    }
    if (previous && !next)
        return previous->value;
    if (!previous && next)
        return next->value;
    if (previous && next && previous->value == next->value)
        return previous->value;
    return std::nullopt;
}

struct OracleLayer {
    std::vector<std::uint8_t> rgba; // 出力と同じ大きさ、straight alpha
    // 出力上で layer 全体を置く矩形 (画素)。等倍で整数位置なら integer。
    double rectX = 0.0, rectY = 0.0, rectWidth = 0.0, rectHeight = 0.0;
    bool integer = true;
    double opacity = 1.0;
};

// 期待値は 2 段ある。混ぜて「一致」と呼ばない。
//   exact     全 layer が等倍・整数位置。既存の MLT oracle (mltSourceOver)
//   で全画素の完全一致を見る。 reference 端数位置・拡縮を含む。MLT の双線形 (interp.h の式)
//   を倍精度で独立に計算した参照。
//             MLT は binary32 で計算して切り捨てるので、参照との差は 1 階調まで許す (事前に固定)。
constexpr int kReferenceBound = 1;

struct OracleOutcome {
    bool applicable = false;
    bool exactTier = true;
    std::string reason;
    int layers = 0;
    long long inked = 0;      // 上の layer が寄与した画素数（0 なら空振り）
    long long differing = 0;  // 1 階調でも違う画素
    long long mismatches = 0; // 許容を超えて違う画素
    int maxDifference = 0;
    QJsonObject firstMismatch;
    std::vector<std::uint8_t> expected;
};

std::vector<std::uint8_t> imageBytes(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(image.width()) *
                                    static_cast<std::size_t>(image.height()) * 4U);
    for (int row = 0; row < image.height(); ++row)
        std::memcpy(bytes.data() + static_cast<std::size_t>(row) *
                                       static_cast<std::size_t>(image.width()) * 4U,
                    image.constScanLine(row), static_cast<std::size_t>(image.width()) * 4U);
    return bytes;
}

OracleOutcome expectedFrame(const Project& project,
                            const std::map<std::string, std::filesystem::path>& artifacts,
                            bool burnSubtitles, std::int64_t frame, bool shiftFirstLayer) {
    OracleOutcome outcome;
    const int width = project.outputWidth;
    const int height = project.outputHeight;
    std::vector<const TimelineClip*> active;
    for (const auto& clip : project.timelineClips) {
        if (!clip.enabled || clip.track.kind != TrackKind::Video)
            continue;
        const auto end = clipEnd(project, clip);
        if (frame < clip.timelineStartFrame || frame >= end)
            continue;
        active.push_back(&clip);
    }
    std::stable_sort(active.begin(), active.end(), [](const auto* left, const auto* right) {
        return left->track.index < right->track.index;
    });
    if (active.empty() || active.front()->kind != TimelineClipKind::Image ||
        active.front()->track.index != 0) {
        outcome.reason = "最下層が画像の背景ではありません";
        return outcome;
    }
    const auto decoded = mvm::media::loadStaticImage(active.front()->mediaPath);
    if (!decoded.success) {
        outcome.reason = "背景を読めません: " + decoded.error;
        return outcome;
    }
    const auto fitted = mvm::media::fitStillImageToRaster(decoded.image, width, height);
    if (!fitted.success) {
        outcome.reason = "背景を配置できません: " + fitted.error;
        return outcome;
    }
    outcome.expected = fitted.raster.rgba;
    std::vector<OracleLayer> layers;
    for (std::size_t index = 1; index < active.size(); ++index) {
        const auto& clip = *active[index];
        const auto local = frame - clip.timelineStartFrame;
        const auto& effects = clip.effects;
        const auto duration = clipEnd(project, clip) - clip.timelineStartFrame;
        if (clip.kind != TimelineClipKind::Math || clip.mathAnimation.introFrames != 0 ||
            clip.speedNum != clip.speedDen || local < effects.fadeInFrames ||
            local >= duration - effects.fadeOutFrames) {
            // Write・fade の途中の値は独立に求めない。
            outcome.reason = clip.name + ": 静止した数式の保持区間ではありません";
            return outcome;
        }
        const auto x = heldValue(effects.positionXKeys, effects.positionXPercent, local);
        const auto y = heldValue(effects.positionYKeys, effects.positionYPercent, local);
        const auto scaleX = heldValue(effects.scaleXKeys, effects.scaleXPercent, local);
        const auto scaleY = heldValue(effects.scaleYKeys, effects.scaleYPercent, local);
        const auto rotation = heldValue(effects.rotationKeys, effects.rotationDegrees, local);
        const auto opacity = heldValue(effects.opacityKeys, effects.opacityPercent, local);
        const auto cropLeft = heldValue(effects.cropLeftKeys, effects.cropLeftPercent, local);
        const auto cropTop = heldValue(effects.cropTopKeys, effects.cropTopPercent, local);
        const auto cropRight = heldValue(effects.cropRightKeys, effects.cropRightPercent, local);
        const auto cropBottom = heldValue(effects.cropBottomKeys, effects.cropBottomPercent, local);
        if (!x || !y || !scaleX || !scaleY || !rotation || !opacity || !cropLeft || !cropTop ||
            !cropRight || !cropBottom) {
            outcome.reason = clip.name + ": 補間の途中の値は独立に求めません";
            return outcome;
        }
        if (*rotation != 0.0 || *cropLeft != 0.0 || *cropTop != 0.0 || *cropRight != 0.0 ||
            *cropBottom != 0.0 || *scaleX <= 0.0 || *scaleY <= 0.0) {
            outcome.reason = clip.name + ": 回転・crop のある layer は期待値の対象外です";
            return outcome;
        }
        // 位置は画面に対する中心のずれ (%)、拡大は中心を保つ。Project の定義から直接求める。
        const double rectWidth = width * *scaleX / 100.0;
        const double rectHeight = height * *scaleY / 100.0;
        const double rectX = (width - rectWidth) / 2.0 + *x / 100.0 * width;
        const double rectY = (height - rectHeight) / 2.0 + *y / 100.0 * height;
        const auto artifact = artifacts.find(clip.id);
        if (artifact == artifacts.end()) {
            outcome.reason = clip.name + ": 書き出しに渡した成果物がありません";
            return outcome;
        }
        auto composed =
            mvm::app::composeMathClipFromPng(artifact->second, clip.math, width, height);
        if (!composed.success || composed.width != width || composed.height != height) {
            outcome.reason = clip.name + ": 数式の成果物を合成できません: " + composed.error;
            return outcome;
        }
        OracleLayer layer;
        layer.rgba = std::move(composed.rgba);
        layer.rectX = rectX + (shiftFirstLayer && layers.empty() ? 1.0 : 0.0);
        layer.rectY = rectY;
        layer.rectWidth = rectWidth;
        layer.rectHeight = rectHeight;
        layer.integer = *scaleX == 100.0 && *scaleY == 100.0 &&
                        std::abs(rectX - std::round(rectX)) <= 1e-6 &&
                        std::abs(rectY - std::round(rectY)) <= 1e-6;
        layer.opacity = *opacity / 100.0;
        outcome.exactTier = outcome.exactTier && layer.integer;
        layers.push_back(std::move(layer));
    }
    if (burnSubtitles && project.subtitles && project.subtitles->visible) {
        for (const auto& cue : project.subtitles->cues) {
            if (frame < cue.startFrame || frame >= cue.endFrame)
                continue;
            QString error;
            const auto image =
                mvm::app::renderSubtitleRaster(cue, project.subtitles->style, width, height, error);
            if (image.isNull() || image.width() != width || image.height() != height) {
                outcome.reason = "字幕を描けません: " + error.toStdString();
                return outcome;
            }
            OracleLayer layer;
            layer.rgba = imageBytes(image);
            layer.rectWidth = width;
            layer.rectHeight = height;
            layers.push_back(std::move(layer));
        }
    }
    outcome.layers = static_cast<int>(layers.size());
    const auto columns = static_cast<std::size_t>(width);
    std::vector<bool> touched(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (const auto& layer : layers) {
        if (!outcome.exactTier) {
            // 参照: 出力画素 → 素材座標 (矩形の中心を合わせて拡縮) → 双線形 → source-over。
            // MLT は素材の範囲外の画素に触れない。
            const double scaleX = width / layer.rectWidth;
            const double scaleY = height / layer.rectHeight;
            const auto channel = [&](int m, int n, std::size_t c, double fx, double fy) {
                const auto at = [&](int column, int row) {
                    return static_cast<double>(layer.rgba[(static_cast<std::size_t>(row) * columns +
                                                           static_cast<std::size_t>(column)) *
                                                              4U +
                                                          c]);
                };
                const double top = at(m, n) + (at(m + 1, n) - at(m, n)) * fx;
                const double bottom = at(m, n + 1) + (at(m + 1, n + 1) - at(m, n + 1)) * fx;
                return top + (bottom - top) * fy;
            };
            for (int row = 0; row < height; ++row) {
                const double sy =
                    (row - (layer.rectY + layer.rectHeight / 2.0)) * scaleY + height / 2.0;
                if (sy < 0.0 || sy > height - 1)
                    continue;
                const int n = std::min(static_cast<int>(std::floor(sy)), height - 2);
                for (int column = 0; column < width; ++column) {
                    const double sx =
                        (column - (layer.rectX + layer.rectWidth / 2.0)) * scaleX + width / 2.0;
                    if (sx < 0.0 || sx > width - 1)
                        continue;
                    const int m = std::min(static_cast<int>(std::floor(sx)), width - 2);
                    const double fx = sx - m;
                    const double fy = sy - n;
                    const auto offset = (static_cast<std::size_t>(row) * columns +
                                         static_cast<std::size_t>(column)) *
                                        4U;
                    auto* to = outcome.expected.data() + offset;
                    const double sourceAlpha = channel(m, n, 3, fx, fy) / 255.0 * layer.opacity;
                    const double destinationAlpha = to[3] / 255.0;
                    const double alpha =
                        sourceAlpha + destinationAlpha - sourceAlpha * destinationAlpha;
                    if (alpha <= 0.0)
                        continue;
                    const double weight = sourceAlpha / alpha;
                    for (std::size_t c = 0; c < 3; ++c)
                        to[c] = static_cast<std::uint8_t>(std::clamp(
                            std::floor(to[c] * (1.0 - weight) + channel(m, n, c, fx, fy) * weight),
                            0.0, 255.0));
                    to[3] = static_cast<std::uint8_t>(
                        std::clamp(std::floor(255.0 * alpha), 0.0, 255.0));
                    if (sourceAlpha > 0.0)
                        touched[offset / 4U] = true;
                }
            }
            continue;
        }
        // 同じ (下の画素, 上の画素) の組は結果も同じ。組ごとに 1 回だけ oracle を評価する。
        std::unordered_map<std::uint64_t, mvm::test::Pixel> cache;
        const int shiftX = static_cast<int>(std::lround(layer.rectX));
        const int shiftY = static_cast<int>(std::lround(layer.rectY));
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const int sourceColumn = column - shiftX;
                const int sourceRow = row - shiftY;
                // MLT は素材の範囲外の画素に触れない。
                if (sourceColumn < 0 || sourceColumn >= width || sourceRow < 0 ||
                    sourceRow >= height)
                    continue;
                const auto* from =
                    layer.rgba.data() + (static_cast<std::size_t>(sourceRow) * columns +
                                         static_cast<std::size_t>(sourceColumn)) *
                                            4U;
                const mvm::test::Pixel source{from[0], from[1], from[2], from[3]};
                const auto offset =
                    (static_cast<std::size_t>(row) * columns + static_cast<std::size_t>(column)) *
                    4U;
                auto* to = outcome.expected.data() + offset;
                const mvm::test::Pixel destination{to[0], to[1], to[2], to[3]};
                std::uint32_t destinationWord = 0;
                std::uint32_t sourceWord = 0;
                std::memcpy(&destinationWord, destination.data(), 4);
                std::memcpy(&sourceWord, source.data(), 4);
                const auto key = (static_cast<std::uint64_t>(destinationWord) << 32) | sourceWord;
                auto found = cache.find(key);
                if (found == cache.end())
                    found = cache
                                .emplace(key, mvm::test::mltSourceOver(destination, source,
                                                                       layer.opacity))
                                .first;
                std::memcpy(to, found->second.data(), 4);
                if (source[3] != 0 && layer.opacity > 0.0)
                    touched[offset / 4U] = true;
            }
        }
    }
    outcome.inked = std::count(touched.begin(), touched.end(), true);
    outcome.applicable = true;
    return outcome;
}

void compareFrame(OracleOutcome& outcome, const std::vector<std::uint8_t>& actual, int width) {
    for (std::size_t offset = 0; offset + 4 <= actual.size(); offset += 4) {
        int difference = 0;
        for (std::size_t channel = 0; channel < 4; ++channel)
            difference = std::max(difference,
                                  std::abs(static_cast<int>(actual[offset + channel]) -
                                           static_cast<int>(outcome.expected[offset + channel])));
        if (difference == 0)
            continue;
        ++outcome.differing;
        outcome.maxDifference = std::max(outcome.maxDifference, difference);
        if (difference <= (outcome.exactTier ? 0 : kReferenceBound))
            continue;
        if (outcome.mismatches == 0) {
            const auto pixel = offset / 4U;
            QJsonArray expected, observed;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                expected.append(outcome.expected[offset + channel]);
                observed.append(actual[offset + channel]);
            }
            outcome.firstMismatch = QJsonObject{
                {QStringLiteral("x"), static_cast<int>(pixel % static_cast<std::size_t>(width))},
                {QStringLiteral("y"), static_cast<int>(pixel / static_cast<std::size_t>(width))},
                {QStringLiteral("expected"), expected},
                {QStringLiteral("actual"), observed}};
        }
        ++outcome.mismatches;
    }
}

// --- 復号した H.264 ---------------------------------------------------------------

struct Decoded {
    bool ran = false;
    QString error;
    QString codec;
    int width = 0, height = 0;
    long long frames = 0;
    bool timestampsExact = false;
    bool frameSizesMatch = false;
    long long firstPts = -1, step = 0;
};

Decoded probeDecoded(const Options& options, const std::filesystem::path& file, int fpsNum,
                     int fpsDen) {
    Decoded decoded;
    QProcess probe;
    probe.start(toQString(options.ffprobe),
                {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                 QStringLiteral("v:0"), QStringLiteral("-show_entries"),
                 QStringLiteral("stream=codec_name,width,height,time_base:frame=pts,width,height"),
                 QStringLiteral("-of"), QStringLiteral("json"), toQString(file)});
    if (!probe.waitForFinished(600000) || probe.exitCode() != 0) {
        decoded.error = QStringLiteral("ffprobe が失敗しました: ") +
                        QString::fromUtf8(probe.readAllStandardError());
        return decoded;
    }
    const auto root = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
    const auto streams = root.value(QStringLiteral("streams")).toArray();
    const auto frames = root.value(QStringLiteral("frames")).toArray();
    if (streams.size() != 1) {
        decoded.error = QStringLiteral("映像 stream が 1 本ではありません");
        return decoded;
    }
    const auto stream = streams.first().toObject();
    decoded.ran = true;
    decoded.codec = stream.value(QStringLiteral("codec_name")).toString();
    decoded.width = stream.value(QStringLiteral("width")).toInt();
    decoded.height = stream.value(QStringLiteral("height")).toInt();
    decoded.frames = frames.size();
    const auto base = stream.value(QStringLiteral("time_base")).toString().split(QLatin1Char('/'));
    if (base.size() != 2 || frames.isEmpty())
        return decoded;
    const long long baseNum = base[0].toLongLong();
    const long long baseDen = base[1].toLongLong();
    // 1 frame = fpsDen / fpsNum 秒 = step * baseNum / baseDen 秒。
    if (baseNum <= 0 || (baseDen * fpsDen) % (baseNum * fpsNum) != 0)
        return decoded;
    decoded.step = (baseDen * fpsDen) / (baseNum * fpsNum);
    decoded.firstPts = frames.first().toObject().value(QStringLiteral("pts")).toInteger(-1);
    decoded.timestampsExact = decoded.firstPts == 0;
    decoded.frameSizesMatch = true;
    for (qsizetype index = 0; index < frames.size(); ++index) {
        const auto frame = frames[index].toObject();
        if (!frame.contains(QStringLiteral("pts")) ||
            frame.value(QStringLiteral("pts")).toInteger(-1) != index * decoded.step)
            decoded.timestampsExact = false;
        if (frame.value(QStringLiteral("width")).toInt() != decoded.width ||
            frame.value(QStringLiteral("height")).toInt() != decoded.height)
            decoded.frameSizesMatch = false;
    }
    return decoded;
}

QString fileSha256(const std::filesystem::path& path) {
    QFile file(toQString(path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

const char* const kPointNames[MVM_EXPORT_OBSERVE_POINT_COUNT] = {
    "producer_frame_request", "producer_frame_return", "image_request",
    "image_return",           "validator_entry",       "validator_return",
    "encoder_submit",         "encoded_frame",         "muxer_packet"};
const char* const kStageNames[MVM_EXPORT_STAGE_COUNT] = {
    "idle",           "graph_build",   "consumer_start", "render",
    "consumer_close", "output_verify", "done",           "failed"};

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string key = argv[index];
        const std::string value = argv[index + 1];
        if (key == "--project")
            options.project =
                std::filesystem::path(QString::fromLocal8Bit(argv[index + 1]).toStdWString());
        else if (key == "--manim")
            options.manim =
                std::filesystem::path(QString::fromLocal8Bit(argv[index + 1]).toStdWString());
        else if (key == "--out")
            options.output =
                std::filesystem::path(QString::fromLocal8Bit(argv[index + 1]).toStdWString());
        else if (key == "--ffprobe")
            options.ffprobe =
                std::filesystem::path(QString::fromLocal8Bit(argv[index + 1]).toStdWString());
        else if (key == "--gdb")
            options.gdb =
                std::filesystem::path(QString::fromLocal8Bit(argv[index + 1]).toStdWString());
        else if (key == "--cap-seconds")
            options.capSeconds = std::atoi(value.c_str());
        else if (key == "--stall-seconds")
            options.stallSeconds = std::atoi(value.c_str());
        else if (key == "--request-timeout-ms")
            options.requestTimeoutMs = std::atoi(value.c_str());
        else if (key == "--stack-samples")
            options.stackSamples = std::atoi(value.c_str());
        else if (key == "--fixture") {
            for (const auto& fixture : kFixtures)
                if (value == fixture.name)
                    options.fixture = &fixture;
        } else if (key == "--inject") {
            if (value == "drop-frame")
                options.inject = Inject::DropFrame;
            else if (value == "reject-frame")
                options.inject = Inject::RejectFrame;
            else if (value == "oracle-shift")
                options.inject = Inject::OracleShift;
            else if (value == "cancel")
                options.inject = Inject::Cancel;
            else if (value != "none")
                return false;
        } else {
            return false;
        }
    }
    return argc % 2 == 1 && options.fixture && !options.project.empty() && !options.manim.empty() &&
           !options.output.empty() && !options.ffprobe.empty() && options.capSeconds > 0 &&
           options.stallSeconds > 0 && options.stackSamples >= 0 && options.stackSamples <= 4;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::fprintf(stderr,
                     "使い方: mvm_math_score_export_parity --project <project.mvm> --manim "
                     "<manim.exe> --out <directory> --ffprobe <ffprobe.exe> --fixture <名前> "
                     "[--gdb <gdb.exe>] [--cap-seconds N] [--stall-seconds N] "
                     "[--stack-samples N] [--inject drop-frame|reject-frame|oracle-shift|cancel]\n"
                     "fixture:");
        for (const auto& fixture : kFixtures)
            std::fprintf(stderr, " %s", fixture.name);
        std::fprintf(stderr, "\n");
        return kExitProtocol;
    }
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません\n");
        return kExitProtocol;
    }

    struct RuntimeGuard {
        ~RuntimeGuard() { mvm_mlt_runtime_shutdown(); }
    } runtimeGuard;

    const Fixture& fixture = *options.fixture;
    std::error_code directoryError;
    std::filesystem::create_directories(options.output, directoryError);
    if (directoryError) {
        std::fprintf(stderr, "出力directoryを作れません\n");
        return kExitProtocol;
    }
    const auto loaded = mvm::project::loadProjectJson(options.project);
    if (!loaded.success) {
        std::fprintf(stderr, "Projectを開けません: %s\n", loaded.error.c_str());
        return kExitProtocol;
    }
    Project project;
    std::string fixtureError;
    const bool built = fixture.kind == FixtureKind::Synthetic
                           ? buildSynthetic(loaded.project, fixture, project, fixtureError)
                       : fixture.kind == FixtureKind::Region
                           ? buildRegion(loaded.project, fixture, project, fixtureError)
                           : (project = loaded.project, true);
    const auto valid =
        built ? mvm::project::validateTimeline(project) : mvm::project::TimelineValidationResult{};
    if (!built || !valid.success || valid.totalFrames != fixture.frames) {
        std::fprintf(stderr, "fixture '%s' を作れません: %s (frame %lld、想定 %lld)\n",
                     fixture.name, (built ? valid.error : fixtureError).c_str(),
                     static_cast<long long>(valid.totalFrames),
                     static_cast<long long>(fixture.frames));
        return kExitProtocol;
    }
    const std::int64_t expectedFrames = fixture.frames;
    const int width = project.outputWidth;
    const int height = project.outputHeight;
    int maxConcurrentMath = 0;
    for (const auto& probe : project.timelineClips) {
        int concurrent = 0;
        for (const auto& clip : project.timelineClips)
            if (clip.kind == TimelineClipKind::Math && clip.enabled &&
                clip.timelineStartFrame <= probe.timelineStartFrame &&
                probe.timelineStartFrame < clipEnd(project, clip))
                ++concurrent;
        maxConcurrentMath = std::max(maxConcurrentMath, concurrent);
    }

    Observation observation;
    MvmExportObserver observer{};
    observer.mux_trace = 1;
    std::mutex stageMutex;
    std::vector<std::pair<std::string, long long>> stages;
    std::map<std::string, std::filesystem::path> artifacts;
    bool burnSubtitles = false;
    int requestTimeoutMs = 0;
    int productTimeoutMs = 0;
    mvm::app::TimelineExportResult exported;
    std::atomic<long long> exportBeginNs{0};
    std::atomic<long long> exportEndNs{0};
    const std::set<std::int64_t> representative(std::begin(fixture.representative),
                                                std::end(fixture.representative));

    mvm::app::MvmController::ExportRunner runner =
        [&](const Project& exportProject, const mvm::app::TimelineExportRequest& request) {
            auto input = request;
            // timeout は製品の値のまま。診断の上限は watchdog が別に持つ。
            productTimeoutMs = input.timeoutMs;
            if (options.requestTimeoutMs > 0)
                input.timeoutMs = options.requestTimeoutMs;
            requestTimeoutMs = input.timeoutMs;
            artifacts = input.mathArtifacts;
            burnSubtitles = input.burnSubtitles;
            input.exportObserver = &observer;
            input.stageObserver = [&](const char* name) {
                std::lock_guard lock(stageMutex);
                stages.emplace_back(name, mvm_mlt_export_observer_now_ns());
            };
            input.encoderFrameValidator = [&](std::int64_t frame, const std::uint8_t* rgba,
                                              int frameWidth, int frameHeight) {
                if (frameWidth != width || frameHeight != height || rgba == nullptr) {
                    std::lock_guard lock(observation.mutex);
                    observation.error = "pre-YUV の寸法が Project と違います";
                    return false;
                }
                if (options.inject == Inject::RejectFrame && frame == 1)
                    return false;
                if (options.inject == Inject::DropFrame && frame == 1)
                    return true; // 観測の欠落を作る。被覆の検査が落ちなければならない
                const auto bytes = static_cast<std::size_t>(frameWidth) *
                                   static_cast<std::size_t>(frameHeight) * 4U;
                std::uint64_t hash = 0;
                int minAlpha = 0;
                digestFrame(rgba, bytes, hash, minAlpha);
                std::lock_guard lock(observation.mutex);
                observation.width = frameWidth;
                observation.height = frameHeight;
                auto& seen = observation.frames[frame];
                if (seen.calls > 0 && seen.hash != hash)
                    seen.repeatDiffers = true;
                ++seen.calls;
                seen.hash = hash;
                seen.minAlpha = minAlpha;
                if (representative.contains(frame) && !observation.samples.contains(frame))
                    observation.samples.emplace(frame,
                                                std::vector<std::uint8_t>(rgba, rgba + bytes));
                return true;
            };
            exportBeginNs.store(mvm_mlt_export_observer_now_ns());
            exported = mvm::app::exportTimeline(exportProject, input);
            exportEndNs.store(mvm_mlt_export_observer_now_ns());
            return exported;
        };
    auto controller = std::make_unique<mvm::app::MvmController>(
        options.project, options.manim, project, nullptr, std::move(runner),
        mvm::app::MvmController::ExportThreadFactory{},
        mvm::app::MvmController::FileRevealer{
            [](const std::filesystem::path&, QString&) { return true; }});
    if (!controller->holdsProjectLock()) {
        std::fprintf(stderr, "%s\n", controller->statusText().toUtf8().constData());
        return kExitProtocol;
    }

    // 数式の成果物は controller の cache が provenance を検査してから Ready にする。
    QElapsedTimer prepareTimer;
    prepareTimer.start();
    const auto mathState = [&](const TimelineClip& clip) {
        return controller->mathClipData(QString::fromStdString(clip.id))
            .value(QStringLiteral("state"))
            .toString();
    };
    bool prepared = false;
    while (!prepared && prepareTimer.elapsed() < 30 * 60 * 1000) {
        prepared = true;
        for (const auto& clip : controller->projectForTest().timelineClips) {
            if (clip.kind != TimelineClipKind::Math || !clip.enabled)
                continue;
            const auto state = mathState(clip);
            if (state == QStringLiteral("error") || state == QStringLiteral("unavailable")) {
                std::fprintf(stderr, "数式を書き出せません: %s\n", clip.name.c_str());
                return kExitProtocol;
            }
            if (state != QStringLiteral("ready"))
                prepared = false;
        }
        QGuiApplication::processEvents();
        QThread::msleep(20);
    }
    if (!prepared) {
        std::fprintf(stderr, "数式の準備が時間内に終わりませんでした\n");
        return kExitProtocol;
    }

    const auto outputPath = options.output / (std::string(fixture.name) + ".mp4");
    auto temporaryPath = outputPath;
    temporaryPath += ".mvmtmp";
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    std::filesystem::remove(temporaryPath, removeError);

    gTapObserver.store(&observer);
    av_log_set_callback(logTap);

    struct TapGuard {
        ~TapGuard() {
            av_log_set_callback(av_log_default_callback);
            gTapObserver.store(nullptr);
        }
    } tapGuard;

    std::map<DWORD, double> threadCpuBegin, threadCpuLatest;
    threadCpuSeconds(threadCpuBegin);
    const double cpuBegin = processCpuSeconds();
    const long long startNs = mvm_mlt_export_observer_now_ns();
    if (!controller->exportTimeline(QUrl::fromLocalFile(toQString(outputPath)))) {
        std::fprintf(stderr, "書き出しを開始できません: %s\n",
                     controller->statusText().toUtf8().constData());
        return kExitProtocol;
    }

    // --- watchdog ----------------------------------------------------------------
    struct Snapshot {
        double wall, cpu;
        long long counts[MVM_EXPORT_OBSERVE_POINT_COUNT];
        long long consumerPosition;
        int stage;
        int appStages;
    };

    std::vector<Snapshot> snapshots;
    const auto seconds = [&](long long ns) { return static_cast<double>(ns - startNs) / 1e9; };
    const auto takeSnapshot = [&] {
        Snapshot snapshot{};
        snapshot.wall = seconds(mvm_mlt_export_observer_now_ns());
        snapshot.cpu = processCpuSeconds() - cpuBegin;
        for (int point = 0; point < MVM_EXPORT_OBSERVE_POINT_COUNT; ++point)
            snapshot.counts[point] = observer.points[point].count;
        snapshot.consumerPosition = observer.consumer_position;
        snapshot.stage = static_cast<int>(observer.stage);
        {
            std::lock_guard lock(stageMutex);
            snapshot.appStages = static_cast<int>(stages.size());
        }
        return snapshot;
    };
    const auto progressOf = [](const Snapshot& snapshot) {
        long long total = snapshot.stage * 1000000LL + snapshot.appStages * 100000000LL;
        for (const auto count : snapshot.counts)
            total += count;
        return total;
    };
    std::string watchdog = "none"; // none / capped-progressing / stall-busy / stall-idle
    QJsonArray stackFiles;
    QString stackDetail;
    bool cancelSent = false;
    bool cancelInjected = false;
    double lastProgressWall = 0.0, lastProgressCpu = 0.0, cancelWall = 0.0;
    long long lastProgress = -1;
    QElapsedTimer tick;
    tick.start();
    qint64 nextSnapshot = 0;
    while (controller->exporting()) {
        QGuiApplication::processEvents();
        QThread::msleep(20);
        if (options.inject == Inject::Cancel && !cancelSent &&
            observer.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count >= 8) {
            controller->cancelTimelineExport();
            cancelSent = cancelInjected = true;
            cancelWall = seconds(mvm_mlt_export_observer_now_ns());
        }
        if (tick.elapsed() < nextSnapshot)
            continue;
        nextSnapshot = tick.elapsed() + 1000;
        const auto snapshot = takeSnapshot();
        // 記録は 600 点まで（10 分）。超えたら 10 秒ごとに間引く。
        if (snapshots.size() < 600 || static_cast<long long>(snapshot.wall) % 10 == 0)
            snapshots.push_back(snapshot);
        threadCpuSeconds(threadCpuLatest);
        const auto progress = progressOf(snapshot);
        if (progress != lastProgress) {
            lastProgress = progress;
            lastProgressWall = snapshot.wall;
            lastProgressCpu = snapshot.cpu;
        }
        if (cancelSent) {
            if (snapshot.wall - cancelWall > 600.0) {
                std::fprintf(stderr, "取消から 10 分経っても書き出しが戻りません\n");
                std::fflush(stderr);
                std::_Exit(kExitStalled);
            }
            continue;
        }
        const bool stalled = snapshot.wall - lastProgressWall >= options.stallSeconds;
        const bool capped = snapshot.wall >= options.capSeconds;
        if (!stalled && !capped)
            continue;
        if (stalled) {
            // 止まっている間の CPU 時間で、待ちと空回りを分ける。
            const double busy =
                (snapshot.cpu - lastProgressCpu) / (snapshot.wall - lastProgressWall);
            watchdog = busy >= 0.5 ? "stall-busy" : "stall-idle";
        } else {
            watchdog = "capped-progressing";
        }
        // 取消・後始末の前に stack を採る。
        for (int sample = 0; sample < options.stackSamples; ++sample) {
            const auto path = options.output / (std::string(fixture.name) + "-stacks-" +
                                                std::to_string(sample) + ".txt");
            if (captureStacks(options, path, stackDetail))
                stackFiles.append(toQString(path));
            if (sample + 1 < options.stackSamples)
                QThread::msleep(3000);
        }
        snapshots.push_back(takeSnapshot());
        controller->cancelTimelineExport();
        cancelSent = true;
        cancelWall = seconds(mvm_mlt_export_observer_now_ns());
    }
    const long long finishNs = mvm_mlt_export_observer_now_ns();
    const double cpuEnd = processCpuSeconds();
    snapshots.push_back(takeSnapshot());
    av_log_set_callback(av_log_default_callback);
    gTapObserver.store(nullptr);

    // --- 判定 --------------------------------------------------------------------
    QJsonArray failures;
    const auto fail = [&](const QString& message) { failures.append(message); };
    const bool finalExists = std::filesystem::exists(outputPath);
    const bool temporaryExists = std::filesystem::exists(temporaryPath);

    QJsonArray missing, repeated, transparent, unstable;
    bool outside = false;
    for (std::int64_t frame = 0; frame < expectedFrames; ++frame) {
        const auto found = observation.frames.find(frame);
        if (found == observation.frames.end()) {
            if (missing.size() < 64)
                missing.append(static_cast<qint64>(frame));
            continue;
        }
        if (found->second.calls > 1 && repeated.size() < 64)
            repeated.append(QJsonObject{{QStringLiteral("frame"), static_cast<qint64>(frame)},
                                        {QStringLiteral("calls"), found->second.calls}});
        if (found->second.minAlpha < kAdmissibleAlpha && transparent.size() < 64)
            transparent.append(static_cast<qint64>(frame));
        if (found->second.repeatDiffers && unstable.size() < 64)
            unstable.append(static_cast<qint64>(frame));
    }
    long long missingCount = 0, repeatedCount = 0, transparentCount = 0, unstableCount = 0;
    int lowestAlpha = 255;
    for (std::int64_t frame = 0; frame < expectedFrames; ++frame) {
        const auto found = observation.frames.find(frame);
        if (found == observation.frames.end()) {
            ++missingCount;
            continue;
        }
        repeatedCount += found->second.calls > 1;
        transparentCount += found->second.minAlpha < kAdmissibleAlpha;
        lowestAlpha = std::min(lowestAlpha, found->second.minAlpha);
        unstableCount += found->second.repeatDiffers;
    }
    for (const auto& entry : observation.frames)
        outside = outside || entry.first < 0 || entry.first >= expectedFrames;

    Decoded decoded;
    QJsonArray oracleFrames;
    int oracleApplicable = 0, oracleInked = 0;
    if (exported.success) {
        if (!finalExists || temporaryExists)
            fail(QStringLiteral("成功したのに最終 file が無い、または一時 file が残っています"));
        if (exported.frameCount != expectedFrames)
            fail(QStringLiteral("書き出した frame 数 %1 が想定 %2 と違います")
                     .arg(exported.frameCount)
                     .arg(expectedFrames));
        if (missingCount != 0 || outside ||
            static_cast<std::int64_t>(observation.frames.size()) != expectedFrames)
            fail(QStringLiteral("pre-YUV の被覆が出力 frame の集合と一致しません (欠落 %1)")
                     .arg(missingCount));
        if (transparentCount != 0)
            fail(QStringLiteral("pre-YUV の alpha が %1 未満の frame が %2 あります (最小 %3)")
                     .arg(kAdmissibleAlpha)
                     .arg(transparentCount)
                     .arg(lowestAlpha));
        if (unstableCount != 0)
            fail(QStringLiteral("再要求で画素が変わった frame が %1 あります").arg(unstableCount));
        if (observation.width != width || observation.height != height)
            fail(QStringLiteral("pre-YUV の寸法が Project と違います"));

        decoded = probeDecoded(options, outputPath, static_cast<int>(project.timelineFpsNum),
                               static_cast<int>(project.timelineFpsDen));
        if (!decoded.ran)
            fail(QStringLiteral("復号した H.264 を調べられません: ") + decoded.error);
        else {
            if (decoded.codec != QStringLiteral("h264"))
                fail(QStringLiteral("映像 codec が h264 ではありません: ") + decoded.codec);
            if (decoded.frames != expectedFrames)
                fail(QStringLiteral("復号した frame 数 %1 が想定 %2 と違います")
                         .arg(decoded.frames)
                         .arg(expectedFrames));
            if (decoded.width != width || decoded.height != height || !decoded.frameSizesMatch)
                fail(QStringLiteral("復号した frame の寸法が Project と違います"));
            if (!decoded.timestampsExact)
                fail(QStringLiteral("復号した frame の時刻が 0 始まりの等間隔ではありません"));
        }

        for (const auto frame : fixture.representative) {
            QJsonObject entry{{QStringLiteral("frame"), static_cast<qint64>(frame)}};
            const auto sample = observation.samples.find(frame);
            if (sample == observation.samples.end()) {
                entry.insert(QStringLiteral("result"), QStringLiteral("no-sample"));
                fail(QStringLiteral("代表 frame %1 の pre-YUV がありません").arg(frame));
                oracleFrames.append(entry);
                continue;
            }
            const QImage image(sample->second.data(), width, height, width * 4,
                               QImage::Format_RGBA8888);
            const auto png = options.output / (std::string(fixture.name) + "-pre-yuv-" +
                                               std::to_string(frame) + ".png");
            if (!image.copy().save(toQString(png), "PNG"))
                fail(QStringLiteral("代表 frame を保存できません"));
            entry.insert(QStringLiteral("png"), toQString(png));
            auto oracle = expectedFrame(project, artifacts, burnSubtitles, frame,
                                        options.inject == Inject::OracleShift);
            entry.insert(QStringLiteral("layers"), oracle.layers);
            if (!oracle.applicable) {
                // 比較していないことを残す。一致として数えない。
                entry.insert(QStringLiteral("result"), QStringLiteral("oracle-not-applicable"));
                entry.insert(QStringLiteral("reason"), QString::fromStdString(oracle.reason));
                oracleFrames.append(entry);
                continue;
            }
            compareFrame(oracle, sample->second, width);
            ++oracleApplicable;
            oracleInked += oracle.inked > 0;
            entry.insert(QStringLiteral("inked_pixels"), static_cast<qint64>(oracle.inked));
            entry.insert(QStringLiteral("tier"), oracle.exactTier
                                                     ? QStringLiteral("mlt-source-over-oracle")
                                                     : QStringLiteral("double-bilinear-reference"));
            entry.insert(QStringLiteral("allowed_channel_difference"),
                         oracle.exactTier ? 0 : kReferenceBound);
            entry.insert(QStringLiteral("differing_pixels"), static_cast<qint64>(oracle.differing));
            entry.insert(QStringLiteral("mismatched_pixels"),
                         static_cast<qint64>(oracle.mismatches));
            entry.insert(QStringLiteral("max_channel_difference"), oracle.maxDifference);
            // exact は全画素の完全一致。within-bound は参照との差が許容内 (完全一致ではない)。
            entry.insert(QStringLiteral("result"),
                         oracle.mismatches != 0  ? QStringLiteral("mismatch")
                         : oracle.differing == 0 ? QStringLiteral("exact")
                                                 : QStringLiteral("within-bound"));
            if (oracle.mismatches != 0) {
                entry.insert(QStringLiteral("first_mismatch"), oracle.firstMismatch);
                fail(QStringLiteral("代表 frame %1 が独立期待値と %2 画素で違います")
                         .arg(frame)
                         .arg(oracle.mismatches));
            }
            oracleFrames.append(entry);
        }
        if (oracleApplicable == 0)
            fail(QStringLiteral("独立期待値と比較できた代表 frame がありません"));
        else if (maxConcurrentMath > 0 && oracleInked == 0)
            fail(QStringLiteral("比較した代表 frame に数式の画素がありません (空振り)"));
    } else if (finalExists || temporaryExists) {
        fail(QStringLiteral("失敗・取消なのに出力 file が残っています"));
    }

    // --- 記録 --------------------------------------------------------------------
    QJsonObject report;
    report.insert(QStringLiteral("format"), QStringLiteral("mvm-math-score-export-diagnosis/1"));
    report.insert(QStringLiteral("fixture"), QString::fromLatin1(fixture.name));
    report.insert(QStringLiteral("inject"), static_cast<int>(options.inject));
    report.insert(QStringLiteral("expected_frames"), static_cast<qint64>(expectedFrames));
    report.insert(QStringLiteral("video_tracks"), static_cast<int>(project.videoTracks.size()));
    report.insert(QStringLiteral("clips"), static_cast<int>(project.timelineClips.size()));
    report.insert(QStringLiteral("max_concurrent_math"), maxConcurrentMath);
    report.insert(QStringLiteral("burn_subtitles"), burnSubtitles);
    report.insert(QStringLiteral("request_timeout_ms"), requestTimeoutMs);
    report.insert(QStringLiteral("product_timeout_ms"), productTimeoutMs);
    report.insert(QStringLiteral("request_timeout_is_product"),
                  requestTimeoutMs == productTimeoutMs);
    report.insert(QStringLiteral("cap_seconds"), options.capSeconds);
    report.insert(QStringLiteral("stall_seconds"), options.stallSeconds);
    report.insert(QStringLiteral("watchdog"), QString::fromStdString(watchdog));
    report.insert(QStringLiteral("cancel_injected"), cancelInjected);
    report.insert(QStringLiteral("export_success"), exported.success);
    report.insert(QStringLiteral("export_cancelled"), exported.cancelled);
    report.insert(QStringLiteral("export_error"), QString::fromStdString(exported.error));
    report.insert(QStringLiteral("export_frame_count"), static_cast<qint64>(exported.frameCount));
    report.insert(QStringLiteral("wall_seconds"), seconds(finishNs));
    report.insert(QStringLiteral("cpu_seconds"), cpuEnd - cpuBegin);
    report.insert(QStringLiteral("export_call_wall_seconds"),
                  static_cast<double>(exportEndNs.load() - exportBeginNs.load()) / 1e9);
    report.insert(QStringLiteral("final_file_exists"), finalExists);
    report.insert(QStringLiteral("temporary_file_exists"), temporaryExists);
    report.insert(QStringLiteral("observer_cancel_requested"),
                  static_cast<int>(observer.cancel_requested));
    report.insert(QStringLiteral("animation_samples"),
                  static_cast<qint64>(observer.animation_samples));
    report.insert(QStringLiteral("animation_keyframes"),
                  static_cast<qint64>(observer.animation_keyframes));
    report.insert(QStringLiteral("consumer_position"),
                  static_cast<qint64>(observer.consumer_position));
    report.insert(QStringLiteral("final_stage"),
                  QString::fromLatin1(kStageNames[std::clamp<long>(
                      static_cast<long>(observer.stage), 0, MVM_EXPORT_STAGE_COUNT - 1)]));

    QJsonArray appStages;
    {
        std::lock_guard lock(stageMutex);
        for (const auto& stage : stages)
            appStages.append(
                QJsonObject{{QStringLiteral("stage"), QString::fromStdString(stage.first)},
                            {QStringLiteral("t"), seconds(stage.second)}});
    }
    report.insert(QStringLiteral("app_stages"), appStages);
    QJsonArray mltStages;
    for (int stage = 0; stage < MVM_EXPORT_STAGE_COUNT; ++stage)
        if (observer.stage_ns[stage] != 0)
            mltStages.append(
                QJsonObject{{QStringLiteral("stage"), QString::fromLatin1(kStageNames[stage])},
                            {QStringLiteral("t"), seconds(observer.stage_ns[stage])}});
    report.insert(QStringLiteral("mlt_stages"), mltStages);

    std::set<long> observerThreads;
    QJsonArray points;
    for (int point = 0; point < MVM_EXPORT_OBSERVE_POINT_COUNT; ++point) {
        const auto& state = observer.points[point];
        QJsonObject entry{{QStringLiteral("point"), QString::fromLatin1(kPointNames[point])},
                          {QStringLiteral("count"), static_cast<qint64>(state.count)}};
        if (state.count > 0) {
            entry.insert(QStringLiteral("first_t"), seconds(state.first_ns));
            entry.insert(QStringLiteral("first_frame"), static_cast<qint64>(state.first_frame));
            entry.insert(QStringLiteral("first_thread"), static_cast<qint64>(state.first_thread));
            entry.insert(QStringLiteral("last_t"), seconds(state.last_ns));
            entry.insert(QStringLiteral("last_frame"), static_cast<qint64>(state.last_frame));
            entry.insert(QStringLiteral("last_thread"), static_cast<qint64>(state.last_thread));
            observerThreads.insert(static_cast<long>(state.first_thread));
            observerThreads.insert(static_cast<long>(state.last_thread));
        }
        points.append(entry);
    }
    report.insert(QStringLiteral("points"), points);
    QJsonArray census;
    for (int index = 0; index < std::min(gCensusCount.load(), kCensusCapacity); ++index)
        census.append(QString::fromLatin1(gCensus[index]).trimmed());
    report.insert(QStringLiteral("ffmpeg_log_formats"), census);

    QJsonArray samples;
    const long sampleAttempts = observer.sample_count;
    const long sampleCount = std::min<long>(sampleAttempts, MVM_EXPORT_OBSERVE_SAMPLE_CAPACITY);
    for (long index = 0; index < sampleCount; ++index) {
        const auto& sample = observer.samples[index];
        samples.append(
            QJsonObject{{QStringLiteral("point"), QString::fromLatin1(kPointNames[sample.point])},
                        {QStringLiteral("ordinal"), static_cast<qint64>(sample.ordinal)},
                        {QStringLiteral("frame"), static_cast<qint64>(sample.frame)},
                        {QStringLiteral("t"), seconds(sample.ns)},
                        {QStringLiteral("thread"), static_cast<qint64>(sample.thread)},
                        {QStringLiteral("stage"), QString::fromLatin1(kStageNames[sample.stage])},
                        {QStringLiteral("cancel"), sample.cancel_requested}});
    }
    report.insert(QStringLiteral("samples"), samples);
    report.insert(QStringLiteral("samples_dropped"),
                  static_cast<qint64>(sampleAttempts - sampleCount));

    QJsonArray timeline;
    for (const auto& snapshot : snapshots) {
        QJsonArray counts;
        for (const auto count : snapshot.counts)
            counts.append(static_cast<qint64>(count));
        timeline.append(QJsonObject{
            {QStringLiteral("wall"), snapshot.wall},
            {QStringLiteral("cpu"), snapshot.cpu},
            {QStringLiteral("counts"), counts},
            {QStringLiteral("consumer_position"), static_cast<qint64>(snapshot.consumerPosition)},
            {QStringLiteral("stage"), QString::fromLatin1(kStageNames[snapshot.stage])},
            {QStringLiteral("app_stages"), snapshot.appStages}});
    }
    report.insert(QStringLiteral("timeline"), timeline);

    std::vector<std::pair<double, DWORD>> threadCpu;
    for (const auto& [thread, cpu] : threadCpuLatest) {
        const auto before = threadCpuBegin.find(thread);
        threadCpu.emplace_back(cpu - (before == threadCpuBegin.end() ? 0.0 : before->second),
                               thread);
    }
    std::sort(threadCpu.rbegin(), threadCpu.rend());
    QJsonArray threads;
    for (std::size_t index = 0; index < std::min<std::size_t>(threadCpu.size(), 12); ++index)
        threads.append(
            QJsonObject{{QStringLiteral("thread"), static_cast<qint64>(threadCpu[index].second)},
                        {QStringLiteral("cpu_seconds"), threadCpu[index].first},
                        {QStringLiteral("seen_at_observe_point"),
                         observerThreads.contains(static_cast<long>(threadCpu[index].second))}});
    report.insert(QStringLiteral("thread_cpu_top"), threads);
    report.insert(QStringLiteral("thread_count_seen"), static_cast<int>(threadCpuLatest.size()));
    report.insert(QStringLiteral("stack_files"), stackFiles);
    report.insert(QStringLiteral("stack_detail"), stackDetail);

    report.insert(QStringLiteral("pre_yuv_observed_frames"),
                  static_cast<qint64>(observation.frames.size()));
    report.insert(QStringLiteral("pre_yuv_missing_count"), missingCount);
    report.insert(QStringLiteral("pre_yuv_missing_first"), missing);
    report.insert(QStringLiteral("pre_yuv_repeated_count"), repeatedCount);
    report.insert(QStringLiteral("pre_yuv_repeated_first"), repeated);
    report.insert(QStringLiteral("pre_yuv_outside_domain"), outside);
    report.insert(QStringLiteral("pre_yuv_admissible_alpha"), kAdmissibleAlpha);
    report.insert(QStringLiteral("pre_yuv_lowest_alpha"), lowestAlpha);
    report.insert(QStringLiteral("pre_yuv_low_alpha_count"), transparentCount);
    report.insert(QStringLiteral("pre_yuv_low_alpha_first"), transparent);
    report.insert(QStringLiteral("pre_yuv_repeat_differs_count"), unstableCount);
    report.insert(QStringLiteral("decoded"),
                  QJsonObject{{QStringLiteral("ran"), decoded.ran},
                              {QStringLiteral("codec"), decoded.codec},
                              {QStringLiteral("width"), decoded.width},
                              {QStringLiteral("height"), decoded.height},
                              {QStringLiteral("frames"), static_cast<qint64>(decoded.frames)},
                              {QStringLiteral("first_pts"), static_cast<qint64>(decoded.firstPts)},
                              {QStringLiteral("pts_step"), static_cast<qint64>(decoded.step)},
                              {QStringLiteral("timestamps_exact"), decoded.timestampsExact},
                              {QStringLiteral("pixels_are_authority"), false}});
    report.insert(QStringLiteral("oracle_frames"), oracleFrames);
    report.insert(QStringLiteral("oracle_applicable_frames"), oracleApplicable);

    QJsonArray artifactList;
    for (const auto& [clip, path] : artifacts) {
        auto provenance = path;
        provenance.replace_extension(L".txt");
        artifactList.append(
            QJsonObject{{QStringLiteral("clip"), QString::fromStdString(clip)},
                        {QStringLiteral("png"), toQString(path.filename())},
                        {QStringLiteral("png_sha256"), fileSha256(path)},
                        {QStringLiteral("provenance_sha256"), fileSha256(provenance)}});
    }
    report.insert(QStringLiteral("math_artifacts"), artifactList);
    if (finalExists)
        report.insert(QStringLiteral("output_sha256"), fileSha256(outputPath));

    // frame ごとの digest。修正の前後で pre-YUV が同じかを runner が比べる。
    {
        QFile digests(toQString(options.output / (std::string(fixture.name) + "-pre-yuv.tsv")));
        if (digests.open(QIODevice::WriteOnly)) {
            for (const auto& [frame, seen] : observation.frames)
                digests.write(QStringLiteral("%1\t%2\t%3\t%4\n")
                                  .arg(frame)
                                  .arg(seen.calls)
                                  .arg(seen.hash, 16, 16, QLatin1Char('0'))
                                  .arg(seen.minAlpha)
                                  .toUtf8());
        }
    }

    int exitCode = kExitPass;
    QString verdict = QStringLiteral("PASS");
    if (!exported.success && (finalExists || temporaryExists)) {
        exitCode = kExitLeak;
        verdict = QStringLiteral("OUTPUT_LEAKED");
    } else if (watchdog == "stall-busy" || watchdog == "stall-idle") {
        exitCode = kExitStalled;
        verdict = QStringLiteral("STALLED");
    } else if (watchdog == "capped-progressing") {
        exitCode = kExitCapped;
        verdict = QStringLiteral("CAPPED_WHILE_PROGRESSING");
    } else if (cancelInjected && !exported.success) {
        exitCode = kExitCancelled;
        verdict = QStringLiteral("CANCELLED");
    } else if (!exported.success) {
        exitCode = kExitExportFailed;
        verdict = QStringLiteral("EXPORT_FAILED");
    } else if (!failures.isEmpty()) {
        exitCode = kExitParityFailed;
        verdict = QStringLiteral("PARITY_FAILED");
    }
    report.insert(QStringLiteral("failures"), failures);
    report.insert(QStringLiteral("verdict"), verdict);
    report.insert(QStringLiteral("exit_code"), exitCode);
    QFile file(toQString(options.output / (std::string(fixture.name) + "-report.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "照合結果を書けません\n");
        return kExitProtocol;
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    file.close();

    const auto& rendered = observer.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN];
    std::fprintf(stderr,
                 "%s: %s wall=%.1fs cpu=%.1fs 合成済み=%lld/%lld encoder=%lld x264=%lld mux=%lld\n",
                 fixture.name, verdict.toUtf8().constData(), seconds(finishNs), cpuEnd - cpuBegin,
                 static_cast<long long>(rendered.count), static_cast<long long>(expectedFrames),
                 static_cast<long long>(observer.points[MVM_EXPORT_OBSERVE_ENCODER_SUBMIT].count),
                 static_cast<long long>(observer.points[MVM_EXPORT_OBSERVE_ENCODED_PACKET].count),
                 static_cast<long long>(observer.points[MVM_EXPORT_OBSERVE_MUXED_PACKET].count));
    for (const auto& failure : failures)
        std::fprintf(stderr, "  不成立: %s\n", failure.toString().toUtf8().constData());
    return exitCode;
}
