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
#include "media/math/math_group_compose.h"
#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media/still_image/static_image.h"
#include "media/still_image/still_image_decoder.h"
#include "mlt_property_probe.h"
#include "mlt_rgba_oracle.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <windows.h>
#include <algorithm>
#include <array>
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
#include <psapi.h>
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
#include <QPainter>
#include <QPainterPath>
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
    // P3-3B: 0〜255 なら、背景をその alpha で一様な画像に差し替える (RGB は alpha と無関係な模様)。
    // 256 なら、alpha が左から右へ 0〜255 に変わる画像にする。257 なら、1〜255 の全部の値を画面
    // 全体へ散らす (どの値も字形の下に来る。0 は含めない)。-1 は元の背景のまま。
    int backgroundAlpha = -1;
    // P3-3D: 対象外の配置の対照。1 = 一部の layer に拡縮・回転・crop を付ける (layer 2・5・8)、
    // 2 = 全部の layer を回転させる (group にできる clip が無い)、3 = 背景 (V1) を拡縮する。
    int geometry = 0;
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
    // 実 Project の 88〜91 秒。分数線の伸縮と分子の移動を含む（元の代表 frame 5340・5370・5400）。
    {"v4-region", FixtureKind::Region, 0, 180, 5280, {60, 90, 120}},
    {"v4-full", FixtureKind::Full, 0, 6840, 0, {600, 5370, 6720}},
    // --- layer の渡し方の比較用 (P1)。上の fixture は変えず、別名で足す。全 frame を比べる。 ---
    {"cmp-integer", FixtureKind::Synthetic, 1, 120, 0, {0, 60, 119}, true}, // 整数位置
    {"cmp-fraction-fade", FixtureKind::Synthetic, 1, 120, 0, {0, 60, 119}}, // 端数位置と fade
    {"cmp-layers-16", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}},    // 重なる複数 layer
    {"cmp-layers-48", FixtureKind::Synthetic, 48, 120, 0, {0, 60, 119}},
    // 実 Project の 20〜22 秒: 分子の移動と分数線の出現 (位置・不透明度の補間の途中を含む)。
    {"cmp-region-motion", FixtureKind::Region, 0, 120, 1200, {30, 60, 96}},
    // 実 Project の 88〜91 秒: 分数線の伸縮 (拡縮する layer は切り出しの対象外になる)。
    {"cmp-region-scale", FixtureKind::Region, 0, 180, 5280, {60, 90, 120}},
    // --- P2-A: keyframe と layer 数を独立に振る。上の fixture は変えない。 ---
    {"p2-one-480", FixtureKind::Synthetic, 1, 480, 0, {0, 240, 449}},
    {"p2-one-1920", FixtureKind::Synthetic, 1, 1920, 0, {0, 960, 1889}},
    {"p2-layers-4-480", FixtureKind::Synthetic, 4, 480, 0, {0, 240, 449}},
    {"p2-layers-48-480", FixtureKind::Synthetic, 48, 480, 0, {0, 240, 449}},
    // v4 の前半・中盤・後半の 1 秒。同じ区間を、graph を Project 全体のまま書き出した場合
    // (v4-full に --range) と比べる。
    {"p2-window-early", FixtureKind::Region, 0, 60, 1200, {0, 30, 59}},
    {"p2-window-middle", FixtureKind::Region, 0, 60, 3400, {0, 30, 59}},
    {"p2-window-late", FixtureKind::Region, 0, 60, 6500, {0, 30, 59}},
    // --- P3-3B: 下地の alpha を変えた対照。16 層 (端数位置・不透明度の補間・重なり) は
    // cmp-layers-16 と同じで、背景だけを差し替える。上の fixture は変えない。 ---
    {"alpha-255", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 255},
    {"alpha-254", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 254},
    {"alpha-253", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 253},
    {"alpha-128", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 128},
    {"alpha-1", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 1},
    {"alpha-0", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 0},
    {"alpha-ramp", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 256},
    {"alpha-all-nonzero", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, 257},
    // --- P3-3D: 書き出しの前の判定の対照。数式は cmp-layers-16 と同じ 16 層。 ---
    {"geom-mixed", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, -1, 1},
    {"geom-all-unsupported", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, -1, 2},
    {"base-transformed", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, -1, 3},
    // --- P3-4: V1 の隙間 (黒の color producer) の上の数式。4 = V1 に clip が無い、
    // 5 = V1 の clip が前半だけ (後半は隙間。clip の境界をまたぐ)。 ---
    {"base-gap", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, -1, 4},
    {"base-half-gap", FixtureKind::Synthetic, 16, 120, 0, {0, 60, 119}, false, -1, 5},
};

enum class Inject { None, DropFrame, RejectFrame, OracleShift, Cancel, CompareCorrupt };

// --compare の結果。完全一致と 1 階調以内を同じ終了コードにしない。
constexpr int kExitCompareWithinOne = 13; // candidate は baseline と 1 階調以内 (完全一致ではない)
constexpr int kExitCompareDiffers = 14;   // 1 階調を超える画素がある
constexpr int kExitCompareInvalid = 15; // 全 frame を比べられなかった、または baseline が再現しない

// 不透明な下地 (alpha 255) へ MLT の affine が重ねた結果の alpha は 255 * (as + ad - as * ad) の
// 切り捨てで、ad >= 254/255 なら 255 - (1 - as) >= 254。これより低い alpha は合成の異常として扱う。
// 代表 frame では alpha も含めた 4 channel を独立期待値と比べる (下の expectedFrame)。
constexpr int kAdmissibleAlpha = 254;

struct Options {
    std::filesystem::path project, manim, output, ffprobe, gdb;
    const Fixture* fixture = nullptr;
    Inject inject = Inject::None;
    // FullFrame 以外なら、baseline の後に同じ fixture をその渡し方でも書き出して全画素を比べる。
    mvm::app::MathLayerStaging compare = mvm::app::MathLayerStaging::FullFrame;
    // 0 は製品の値のまま。並列度の測定にだけ使う (report に残る)。
    int renderThreads = 0;
    mvm::app::MathLayerStaging mainStaging = mvm::app::MathLayerStaging::FullFrame;
    // 診断用 (0 が製品の動作)。値を変えずに keyframe の数だけを増やす。
    int keyframeStride = 0;
    // 診断用 (count 0 は全体)。graph は Project 全体のまま、この区間だけを書き出す。
    // parity (独立期待値・復号) は判定しない。
    std::int64_t rangeFirst = 0, rangeCount = 0;
    // 試作 (P2-A): 主の書き出しで transition を区間ごとに分ける / 比較の candidate として分ける。
    bool splitTransitions = false;
    bool compareSplit = false;
    // P3-1.1: 全 frame について、純粋な合成器の出力を MLT の pre-YUV と byte 単位で比べる。
    bool compositorAll = false;
    // P3-1.2: 比較の対照として入れる故障 (0 なし、1 成果物なし、2 成果物を読めない、3 mapping
    // なし)。
    int compositorFaultCode = 0;
    // P3-1.2: 合成器へ渡す値 (false は書き出しの計画の倍精度の値、true は MLT と同じ "%g" 経由)。
    bool compositorSerialized = false;
    // P3-1.2 の診断: Synthetic fixture の数式 layer のうち、この番号 (0 始まり) だけを残す。
    // 空なら fixture のまま。製品の条件でも固定 fixture でもない (report に残る)。
    std::vector<int> layerSelect;
    // P3-1.2 の診断: frame:x:y の画素について、MLT と合成器の値と layer ごとの途中の値を残す。
    std::vector<std::array<std::int64_t, 3>> probes;
    int splitFault = 0; // candidate だけに入れる故障 (比較の対照)
    // 試作 (P3-3A): 数式 group の transition。主の書き出しを含む全部の書き出しで使う / 比較の
    // candidate だけで使う。どちらも製品の経路ではない (report に残る)。
    bool mathGroupMain = false;
    bool compareMathGroup = false;
    int mathGroupFault = 0; // group を使う書き出しへ入れる故障 (対照)
    // P3-3B。
    int mathGroupMainMode = 1;    // --math-group の mode (1 合成、2 測るだけ)
    int compareMathGroupMode = 1; // --compare math-group / math-group-probe
    int mathGroupRule = 0;        // 0 = 下に従来の layer が重ならない clip だけ、1 = 順序だけを守る
    // P3-3D。
    bool ignoreAlphaProof = false; // V1 の alpha の証明を無いものとして扱う (証明が得られない対照)
    // P3-5A: frame ごとに、全 byte の SHA-256 も採る (64 bit の digest とは独立の hash)。
    bool strongDigest = false;
    // P3-5A: この frame (timeline の絶対 frame) の RGBA をそのまま file へ書く。
    std::set<std::int64_t> dumpFrames;
    // P3-6A: 製品の選択 (MvmController の内部の選択 → 要求の mathGroupOptimization) で書き出す。
    // harness は observer の診断用の mode を設定しない (書き出しが、要求を見て自分で決める)。
    int productMain = 0;    // --product-math-group: 0 指定なし、1 off、2 on (主の書き出し)
    int compareProduct = 0; // --compare product-off / product-math-group: 1 off、2 on (candidate)
    // candidate に observer を付けるか。既定は付けない (製品の要求と同じ)。diagnostic なら付ける
    // (故障の注入・下地の観測を、製品の選択と組み合わせるため)。
    bool productObserver = false;
    int v1BackgroundFault = 0; // P3-5A.2: V1 の背景の保護の対照用の故障 (mvm_mlt_v1_background.h)
    int forgeAlphaProof = 0;   // 非 0: V1 の alpha の証明をこの値に差し替える (誤った証明の対照)
    int candidateRepeats = 1;  // candidate を同じ process の中で続けて書き出す回数
    int cancelAfterImages = 8; // --inject cancel: 合成済みの画像がこの数になったら取り消す
    int mathGroupAlphaFloor = -1; // 0〜255 なら合成器の契約の下限の代わりに使う (実験)
    // 準備済みの画像の上限 (事前に固定)。-1 は持ち方ごとの既定: 全画面は 800 MB (P3-3A の実測
    // 783 MB)、中身のある範囲だけは 64 MB (P3-3C)。
    long long mathGroupBudgetMb = -1;
    // 分け方 (0 は既定値): 変わる区間の上限 / 変わらない区間の下限 / 変わらない区間の上限。
    int splitAnimatedRun = 0, splitStaticRun = 0, splitStaticChunk = 0;
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
    if (fixture.geometry == 3) // V1 に effect を付ける (出す画素を確かめていない状態になる)
        bottom.effects.scaleXPercent = bottom.effects.scaleYPercent = 110.0;
    if (fixture.geometry == 5)
        bottom.sourceOutFrame = bottom.sourceFrameCount = fixture.frames / 2;
    if (fixture.geometry != 4)
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
        // P3-3D: 対象外の配置。key を消して、clip 全体で一定の値にする。
        const bool scale = fixture.geometry == 1 && layer == 2;
        const bool rotate = fixture.geometry == 2 || (fixture.geometry == 1 && layer == 5);
        const bool crop = fixture.geometry == 1 && layer == 8;
        if (scale) {
            clip.effects.scaleXKeys.clear();
            clip.effects.scaleYKeys.clear();
            clip.effects.scaleXPercent = clip.effects.scaleYPercent = 120.0;
        }
        if (rotate) {
            clip.effects.rotationKeys.clear();
            clip.effects.rotationDegrees = 12.0;
        }
        if (crop) {
            clip.effects.cropLeftKeys.clear();
            clip.effects.cropLeftPercent = 10.0;
        }
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
    QByteArray sha256;          // --strong-digest のときだけ (全 byte の SHA-256)
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
// P3 の spike (--reference-mode): 0 = 倍精度の参照 (既定)、1・2 = binary32 で MLT の式をなぞる。
// 判定の基準 (上の許容) は変えない。差のある画素数を report で読むためだけに使う。
int gReferenceMode = 0;
// P3-3C: 準備済みの画像の持ち方 (--prepared-storage)。既定は中身のある範囲だけ。
mvm::math::MathRasterStorage gPreparedStorage = mvm::math::MathRasterStorage::RoiOnly;
const mvm::app::TimelineExportPlan* gPlan = nullptr;

struct OracleOutcome {
    bool applicable = false;
    bool unsupported = false; // 合成器が対応していない変形を含む (比べていない)
    bool exactTier = true;
    std::string reason;
    int layers = 0;
    long long inked = 0;     // 上の layer が寄与した画素数（0 なら空振り）
    long long differing = 0; // 1 階調でも違う画素
    QString firstDiffering;
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
        if (gReferenceMode >= 7 && gPlan) {
            bool cropFilter = false;
            // mode 7: 位置と不透明度を、書き出しが MLT へ渡すのと同じ計画の値にする。
            // 上の独立な導出 (Project の % から直接) と、倍精度の末尾の桁が違いうる。
            for (const auto& mapped : gPlan->clips) {
                if (mapped.audio || mapped.subtitle || mapped.renderClip.id != clip.id ||
                    frame < mapped.timelineStartFrame ||
                    frame >= mapped.timelineStartFrame + mapped.timelineDurationFrames)
                    continue;
                const auto at = static_cast<std::size_t>(frame - mapped.timelineStartFrame);
                layer.rectX =
                    mapped.motionFrames.empty() ? mapped.rectX : mapped.motionFrames[at].rectX;
                layer.rectY =
                    mapped.motionFrames.empty() ? mapped.rectY : mapped.motionFrames[at].rectY;
                layer.opacity = mapped.opacityKeys[at].opacity;
                cropFilter = !mapped.motionFrames.empty();
            }
            if ((gReferenceMode == 8 || gReferenceMode == 9) && cropFilter) {
                // mode 8: 書き出しは motion のある clip に qtcrop を付ける (crop が 0 でも)。
                // MLT の qtcrop は画像を QPainter で描き直す (filter_qtcrop.cpp)。同じ手順を通す。
                QImage foreground(layer.rgba.data(), width, height, width * 4,
                                  QImage::Format_RGBA8888);
                QImage background(width, height, QImage::Format_RGBA8888);
                background.fill(QColor(0, 0, 0, 0));
                {
                    QPainter painter(&background);
                    painter.setRenderHints(QPainter::Antialiasing);
                    QPainterPath path;
                    path.addRoundedRect(0, 0, width, height, 0, 0);
                    painter.setClipPath(path);
                    painter.drawImage(QPointF(0, 0), foreground.copy());
                }
                layer.rgba = imageBytes(background);
            }
        }
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
        if (gReferenceMode == 9) {
            // mode 9: 純粋な合成器 (src/media/math/math_group_compose) そのものを使う。
            // 字形の範囲 (alpha > 0 の外接矩形) を渡して、範囲だけを走査する経路を通す。
            mvm::math::detail::UncheckedMathGroupLayer input;
            input.rgba = layer.rgba.data();
            input.width = width;
            input.height = height;
            input.rectX = layer.rectX;
            input.rectY = layer.rectY;
            input.opacity = layer.opacity;
            int left = width, top = height, right = -1, bottom = -1;
            for (int row = 0; row < height; ++row)
                for (int column = 0; column < width; ++column)
                    if (layer.rgba[(static_cast<std::size_t>(row) * columns +
                                    static_cast<std::size_t>(column)) *
                                       4U +
                                   3U] != 0) {
                        left = std::min(left, column);
                        right = std::max(right, column);
                        top = std::min(top, row);
                        bottom = std::max(bottom, row);
                    }
            if (right >= 0) {
                input.roiX = left;
                input.roiY = top;
                input.roiWidth = right - left + 1;
                input.roiHeight = bottom - top + 1;
            }
            const auto before = outcome.expected;
            std::string composeError;
            if (layer.rectWidth != width || layer.rectHeight != height ||
                !mvm::math::detail::composeMathGroupUnchecked(outcome.expected.data(), width,
                                                              height, columns * 4U, &input, 1,
                                                              composeError)) {
                outcome.reason = "合成器が対応していない layer です: " + composeError;
                return outcome;
            }
            for (std::size_t pixel = 0; pixel < touched.size(); ++pixel)
                if (std::memcmp(before.data() + pixel * 4U, outcome.expected.data() + pixel * 4U,
                                4) != 0)
                    touched[pixel] = true;
            continue;
        }
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
                    if (gReferenceMode != 0) {
                        // P3 の spike: MLT の interp.h (interpBL_b32) の式を、binary32 のまま
                        // source の 順に計算する。独立した合成で MLT と byte
                        // 単位で一致させられるかを見る。 mode 2 は既存 oracle と同じく、不透明度に
                        // 1/255 を先に掛ける。
                        const float x = static_cast<float>(sx);
                        const float y = static_cast<float>(sy);
                        const int mf = std::min(static_cast<int>(std::floor(x)), width - 2);
                        const int nf = std::min(static_cast<int>(std::floor(y)), height - 2);
                        const float dx = x - static_cast<float>(mf);
                        const float dy = y - static_cast<float>(nf);
                        const auto texel = [&](int column2, int row2, std::size_t c) {
                            return static_cast<float>(
                                layer.rgba[(static_cast<std::size_t>(row2) * columns +
                                            static_cast<std::size_t>(column2)) *
                                               4U +
                                           c]);
                        };
                        const auto sample = [&](std::size_t c) {
                            const float a =
                                texel(mf, nf, c) + (texel(mf + 1, nf, c) - texel(mf, nf, c)) * dx;
                            const float b = texel(mf, nf + 1, c) +
                                            (texel(mf + 1, nf + 1, c) - texel(mf, nf + 1, c)) * dx;
                            return a + (b - a) * dy;
                        };
                        const float opacity = static_cast<float>(layer.opacity);
                        // mode 3: 下の alpha も割り算ではなく 1/255 の掛け算にする (既存 oracle
                        // と同じ)。
                        const float below = gReferenceMode == 3
                                                ? static_cast<float>(to[3]) * (1.0f / 255.0f)
                                                : static_cast<float>(to[3]) / 255.0f;
                        const float above = gReferenceMode >= 2
                                                ? sample(3) * (opacity * (1.0f / 255.0f))
                                                : sample(3) / 255.0f * opacity;
                        const float total = above + below - above * below;
                        if (total <= 0.0f)
                            continue;
                        const float weight = above / total;
                        for (std::size_t c = 0; c < 3; ++c) {
                            // 色の式の候補 (1 画素の差の原因の切り分け):
                            //   4 = 下 + (上 - 下) * 重み、5・6 = 積和を 1 回の丸めで行う (FMA)。
                            const float lower = static_cast<float>(to[c]);
                            const float upper = sample(c);
                            const float mixed =
                                gReferenceMode == 4 ? lower + (upper - lower) * weight
                                : gReferenceMode == 5
                                    ? std::fma(upper, weight, lower * (1.0f - weight))
                                : gReferenceMode == 6
                                    ? std::fma(lower, 1.0f - weight, upper * weight)
                                    : lower * (1.0f - weight) + upper * weight;
                            to[c] = static_cast<std::uint8_t>(mixed);
                        }
                        to[3] = static_cast<std::uint8_t>(255 * total);
                        if (above > 0.0f)
                            touched[offset / 4U] = true;
                        continue;
                    }
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
        if (outcome.differing == 0) {
            // 最初に違った画素 (許容の内外によらず残す)。
            const auto pixel = offset / 4U;
            outcome.firstDiffering =
                QStringLiteral("x=%1 y=%2 expected=%3,%4,%5,%6 actual=%7,%8,%9,%10")
                    .arg(pixel % static_cast<std::size_t>(width))
                    .arg(pixel / static_cast<std::size_t>(width))
                    .arg(outcome.expected[offset])
                    .arg(outcome.expected[offset + 1])
                    .arg(outcome.expected[offset + 2])
                    .arg(outcome.expected[offset + 3])
                    .arg(actual[offset])
                    .arg(actual[offset + 1])
                    .arg(actual[offset + 2])
                    .arg(actual[offset + 3]);
        }
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

// --- 合成器の全 frame 比較 (P3-1.1・P3-1.2) --------------------------------------
//
// 1 frame の結果は 3 つに分ける。混ぜて数えない。
//   Ready        期待値を作れた (この後 MLT の pre-YUV と全画素を比べる)
//   Unsupported  事前に決めた対象外の変形を含む (下の kUnsupported* だけ)
//   Error        それ以外のすべて (成果物・mapping・準備の失敗、合成器の想定外の拒否)
// Error は「対象外」に数えない。1 つでもあれば比較全体が不成立になる。
enum class CompositorStatus { Ready, Unsupported, Error };

// 事前に決めた対象外の理由。ここに無い理由で frame を対象外にしない。
constexpr const char* kUnsupportedNotMath = "unsupported-non-math-layer";
constexpr const char* kUnsupportedMathWrite = "unsupported-math-write";
constexpr const char* kUnsupportedMathTransform = "unsupported-math-transform";
constexpr const char* kUnsupportedGeometry = "unsupported-scale-rotation-shear-crop";

// 比較の対照 (negative control) として入れる故障。どれも Error にならなければならない。
enum class CompositorFault { None, MissingArtifact, UnreadableArtifact, MissingMapping };

// MLT は animation の値を文字列 ("%g") へ直してから読み直す。合成器へ渡す値を同じ形にするか。
enum class CompositorValues { Plan, Serialized };

double serializedValue(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%g", value);
    return std::strtod(text, nullptr);
}

QString bytesSha256(const std::uint8_t* data, std::size_t size) {
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size)),
            QCryptographicHash::Sha256)
            .toHex());
}

// 準備済みの入力。画素と字形の範囲は合成器の PreparedMathRaster が持つ (作った後は変えられず、
// 範囲は画素から求まる。外から渡された矩形を使わない)。
struct PreparedRaster {
    mvm::math::PreparedMathRaster image;
    bool qtcrop = false;
    std::string source;     // 成果物の path、または字幕の識別
    QString sourceSha256;   // 成果物 file の内容
    QString preparedSha256; // 前処理 (qtcrop の描き直し) の後の RGBA
    QString preprocessing;  // 前処理の識別
};

struct CompositorLayer {
    std::string clipId, name;
    int track = 0;
    const PreparedRaster* raster = nullptr;
    double rectX = 0.0, rectY = 0.0, opacity = 1.0;     // 合成器へ渡した値
    double planX = 0.0, planY = 0.0, planOpacity = 1.0; // 書き出しの計画の値
};

struct CompositorFrame {
    CompositorStatus status = CompositorStatus::Error;
    std::string reason;
    std::vector<std::uint8_t> background;
    std::vector<std::uint8_t> expected;
    std::vector<CompositorLayer> layers;
};

// 1 回の比較 run が所有する文脈。Project・計画・成果物の対応はこの run のものだけを参照し、
// 準備済みの画像は run の外へ出ない (別の Project・別の run の画像を使い回せない)。
class CompositorRun {
public:
    CompositorRun(const Project& project, const mvm::app::TimelineExportPlan& plan,
                  std::map<std::string, std::filesystem::path> artifacts, bool burnSubtitles,
                  CompositorFault fault, CompositorValues values)
        : project_(project), plan_(plan), artifacts_(std::move(artifacts)),
          burnSubtitles_(burnSubtitles), fault_(fault), values_(values),
          width_(project.outputWidth), height_(project.outputHeight) {
        for (const auto& clip : project_.timelineClips) {
            if (clip.kind != TimelineClipKind::Math || !clip.enabled)
                continue;
            faultClip_ = clip.id; // 故障は最後の (最も上の) 数式 clip に入れる
        }
        if (fault_ == CompositorFault::MissingArtifact)
            artifacts_.erase(faultClip_);
        if (fault_ == CompositorFault::UnreadableArtifact && artifacts_.contains(faultClip_))
            artifacts_[faultClip_] += ".missing";
    }

    CompositorRun(const CompositorRun&) = delete;
    CompositorRun& operator=(const CompositorRun&) = delete;

    CompositorFrame frame(std::int64_t frame) {
        CompositorFrame out;
        const auto finish = [&](CompositorStatus status, std::string reason) {
            out.status = status;
            out.reason = std::move(reason);
            return std::move(out);
        };
        std::vector<const TimelineClip*> active;
        for (const auto& clip : project_.timelineClips) {
            if (!clip.enabled || clip.track.kind != TrackKind::Video)
                continue;
            if (frame < clip.timelineStartFrame || frame >= clipEnd(project_, clip))
                continue;
            active.push_back(&clip);
        }
        std::stable_sort(active.begin(), active.end(), [](const auto* left, const auto* right) {
            return left->track.index < right->track.index;
        });
        if (active.empty() || active.front()->kind != TimelineClipKind::Image ||
            active.front()->track.index != 0)
            return finish(CompositorStatus::Error, "最下層が画像の背景ではありません");
        std::string error;
        const auto* background = backgroundRaster(active.front()->mediaPath, error);
        if (!background)
            return finish(CompositorStatus::Error, error);
        out.background = *background;
        for (std::size_t index = 1; index < active.size(); ++index) {
            const auto& clip = *active[index];
            if (clip.kind != TimelineClipKind::Math)
                return finish(CompositorStatus::Unsupported, kUnsupportedNotMath);
            const mvm::app::TimelineExportClipMapping* mapped = nullptr;
            int matches = 0;
            for (const auto& candidate : plan_.clips) {
                if (candidate.audio || candidate.subtitle || candidate.renderClip.id != clip.id ||
                    frame < candidate.timelineStartFrame ||
                    frame >= candidate.timelineStartFrame + candidate.timelineDurationFrames)
                    continue;
                if (fault_ == CompositorFault::MissingMapping && clip.id == faultClip_)
                    continue;
                mapped = &candidate;
                ++matches;
            }
            if (matches != 1)
                return finish(CompositorStatus::Error,
                              clip.name + ": 書き出しの計画の mapping が " +
                                  std::to_string(matches) + " 件です (1 件でなければならない)");
            if (mapped->mathWrite)
                return finish(CompositorStatus::Unsupported, kUnsupportedMathWrite);
            if (!mapped->mathTransformId.empty())
                return finish(CompositorStatus::Unsupported, kUnsupportedMathTransform);
            const auto at = static_cast<std::size_t>(frame - mapped->timelineStartFrame);
            const bool motion = !mapped->motionFrames.empty();
            if (at >= mapped->opacityKeys.size() ||
                mapped->opacityKeys[at].localFrame != static_cast<std::int64_t>(at) ||
                (motion && (at >= mapped->motionFrames.size() ||
                            mapped->motionFrames[at].localFrame != static_cast<std::int64_t>(at))))
                return finish(CompositorStatus::Error,
                              clip.name + ": 計画に frame ごとの値がありません");
            const auto* moving = motion ? &mapped->motionFrames[at] : nullptr;
            const double rectWidth = motion ? moving->rectWidth : mapped->rectWidth;
            const double rectHeight = motion ? moving->rectHeight : mapped->rectHeight;
            const bool transformed =
                motion ? moving->rotationDegrees != 0.0 || moving->shearDegrees != 0.0 ||
                             moving->cropLeft != 0.0 || moving->cropTop != 0.0 ||
                             moving->cropRight != 0.0 || moving->cropBottom != 0.0
                       : mapped->rotationDegrees != 0.0 || mapped->shearDegrees != 0.0 ||
                             mapped->cropLeft != 0 || mapped->cropTop != 0 ||
                             mapped->cropRight != 0 || mapped->cropBottom != 0;
            if (rectWidth != width_ || rectHeight != height_ || transformed)
                return finish(CompositorStatus::Unsupported, kUnsupportedGeometry);
            const auto* raster = mathRaster(clip, motion, error);
            if (!raster)
                return finish(CompositorStatus::Error, clip.name + ": " + error);
            CompositorLayer layer;
            layer.clipId = clip.id;
            layer.name = clip.name;
            layer.track = clip.track.index;
            layer.raster = raster;
            layer.planX = motion ? moving->rectX : mapped->rectX;
            layer.planY = motion ? moving->rectY : mapped->rectY;
            layer.planOpacity = mapped->opacityKeys[at].opacity;
            const bool serialized = values_ == CompositorValues::Serialized;
            layer.rectX = serialized ? serializedValue(layer.planX) : layer.planX;
            layer.rectY = serialized ? serializedValue(layer.planY) : layer.planY;
            layer.opacity = serialized ? serializedValue(layer.planOpacity) : layer.planOpacity;
            out.layers.push_back(std::move(layer));
        }
        if (burnSubtitles_ && project_.subtitles && project_.subtitles->visible) {
            for (std::size_t index = 0; index < project_.subtitles->cues.size(); ++index) {
                const auto& cue = project_.subtitles->cues[index];
                if (frame < cue.startFrame || frame >= cue.endFrame)
                    continue;
                const auto* raster = subtitleRaster(index, error);
                if (!raster)
                    return finish(CompositorStatus::Error, error);
                CompositorLayer layer;
                layer.clipId = "subtitle:" + std::to_string(index);
                layer.name = layer.clipId;
                layer.track = -1;
                layer.raster = raster;
                out.layers.push_back(std::move(layer));
            }
        }
        out.expected = out.background;
        std::vector<mvm::math::MathGroupPlacement> placements;
        for (const auto& layer : out.layers)
            placements.push_back({&layer.raster->image, layer.rectX, layer.rectY, layer.opacity});
        // 事前に決めた対象外はここまでに除いてある。ここでの拒否は想定外なので ERROR にする。
        if (!mvm::math::composeMathGroup(out.expected.data(), width_, height_,
                                         static_cast<std::size_t>(width_) * 4U, placements.data(),
                                         placements.size(), error))
            return finish(CompositorStatus::Error, "合成器が入力を拒否しました: " + error);
        return finish(CompositorStatus::Ready, {});
    }

    // 1 layer だけを重ねる (画素の途中の値を追う trace 用)。途中の下地は alpha が 254 の画素を
    // 含むので、検査を通さない入口を使う。範囲は準備済みの画像のものをそのまま渡す。
    bool composeLayer(std::vector<std::uint8_t>& destination, const CompositorLayer& layer,
                      std::string& error) const {
        const auto& image = layer.raster->image;
        mvm::math::detail::UncheckedMathGroupLayer input;
        input.rgba = image.storedRgba();
        input.storedRegion = image.storage() == mvm::math::MathRasterStorage::RoiOnly;
        input.storedX = image.storedX();
        input.storedY = image.storedY();
        input.storedWidth = image.storedWidth();
        input.storedHeight = image.storedHeight();
        input.width = width_;
        input.height = height_;
        input.rectX = layer.rectX;
        input.rectY = layer.rectY;
        input.opacity = layer.opacity;
        input.roiX = image.roiX();
        input.roiY = image.roiY();
        input.roiWidth = image.roiWidth();
        input.roiHeight = image.roiHeight();
        return mvm::math::detail::composeMathGroupUnchecked(destination.data(), width_, height_,
                                                            static_cast<std::size_t>(width_) * 4U,
                                                            &input, 1, error);
    }

    // run の終わりに、使った入力が準備の時から変わっていないことを確かめる。
    bool verifyInputs(std::string& error) const {
        for (const auto& [key, raster] : rasters_) {
            if (bytesSha256(raster.image.storedRgba(), raster.image.storedBytes()) !=
                raster.preparedSha256) {
                error = "準備済みの画像が run の途中で変わりました: " + key;
                return false;
            }
            if (!raster.sourceSha256.isEmpty() &&
                fileSha256Of(std::filesystem::path(
                    QString::fromStdString(raster.source).toStdWString())) != raster.sourceSha256) {
                error = "成果物が run の途中で変わりました: " + raster.source;
                return false;
            }
        }
        return true;
    }

    QJsonArray rasterIdentities() const {
        QJsonArray out;
        for (const auto& [key, raster] : rasters_)
            out.append(
                QJsonObject{{QStringLiteral("key"), QString::fromStdString(key)},
                            {QStringLiteral("source"), QString::fromStdString(raster.source)},
                            {QStringLiteral("source_sha256"), raster.sourceSha256},
                            {QStringLiteral("preprocessing"), raster.preprocessing},
                            {QStringLiteral("prepared_sha256"), raster.preparedSha256},
                            {QStringLiteral("roi"),
                             QJsonArray{raster.image.roiX(), raster.image.roiY(),
                                        raster.image.roiWidth(), raster.image.roiHeight()}}});
        return out;
    }

    int width() const { return width_; }

    int height() const { return height_; }

private:
    static QString fileSha256Of(const std::filesystem::path& path) {
        QFile file(QString::fromStdWString(path.wstring()));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        return QString::fromLatin1(hash.result().toHex());
    }

    std::size_t bytes() const {
        return static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U;
    }

    // 画素を合成器の準備へ渡して確定させる。以後は画素も範囲も変えられない。
    bool seal(PreparedRaster& raster, const std::vector<std::uint8_t>& rgba,
              std::string& error) const {
        if (rgba.size() != bytes()) {
            error = "準備する画像の大きさが出力と違います";
            return false;
        }
        if (!mvm::math::PreparedMathRaster::prepare(rgba.data(), width_, height_,
                                                    static_cast<std::size_t>(width_) * 4U,
                                                    raster.image, error, gPreparedStorage))
            return false;
        // 保存している画素 (持ち方による) の SHA-256。
        raster.preparedSha256 = bytesSha256(raster.image.storedRgba(), raster.image.storedBytes());
        return true;
    }

    const std::vector<std::uint8_t>* backgroundRaster(const std::filesystem::path& path,
                                                      std::string& error) {
        const auto key = path.u8string();
        const std::string id(key.begin(), key.end());
        if (const auto found = backgrounds_.find(id); found != backgrounds_.end())
            return &found->second;
        const auto decoded = mvm::media::loadStaticImage(path);
        if (!decoded.success) {
            error = "背景を読めません: " + decoded.error;
            return nullptr;
        }
        const auto fitted = mvm::media::fitStillImageToRaster(decoded.image, width_, height_);
        if (!fitted.success) {
            error = "背景を配置できません: " + fitted.error;
            return nullptr;
        }
        return &backgrounds_.emplace(id, fitted.raster.rgba).first->second;
    }

    // 数式 clip の画像。書き出しが MLT へ渡すものと同じにする: motion のある clip は qtcrop
    // と同じ描き直しを通す。識別は成果物の内容・色などの入力・出力の大きさ・前処理で決める。
    const PreparedRaster* mathRaster(const TimelineClip& clip, bool qtcrop, std::string& error) {
        const auto artifact = artifacts_.find(clip.id);
        if (artifact == artifacts_.end()) {
            error = "書き出しに渡した成果物がありません";
            return nullptr;
        }
        auto identity = artifactSha_.find(clip.id);
        if (identity == artifactSha_.end()) {
            const auto sha = fileSha256Of(artifact->second);
            if (sha.isEmpty()) {
                error = "成果物を読めません";
                return nullptr;
            }
            identity = artifactSha_.emplace(clip.id, sha).first;
        }
        const std::string key =
            identity->second.toStdString() + "|" + clip.math.syntax + "|" +
            std::to_string(clip.math.fontSize) + "|" + clip.math.color + "|" +
            clip.math.backgroundColor + "|" + std::to_string(width_) + "x" +
            std::to_string(height_) + (qtcrop ? "|qtcrop-rect-full-radius-0" : "|none") + "|" +
            bytesSha256(reinterpret_cast<const std::uint8_t*>(clip.math.source.data()),
                        clip.math.source.size())
                .toStdString();
        if (const auto found = rasters_.find(key); found != rasters_.end())
            return &found->second;
        auto composed =
            mvm::app::composeMathClipFromPng(artifact->second, clip.math, width_, height_);
        if (!composed.success || composed.width != width_ || composed.height != height_) {
            error = "数式の成果物を合成できません: " + composed.error;
            return nullptr;
        }
        PreparedRaster raster;
        std::vector<std::uint8_t> rgba = std::move(composed.rgba);
        raster.qtcrop = qtcrop;
        const auto path = artifact->second.u8string();
        raster.source = std::string(path.begin(), path.end());
        raster.sourceSha256 = identity->second;
        raster.preprocessing = qtcrop ? QStringLiteral("qtcrop rect=0,0,%1,%2 radius=0 "
                                                       "color=#00000000 (QPainter 描き直し)")
                                            .arg(width_)
                                            .arg(height_)
                                      : QStringLiteral("none");
        if (qtcrop) {
            // MLT の qtcrop (filter_qtcrop.cpp) と同じ描き直し。
            QImage foreground(rgba.data(), width_, height_, width_ * 4, QImage::Format_RGBA8888);
            QImage repainted(width_, height_, QImage::Format_RGBA8888);
            repainted.fill(QColor(0, 0, 0, 0));
            {
                QPainter painter(&repainted);
                painter.setRenderHints(QPainter::Antialiasing);
                QPainterPath path2;
                path2.addRoundedRect(0, 0, width_, height_, 0, 0);
                painter.setClipPath(path2);
                painter.drawImage(QPointF(0, 0), foreground.copy());
            }
            rgba = imageBytes(repainted);
        }
        if (!seal(raster, rgba, error))
            return nullptr;
        return &rasters_.emplace(key, std::move(raster)).first->second;
    }

    const PreparedRaster* subtitleRaster(std::size_t index, std::string& error) {
        const std::string key = "subtitle:" + std::to_string(index);
        if (const auto found = rasters_.find(key); found != rasters_.end())
            return &found->second;
        QString renderError;
        const auto image =
            mvm::app::renderSubtitleRaster(project_.subtitles->cues[index],
                                           project_.subtitles->style, width_, height_, renderError);
        if (image.isNull() || image.width() != width_ || image.height() != height_) {
            error = "字幕を描けません: " + renderError.toStdString();
            return nullptr;
        }
        PreparedRaster raster;
        raster.source = key;
        raster.preprocessing = QStringLiteral("none");
        if (!seal(raster, imageBytes(image), error))
            return nullptr;
        return &rasters_.emplace(key, std::move(raster)).first->second;
    }

    const Project& project_;
    const mvm::app::TimelineExportPlan& plan_;
    std::map<std::string, std::filesystem::path> artifacts_;
    const bool burnSubtitles_;
    const CompositorFault fault_;
    const CompositorValues values_;
    const int width_, height_;
    std::string faultClip_;
    std::map<std::string, QString> artifactSha_;
    std::map<std::string, PreparedRaster> rasters_; // node は動かない (pointer を貸す)
    std::map<std::string, std::vector<std::uint8_t>> backgrounds_;
};

QString floatText(double value) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.17g", value);
    return QString::fromLatin1(text);
}

QJsonArray rgbaArray(const std::uint8_t* pixel) {
    return QJsonArray{pixel[0], pixel[1], pixel[2], pixel[3]};
}

// 1 画素について、layer を 1 枚ずつ重ねた途中の値を残す。合成器そのものを layer ごとに呼ぶ。
// 素材座標と texel は、合成器と同じ求め方 (倍精度で 1.0 を足し続けて binary32 へ落とす) で出す。
QJsonArray tracePixel(const CompositorRun& run, const CompositorFrame& frame, int x, int y) {
    const int width = run.width();
    const int height = run.height();
    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x)) *
                        4U;
    const auto coordinate = [](double rect, int size, int index) {
        double position = -(rect + static_cast<double>(size) * 1.0 / 2.0);
        for (int step = 0; step < index; ++step)
            position += 1.0;
        return position + static_cast<double>(size) / 2.0;
    };
    QJsonArray out;
    auto canvas = frame.background;
    out.append(QJsonObject{{QStringLiteral("layer"), QStringLiteral("background")},
                           {QStringLiteral("after"), rgbaArray(canvas.data() + offset)}});
    for (std::size_t index = 0; index < frame.layers.size(); ++index) {
        const auto& layer = frame.layers[index];
        std::uint8_t before[4];
        std::memcpy(before, canvas.data() + offset, 4);
        std::string error;
        if (!run.composeLayer(canvas, layer, error))
            break;
        const double sx = coordinate(layer.rectX, width, x);
        const double sy = coordinate(layer.rectY, height, y);
        QJsonObject entry{{QStringLiteral("index"), static_cast<int>(index)},
                          {QStringLiteral("clip"), QString::fromStdString(layer.clipId)},
                          {QStringLiteral("name"), QString::fromStdString(layer.name)},
                          {QStringLiteral("source_x"), floatText(sx)},
                          {QStringLiteral("source_y"), floatText(sy)},
                          {QStringLiteral("before"), rgbaArray(before)},
                          {QStringLiteral("after"), rgbaArray(canvas.data() + offset)}};
        bool contributes = false;
        if (sx >= 0.0 && sx <= width - 1 && sy >= 0.0 && sy <= height - 1) {
            const float fxSource = static_cast<float>(sx);
            const float fySource = static_cast<float>(sy);
            const int m = std::min(static_cast<int>(std::floor(fxSource)), width - 2);
            const int n = std::min(static_cast<int>(std::floor(fySource)), height - 2);
            QJsonArray texels;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    // 元の画像の座標で読む (保存していない texel は 0)。
                    std::uint8_t texel[4];
                    layer.raster->image.texel(m + dx, n + dy, texel);
                    contributes = contributes || texel[3] != 0;
                    texels.append(rgbaArray(texel));
                }
            entry.insert(QStringLiteral("texel_column"), m);
            entry.insert(QStringLiteral("texel_row"), n);
            entry.insert(QStringLiteral("weight_x"), floatText(fxSource - static_cast<float>(m)));
            entry.insert(QStringLiteral("weight_y"), floatText(fySource - static_cast<float>(n)));
            entry.insert(QStringLiteral("texels"), texels);
        }
        entry.insert(QStringLiteral("contributes"), contributes);
        if (!contributes && std::memcmp(before, canvas.data() + offset, 4) == 0)
            continue; // この画素に掛からない layer は一覧 (layers) にだけ残す
        entry.insert(QStringLiteral("opacity"), floatText(layer.opacity));
        entry.insert(QStringLiteral("opacity_binary32"),
                     floatText(static_cast<float>(layer.opacity)));
        entry.insert(QStringLiteral("qtcrop"), layer.raster->qtcrop);
        out.append(entry);
    }
    return out;
}

QJsonArray layerList(const CompositorFrame& frame) {
    QJsonArray out;
    for (const auto& layer : frame.layers)
        out.append(QJsonObject{{QStringLiteral("clip"), QString::fromStdString(layer.clipId)},
                               {QStringLiteral("name"), QString::fromStdString(layer.name)},
                               {QStringLiteral("track"), layer.track},
                               {QStringLiteral("plan_x"), floatText(layer.planX)},
                               {QStringLiteral("plan_y"), floatText(layer.planY)},
                               {QStringLiteral("plan_opacity"), floatText(layer.planOpacity)},
                               {QStringLiteral("used_x"), floatText(layer.rectX)},
                               {QStringLiteral("used_y"), floatText(layer.rectY)},
                               {QStringLiteral("used_opacity"), floatText(layer.opacity)},
                               {QStringLiteral("qtcrop"), layer.raster->qtcrop},
                               {QStringLiteral("artifact_sha256"), layer.raster->sourceSha256},
                               {QStringLiteral("prepared_sha256"), layer.raster->preparedSha256}});
    return out;
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

// --- 数式 group の観測 (P3-3A・P3-3B) ---------------------------------------------

long long workingSetBytes(bool peak) {
    PROCESS_MEMORY_COUNTERS counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return static_cast<long long>(peak ? counters.PeakWorkingSetSize : counters.WorkingSetSize);
}

// 背景の alpha ごとの集計 (alpha の対照 fixture)。
struct AlphaTally {
    long long pixels = 0;           // その alpha の背景の画素 (全 frame の延べ)
    long long touched = 0;          // MLT の出力が背景と違う画素
    long long touchedAlphaOnly = 0; // そのうち、alpha が 0 のままで色だけが変わった画素
    long long mismatching = 0;      // candidate が MLT と違う画素
};

// group の合成の直前の下地 (group の下の全 layer を重ねた RGBA) の alpha の分布。
// callback は render worker から並行に呼ばれる。集計は group ごとに全部の呼び出しを足し、
// 呼び出しごとの記録は上限までにする (超えたら truncated を立てる)。
struct GroupProbe {
    static constexpr int kAssignmentCapacity = 65536;
    static constexpr int kSampleLimit = 256;

    struct PerGroup {
        long long calls = 0;
        long long callsBelowOpaque = 0;          // 最小の alpha が 255 未満だった呼び出し
        long long callsFootprintBelowOpaque = 0; // 字形の範囲の中に 255 未満の画素があった呼び出し
        int minimumAlpha = 255;
        int minimumFootprintAlpha = 255;
        std::int64_t firstFrame = -1, lastFrame = -1;
        std::array<long long, 256> full{};      // 全画面
        std::array<long long, 256> footprint{}; // その frame で見えている member の字形の範囲
    };

    std::mutex mutex;
    std::map<int, PerGroup> groups;
    QJsonArray samples;
    long long calls = 0;
    std::vector<int> assignment; // MLT の層が書く (clip ごと)

    static QJsonObject histogramJson(const std::array<long long, 256>& histogram) {
        long long lower = 0, total = 0;
        QJsonObject values; // 0 でない値だけ
        int minimum = 255, maximum = 0;
        for (int alpha = 0; alpha < 256; ++alpha) {
            const auto count = histogram[static_cast<std::size_t>(alpha)];
            if (count == 0)
                continue;
            total += count;
            minimum = std::min(minimum, alpha);
            maximum = std::max(maximum, alpha);
            if (alpha < 253)
                lower += count;
            values.insert(QString::number(alpha), static_cast<qint64>(count));
        }
        return QJsonObject{{QStringLiteral("pixels"), static_cast<qint64>(total)},
                           {QStringLiteral("minimum"), total > 0 ? minimum : -1},
                           {QStringLiteral("maximum"), total > 0 ? maximum : -1},
                           {QStringLiteral("alpha_255"), static_cast<qint64>(histogram[255])},
                           {QStringLiteral("alpha_254"), static_cast<qint64>(histogram[254])},
                           {QStringLiteral("alpha_253"), static_cast<qint64>(histogram[253])},
                           {QStringLiteral("alpha_below_253"), static_cast<qint64>(lower)},
                           {QStringLiteral("histogram"), values}};
    }

    QJsonObject toJson() {
        std::lock_guard lock(mutex);
        std::array<long long, 256> full{}, footprint{};
        QJsonArray perGroup;
        long long below = 0, footprintBelow = 0;
        for (const auto& [id, group] : groups) {
            for (std::size_t alpha = 0; alpha < 256; ++alpha) {
                full[alpha] += group.full[alpha];
                footprint[alpha] += group.footprint[alpha];
            }
            below += group.callsBelowOpaque;
            footprintBelow += group.callsFootprintBelowOpaque;
            perGroup.append(QJsonObject{
                {QStringLiteral("group"), id},
                {QStringLiteral("calls"), static_cast<qint64>(group.calls)},
                {QStringLiteral("first_frame"), static_cast<qint64>(group.firstFrame)},
                {QStringLiteral("last_frame"), static_cast<qint64>(group.lastFrame)},
                {QStringLiteral("minimum_alpha"), group.minimumAlpha},
                {QStringLiteral("minimum_alpha_in_glyph_roi"), group.minimumFootprintAlpha},
                {QStringLiteral("calls_with_alpha_below_255"),
                 static_cast<qint64>(group.callsBelowOpaque)},
                {QStringLiteral("calls_with_alpha_below_255_in_glyph_roi"),
                 static_cast<qint64>(group.callsFootprintBelowOpaque)},
                {QStringLiteral("full_frame"), histogramJson(group.full)},
                {QStringLiteral("glyph_roi"), histogramJson(group.footprint)}});
        }
        return QJsonObject{
            {QStringLiteral("calls"), static_cast<qint64>(calls)},
            {QStringLiteral("calls_with_alpha_below_255"), static_cast<qint64>(below)},
            {QStringLiteral("calls_with_alpha_below_255_in_glyph_roi"),
             static_cast<qint64>(footprintBelow)},
            {QStringLiteral("full_frame"), histogramJson(full)},
            {QStringLiteral("glyph_roi"), histogramJson(footprint)},
            {QStringLiteral("groups"), perGroup},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("samples_limit"), kSampleLimit},
            {QStringLiteral("samples_truncated"), calls > samples.size()}};
    }
};

void groupProbeCallback(void* opaque, long long frame, int group, const unsigned char* rgba,
                        int width, int height, std::size_t stride,
                        const MvmExportMathGroupLayer* layers, int count) {
    auto* probe = static_cast<GroupProbe*>(opaque);
    std::array<long long, 256> full{}, footprint{};
    // 字形の範囲: member ごとに、準備済みの画像の範囲を置く位置へずらし、補間の分だけ 1
    // 画素広げる。
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height));
    for (int index = 0; index < count; ++index) {
        const auto* raster =
            static_cast<const mvm::math::PreparedMathRaster*>(layers[index].raster);
        if (!raster || raster->roiWidth() <= 0)
            continue;
        const int left =
            std::max(0, static_cast<int>(std::floor(raster->roiX() + layers[index].rect_x)) - 1);
        const int top =
            std::max(0, static_cast<int>(std::floor(raster->roiY() + layers[index].rect_y)) - 1);
        const int right =
            std::min(width, static_cast<int>(std::ceil(raster->roiX() + raster->roiWidth() +
                                                       layers[index].rect_x)) +
                                1);
        const int bottom =
            std::min(height, static_cast<int>(std::ceil(raster->roiY() + raster->roiHeight() +
                                                        layers[index].rect_y)) +
                                 1);
        for (int row = top; row < bottom; ++row)
            std::memset(mask.data() +
                            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(left),
                        1, static_cast<std::size_t>(std::max(0, right - left)));
    }
    for (int row = 0; row < height; ++row) {
        const auto* line = rgba + static_cast<std::size_t>(row) * stride;
        const auto* masked =
            mask.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        for (int column = 0; column < width; ++column) {
            const auto alpha = line[static_cast<std::size_t>(column) * 4U + 3U];
            ++full[alpha];
            footprint[alpha] += masked[column];
        }
    }
    int minimum = 255, footprintMinimum = 255;
    long long footprintPixels = 0;
    for (int alpha = 255; alpha >= 0; --alpha) {
        if (full[static_cast<std::size_t>(alpha)] != 0)
            minimum = alpha;
        if (footprint[static_cast<std::size_t>(alpha)] != 0)
            footprintMinimum = alpha;
        footprintPixels += footprint[static_cast<std::size_t>(alpha)];
    }
    std::lock_guard lock(probe->mutex);
    ++probe->calls;
    auto& entry = probe->groups[group];
    ++entry.calls;
    entry.callsBelowOpaque += minimum < 255;
    entry.callsFootprintBelowOpaque += footprintMinimum < 255;
    entry.minimumAlpha = std::min(entry.minimumAlpha, minimum);
    entry.minimumFootprintAlpha = std::min(entry.minimumFootprintAlpha, footprintMinimum);
    entry.firstFrame =
        entry.firstFrame < 0 ? frame : std::min<std::int64_t>(entry.firstFrame, frame);
    entry.lastFrame = std::max<std::int64_t>(entry.lastFrame, frame);
    for (std::size_t alpha = 0; alpha < 256; ++alpha) {
        entry.full[alpha] += full[alpha];
        entry.footprint[alpha] += footprint[alpha];
    }
    if (probe->samples.size() < GroupProbe::kSampleLimit) {
        long long lower = 0, footprintLower = 0;
        for (std::size_t alpha = 0; alpha < 253; ++alpha) {
            lower += full[alpha];
            footprintLower += footprint[alpha];
        }
        probe->samples.append(QJsonObject{
            {QStringLiteral("frame"), static_cast<qint64>(frame)},
            {QStringLiteral("group"), group},
            {QStringLiteral("visible_members"), count},
            {QStringLiteral("minimum_alpha"), minimum},
            {QStringLiteral("alpha_255"), static_cast<qint64>(full[255])},
            {QStringLiteral("alpha_254"), static_cast<qint64>(full[254])},
            {QStringLiteral("alpha_253"), static_cast<qint64>(full[253])},
            {QStringLiteral("alpha_below_253"), lower},
            {QStringLiteral("glyph_roi_pixels"), footprintPixels},
            {QStringLiteral("glyph_roi_minimum_alpha"), footprintMinimum},
            {QStringLiteral("glyph_roi_alpha_254"), static_cast<qint64>(footprint[254])},
            {QStringLiteral("glyph_roi_alpha_253"), static_cast<qint64>(footprint[253])},
            {QStringLiteral("glyph_roi_alpha_below_253"), footprintLower}});
    }
}

QJsonObject groupJson(const MvmExportObserver& observed, long long live, long long active) {
    const auto frames = observed.math_group_frames;
    const auto per = [&](long long ns) {
        return frames > 0 ? static_cast<double>(ns) / 1e6 / static_cast<double>(frames) : 0.0;
    };
    return QJsonObject{
        {QStringLiteral("enabled"), observed.diagnostic_math_group != 0},
        {QStringLiteral("mode"), observed.diagnostic_math_group},
        {QStringLiteral("fault"), observed.diagnostic_math_group_fault},
        {QStringLiteral("transitions"), static_cast<qint64>(observed.math_group_transitions)},
        {QStringLiteral("grouped_clips"), static_cast<qint64>(observed.math_group_clips)},
        {QStringLiteral("retained_clips"), static_cast<qint64>(observed.math_group_retained_clips)},
        {QStringLiteral("retained_geometry"),
         static_cast<qint64>(observed.math_group_retained_geometry)},
        {QStringLiteral("retained_above_retained_layer"),
         static_cast<qint64>(observed.math_group_retained_above)},
        {QStringLiteral("retained_first_reason"),
         QString::fromLatin1(observed.math_group_retained_reason)},
        {QStringLiteral("affine_transitions"), static_cast<qint64>(observed.overlay_transitions)},
        {QStringLiteral("callbacks"), static_cast<qint64>(observed.math_group_callbacks)},
        {QStringLiteral("composed_frames"), static_cast<qint64>(frames)},
        {QStringLiteral("composed_layer_frames"),
         static_cast<qint64>(observed.math_group_layer_frames)},
        {QStringLiteral("raster_bytes"), static_cast<qint64>(observed.math_group_raster_bytes)},
        {QStringLiteral("raster_count"), static_cast<qint64>(observed.math_group_raster_count)},
        {QStringLiteral("raster_shared"), static_cast<qint64>(observed.math_group_raster_shared)},
        {QStringLiteral("raster_conflicts"),
         static_cast<qint64>(observed.math_group_raster_conflicts)},
        {QStringLiteral("decision"),
         observed.math_group_decision == MVM_MATH_GROUP_DECISION_ELIGIBLE
             ? QStringLiteral("ELIGIBLE")
         : observed.math_group_decision == MVM_MATH_GROUP_DECISION_INELIGIBLE
             ? QStringLiteral("INELIGIBLE")
         : observed.math_group_decision == MVM_MATH_GROUP_DECISION_UNKNOWN
             ? QStringLiteral("UNKNOWN")
             : QStringLiteral("NONE")},
        {QStringLiteral("decision_reason"),
         QString::fromLatin1(observed.math_group_decision_reason)},
        {QStringLiteral("graph"), observed.math_group_graph == 1 ? QStringLiteral("math-group")
                                                                 : QStringLiteral("conventional")},
        {QStringLiteral("base_alpha_proven"), observed.math_group_base_alpha_proven},
        {QStringLiteral("base_clips_checked"),
         static_cast<qint64>(observed.math_group_base_clips_checked)},
        {QStringLiteral("source_bytes_read"),
         static_cast<qint64>(observed.math_group_source_bytes)},
        {QStringLiteral("raster_live_bytes_after_prepare"),
         static_cast<qint64>(observed.math_group_raster_live_bytes)},
        {QStringLiteral("prepare_ms"), static_cast<double>(observed.math_group_prepare_ns) / 1e6},
        {QStringLiteral("working_set_after_prepare_mb"),
         static_cast<double>(observed.math_group_working_set_at_export) / 1048576.0},
        {QStringLiteral("copy_ms_per_frame"), per(observed.math_group_copy_ns)},
        {QStringLiteral("compose_ms_per_frame"), per(observed.math_group_compose_ns)},
        {QStringLiteral("callback_ms_per_frame"), per(observed.math_group_callback_ns)},
        {QStringLiteral("upstream_ms_per_frame"), per(observed.math_group_upstream_ns)},
        {QStringLiteral("callback_seconds_total"),
         static_cast<double>(observed.math_group_callback_ns) / 1e9},
        {QStringLiteral("errors"), static_cast<qint64>(observed.math_group_errors)},
        {QStringLiteral("error"), QString::fromUtf8(observed.math_group_error)},
        {QStringLiteral("error_frame"), static_cast<qint64>(observed.math_group_error_frame)},
        {QStringLiteral("live_after_export"), live},
        {QStringLiteral("active_after_export"), active}};
}

// MLT の層が clip ごとに書いた結果を、書き出しの計画 (同じ並び) の clip と対応づける。
QJsonObject assignmentJson(const GroupProbe& probe, const mvm::app::TimelineExportPlan& plan) {
    constexpr int kListLimit = 200;
    QJsonArray retained, groups, conventional;
    std::map<int, QJsonObject> perGroup;
    std::map<int, int> members;
    long long grouped = 0, geometry = 0, above = 0, undeclared = 0;
    for (std::size_t index = 0;
         plan.success && index < plan.clips.size() && index < probe.assignment.size(); ++index) {
        const auto& clip = plan.clips[index];
        const int code = probe.assignment[index];
        const QJsonObject described{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("name"), QString::fromStdString(clip.renderClip.name)},
            {QStringLiteral("track"), clip.videoTrackIndex},
            {QStringLiteral("start"), static_cast<qint64>(clip.timelineStartFrame)},
            {QStringLiteral("frames"), static_cast<qint64>(clip.timelineDurationFrames)}};
        if (code > 0) {
            ++grouped;
            if (++members[code] == 1)
                perGroup[code] = QJsonObject{{QStringLiteral("group"), code},
                                             {QStringLiteral("first_member"), described}};
            continue;
        }
        if (clip.audio || clip.videoTrackIndex <= 0)
            continue;
        auto entry = described;
        entry.insert(QStringLiteral("reason"), code == -1   ? QStringLiteral("geometry")
                                               : code == -2 ? QStringLiteral("above-retained-layer")
                                                            : QStringLiteral("not-declared"));
        geometry += code == -1;
        above += code == -2;
        undeclared += code == 0;
        // 従来の経路に残る上位 track の clip (group の下地を作りうる layer)。
        if (conventional.size() < kListLimit)
            conventional.append(entry);
    }
    for (auto& [id, entry] : perGroup) {
        entry.insert(QStringLiteral("members"), members[id]);
        groups.append(entry);
    }
    return QJsonObject{{QStringLiteral("plan_available"), plan.success},
                       {QStringLiteral("grouped_clips"), grouped},
                       {QStringLiteral("groups"), groups},
                       {QStringLiteral("conventional_geometry"), geometry},
                       {QStringLiteral("conventional_above_retained_layer"), above},
                       {QStringLiteral("conventional_not_declared"), undeclared},
                       {QStringLiteral("conventional_layers"), conventional},
                       {QStringLiteral("conventional_layers_limit"), kListLimit},
                       {QStringLiteral("conventional_layers_truncated"),
                        geometry + above + undeclared > conventional.size()}};
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
        else if (key == "--compositor-all")
            options.compositorAll = value == "1";
        else if (key == "--compositor-fault") {
            options.compositorFaultCode = value == "missing-artifact"      ? 1
                                          : value == "unreadable-artifact" ? 2
                                          : value == "missing-mapping"     ? 3
                                                                           : -1;
            if (options.compositorFaultCode < 0)
                return false;
        } else if (key == "--compositor-values") {
            if (value != "plan" && value != "serialized")
                return false;
            options.compositorSerialized = value == "serialized";
        } else if (key == "--layer-select") {
            for (const auto& item : QString::fromStdString(value).split(QLatin1Char(','))) {
                bool ok = false;
                options.layerSelect.push_back(item.toInt(&ok));
                if (!ok || options.layerSelect.back() < 0)
                    return false;
            }
        } else if (key == "--probe") {
            for (const auto& item : QString::fromStdString(value).split(QLatin1Char(','))) {
                const auto parts = item.split(QLatin1Char(':'));
                if (parts.size() != 3)
                    return false;
                options.probes.push_back(
                    {parts[0].toLongLong(), parts[1].toLongLong(), parts[2].toLongLong()});
            }
        } else if (key == "--reference-mode")
            gReferenceMode = std::atoi(value.c_str());
        else if (key == "--keyframe-stride")
            options.keyframeStride = std::atoi(value.c_str());
        else if (key == "--range") {
            const auto colon = value.find(':');
            if (colon == std::string::npos)
                return false;
            options.rangeFirst = std::atoll(value.substr(0, colon).c_str());
            options.rangeCount = std::atoll(value.substr(colon + 1).c_str());
            if (options.rangeFirst < 0 || options.rangeCount <= 0)
                return false;
        } else if (key == "--request-timeout-ms")
            options.requestTimeoutMs = std::atoi(value.c_str());
        else if (key == "--stack-samples")
            options.stackSamples = std::atoi(value.c_str());
        else if (key == "--fixture") {
            for (const auto& fixture : kFixtures)
                if (value == fixture.name)
                    options.fixture = &fixture;
        } else if (key == "--split-transitions") {
            options.splitTransitions = value == "1";
        } else if (key == "--split-policy") {
            // "変わる区間の上限/変わらない区間の下限[/変わらない区間の上限]"
            if (std::sscanf(value.c_str(), "%d/%d/%d", &options.splitAnimatedRun,
                            &options.splitStaticRun, &options.splitStaticChunk) < 2)
                return false;
        } else if (key == "--split-fault") {
            options.splitFault = std::atoi(value.c_str());
        } else if (key == "--math-group") {
            options.mathGroupMain = value == "1" || value == "probe" || value == "auto";
            options.mathGroupMainMode = value == "probe" ? 2 : value == "auto" ? 3 : 1;
        } else if (key == "--base-alpha-proof") {
            if (value != "use" && value != "none" && value.rfind("forge:", 0) != 0)
                return false;
            options.ignoreAlphaProof = value == "none";
            if (value.rfind("forge:", 0) == 0)
                options.forgeAlphaProof = std::atoi(value.c_str() + 6);
        } else if (key == "--product-math-group") {
            if (value != "on" && value != "off")
                return false;
            options.productMain = value == "on" ? 2 : 1;
        } else if (key == "--product-observer") {
            if (value != "diagnostic" && value != "none")
                return false;
            options.productObserver = value == "diagnostic";
        } else if (key == "--v1-background-fault") {
            options.v1BackgroundFault = std::atoi(value.c_str());
        } else if (key == "--strong-digest") {
            options.strongDigest = value == "1";
        } else if (key == "--dump-frames") {
            for (const auto& part : QString::fromStdString(value).split(QLatin1Char(',')))
                options.dumpFrames.insert(part.toLongLong());
        } else if (key == "--candidate-repeats") {
            options.candidateRepeats = std::atoi(value.c_str());
            if (options.candidateRepeats < 1 || options.candidateRepeats > 16)
                return false;
        } else if (key == "--cancel-after") {
            options.cancelAfterImages = std::atoi(value.c_str());
            if (options.cancelAfterImages < 0)
                return false;
        } else if (key == "--math-group-rule") {
            options.mathGroupRule = std::atoi(value.c_str());
            if (options.mathGroupRule != 0 && options.mathGroupRule != 1)
                return false;
        } else if (key == "--math-group-alpha-floor") {
            options.mathGroupAlphaFloor = std::atoi(value.c_str());
            if (options.mathGroupAlphaFloor < 0 || options.mathGroupAlphaFloor > 255)
                return false;
        } else if (key == "--prepared-storage") {
            if (value != "roi" && value != "full")
                return false;
            gPreparedStorage = value == "full" ? mvm::math::MathRasterStorage::FullFrame
                                               : mvm::math::MathRasterStorage::RoiOnly;
        } else if (key == "--math-group-budget-mb") {
            options.mathGroupBudgetMb = std::atoll(value.c_str());
        } else if (key == "--math-group-fault") {
            options.mathGroupFault = std::atoi(value.c_str());
        } else if (key == "--compare") {
            if (value == "split-transitions")
                options.compareSplit = true;
            else if (value == "math-group")
                options.compareMathGroup = true;
            else if (value == "math-group-probe") {
                options.compareMathGroup = true;
                options.compareMathGroupMode = 2;
            } else if (value == "math-group-auto") {
                options.compareMathGroup = true;
                options.compareMathGroupMode = 3;
            } else if (value == "product-math-group" || value == "product-off") {
                // candidate を製品の選択で書き出す (判定つき = mode 3 と同じ検査を通す)。
                options.compareMathGroup = true;
                options.compareMathGroupMode = 3;
                options.compareProduct = value == "product-off" ? 1 : 2;
            } else if (value == "bounding-box")
                options.compare = mvm::app::MathLayerStaging::BoundingBox;
            else if (value == "anchored-box")
                options.compare = mvm::app::MathLayerStaging::AnchoredBox;
            else
                return false;
        } else if (key == "--staging") {
            // 主の書き出しそのものを試作の渡し方で行う (製品の経路ではない。report に残る)。
            if (value == "bounding-box")
                options.mainStaging = mvm::app::MathLayerStaging::BoundingBox;
            else if (value == "anchored-box")
                options.mainStaging = mvm::app::MathLayerStaging::AnchoredBox;
            else
                return false;
        } else if (key == "--render-threads") {
            options.renderThreads = std::atoi(value.c_str());
        } else if (key == "--inject") {
            if (value == "drop-frame")
                options.inject = Inject::DropFrame;
            else if (value == "reject-frame")
                options.inject = Inject::RejectFrame;
            else if (value == "oracle-shift")
                options.inject = Inject::OracleShift;
            else if (value == "cancel")
                options.inject = Inject::Cancel;
            else if (value == "compare-corrupt")
                options.inject = Inject::CompareCorrupt;
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
    if (built && !options.layerSelect.empty()) {
        if (fixture.kind != FixtureKind::Synthetic) {
            std::fprintf(stderr, "--layer-select は Synthetic fixture だけに使えます\n");
            return kExitProtocol;
        }
        // track 1.. が数式 layer 0..。選んだものだけを、順序を保って下から詰め直す。
        std::vector<TimelineClip> kept;
        kept.push_back(project.timelineClips.front());
        for (const int layer : options.layerSelect) {
            if (layer >= fixture.layers ||
                (kept.size() > 1 && kept.back().track.index >= layer + 1)) {
                std::fprintf(stderr, "--layer-select は昇順の layer 番号で指定します\n");
                return kExitProtocol;
            }
            kept.push_back(project.timelineClips[static_cast<std::size_t>(layer) + 1]);
            kept.back().track.index = layer + 1;
        }
        project.timelineClips = std::move(kept);
    }
    const auto valid =
        built ? mvm::project::validateTimeline(project) : mvm::project::TimelineValidationResult{};
    if (!built || !valid.success || valid.totalFrames != fixture.frames) {
        std::fprintf(stderr, "fixture '%s' を作れません: %s (frame %lld、想定 %lld)\n",
                     fixture.name, (built ? valid.error : fixtureError).c_str(),
                     static_cast<long long>(valid.totalFrames),
                     static_cast<long long>(fixture.frames));
        return kExitProtocol;
    }
    // P3-3B: 下地の alpha の対照。背景を、alpha を指定した画像に差し替える。
    std::vector<std::uint8_t> alphaBackground;
    if (fixture.backgroundAlpha >= 0) {
        const int imageWidth = project.outputWidth, imageHeight = project.outputHeight;
        QImage image(imageWidth, imageHeight, QImage::Format_RGBA8888);
        for (int row = 0; row < imageHeight; ++row) {
            auto* line = image.scanLine(row);
            for (int column = 0; column < imageWidth; ++column) {
                // RGB は alpha と無関係に決める (0 も 255 も現れる)。
                line[column * 4] = static_cast<std::uint8_t>((column * 7 + row * 3) & 255);
                line[column * 4 + 1] = static_cast<std::uint8_t>((column ^ row) & 255);
                line[column * 4 + 2] = static_cast<std::uint8_t>((row * 5 + 40) & 255);
                const int alphaValue = fixture.backgroundAlpha == 256 ? column * 256 / imageWidth
                                       : fixture.backgroundAlpha == 257
                                           ? 1 + (column * 37 + row * 101) % 255
                                           : fixture.backgroundAlpha;
                line[column * 4 + 3] = static_cast<std::uint8_t>(alphaValue);
            }
        }
        const auto backgroundPath =
            options.output / (std::string(fixture.name) + "-background.png");
        if (!image.save(toQString(backgroundPath), "PNG")) {
            std::fprintf(stderr, "背景の画像を書けません\n");
            return kExitProtocol;
        }
        project.timelineClips.front().mediaPath = backgroundPath;
        alphaBackground = imageBytes(image);
    }
    const int admissibleAlpha = fixture.backgroundAlpha >= 0 ? 0 : kAdmissibleAlpha;
    if (options.rangeCount > 0 && options.rangeFirst + options.rangeCount > fixture.frames) {
        std::fprintf(stderr, "--range が fixture の尺を超えています\n");
        return kExitProtocol;
    }
    const bool rangeMode = options.rangeCount > 0;
    // 区間だけを書き出すとき、以降の frame 番号は区間の先頭を 0 として扱う。
    const std::int64_t frameBase = rangeMode ? options.rangeFirst : 0;
    const std::int64_t expectedFrames = rangeMode ? options.rangeCount : fixture.frames;
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
    const auto applyDiagnosticInputs = [&](MvmExportObserver& target) {
        target.diagnostic_keyframe_stride = options.keyframeStride;
        target.diagnostic_first_frame = options.rangeFirst;
        target.diagnostic_frame_count = options.rangeCount;
        target.diagnostic_split_transitions = options.splitTransitions ? 1 : 0;
        target.diagnostic_split_animated_run = options.splitAnimatedRun;
        target.diagnostic_split_static_run = options.splitStaticRun;
        target.diagnostic_split_static_chunk = options.splitStaticChunk;
        target.diagnostic_math_group = options.mathGroupMain ? options.mathGroupMainMode : 0;
        target.diagnostic_math_group_fault =
            options.mathGroupMain || options.productMain == 2 ? options.mathGroupFault : 0;
        target.diagnostic_math_group_rule = options.mathGroupRule;
        target.diagnostic_math_group_ignore_alpha_proof = options.ignoreAlphaProof ? 1 : 0;
        target.diagnostic_math_group_forge_alpha_proof = options.forgeAlphaProof;
        target.diagnostic_v1_background_fault = options.v1BackgroundFault;
        target.diagnostic_math_group_alpha_override = options.mathGroupAlphaFloor >= 0 ? 1 : 0;
        target.diagnostic_math_group_alpha_floor =
            options.mathGroupAlphaFloor >= 0 ? options.mathGroupAlphaFloor : 0;
        const bool fullStorage = gPreparedStorage == mvm::math::MathRasterStorage::FullFrame;
        target.diagnostic_math_group_storage = fullStorage ? 1 : 0;
        const long long budgetMb = options.mathGroupBudgetMb >= 0 ? options.mathGroupBudgetMb
                                   : fullStorage                  ? 800
                                                                  : 64;
        target.diagnostic_math_group_raster_budget_bytes = budgetMb * 1024 * 1024;
    };
    // 数式 group の下地の観測と clip ごとの結果 (group を使う書き出しごとに 1 組)。
    GroupProbe mainProbe, candidateProbe;
    const auto attachProbe = [](MvmExportObserver& target, GroupProbe& probe) {
        probe.assignment.assign(GroupProbe::kAssignmentCapacity, 0);
        target.diagnostic_math_group_probe = &groupProbeCallback;
        target.diagnostic_math_group_probe_opaque = &probe;
        target.diagnostic_math_group_assignment = probe.assignment.data();
        target.diagnostic_math_group_assignment_capacity = GroupProbe::kAssignmentCapacity;
    };
    applyDiagnosticInputs(observer);
    if (options.mathGroupMain || options.productMain == 2)
        attachProbe(observer, mainProbe);
    long long mainWorkingSetBeforePrepare = 0;
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

    // --compare: 同じ process で続けて行う追加の書き出し (candidate と、再現性を見る baseline)。
    struct ExtraRun {
        mvm::app::MathLayerStaging staging = mvm::app::MathLayerStaging::FullFrame;
        bool split = false;
        bool mathGroup = false;
        bool product = false;     // 製品の選択で書き出す (candidate)
        int requestModeSeen = -1; // controller が要求へ入れた mathGroupOptimization (0 Off、1 Auto)
        bool observerAttached = true; // 書き出しへ observer を渡したか
        // controller が作った要求に、最初から observer が付いていたか (製品の要求は付けない)。
        bool controllerSuppliedObserver = false;
        QString outputSha256;                       // 書き出した MP4 (消す前に採る。失敗なら空)
        mvm::app::MathGroupExportReport report;     // 書き出しの結果に付いた記録 (最後の回)
        long long groupLive = -1, groupActive = -1; // 書き出しの後に残っていた数 (0 が正)
        long long workingSetBeforePrepare = 0;      // staging の後、画像の準備の前
        long long rasterLiveAfter = -1; // 書き出しの後に生きている準備済みの画素 (0 が正)
        long long workingSetAfter = 0;  // 書き出しの関数から戻った直後
        MvmExportObserver observer{};
        std::function<bool(std::int64_t, const std::uint8_t*, int, int)> validator;
        mvm::app::TimelineExportResult result;
        long long beginNs = 0, endNs = 0;
        double cpuSeconds = 0.0;
        std::vector<long long> regionPixels; // 切り出して渡した数式 clip ごとの画素数
        mvm::test::MltPropertyProbe::Snapshot probeBegin, probeEnd;
    };

    ExtraRun* extra = nullptr;
    int mainRequestModeSeen = -1;
    bool mainControllerSuppliedObserver = false;
    mvm::app::MathGroupExportReport mainReport;
    std::map<std::int64_t, QByteArray> baselineFrames;
    long long mainGroupLive = -1, mainGroupActive = -1;
    long long mainRasterLiveAfter = -1, mainWorkingSetAfter = 0;

    mvm::app::MvmController::ExportRunner runner =
        [&](const Project& exportProject, const mvm::app::TimelineExportRequest& request) {
            auto input = request;
            if (options.renderThreads > 0)
                input.renderThreads = options.renderThreads; // 製品の値 (4) ではない
            if (extra) {
                // 比較用の追加の書き出し。同じ controller・同じ要求で、layer の渡し方だけを変える。
                if (options.requestTimeoutMs > 0)
                    input.timeoutMs = options.requestTimeoutMs;
                applyDiagnosticInputs(extra->observer);
                if (extra->split) {
                    extra->observer.diagnostic_split_transitions = 1;
                    extra->observer.diagnostic_split_fault = options.splitFault;
                }
                extra->requestModeSeen = static_cast<int>(input.mathGroupOptimization);
                extra->controllerSuppliedObserver = request.exportObserver != nullptr;
                if (extra->product) {
                    // 製品の選択: mode は書き出しが要求から決める (harness は設定しない)。
                    extra->observer.diagnostic_math_group = 0;
                    extra->observerAttached = options.productObserver;
                    if (options.productObserver) {
                        extra->observer.diagnostic_math_group_fault = options.mathGroupFault;
                        attachProbe(extra->observer, candidateProbe);
                    }
                } else if (extra->mathGroup) {
                    extra->observer.diagnostic_math_group = options.compareMathGroupMode;
                    extra->observer.diagnostic_math_group_fault = options.mathGroupFault;
                    attachProbe(extra->observer, candidateProbe);
                }
                input.stageObserver = [&](const char* name) {
                    if (std::string(name) == "staging-end")
                        extra->workingSetBeforePrepare = workingSetBytes(false);
                };
                input.exportObserver = extra->observerAttached ? &extra->observer : nullptr;
                input.mathLayerStaging = extra->staging;
                input.mathStagingObserver = [&](const std::string&, int, int, int regionWidth,
                                                int regionHeight) {
                    extra->regionPixels.push_back(static_cast<long long>(regionWidth) *
                                                  regionHeight);
                };
                input.encoderFrameValidator = extra->validator;
                extra->beginNs = mvm_mlt_export_observer_now_ns();
                extra->result = mvm::app::exportTimeline(exportProject, input);
                extra->endNs = mvm_mlt_export_observer_now_ns();
                extra->report = extra->result.mathGroup;
                if (!extra->observerAttached) {
                    // observer を渡していないので、結果に付いた記録を集計用の入れ物へ写す
                    // (回数は、くり返しの分を足す。判定は最後の回)。
                    const auto& recorded = extra->result.mathGroup;
                    auto& sink = extra->observer;
                    sink.math_group_decision =
                        recorded.decision == "ELIGIBLE"     ? MVM_MATH_GROUP_DECISION_ELIGIBLE
                        : recorded.decision == "INELIGIBLE" ? MVM_MATH_GROUP_DECISION_INELIGIBLE
                        : recorded.decision == "UNKNOWN"    ? MVM_MATH_GROUP_DECISION_UNKNOWN
                                                            : MVM_MATH_GROUP_DECISION_NONE;
                    std::snprintf(sink.math_group_decision_reason,
                                  sizeof(sink.math_group_decision_reason), "%s",
                                  recorded.reason.c_str());
                    sink.diagnostic_math_group = recorded.evaluated ? 3 : 0;
                    sink.math_group_graph = recorded.mathGroupGraph ? 1 : 0;
                    sink.math_group_base_alpha_proven = recorded.baseAlphaProven;
                    sink.math_group_transitions += recorded.groups;
                    sink.math_group_clips += recorded.groupedClips;
                    sink.math_group_retained_clips += recorded.retainedClips;
                    sink.overlay_transitions += recorded.affineTransitions;
                    sink.math_group_frames += recorded.composedFrames;
                    sink.math_group_callbacks += recorded.callbacks;
                    sink.math_group_errors += recorded.errors;
                    sink.math_group_raster_count = recorded.rasterCount;
                    sink.math_group_raster_bytes = recorded.rasterBytes;
                    if (recorded.errors > 0 && !extra->result.error.empty())
                        std::snprintf(sink.math_group_error, sizeof(sink.math_group_error), "%s",
                                      extra->result.error.c_str());
                }
                extra->groupLive = mvm_mlt_math_group_live();
                extra->groupActive = mvm_mlt_math_group_active();
                extra->rasterLiveAfter = mvm::math::preparedMathRasterLiveBytes();
                extra->workingSetAfter = workingSetBytes(false);
                return extra->result;
            }
            input.mathLayerStaging = options.mainStaging;
            mainRequestModeSeen = static_cast<int>(input.mathGroupOptimization);
            mainControllerSuppliedObserver = request.exportObserver != nullptr;
            // timeout は製品の値のまま。診断の上限は watchdog が別に持つ。
            productTimeoutMs = input.timeoutMs;
            if (options.requestTimeoutMs > 0)
                input.timeoutMs = options.requestTimeoutMs;
            requestTimeoutMs = input.timeoutMs;
            artifacts = input.mathArtifacts;
            burnSubtitles = input.burnSubtitles;
            input.exportObserver = &observer;
            input.stageObserver = [&](const char* name) {
                if (std::string(name) == "staging-end")
                    mainWorkingSetBeforePrepare = workingSetBytes(false);
                std::lock_guard lock(stageMutex);
                stages.emplace_back(name, mvm_mlt_export_observer_now_ns());
            };
            input.encoderFrameValidator = [&](std::int64_t absoluteFrame, const std::uint8_t* rgba,
                                              int frameWidth, int frameHeight) {
                const std::int64_t frame = absoluteFrame - frameBase;
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
                QByteArray sha256;
                if (options.strongDigest)
                    sha256 =
                        QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(rgba),
                                                                static_cast<qsizetype>(bytes)),
                                                 QCryptographicHash::Sha256);
                if (options.dumpFrames.contains(absoluteFrame)) {
                    // 同じ frame が再要求されても、最初の 1 回だけ書く (名前は絶対 frame)。
                    const auto path = options.output / (std::string(fixture.name) + "-frame-" +
                                                        std::to_string(absoluteFrame) + ".rgba");
                    QFile dump(toQString(path));
                    if (!dump.exists() && dump.open(QIODevice::WriteOnly))
                        dump.write(reinterpret_cast<const char*>(rgba), static_cast<qint64>(bytes));
                }
                // 比較のときは baseline の全 frame を保持する (ほぼ単色なので圧縮すると小さい)。
                QByteArray packed;
                if (options.compare != mvm::app::MathLayerStaging::FullFrame ||
                    options.compareSplit || options.compareMathGroup || options.compositorAll)
                    packed = qCompress(rgba, static_cast<qsizetype>(bytes), 1);
                std::lock_guard lock(observation.mutex);
                if (!packed.isEmpty())
                    baselineFrames.try_emplace(frame, std::move(packed));
                observation.width = frameWidth;
                observation.height = frameHeight;
                auto& seen = observation.frames[frame];
                if (seen.calls > 0 && (seen.hash != hash || seen.sha256 != sha256))
                    seen.repeatDiffers = true;
                ++seen.calls;
                seen.hash = hash;
                seen.sha256 = sha256;
                seen.minAlpha = minAlpha;
                if (representative.contains(frame) && !observation.samples.contains(frame))
                    observation.samples.emplace(frame,
                                                std::vector<std::uint8_t>(rgba, rgba + bytes));
                return true;
            };
            exportBeginNs.store(mvm_mlt_export_observer_now_ns());
            exported = mvm::app::exportTimeline(exportProject, input);
            exportEndNs.store(mvm_mlt_export_observer_now_ns());
            mainReport = exported.mathGroup;
            mainGroupLive = mvm_mlt_math_group_live();
            mainGroupActive = mvm_mlt_math_group_active();
            mainRasterLiveAfter = mvm::math::preparedMathRasterLiveBytes();
            mainWorkingSetAfter = workingSetBytes(false);
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

    // affine・qtcrop が libmlt から呼ぶ property 取得関数を計時する (file
    // は変えない。終了時に戻す)。
    const int probeHooks = mvm::test::MltPropertyProbe::install(L"libmltplus.dll") +
                           mvm::test::MltPropertyProbe::install(L"libmltqt6.dll");

    struct ProbeGuard {
        ~ProbeGuard() { mvm::test::MltPropertyProbe::remove(); }
    } probeGuard;

    const auto probeMainBegin = mvm::test::MltPropertyProbe::snapshot();
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
    // 製品の選択 (MvmController の内部の選択)。指定が無ければ触らない (既定は off)。
    const bool controllerDefaultSelection = controller->mathGroupExportOptimization();
    if (options.productMain != 0)
        controller->setMathGroupExportOptimization(options.productMain == 2);
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
            observer.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count >= options.cancelAfterImages) {
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
    const auto probeMainEnd = mvm::test::MltPropertyProbe::snapshot();
    const long long finishNs = mvm_mlt_export_observer_now_ns();
    const double cpuEnd = processCpuSeconds();
    snapshots.push_back(takeSnapshot());
    av_log_set_callback(av_log_default_callback);
    gTapObserver.store(nullptr);

    // --- baseline と candidate の比較 ---------------------------------------------
    // 同じ process・同じ build で、candidate、もう一度 baseline の順に書き出す。2 回目の baseline
    // は 1 回目と全 frame 同じでなければならない (違えば比較そのものが成り立たない)。
    struct FrameDifference {
        long long pixels = 0; // 1 階調でも違う画素
        long long beyond = 0; // 1 階調を超えて違う画素
        int maxDifference = 0;
        int left = 0, top = 0, right = 0, bottom = 0;
    };

    const bool comparing = options.compare != mvm::app::MathLayerStaging::FullFrame ||
                           options.compareSplit || options.compareMathGroup;
    ExtraRun candidateRun, repeatRun;
    std::mutex compareMutex;
    std::set<std::int64_t> candidateSeen, repeatSeen;
    std::map<std::int64_t, FrameDifference> candidateDifferences;
    std::array<AlphaTally, 256> alphaTallies{};
    int candidateExports = 0, candidateFailures = 0, candidateLeaks = 0;
    QJsonObject alphaExamples[256];
    long long compareUnexpected = 0, repeatMismatches = 0;
    bool compareRan = false;
    if (comparing && exported.success && watchdog == "none") {
        const auto frameBytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
        const auto baselineHash = [&](std::int64_t frame) -> std::optional<std::uint64_t> {
            std::lock_guard lock(observation.mutex);
            const auto found = observation.frames.find(frame);
            if (found == observation.frames.end())
                return std::nullopt;
            return found->second.hash;
        };
        candidateRun.staging = options.compare;
        candidateRun.split = options.compareSplit;
        candidateRun.mathGroup = options.compareMathGroup;
        candidateRun.product = options.compareProduct != 0;
        if (candidateRun.product)
            controller->setMathGroupExportOptimization(false); // 主の書き出し (baseline) は off
        // baseline と同じ渡し方を保つ (比べるのは candidate の 1 点だけ)。
        if (options.compareSplit || options.compareMathGroup)
            candidateRun.staging = repeatRun.staging = options.mainStaging;
        candidateRun.validator = [&](std::int64_t absoluteFrame, const std::uint8_t* rgba,
                                     int frameWidth, int frameHeight) {
            const std::int64_t frame = absoluteFrame - frameBase;
            std::uint64_t hash = 0;
            int minAlpha = 0;
            const auto expected = baselineHash(frame);
            if (!expected || frameWidth != width || frameHeight != height) {
                std::lock_guard lock(compareMutex);
                ++compareUnexpected;
                return true;
            }
            digestFrame(rgba, frameBytes, hash, minAlpha);
            const bool corrupt = options.inject == Inject::CompareCorrupt && frame == 1;
            FrameDifference difference;
            if (!alphaBackground.empty() && alphaBackground.size() == frameBytes) {
                // 背景の alpha ごとに、数式が触れた画素と、candidate が baseline
                // と違う画素を数える。
                QByteArray packed;
                {
                    std::lock_guard lock(observation.mutex);
                    packed = baselineFrames.at(frame);
                }
                const QByteArray baseline = qUncompress(packed);
                if (static_cast<std::size_t>(baseline.size()) == frameBytes) {
                    const auto* base = reinterpret_cast<const std::uint8_t*>(baseline.constData());
                    std::array<AlphaTally, 256> local{};
                    QJsonObject example[256];
                    for (std::size_t offset = 0; offset < frameBytes; offset += 4) {
                        const auto* background = alphaBackground.data() + offset;
                        auto& tally = local[background[3]];
                        const bool touched = std::memcmp(base + offset, background, 4) != 0;
                        ++tally.pixels;
                        tally.touched += touched;
                        // MLT が、数式の画素の無い所で背景を書き換えた (alpha 0 の画素など)。
                        tally.touchedAlphaOnly +=
                            touched && base[offset + 3] == background[3] && background[3] == 0;
                        if (std::memcmp(base + offset, rgba + offset, 4) == 0)
                            continue;
                        ++tally.mismatching;
                        if (example[background[3]].isEmpty())
                            example[background[3]] = QJsonObject{
                                {QStringLiteral("frame"), static_cast<qint64>(frame)},
                                {QStringLiteral("x"),
                                 static_cast<int>((offset / 4U) % static_cast<std::size_t>(width))},
                                {QStringLiteral("y"),
                                 static_cast<int>((offset / 4U) / static_cast<std::size_t>(width))},
                                {QStringLiteral("background"), rgbaArray(background)},
                                {QStringLiteral("mlt"), rgbaArray(base + offset)},
                                {QStringLiteral("candidate"), rgbaArray(rgba + offset)}};
                    }
                    std::lock_guard lock(compareMutex);
                    for (int alpha = 0; alpha < 256; ++alpha) {
                        auto& total = alphaTallies[static_cast<std::size_t>(alpha)];
                        const auto& part = local[static_cast<std::size_t>(alpha)];
                        total.pixels += part.pixels;
                        total.touched += part.touched;
                        total.touchedAlphaOnly += part.touchedAlphaOnly;
                        total.mismatching += part.mismatching;
                        if (alphaExamples[alpha].isEmpty() && !example[alpha].isEmpty())
                            alphaExamples[alpha] = example[alpha];
                    }
                }
            }
            if (hash != *expected || corrupt) {
                QByteArray packed;
                {
                    std::lock_guard lock(observation.mutex);
                    packed = baselineFrames.at(frame);
                }
                QByteArray baseline = qUncompress(packed);
                if (static_cast<std::size_t>(baseline.size()) != frameBytes) {
                    std::lock_guard lock(compareMutex);
                    ++compareUnexpected;
                    return true;
                }
                if (corrupt) // 比較が効いていることの対照: baseline の 1 画素を 2 階調ずらす
                    baseline[0] = static_cast<char>(static_cast<std::uint8_t>(baseline[0]) ^ 2U);
                const auto* base = reinterpret_cast<const std::uint8_t*>(baseline.constData());
                difference.left = width;
                difference.top = height;
                difference.right = difference.bottom = -1;
                for (std::size_t offset = 0; offset < frameBytes; offset += 4) {
                    int worst = 0;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        worst = std::max(worst, std::abs(static_cast<int>(rgba[offset + channel]) -
                                                         static_cast<int>(base[offset + channel])));
                    if (worst == 0)
                        continue;
                    const int column =
                        static_cast<int>((offset / 4U) % static_cast<std::size_t>(width));
                    const int row =
                        static_cast<int>((offset / 4U) / static_cast<std::size_t>(width));
                    ++difference.pixels;
                    difference.beyond += worst > 1;
                    difference.maxDifference = std::max(difference.maxDifference, worst);
                    difference.left = std::min(difference.left, column);
                    difference.right = std::max(difference.right, column);
                    difference.top = std::min(difference.top, row);
                    difference.bottom = std::max(difference.bottom, row);
                }
            }
            std::lock_guard lock(compareMutex);
            candidateSeen.insert(frame);
            if (difference.pixels > 0)
                candidateDifferences[frame] = difference;
            return true;
        };
        repeatRun.validator = [&](std::int64_t absoluteFrame, const std::uint8_t* rgba,
                                  int frameWidth, int frameHeight) {
            const std::int64_t frame = absoluteFrame - frameBase;
            std::uint64_t hash = 0;
            int minAlpha = 0;
            const auto expected = baselineHash(frame);
            if (expected && frameWidth == width && frameHeight == height)
                digestFrame(rgba, frameBytes, hash, minAlpha);
            std::lock_guard lock(compareMutex);
            repeatSeen.insert(frame);
            if (!expected || hash != *expected)
                ++repeatMismatches;
            return true;
        };
        const auto runExtra = [&](ExtraRun& run, const char* suffix) {
            const auto path = options.output / (std::string(fixture.name) + "-" + suffix + ".mp4");
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            extra = &run;
            // candidate だけを製品の選択で書き出す。baseline のくり返しは off (既定) へ戻す。
            controller->setMathGroupExportOptimization(run.product && options.compareProduct == 2);
            run.probeBegin = mvm::test::MltPropertyProbe::snapshot();
            const double cpuBefore = processCpuSeconds();
            if (controller->exportTimeline(QUrl::fromLocalFile(toQString(path)))) {
                QElapsedTimer elapsed;
                elapsed.start();
                bool cancelled = false;
                while (controller->exporting()) {
                    QGuiApplication::processEvents();
                    QThread::msleep(20);
                    if (!cancelled && elapsed.elapsed() > options.capSeconds * 1000LL) {
                        controller->cancelTimelineExport();
                        cancelled = true;
                    }
                }
            }
            run.cpuSeconds = processCpuSeconds() - cpuBefore;
            run.probeEnd = mvm::test::MltPropertyProbe::snapshot();
            extra = nullptr;
            run.outputSha256 =
                std::filesystem::exists(path, ignored) ? fileSha256(path) : QString();
            std::filesystem::remove(path, ignored);
        };
        // candidate は、同じ process の中で指定の回数だけ続けて書き出す (どの回も全 frame
        // を比べる)。
        for (int repeat = 0; repeat < options.candidateRepeats; ++repeat) {
            runExtra(candidateRun, "candidate");
            ++candidateExports;
            candidateFailures += !candidateRun.result.success;
            candidateLeaks += candidateRun.groupLive != 0 || candidateRun.groupActive != 0 ||
                              candidateRun.rasterLiveAfter != 0;
        }
        runExtra(repeatRun, "baseline-repeat");
        compareRan = true;
    }

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
        if (found->second.minAlpha < admissibleAlpha && transparent.size() < 64)
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
        transparentCount += found->second.minAlpha < admissibleAlpha;
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
                     .arg(admissibleAlpha)
                     .arg(transparentCount)
                     .arg(lowestAlpha));
        if (unstableCount != 0)
            fail(QStringLiteral("再要求で画素が変わった frame が %1 あります").arg(unstableCount));
        if (observation.width != width || observation.height != height)
            fail(QStringLiteral("pre-YUV の寸法が Project と違います"));

        if (!rangeMode)
            decoded = probeDecoded(options, outputPath, static_cast<int>(project.timelineFpsNum),
                                   static_cast<int>(project.timelineFpsDen));
        if (rangeMode) {
            // 区間だけの診断。復号と独立期待値の照合は行わない (行っていないことを report に残す)。
        } else if (!decoded.ran)
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

        mvm::app::TimelineExportRequest planRequest;
        planRequest.width = width;
        planRequest.height = height;
        planRequest.fpsNum = static_cast<int>(project.timelineFpsNum);
        planRequest.fpsDen = static_cast<int>(project.timelineFpsDen);
        const auto referencePlan = mvm::app::mapTimelineExportPlan(project, planRequest);
        gPlan = referencePlan.success ? &referencePlan : nullptr;
        for (const auto frame : fixture.representative) {
            // 独立期待値は、不透明な背景と、等倍・回転なし・crop なしの配置を前提にしている。
            if (rangeMode || fixture.backgroundAlpha >= 0 || fixture.geometry != 0)
                break;
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
            entry.insert(QStringLiteral("first_differing"), oracle.firstDiffering);
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
        if (rangeMode || fixture.backgroundAlpha >= 0 || fixture.geometry != 0) {
        } else if (oracleApplicable == 0)
            fail(QStringLiteral("独立期待値と比較できた代表 frame がありません"));
        else if (maxConcurrentMath > 0 && oracleInked == 0)
            fail(QStringLiteral("比較した代表 frame に数式の画素がありません (空振り)"));
    } else if (finalExists || temporaryExists) {
        fail(QStringLiteral("失敗・取消なのに出力 file が残っています"));
    }

    // --- 合成器の全 frame 比較 (P3-1.1・P3-1.2) -----------------------------------
    QJsonObject compositorAllReport;
    if (options.compositorAll && exported.success && !rangeMode) {
        mvm::app::TimelineExportRequest allRequest;
        allRequest.width = width;
        allRequest.height = height;
        allRequest.fpsNum = static_cast<int>(project.timelineFpsNum);
        allRequest.fpsDen = static_cast<int>(project.timelineFpsDen);
        const auto allPlan = mvm::app::mapTimelineExportPlan(project, allRequest);
        // 記録の上限 (事前に固定)。超えた分は数だけを数え、truncated を立てる。
        constexpr int kMismatchLogLimit = 256;
        constexpr int kErrorLogLimit = 64;
        long long compared = 0, mismatching = 0, differingPixels = 0, inkedFrames = 0;
        QJsonArray missingIds, unsupportedIds, errorIds, errorLog, mismatchLog, mismatchFrameIds,
            probeLog, rasterIdentities;
        QString firstMismatch;
        std::map<std::string, int> unsupportedReasons;
        bool inputsVerified = false;
        std::string verifyError = "書き出しの計画を作れません";
        if (allPlan.success) {
            CompositorRun run(project, allPlan, artifacts, burnSubtitles,
                              static_cast<CompositorFault>(options.compositorFaultCode),
                              options.compositorSerialized ? CompositorValues::Serialized
                                                           : CompositorValues::Plan);
            for (std::int64_t frame = 0; frame < expectedFrames; ++frame) {
                const auto stored = baselineFrames.find(frame);
                if (stored == baselineFrames.end()) {
                    missingIds.append(static_cast<qint64>(frame));
                    continue;
                }
                const auto composed = run.frame(frame);
                if (composed.status == CompositorStatus::Unsupported) {
                    unsupportedIds.append(static_cast<qint64>(frame)); // 比べていない
                    ++unsupportedReasons[composed.reason];
                    continue;
                }
                const QByteArray baseline = qUncompress(stored->second);
                if (composed.status == CompositorStatus::Error ||
                    static_cast<std::size_t>(baseline.size()) != composed.expected.size()) {
                    errorIds.append(static_cast<qint64>(frame));
                    if (errorLog.size() < kErrorLogLimit)
                        errorLog.append(QJsonObject{
                            {QStringLiteral("frame"), static_cast<qint64>(frame)},
                            {QStringLiteral("reason"),
                             composed.status == CompositorStatus::Error
                                 ? QString::fromStdString(composed.reason)
                                 : QStringLiteral("pre-YUV の大きさが期待値と違います")}});
                    continue;
                }
                const auto* actual = reinterpret_cast<const std::uint8_t*>(baseline.constData());
                ++compared;
                inkedFrames += composed.expected != composed.background;
                long long framePixels = 0;
                for (std::size_t offset = 0; offset < composed.expected.size(); offset += 4) {
                    if (std::memcmp(actual + offset, composed.expected.data() + offset, 4) == 0)
                        continue;
                    ++framePixels;
                    const int x = static_cast<int>((offset / 4U) % static_cast<std::size_t>(width));
                    const int y = static_cast<int>((offset / 4U) / static_cast<std::size_t>(width));
                    if (firstMismatch.isEmpty())
                        firstMismatch = QStringLiteral("frame %1 x=%2 y=%3 expected=%4,%5,%6,%7 "
                                                       "actual=%8,%9,%10,%11")
                                            .arg(frame)
                                            .arg(x)
                                            .arg(y)
                                            .arg(composed.expected[offset])
                                            .arg(composed.expected[offset + 1])
                                            .arg(composed.expected[offset + 2])
                                            .arg(composed.expected[offset + 3])
                                            .arg(actual[offset])
                                            .arg(actual[offset + 1])
                                            .arg(actual[offset + 2])
                                            .arg(actual[offset + 3]);
                    if (mismatchLog.size() >= kMismatchLogLimit)
                        continue;
                    QJsonArray channels;
                    for (int channel = 0; channel < 4; ++channel)
                        if (actual[offset + static_cast<std::size_t>(channel)] !=
                            composed.expected[offset + static_cast<std::size_t>(channel)])
                            channels.append(QString(QLatin1Char("RGBA"[channel])));
                    mismatchLog.append(
                        QJsonObject{{QStringLiteral("frame"), static_cast<qint64>(frame)},
                                    {QStringLiteral("x"), x},
                                    {QStringLiteral("y"), y},
                                    {QStringLiteral("channels"), channels},
                                    {QStringLiteral("compositor"),
                                     rgbaArray(composed.expected.data() + offset)},
                                    {QStringLiteral("mlt"), rgbaArray(actual + offset)},
                                    {QStringLiteral("layers"), layerList(composed)},
                                    {QStringLiteral("trace"), tracePixel(run, composed, x, y)}});
                }
                if (framePixels > 0) {
                    ++mismatching;
                    differingPixels += framePixels;
                    mismatchFrameIds.append(static_cast<qint64>(frame));
                }
                for (const auto& probe : options.probes) {
                    if (probe[0] != frame || probe[1] < 0 || probe[1] >= width || probe[2] < 0 ||
                        probe[2] >= height)
                        continue;
                    const auto offset =
                        (static_cast<std::size_t>(probe[2]) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(probe[1])) *
                        4U;
                    probeLog.append(
                        QJsonObject{{QStringLiteral("frame"), static_cast<qint64>(frame)},
                                    {QStringLiteral("x"), static_cast<int>(probe[1])},
                                    {QStringLiteral("y"), static_cast<int>(probe[2])},
                                    {QStringLiteral("compositor"),
                                     rgbaArray(composed.expected.data() + offset)},
                                    {QStringLiteral("mlt"), rgbaArray(actual + offset)},
                                    {QStringLiteral("trace"),
                                     tracePixel(run, composed, static_cast<int>(probe[1]),
                                                static_cast<int>(probe[2]))}});
                }
            }
            inputsVerified = run.verifyInputs(verifyError);
            rasterIdentities = run.rasterIdentities();
        }
        QJsonObject reasonObject;
        for (const auto& [reason, count] : unsupportedReasons)
            reasonObject.insert(QString::fromStdString(reason), count);
        const long long missingFrames = missingIds.size();
        const long long unsupportedFrames = unsupportedIds.size();
        const long long errorFrames = errorIds.size();
        const bool complete =
            allPlan.success && inputsVerified && missingFrames == 0 && errorFrames == 0 &&
            compared + unsupportedFrames == static_cast<long long>(expectedFrames);
        const QString verdict = !complete          ? QStringLiteral("error")
                                : compared == 0    ? QStringLiteral("nothing-compared")
                                : mismatching != 0 ? QStringLiteral("mismatch")
                                                   : QStringLiteral("exact");
        compositorAllReport = (QJsonObject{
            {QStringLiteral("verdict"), verdict},
            {QStringLiteral("plan_available"), allPlan.success},
            {QStringLiteral("values"), options.compositorSerialized
                                           ? QStringLiteral("serialized-%g")
                                           : QStringLiteral("plan")},
            {QStringLiteral("fault"), options.compositorFaultCode},
            {QStringLiteral("inputs_verified"), inputsVerified},
            {QStringLiteral("inputs_error"),
             inputsVerified ? QString() : QString::fromStdString(verifyError)},
            {QStringLiteral("frames_expected"), static_cast<qint64>(expectedFrames)},
            {QStringLiteral("frames_observed"), static_cast<qint64>(baselineFrames.size())},
            {QStringLiteral("frames_compared"), compared},
            {QStringLiteral("frames_missing"), missingFrames},
            {QStringLiteral("frames_unsupported"), unsupportedFrames},
            {QStringLiteral("frames_error"), errorFrames},
            {QStringLiteral("frames_with_math_pixels"), inkedFrames},
            {QStringLiteral("frames_mismatching"), mismatching},
            {QStringLiteral("differing_pixels"), differingPixels},
            {QStringLiteral("first_mismatch"), firstMismatch},
            {QStringLiteral("missing_frame_ids"), missingIds},
            {QStringLiteral("unsupported_frame_ids"), unsupportedIds},
            {QStringLiteral("unsupported_reasons"), reasonObject},
            {QStringLiteral("error_frame_ids"), errorIds},
            {QStringLiteral("errors"), errorLog},
            {QStringLiteral("errors_truncated"), errorFrames > errorLog.size()},
            {QStringLiteral("mismatching_frame_ids"), mismatchFrameIds},
            {QStringLiteral("mismatches"), mismatchLog},
            {QStringLiteral("mismatches_log_limit"), kMismatchLogLimit},
            {QStringLiteral("mismatches_truncated"), differingPixels > mismatchLog.size()},
            {QStringLiteral("probes"), probeLog},
            {QStringLiteral("rasters"), rasterIdentities}});
        if (!allPlan.success || !inputsVerified)
            fail(QStringLiteral("合成器の全 frame 比較: 入力を検証できません (%1)")
                     .arg(QString::fromStdString(verifyError)));
        if (errorFrames != 0)
            fail(QStringLiteral("合成器の全 frame 比較: %1 frame が ERROR です (対象外ではない)")
                     .arg(errorFrames));
        if (missingFrames != 0 || compared + unsupportedFrames + errorFrames + missingFrames !=
                                      static_cast<long long>(expectedFrames))
            fail(QStringLiteral("合成器の全 frame 比較: 比べられない frame があります (欠落 %1)")
                     .arg(missingFrames));
        if (mismatching != 0)
            fail(QStringLiteral("合成器の全 frame 比較: %1 frame が MLT と違います (%2)")
                     .arg(mismatching)
                     .arg(firstMismatch));
        if (compared == 0)
            fail(QStringLiteral("合成器の全 frame 比較: 比べた frame がありません"));
        std::fprintf(stderr,
                     "合成器の全 frame 比較: 想定 %lld 観測 %lld 比較 %lld 欠落 %lld 対象外 %lld "
                     "ERROR %lld 不一致 %lld (差のある画素 %lld)\n",
                     static_cast<long long>(expectedFrames),
                     static_cast<long long>(baselineFrames.size()), compared, missingFrames,
                     unsupportedFrames, errorFrames, mismatching, differingPixels);
    }

    // --- 記録 --------------------------------------------------------------------
    QJsonObject report;
    if (options.compositorAll)
        report.insert(QStringLiteral("compositor_all"), compositorAllReport);
    report.insert(QStringLiteral("format"), QStringLiteral("mvm-math-score-export-diagnosis/1"));
    report.insert(QStringLiteral("fixture"), QString::fromLatin1(fixture.name));
    report.insert(QStringLiteral("inject"), static_cast<int>(options.inject));
    {
        // 数式 group (主の書き出し)。clip ごとの結果の対応づけに、書き出しと同じ計画を使う。
        mvm::app::TimelineExportRequest groupPlanRequest;
        groupPlanRequest.width = width;
        groupPlanRequest.height = height;
        groupPlanRequest.fpsNum = static_cast<int>(project.timelineFpsNum);
        groupPlanRequest.fpsDen = static_cast<int>(project.timelineFpsDen);
        const auto groupPlan = mvm::app::mapTimelineExportPlan(project, groupPlanRequest);
        auto main = groupJson(observer, mainGroupLive, mainGroupActive);
        main.insert(QStringLiteral("enabled"), options.mathGroupMain);
        main.insert(QStringLiteral("rule"), options.mathGroupRule);
        main.insert(QStringLiteral("alpha_floor_override"), options.mathGroupAlphaFloor);
        main.insert(QStringLiteral("budget_megabytes"),
                    static_cast<qint64>(options.mathGroupBudgetMb));
        main.insert(QStringLiteral("working_set_before_prepare_mb"),
                    static_cast<double>(mainWorkingSetBeforePrepare) / 1048576.0);
        main.insert(QStringLiteral("raster_live_bytes_after_export"), mainRasterLiveAfter);
        main.insert(QStringLiteral("working_set_after_export_mb"),
                    static_cast<double>(mainWorkingSetAfter) / 1048576.0);
        main.insert(QStringLiteral("storage"),
                    gPreparedStorage == mvm::math::MathRasterStorage::FullFrame
                        ? QStringLiteral("full-frame")
                        : QStringLiteral("roi-only"));
        if (options.mathGroupMain || options.productMain == 2) {
            main.insert(QStringLiteral("destination"), mainProbe.toJson());
            main.insert(QStringLiteral("assignment"), assignmentJson(mainProbe, groupPlan));
        }
        report.insert(QStringLiteral("math_group_main"), main);
        if (options.compareMathGroup) {
            report.insert(QStringLiteral("math_group_candidate_destination"),
                          candidateProbe.toJson());
            report.insert(QStringLiteral("math_group_candidate_assignment"),
                          assignmentJson(candidateProbe, groupPlan));
        }
        {
            const long long renderBegin = observer.stage_ns[MVM_EXPORT_STAGE_RENDER];
            const long long renderEnd = observer.stage_ns[MVM_EXPORT_STAGE_CONSUMER_CLOSE];
            report.insert(
                QStringLiteral("main_render_seconds"),
                renderEnd > renderBegin ? static_cast<double>(renderEnd - renderBegin) / 1e9 : 0.0);
            const auto count = [&](int point) {
                return static_cast<qint64>(observer.points[point].count);
            };
            report.insert(QStringLiteral("main_encoder_submitted"),
                          count(MVM_EXPORT_OBSERVE_ENCODER_SUBMIT));
            report.insert(QStringLiteral("main_encoded_packets"),
                          count(MVM_EXPORT_OBSERVE_ENCODED_PACKET));
            report.insert(QStringLiteral("main_muxed_packets"),
                          count(MVM_EXPORT_OBSERVE_MUXED_PACKET));
        }
        report.insert(QStringLiteral("process_peak_working_set_mb"),
                      static_cast<double>(workingSetBytes(true)) / 1048576.0);
        report.insert(QStringLiteral("process_working_set_mb"),
                      static_cast<double>(workingSetBytes(false)) / 1048576.0);
    }
    {
        // 空でなければ固定 fixture そのものではない (診断用に layer を絞っている)。
        QJsonArray selected;
        for (const int layer : options.layerSelect)
            selected.append(layer);
        report.insert(QStringLiteral("diagnostic_layer_select"), selected);
    }
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
        if (!sample.ready)
            continue; // 予約だけされて書き終わっていない枠
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
    report.insert(QStringLiteral("pre_yuv_admissible_alpha"), admissibleAlpha);
    report.insert(QStringLiteral("background_alpha"), fixture.backgroundAlpha);
    {
        const auto reportJson = [](const mvm::app::MathGroupExportReport& recorded) {
            return QJsonObject{
                {QStringLiteral("requested"),
                 recorded.requested == mvm::app::MathGroupOptimization::Auto
                     ? QStringLiteral("auto")
                     : QStringLiteral("off")},
                {QStringLiteral("evaluated"), recorded.evaluated},
                {QStringLiteral("decision"), QString::fromStdString(recorded.decision)},
                {QStringLiteral("reason"), QString::fromStdString(recorded.reason)},
                {QStringLiteral("graph"), recorded.mathGroupGraph ? QStringLiteral("math-group")
                                                                  : QStringLiteral("conventional")},
                {QStringLiteral("groups"), static_cast<qint64>(recorded.groups)},
                {QStringLiteral("grouped_clips"), static_cast<qint64>(recorded.groupedClips)},
                {QStringLiteral("retained_clips"), static_cast<qint64>(recorded.retainedClips)},
                {QStringLiteral("affine_transitions"),
                 static_cast<qint64>(recorded.affineTransitions)},
                {QStringLiteral("composed_frames"), static_cast<qint64>(recorded.composedFrames)},
                {QStringLiteral("callbacks"), static_cast<qint64>(recorded.callbacks)},
                {QStringLiteral("errors"), static_cast<qint64>(recorded.errors)},
                {QStringLiteral("raster_count"), static_cast<qint64>(recorded.rasterCount)},
                {QStringLiteral("raster_bytes"), static_cast<qint64>(recorded.rasterBytes)},
                {QStringLiteral("base_alpha_proven"), recorded.baseAlphaProven},
                {QStringLiteral("live_transitions_after"),
                 static_cast<qint64>(recorded.liveTransitionsAfter)},
                {QStringLiteral("active_callbacks_after"),
                 static_cast<qint64>(recorded.activeCallbacksAfter)}};
        };
        QJsonObject product{
            {QStringLiteral("controller_default_selection"), controllerDefaultSelection},
            {QStringLiteral("main_option"), options.productMain},
            {QStringLiteral("main_request_mode_seen"), mainRequestModeSeen},
            {QStringLiteral("main_controller_supplied_observer"), mainControllerSuppliedObserver},
            {QStringLiteral("main_report"), reportJson(mainReport)},
            {QStringLiteral("controller_last_report"),
             reportJson(controller->lastExportMathGroupReport())}};
        const ExtraRun* candidateSummary = comparing ? &candidateRun : nullptr;
        if (candidateSummary) {
            product.insert(QStringLiteral("candidate_option"), options.compareProduct);
            product.insert(QStringLiteral("candidate_request_mode_seen"),
                           candidateSummary->requestModeSeen);
            product.insert(QStringLiteral("candidate_observer_attached"),
                           candidateSummary->observerAttached);
            product.insert(QStringLiteral("candidate_controller_supplied_observer"),
                           candidateSummary->controllerSuppliedObserver);
            product.insert(QStringLiteral("candidate_output_sha256"),
                           candidateSummary->outputSha256);
            product.insert(QStringLiteral("candidate_report"),
                           reportJson(candidateSummary->report));
        }
        report.insert(QStringLiteral("product_selection"), product);
    }
    report.insert(
        QStringLiteral("v1_background"),
        QJsonObject{
            {QStringLiteral("fault"), options.v1BackgroundFault},
            {QStringLiteral("checked_requests"),
             static_cast<qint64>(observer.v1_background_frames)},
            {QStringLiteral("violations"), static_cast<qint64>(observer.v1_background_violations)},
            {QStringLiteral("rejected_before_mlt"),
             static_cast<qint64>(observer.v1_background_rejected)},
            {QStringLiteral("requested_formats_mask"), observer.v1_background_requested_formats}});
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
                // 5 列目 (SHA-256) は --strong-digest のときだけ付く。
                digests.write(
                    QStringLiteral("%1\t%2\t%3\t%4%5\n")
                        .arg(frame)
                        .arg(seen.calls)
                        .arg(seen.hash, 16, 16, QLatin1Char('0'))
                        .arg(seen.minAlpha)
                        .arg(seen.sha256.isEmpty()
                                 ? QString()
                                 : QLatin1Char('\t') + QString::fromLatin1(seen.sha256.toHex()))
                        .toUtf8());
        }
    }

    // --- 比較の記録 --------------------------------------------------------------
    QString compareVerdict = QStringLiteral("NOT_REQUESTED");
    if (comparing) {
        const auto runObject = [&](const MvmExportObserver& observed, double wallSeconds,
                                   double cpuSeconds, bool success) {
            const auto images = observed.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count;
            const long long renderBegin = observed.stage_ns[MVM_EXPORT_STAGE_RENDER];
            const long long renderEnd = observed.stage_ns[MVM_EXPORT_STAGE_CONSUMER_CLOSE];
            return QJsonObject{
                {QStringLiteral("success"), success},
                {QStringLiteral("wall_seconds"), wallSeconds},
                {QStringLiteral("cpu_seconds"), cpuSeconds},
                {QStringLiteral("render_seconds"),
                 renderEnd > renderBegin ? static_cast<double>(renderEnd - renderBegin) / 1e9
                                         : 0.0},
                {QStringLiteral("composited_images"), static_cast<qint64>(images)},
                // 合成だけの時間 (検査・encoder を含まない)。全 render worker の和を回数で割る。
                {QStringLiteral("compose_ms_per_image"),
                 images > 0 ? static_cast<double>(observed.image_ns_total) / 1e6 /
                                  static_cast<double>(images)
                            : 0.0},
                {QStringLiteral("animation_keyframes"),
                 static_cast<qint64>(observed.animation_keyframes)}};
        };
        QJsonObject compare;
        compare.insert(QStringLiteral("candidate"),
                       options.compareSplit       ? QStringLiteral("split-transitions")
                       : options.compareMathGroup ? QStringLiteral("math-group")
                       : options.compare == mvm::app::MathLayerStaging::BoundingBox
                           ? QStringLiteral("bounding-box")
                           : QStringLiteral("anchored-box"));
        compare.insert(QStringLiteral("candidate_fault"),
                       options.compareMathGroup ? options.mathGroupFault : options.splitFault);
        const auto groupObject = [&](const MvmExportObserver& observed, long long live,
                                     long long active) {
            return groupJson(observed, live, active);
        };
        compare.insert(QStringLiteral("baseline_group"),
                       groupObject(observer, mainGroupLive, mainGroupActive));
        compare.insert(
            QStringLiteral("candidate_group"),
            groupObject(candidateRun.observer, candidateRun.groupLive, candidateRun.groupActive));
        compare.insert(QStringLiteral("repeat_group"),
                       groupObject(repeatRun.observer, repeatRun.groupLive, repeatRun.groupActive));
        compare.insert(QStringLiteral("candidate_mode"), options.compareMathGroupMode);
        compare.insert(QStringLiteral("candidate_rule"), options.mathGroupRule);
        compare.insert(QStringLiteral("candidate_alpha_floor_override"),
                       options.mathGroupAlphaFloor);
        compare.insert(QStringLiteral("candidate_working_set_before_prepare_mb"),
                       static_cast<double>(candidateRun.workingSetBeforePrepare) / 1048576.0);
        compare.insert(QStringLiteral("candidate_raster_live_bytes_after_export"),
                       candidateRun.rasterLiveAfter);
        compare.insert(QStringLiteral("candidate_working_set_after_export_mb"),
                       static_cast<double>(candidateRun.workingSetAfter) / 1048576.0);
        compare.insert(QStringLiteral("candidate_storage"),
                       gPreparedStorage == mvm::math::MathRasterStorage::FullFrame
                           ? QStringLiteral("full-frame")
                           : QStringLiteral("roi-only"));
        if (!alphaBackground.empty()) {
            // 背景の alpha ごと: 画素の数、MLT が背景から変えた画素、candidate が MLT と違う画素。
            QJsonArray byAlpha;
            for (int alpha = 0; alpha < 256; ++alpha) {
                const auto& tally = alphaTallies[static_cast<std::size_t>(alpha)];
                if (tally.pixels == 0)
                    continue;
                QJsonObject entry{
                    {QStringLiteral("background_alpha"), alpha},
                    {QStringLiteral("pixels"), static_cast<qint64>(tally.pixels)},
                    {QStringLiteral("changed_by_mlt"), static_cast<qint64>(tally.touched)},
                    {QStringLiteral("changed_by_mlt_keeping_alpha_zero"),
                     static_cast<qint64>(tally.touchedAlphaOnly)},
                    {QStringLiteral("mismatching"), static_cast<qint64>(tally.mismatching)}};
                if (!alphaExamples[alpha].isEmpty())
                    entry.insert(QStringLiteral("first_mismatch"), alphaExamples[alpha]);
                byAlpha.append(entry);
            }
            compare.insert(QStringLiteral("by_background_alpha"), byAlpha);
        }
        // candidate の書き出しが失敗したときの理由。画素の不一致による検出と区別する。
        compare.insert(QStringLiteral("candidate_error"),
                       QString::fromStdString(candidateRun.result.error));
        compare.insert(QStringLiteral("candidate_overlay_transitions"),
                       static_cast<qint64>(candidateRun.observer.overlay_transitions));
        compare.insert(QStringLiteral("ran"), compareRan);
        compare.insert(
            QStringLiteral("baseline"),
            runObject(observer,
                      static_cast<double>(exportEndNs.load() - exportBeginNs.load()) / 1e9,
                      cpuEnd - cpuBegin, exported.success));
        compare.insert(
            QStringLiteral("candidate_run"),
            runObject(candidateRun.observer,
                      static_cast<double>(candidateRun.endNs - candidateRun.beginNs) / 1e9,
                      candidateRun.cpuSeconds, candidateRun.result.success));
        compare.insert(QStringLiteral("baseline_repeat"),
                       runObject(repeatRun.observer,
                                 static_cast<double>(repeatRun.endNs - repeatRun.beginNs) / 1e9,
                                 repeatRun.cpuSeconds, repeatRun.result.success));
        long long regionTotal = 0;
        for (const auto pixels : candidateRun.regionPixels)
            regionTotal += pixels;
        int mathClips = 0;
        for (const auto& clip : project.timelineClips)
            mathClips += clip.kind == TimelineClipKind::Math && clip.enabled;
        compare.insert(QStringLiteral("math_clips"), mathClips);
        compare.insert(QStringLiteral("cropped_clips"),
                       static_cast<int>(candidateRun.regionPixels.size()));
        compare.insert(QStringLiteral("cropped_area_ratio_mean"),
                       candidateRun.regionPixels.empty()
                           ? 0.0
                           : static_cast<double>(regionTotal) /
                                 static_cast<double>(candidateRun.regionPixels.size()) /
                                 (static_cast<double>(width) * height));

        // frame ごとの分類 (集計の軸に使うだけ。期待値には使わない)。
        struct Bucket {
            long long frames = 0, exact = 0, withinOne = 0, beyond = 0;
        };

        std::map<std::string, Bucket> buckets;
        long long differingPixels = 0, beyondPixels = 0, framesExact = 0, framesWithinOne = 0,
                  framesBeyond = 0, framesUncompared = 0;
        int worstDifference = 0;
        QJsonArray differingFrames;
        for (std::int64_t frame = 0; frame < expectedFrames; ++frame) {
            if (!candidateSeen.contains(frame)) {
                ++framesUncompared; // 比べていない frame は一致に数えない
                continue;
            }
            int layers = 0;
            bool fractional = false, interpolating = false, fading = false, scaled = false;
            for (const auto& clip : project.timelineClips) {
                if (clip.kind != TimelineClipKind::Math || !clip.enabled)
                    continue;
                const auto end = clipEnd(project, clip);
                if (frame + frameBase < clip.timelineStartFrame || frame + frameBase >= end)
                    continue;
                ++layers;
                const auto local = frame + frameBase - clip.timelineStartFrame;
                const auto duration = end - clip.timelineStartFrame;
                const auto at = mvm::project::evaluateClipEffects(clip.effects, local);
                const auto differs = [&](std::int64_t other) {
                    if (other < 0 || other >= duration)
                        return false;
                    const auto value = mvm::project::evaluateClipEffects(clip.effects, other);
                    return value.positionXPercent != at.positionXPercent ||
                           value.positionYPercent != at.positionYPercent ||
                           value.scaleXPercent != at.scaleXPercent ||
                           value.scaleYPercent != at.scaleYPercent ||
                           value.opacityPercent != at.opacityPercent;
                };
                interpolating = interpolating || differs(local - 1) || differs(local + 1);
                fading = fading || local < clip.effects.fadeInFrames ||
                         local >= duration - clip.effects.fadeOutFrames;
                scaled = scaled || at.scaleXPercent != 100.0 || at.scaleYPercent != 100.0;
                const double dx = at.positionXPercent / 100.0 * width;
                const double dy = at.positionYPercent / 100.0 * height;
                fractional = fractional || std::abs(dx - std::round(dx)) > 1e-6 ||
                             std::abs(dy - std::round(dy)) > 1e-6;
            }
            const auto found = candidateDifferences.find(frame);
            const int kind = found == candidateDifferences.end() ? 0
                             : found->second.beyond > 0          ? 2
                                                                 : 1;
            framesExact += kind == 0;
            framesWithinOne += kind == 1;
            framesBeyond += kind == 2;
            const auto add = [&](const char* name, bool applies) {
                if (!applies)
                    return;
                auto& bucket = buckets[name];
                ++bucket.frames;
                bucket.exact += kind == 0;
                bucket.withinOne += kind == 1;
                bucket.beyond += kind == 2;
            };
            add("all", true);
            add("no_math_layer", layers == 0);
            add("integer_position_static", layers > 0 && !fractional && !interpolating && !fading);
            add("fractional_position_static", fractional && !interpolating && !fading);
            add("interpolating", interpolating);
            add("fading", fading);
            add("scaled_layer_present", scaled);
            add("multiple_layers", layers >= 2);
            if (kind != 0) {
                const auto& difference = found->second;
                differingPixels += difference.pixels;
                beyondPixels += difference.beyond;
                worstDifference = std::max(worstDifference, difference.maxDifference);
                if (differingFrames.size() < 48)
                    differingFrames.append(QJsonObject{
                        {QStringLiteral("frame"), static_cast<qint64>(frame)},
                        {QStringLiteral("layers"), layers},
                        {QStringLiteral("differing_pixels"),
                         static_cast<qint64>(difference.pixels)},
                        {QStringLiteral("pixels_beyond_one"),
                         static_cast<qint64>(difference.beyond)},
                        {QStringLiteral("max_channel_difference"), difference.maxDifference},
                        {QStringLiteral("region"),
                         QJsonArray{difference.left, difference.top, difference.right,
                                    difference.bottom}}});
            }
        }
        QJsonObject bucketObject;
        for (const auto& [name, bucket] : buckets)
            bucketObject.insert(QString::fromLatin1(name.c_str()),
                                QJsonObject{{QStringLiteral("frames"), bucket.frames},
                                            {QStringLiteral("exact"), bucket.exact},
                                            {QStringLiteral("within_one"), bucket.withinOne},
                                            {QStringLiteral("beyond_one"), bucket.beyond}});
        compare.insert(QStringLiteral("frames_expected"), static_cast<qint64>(expectedFrames));
        compare.insert(QStringLiteral("frames_exact"), framesExact);
        compare.insert(QStringLiteral("frames_within_one"), framesWithinOne);
        compare.insert(QStringLiteral("frames_beyond_one"), framesBeyond);
        compare.insert(QStringLiteral("frames_uncompared"), framesUncompared);
        compare.insert(QStringLiteral("differing_pixels"), differingPixels);
        compare.insert(QStringLiteral("pixels_beyond_one"), beyondPixels);
        compare.insert(QStringLiteral("max_channel_difference"), worstDifference);
        compare.insert(QStringLiteral("unexpected_callbacks"), compareUnexpected);
        compare.insert(QStringLiteral("baseline_repeat_frames"),
                       static_cast<qint64>(repeatSeen.size()));
        compare.insert(QStringLiteral("baseline_repeat_mismatches"), repeatMismatches);
        compare.insert(QStringLiteral("by_category"), bucketObject);
        compare.insert(QStringLiteral("differing_frames_first"), differingFrames);
        // 数式 group: 書き出しの後に transition・callback が残っていないこと、candidate が実際に
        // clip をまとめたこと (まとめていなければ比較は空振り)、baseline は group
        // を使っていないこと。
        const bool groupClean =
            mainGroupLive == 0 && mainGroupActive == 0 && candidateRun.groupLive == 0 &&
            candidateRun.groupActive == 0 && repeatRun.groupLive == 0 &&
            repeatRun.groupActive == 0 &&
            (!options.compareMathGroup || candidateRun.rasterLiveAfter == 0) &&
            candidateLeaks == 0 && candidateFailures == 0 &&
            (!options.compareMathGroup ||
             (observer.math_group_clips == 0 && repeatRun.observer.math_group_clips == 0 &&
              (candidateRun.observer.math_group_graph == 0
                   // 従来の graph を選んだ (mode 3 で ELIGIBLE でなかった): group を 1 つも使わず、
                   // affine の数が製品の経路と同じであること。判定が記録されていること。
                   ? options.compareMathGroupMode == 3 &&
                         (options.compareProduct == 1
                              // 製品の選択 off: 判定そのものをしない (従来の graph のまま)。
                              ? candidateRun.observer.math_group_decision ==
                                    MVM_MATH_GROUP_DECISION_NONE
                              : candidateRun.observer.math_group_decision !=
                                        MVM_MATH_GROUP_DECISION_ELIGIBLE &&
                                    candidateRun.observer.math_group_decision !=
                                        MVM_MATH_GROUP_DECISION_NONE) &&
                         candidateRun.observer.math_group_clips == 0 &&
                         candidateRun.observer.math_group_callbacks == 0 &&
                         candidateRun.observer.overlay_transitions ==
                             observer.overlay_transitions * candidateExports
                   : options.compareProduct != 1 && candidateRun.observer.math_group_clips > 0 &&
                         (options.compareMathGroupMode == 2
                              ? candidateRun.observer.math_group_callbacks > 0
                              : candidateRun.observer.math_group_frames > 0))));
        compare.insert(QStringLiteral("candidate_exports"), candidateExports);
        compare.insert(QStringLiteral("candidate_export_failures"), candidateFailures);
        compare.insert(QStringLiteral("candidate_leaks"), candidateLeaks);
        compare.insert(QStringLiteral("group_clean"), groupClean);
        const bool comparable =
            compareRan && candidateRun.result.success && repeatRun.result.success &&
            framesUncompared == 0 && compareUnexpected == 0 && repeatMismatches == 0 &&
            static_cast<std::int64_t>(repeatSeen.size()) == expectedFrames && groupClean;
        compareVerdict = !comparable           ? QStringLiteral("COMPARE_INVALID")
                         : framesBeyond > 0    ? QStringLiteral("DIFFERS_BEYOND_ONE")
                         : framesWithinOne > 0 ? QStringLiteral("WITHIN_ONE_NOT_EXACT")
                                               : QStringLiteral("EXACT");
        compare.insert(QStringLiteral("verdict"), compareVerdict);
        report.insert(QStringLiteral("compare"), compare);
    }
    // --- property 取得の計時 (P2-A) ----------------------------------------------
    const auto probeObject = [&](const mvm::test::MltPropertyProbe::Snapshot& begin,
                                 const mvm::test::MltPropertyProbe::Snapshot& end,
                                 const MvmExportObserver& observed) {
        using Probe = mvm::test::MltPropertyProbe;
        QJsonObject object;
        double totalSeconds = 0.0;
        for (int function = 0; function < Probe::FunctionCount; ++function) {
            QJsonObject names;
            for (int name = 0; name < Probe::NameCount; ++name) {
                const auto calls = end.calls[function][name] - begin.calls[function][name];
                const auto spent =
                    Probe::seconds(end.ticks[function][name] - begin.ticks[function][name]);
                totalSeconds += spent;
                names.insert(QString::fromLatin1(Probe::kNameLabels[name]),
                             QJsonObject{{QStringLiteral("calls"), static_cast<qint64>(calls)},
                                         {QStringLiteral("seconds"), spent}});
            }
            object.insert(QString::fromLatin1(Probe::kFunctionNames[function]), names);
        }
        long long stringTicks = 0;
        for (const int name : {Probe::Rect, Probe::Rotate, Probe::Shear})
            stringTicks += end.ticks[Probe::Get][name] - begin.ticks[Probe::Get][name];
        const double stringSeconds = Probe::seconds(stringTicks);
        const double composeSeconds = static_cast<double>(observed.image_ns_total) / 1e9;
        const auto images = observed.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count;
        const long long build = observed.stage_ns[MVM_EXPORT_STAGE_GRAPH_BUILD];
        const long long start = observed.stage_ns[MVM_EXPORT_STAGE_CONSUMER_START];
        object.insert(QStringLiteral("all_hooked_seconds"), totalSeconds);
        // animation の付いた property を文字列で読む呼び出し (この中で animation
        // 全体が文字列になる)。
        object.insert(QStringLiteral("animated_string_get_seconds"), stringSeconds);
        object.insert(QStringLiteral("compose_seconds"), composeSeconds);
        object.insert(QStringLiteral("animated_string_get_share_of_compose"),
                      composeSeconds > 0.0 ? stringSeconds / composeSeconds : 0.0);
        object.insert(QStringLiteral("composited_images"), static_cast<qint64>(images));
        object.insert(QStringLiteral("compose_ms_per_image"),
                      images > 0 ? composeSeconds * 1e3 / static_cast<double>(images) : 0.0);
        object.insert(QStringLiteral("animated_string_get_ms_per_image"),
                      images > 0 ? stringSeconds * 1e3 / static_cast<double>(images) : 0.0);
        object.insert(QStringLiteral("graph_build_seconds"),
                      start > build ? static_cast<double>(start - build) / 1e9 : 0.0);
        object.insert(QStringLiteral("animation_samples"),
                      static_cast<qint64>(observed.animation_samples));
        object.insert(QStringLiteral("animation_keyframes"),
                      static_cast<qint64>(observed.animation_keyframes));
        return object;
    };
    report.insert(QStringLiteral("property_probe_hooks"), probeHooks);
    report.insert(QStringLiteral("property_probe_threads"),
                  mvm::test::MltPropertyProbe::threadCount());
    report.insert(QStringLiteral("property_probe"),
                  probeObject(probeMainBegin, probeMainEnd, observer));
    if (comparing && compareRan) {
        report.insert(
            QStringLiteral("property_probe_candidate"),
            probeObject(candidateRun.probeBegin, candidateRun.probeEnd, candidateRun.observer));
        report.insert(QStringLiteral("property_probe_baseline_repeat"),
                      probeObject(repeatRun.probeBegin, repeatRun.probeEnd, repeatRun.observer));
    }
    report.insert(QStringLiteral("keyframe_stride"), options.keyframeStride);
    report.insert(QStringLiteral("split_transitions"), options.splitTransitions);
    report.insert(QStringLiteral("split_policy"), QStringLiteral("%1/%2/%3")
                                                      .arg(options.splitAnimatedRun)
                                                      .arg(options.splitStaticRun)
                                                      .arg(options.splitStaticChunk));
    report.insert(
        QStringLiteral("frame_request_ms_per_image"),
        observer.points[MVM_EXPORT_OBSERVE_FRAME_RETURN].count > 0
            ? static_cast<double>(observer.frame_ns_total) / 1e6 /
                  static_cast<double>(observer.points[MVM_EXPORT_OBSERVE_FRAME_RETURN].count)
            : 0.0);
    report.insert(QStringLiteral("overlay_transitions"),
                  static_cast<qint64>(observer.overlay_transitions));
    report.insert(QStringLiteral("range_first"), static_cast<qint64>(options.rangeFirst));
    report.insert(QStringLiteral("range_count"), static_cast<qint64>(options.rangeCount));
    report.insert(QStringLiteral("parity_claimed"), !rangeMode);
    report.insert(QStringLiteral("render_threads_override"), options.renderThreads);
    report.insert(QStringLiteral("main_staging"),
                  options.mainStaging == mvm::app::MathLayerStaging::FullFrame
                      ? QStringLiteral("full-frame (product)")
                  : options.mainStaging == mvm::app::MathLayerStaging::BoundingBox
                      ? QStringLiteral("bounding-box (prototype)")
                      : QStringLiteral("anchored-box (prototype)"));
    report.insert(
        QStringLiteral("compose_ms_per_image"),
        observer.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count > 0
            ? static_cast<double>(observer.image_ns_total) / 1e6 /
                  static_cast<double>(observer.points[MVM_EXPORT_OBSERVE_IMAGE_RETURN].count)
            : 0.0);

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
    // 比較は baseline の照合が成立したときだけ判定に使う。
    if (exitCode == kExitPass && comparing) {
        if (compareVerdict == QStringLiteral("COMPARE_INVALID"))
            exitCode = kExitCompareInvalid;
        else if (compareVerdict == QStringLiteral("DIFFERS_BEYOND_ONE"))
            exitCode = kExitCompareDiffers;
        else if (compareVerdict == QStringLiteral("WITHIN_ONE_NOT_EXACT"))
            exitCode = kExitCompareWithinOne;
        if (exitCode != kExitPass)
            verdict = compareVerdict;
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
