// P2-8: MathTransform P2 の統合受け入れ (実 Manim + MiKTeX、CTest に登録しない)。
//
//   mvm_test_text_ui_input --math-p28-acceptance <manim.exe> <作業 directory (存在しないこと)>
//
// 二次方程式の導出 E1 → E2 → E3 を、製品の Main.qml と controller の操作で作る。
//   A. 作成: 数式 clip の追加メニュー・式の入力欄・Write の欄・編集点の「数式の変形を適用」・
//      トランジションの inspector (配置・長さ) を使う。変形を Project へ直接書かない。
//      保存は controller の saveProject (製品の保存)。
//   B. 開き直し: 保存した file を新しい controller と新しい Main.qml で開く。作成時の cache は
//      退避して保持し、開き直した Project から実 Manim で静止・Write・変形を描く。
//   C. 実 D3D11 preview: Write・両方の変形の frame 0 / cut の前後 / 最後 / 直後の静止。
//      mask が遅れて届く再生 (P1.2 / P2-5 の保留の仕組み)、E1 → E3 の通し再生。
//   D. 書き出し: preview の mask を OverBudget にしてから、製品の書き出し (映像のみ) を行い、
//      復号した frame を製品の preview の合成と比べる (hard cut・ずらし・二重の不透明度の対照)。
//   E. 負例: 変形の frame の破損・欠落、端点の式の変更 (古い key)、backend の不在。
//      いずれも書き出しを拒否し、保存した Project を変えず、Blend に置き換えない。
// 画面を表示するが、window は入力を透過し前面を取らない (操作可)。
#include "app/math_clip_render.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "math_raster_cache.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "test_window_focus.h"
#include "waveform_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>

namespace {

namespace project = mvm::project;
using mvm::app::MathRasterCache;
using mvm::app::MvmController;

int checks = 0;
int failures = 0;
// 試験の window が swap した回数 (render thread で数える)。提示を待てないときの診断に使う。
std::atomic<long long> frameSwaps{0};
QQuickWindow* shownWindow = nullptr;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void note(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
}

bool pump(const std::function<bool()>& done, int milliseconds = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
    return done();
}

void settle(int milliseconds) {
    pump([] { return false; }, milliseconds);
}

// ---- 受け入れの値 (いずれも手で数えた値) ----
// timeline は 60 fps、出力 1920x1080。数式 clip は既定 5 秒 (300 frame)。
constexpr int kOutputWidth = 1920;
constexpr int kOutputHeight = 1080;
constexpr qint64 kClipFrames = 300;
constexpr qint64 kE1Start = 0;
constexpr qint64 kE2Start = 300;
constexpr qint64 kE3Start = 600;
constexpr qint64 kEnd = 900;
// E1 の Write は 1.5 秒 = 90 frame (timeline frame 0..89)。
constexpr qint64 kWriteFrames = 90;
// T1 (E1 → E2): 作成時は cut で開始 (0 / 60)。配置を「中央」にして 30 / 30。区間は 270..329。
constexpr qint64 kT1Before = 30;
constexpr qint64 kT1After = 30;
constexpr qint64 kT1Start = kE2Start - kT1Before;
constexpr qint64 kT1Frames = kT1Before + kT1After;
// T2 (E2 → E3): 長さ欄で 0.5 秒 (0 / 30)、配置を「中央」にして 15 / 15。区間は 585..614。
constexpr qint64 kT2Before = 15;
constexpr qint64 kT2After = 15;
constexpr qint64 kT2Start = kE3Start - kT2Before;
constexpr qint64 kT2Frames = kT2Before + kT2After;
// 3 本に等しく一定の ClipEffects (不透明度 80%)。合成で 1 回だけ掛かること (二重なら 64%) を見る。
constexpr double kOpacityPercent = 80.0;

const std::string kE1 = "x^2 + \\frac{b}{a}x = -\\frac{c}{a}";
const std::string kE2 = "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                        "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2";
const std::string kE3 = "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}";

const char* kFfmpeg = "C:/msys64/ucrt64/bin/ffmpeg.exe";
const char* kFfprobe = "C:/msys64/ucrt64/bin/ffprobe.exe";

// ---- 画面の部品 ----

void collectItems(QQuickItem* item, QList<QQuickItem*>& out) {
    out.push_back(item);
    for (QQuickItem* child : item->childItems())
        collectItems(child, out);
}

QQuickItem* findVisualItem(QQuickWindow* window, const QString& name) {
    QList<QQuickItem*> items;
    collectItems(window->contentItem(), items);
    for (QQuickItem* item : items)
        if (item->objectName() == name)
            return item;
    return nullptr;
}

QPoint itemCenter(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

QString textOf(QQuickItem* item) {
    return item ? item->property("text").toString() : QString();
}

void typeText(QWindow* window, const char* text) {
    for (const char* c = text; *c; ++c)
        QTest::keyClick(window, *c);
}

// 製品の Main.qml を controller で開き、preview を付ける。window は入力を透過し前面を取らない。
struct ProductWindow {
    mvm::app::WaveformCache waveforms;
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;

    bool open(MvmController& controller, std::string& reason) {
        auto properties = mvm::app::testFixedWindowInitialProperties();
        properties.insert(QStringLiteral("mvmController"), QVariant::fromValue(&controller));
        properties.insert(QStringLiteral("waveformCache"), QVariant::fromValue(&waveforms));
        if (!mvm::app::testFixedWindowRequested())
            properties.insert(QStringLiteral("flags"), mvm::test::backgroundWindowFlags());
        engine.setInitialProperties(properties);
        engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        window = engine.rootObjects().isEmpty()
                     ? nullptr
                     : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (!window) {
            reason = "Main.qml を読めません";
            return false;
        }
        QString isolated;
        if (!QTest::qWaitForWindowExposed(window) || !mvm::test::focusWithoutForeground(window) ||
            !mvm::test::isolatedFromUserInput(window, isolated)) {
            reason = "PROTOCOL_INVALID: 試験の window を操作から隔離できません: " +
                     isolated.toStdString();
            return false;
        }
        window->setProperty("leftPanelWidth", 500);
        window->setProperty("leftPanelTab", 0);
        QObject::connect(
            window, &QQuickWindow::frameSwapped, window, [] { ++frameSwaps; },
            Qt::DirectConnection);
        shownWindow = window;
        controller.attachPreview(
            window->findChild<mvm::app::PreviewEngineRhiItem*>(QStringLiteral("previewSurface")));
        return true;
    }

    QQuickItem* item(const char* name) const {
        return findVisualItem(window, QString::fromLatin1(name));
    }

    void click(QQuickItem* target) {
        QTest::mouseClick(window, Qt::LeftButton, {}, itemCenter(target));
        settle(300);
    }
};

// ---- Project の参照 ----

const project::TimelineClip* clipById(const project::Project& p, const std::string& id) {
    for (const auto& clip : p.timelineClips)
        if (clip.id == id)
            return &clip;
    return nullptr;
}

const project::TimelineTransition* transitionBetween(const project::Project& p,
                                                     const std::string& outgoing,
                                                     const std::string& incoming) {
    for (const auto& transition : p.timelineTransitions)
        if (transition.outgoingClipId == outgoing && transition.incomingClipId == incoming)
            return &transition;
    return nullptr;
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// ---- 合成の画素 ----

// still に、animation が frame で見せる patch を重ねた画素 (engine が texture の矩形を
// 置き換えるのと同じ)。state が負なら still のまま。patch は無関係な値で埋めてから呼ぶ
// (engine は作業領域を使い回すので、fillPatch が矩形の全画素を書くことを確かめる)。
std::vector<std::uint8_t> presentedPixels(const mvm::preview::PreviewCompositionLayer& layer,
                                          std::int64_t frame) {
    auto rgba = layer.stillImage->rgba;
    const auto* animation = layer.stillAnimation.get();
    if (!animation)
        return rgba;
    const auto state = animation->stateAt(frame);
    if (state < 0)
        return rgba;
    const auto rect = animation->patchRect();
    std::vector<std::uint8_t> patch(
        static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 4U, 0xCD);
    animation->fillPatch(state, patch.data());
    const std::size_t row = static_cast<std::size_t>(rect.width) * 4U;
    const auto width = static_cast<std::size_t>(layer.stillImage->width);
    for (int y = 0; y < rect.height; ++y)
        std::memcpy(rgba.data() + (static_cast<std::size_t>(rect.y + y) * width +
                                   static_cast<std::size_t>(rect.x)) *
                                      4U,
                    patch.data() + static_cast<std::size_t>(y) * row, row);
    return rgba;
}

std::size_t differingPixels(const std::vector<std::uint8_t>& a,
                            const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size())
        return std::numeric_limits<std::size_t>::max();
    std::size_t count = 0;
    for (std::size_t at = 0; at < a.size(); at += 4)
        count += std::memcmp(a.data() + at, b.data() + at, 4) != 0;
    return count;
}

// alpha の合計 (被覆の量)。Write の進み具合の単調性を見る。
std::uint64_t alphaSum(const std::vector<std::uint8_t>& rgba) {
    std::uint64_t sum = 0;
    for (std::size_t at = 3; at < rgba.size(); at += 4)
        sum += rgba[at];
    return sum;
}

std::uint8_t maxAlpha(const std::vector<std::uint8_t>& rgba) {
    std::uint8_t most = 0;
    for (std::size_t at = 3; at < rgba.size(); at += 4)
        most = std::max(most, rgba[at]);
    return most;
}

std::vector<std::uint8_t> shiftedRight(const std::vector<std::uint8_t>& rgba, int pixels) {
    std::vector<std::uint8_t> out(rgba.size(), 0);
    for (int y = 0; y < kOutputHeight; ++y)
        for (int x = 0; x + pixels < kOutputWidth; ++x)
            std::memcpy(
                out.data() + (static_cast<std::size_t>(y) * kOutputWidth +
                              static_cast<std::size_t>(x + pixels)) *
                                 4U,
                rgba.data() +
                    (static_cast<std::size_t>(y) * kOutputWidth + static_cast<std::size_t>(x)) * 4U,
                4);
    return out;
}

// 合成の数式の静止画 layer (同じ frame に数式 clip は 1 本だけ)。数と一緒に返す。
std::optional<mvm::preview::PreviewCompositionLayer>
stillLayerOf(const std::shared_ptr<const mvm::preview::CompositionSnapshot>& composition,
             int* count = nullptr) {
    if (count)
        *count = 0;
    std::optional<mvm::preview::PreviewCompositionLayer> found;
    if (!composition)
        return found;
    for (const auto& layer : composition->layers)
        if (layer.stillImage) {
            if (count)
                ++*count;
            if (!found)
                found = layer;
        }
    return found;
}

// 提示の完了を待つ。条件 (engine の status を lock して読む) は 20 ms ごとに評価する。
// 2 ms ごとに評価すると、試験の window の swap が 30 秒で 3 回まで落ち、paused の seek が
// 完了しなかった (診断の series、docs/math-clips.md の P2-8)。
bool waitPresented(const std::function<bool()>& done, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() >= milliseconds)
            return false;
        settle(20);
    }
    return true;
}

bool seekWhenReady(MvmController& controller, qint64 frame) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (controller.seekTimelineFrame(frame))
            return true;
        settle(50);
    }
    note("seek できません: " + controller.statusText().toStdString());
    return false;
}

// 提示を待てなかったときの診断: engine の状態を 3 秒見る (組み直しが続くのか、提示が止まって
// いるのかを分ける)。合否には使わない。
void diagnosePreview(MvmController& controller, const std::string& label) {
    std::shared_ptr<const mvm::preview::CompositionSnapshot> last =
        controller.submittedCompositionForTest();
    int changes = 0;
    for (int sample = 0; sample < 30; ++sample) {
        const auto engine = controller.previewEngineForTest();
        const auto s = engine ? engine->status() : mvm::preview::PreviewStatus{};
        const auto submitted = controller.submittedCompositionForTest();
        changes += submitted != last;
        last = submitted;
        if (sample % 5 == 0) {
            const auto id = [](const std::optional<mvm::preview::AcceptedComposition>& c) {
                return c ? std::to_string(c->id.value) + "/" + std::to_string(c->revision)
                         : std::string("-");
            };
            note(label + ": 診断: engine state " + std::to_string(static_cast<int>(s.state)) +
                 " accepted " + id(s.latestAcceptedDesiredComposition) + " presented " +
                 id(s.lastPresentedComposition) + " position " +
                 std::to_string(s.position.outputFrame) + " playhead " +
                 std::to_string(controller.playheadFrame()) + " rebuilds " +
                 std::to_string(controller.playbackRebuildCount()) + " swap " +
                 std::to_string(frameSwaps.load()) + " exposed " +
                 std::to_string(shownWindow && shownWindow->isExposed()));
        }
        settle(100);
    }
    note(label + ": 診断: 3 秒間の submitted の合成の入れ替わり " + std::to_string(changes) +
         "、status " + controller.statusText().toStdString());
}

// 実 D3D11 preview へ seek し、engine が提示した (submit 済みの) 合成の数式 layer を返す。
// wantAnimated なら、その frame の animation の state が 0 以上になる (mask が memory に来る)
// まで待つ。
std::optional<mvm::preview::PreviewCompositionLayer> presentedLayerAt(MvmController& controller,
                                                                      qint64 frame,
                                                                      bool wantAnimated,
                                                                      int* layers = nullptr) {
    if (!seekWhenReady(controller, frame))
        return std::nullopt;
    const auto swapsAtSeek = frameSwaps.load();
    std::optional<mvm::preview::PreviewCompositionLayer> layer;
    const bool ok = waitPresented(
        [&] {
            if (controller.playheadFrame() != frame || !controller.previewPresentedLatest())
                return false;
            layer = stillLayerOf(controller.submittedCompositionForTest(), layers);
            if (!layer)
                return false;
            if (!wantAnimated)
                return true;
            return layer->stillAnimation && layer->stillAnimation->stateAt(frame) >= 0;
        },
        30000);
    if (!ok) {
        note("frame " + std::to_string(frame) +
             " の提示を待てません: " + controller.statusText().toStdString() + " (待つ間の swap " +
             std::to_string(frameSwaps.load() - swapsAtSeek) + ")");
        diagnosePreview(controller, "frame " + std::to_string(frame));
        return std::nullopt;
    }
    return layer;
}

// ---- engine の評価の記録 (P1.2 / P2-5 の observer) ----

struct Observation {
    std::mutex mutex;
    // (変形の ID / clip の ID、出力 frame、見せた frame)。Write は ID を "write" とする。
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> records;

    void clear() {
        std::lock_guard lock(mutex);
        records.clear();
    }

    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> snapshot() {
        std::lock_guard lock(mutex);
        return records;
    }
};

// 手で数えた区間で「変形の frame = 出力 frame - 区間の先頭」(区間の外は -1)。
std::int64_t expectedTransformFrame(const std::string& transitionId, const std::string& t1,
                                    std::int64_t frame) {
    const qint64 start = transitionId == t1 ? kT1Start : kT2Start;
    const qint64 count = transitionId == t1 ? kT1Frames : kT2Frames;
    return frame >= start && frame < start + count ? frame - start : -1;
}

std::int64_t expectedWriteFrame(std::int64_t frame) {
    return frame >= kE1Start && frame < kE1Start + kWriteFrames ? frame - kE1Start : -1;
}

// ---- 書き出しの復号と比較 ----

std::optional<QByteArray> decodeFrame(const std::filesystem::path& video, qint64 frame) {
    QProcess decoder;
    decoder.start(QString::fromLatin1(kFfmpeg),
                  {"-v", "error", "-i", QString::fromStdWString(video.wstring()), "-vf",
                   QStringLiteral("select=eq(n\\,%1)").arg(frame), "-frames:v", "1", "-f",
                   "rawvideo", "-pix_fmt", "rgba", "-"});
    if (!decoder.waitForFinished(120000) || decoder.exitCode() != 0)
        return std::nullopt;
    const auto bytes = decoder.readAllStandardOutput();
    if (bytes.size() != static_cast<qsizetype>(kOutputWidth) * kOutputHeight * 4)
        return std::nullopt;
    return bytes;
}

struct Comparison {
    std::size_t compared = 0; // 比べた channel の数
    std::size_t visible = 0;  // alpha > 128 の画素
    std::size_t support = 0;  // alpha > 0 の画素
    std::size_t bad = 0;      // 差が 60 を超えた channel
    long long totalError = 0;

    // P2-6 の許容 (4:2:0 の色差の広がりを含む)。対象画素 0 件では成功にしない。
    bool passes() const {
        return visible > 100 && compared > 0 && totalError < static_cast<long long>(support) * 50 &&
               bad < visible;
    }
};

// 期待: 製品の preview の画素 (straight alpha) を黒の上に opacity で 1 回だけ重ねた色。
Comparison compareDecoded(const QByteArray& decoded, const std::vector<std::uint8_t>& expected,
                          double opacity) {
    Comparison result;
    const auto* actual = reinterpret_cast<const unsigned char*>(decoded.constData());
    for (std::size_t at = 0; at < expected.size(); at += 4) {
        const int alpha = expected[at + 3];
        result.visible += alpha > 128;
        result.support += alpha > 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int wanted =
                static_cast<int>(std::lround(expected[at + channel] * (alpha / 255.0) * opacity));
            const int difference = std::abs(static_cast<int>(actual[at + channel]) - wanted);
            result.totalError += difference;
            result.bad += difference > 60;
            ++result.compared;
        }
    }
    return result;
}

std::string describe(const Comparison& c) {
    return "比較=" + std::to_string(c.compared) + " 被覆=" + std::to_string(c.visible) +
           " 非零被覆=" + std::to_string(c.support) + " 誤差和=" + std::to_string(c.totalError) +
           " 大差=" + std::to_string(c.bad);
}

// cache の transform directory の file と更新時刻 (書き出しが描き直していないことの証拠)。
std::map<std::string, std::filesystem::file_time_type> listTree(const std::filesystem::path& root) {
    std::map<std::string, std::filesystem::file_time_type> files;
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
        if (entry.is_regular_file(error))
            files.emplace(std::filesystem::relative(entry.path(), root, error).generic_string(),
                          entry.last_write_time(error));
    return files;
}

bool waitExport(MvmController& controller) {
    return pump([&] { return !controller.exporting(); }, 900000);
}

QUrl urlOf(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
}

// ---- A. 製品の UI で作る ----

struct Authored {
    project::Project project;
    std::string e1, e2, e3; // clip ID
    std::string t1, t2;     // transition ID
    std::string savedBytes; // 保存した file
};

bool authorThroughProductUi(const std::filesystem::path& projectPath,
                            const std::filesystem::path& manim, Authored& out) {
    auto fixture = project::createDefaultProject();
    // 一般の前提: timeline の終端 (E3 の終わり) の marker。clip の無い位置へ再生ヘッドを置ける
    // ようにする (数式 clip の追加メニューは再生ヘッドの位置に置く)。変形は書かない。
    fixture.timelineMarkers = {kEnd};
    check(fixture.timelineFpsNum == 60 && fixture.timelineFpsDen == 1 &&
              fixture.outputWidth == kOutputWidth && fixture.outputHeight == kOutputHeight,
          "A: 前提: 既定の Project は 60fps・1920x1080");
    check(fixture.timelineClips.empty() && fixture.timelineTransitions.empty(),
          "A: 前提: fixture に clip・トランジションは無い");
    check(project::saveProjectJson(fixture, projectPath).success, "A: fixture の保存");

    MvmController controller(projectPath, manim, fixture);
    check(controller.holdsProjectLock(), "A: controller が Project lock を持つ");
    ProductWindow ui;
    std::string reason;
    if (!ui.open(controller, reason)) {
        note(reason);
        controller.shutdown();
        return false;
    }
    check(pump([&] { return controller.previewReady(); }, 30000), "A: preview の準備");
    auto& cache = controller.mathRastersForTest();
    QElapsedTimer timer;
    timer.start();
    check(pump([&] { return cache.backendState() != MathRasterCache::BackendState::Checking; },
               180000) &&
              cache.backendState() == MathRasterCache::BackendState::Available,
          "A: 実 Manim の確認が通る: " + cache.backendMessage().toStdString());
    note("A: toolchain " + cache.toolchainText().toStdString() + " (" +
         std::to_string(timer.elapsed()) + " ms)");

    auto* addMenu = ui.window->findChild<QObject*>(QStringLiteral("addMathClipMenuItem"));
    auto* editor = ui.window->findChild<QQuickItem*>(QStringLiteral("mathSourceEditor"));
    if (!addMenu || !editor) {
        check(false, "A: 数式 clip の追加メニューと入力欄が製品の画面にありません");
        controller.shutdown();
        return false;
    }

    // --- 1. 3 本の数式 clip を、追加メニューと入力欄で作る ---
    struct Want {
        qint64 start;
        const std::string* source;
        std::string* id;
    };

    for (const Want want : {Want{kE1Start, &kE1, &out.e1}, Want{kE2Start, &kE2, &out.e2},
                            Want{kE3Start, &kE3, &out.e3}}) {
        check(seekWhenReady(controller, want.start) && controller.playheadFrame() == want.start,
              "A: 再生ヘッドを " + std::to_string(want.start) + " に置く");
        const auto before = controller.projectForTest().timelineClips.size();
        check(QMetaObject::invokeMethod(addMenu, "triggered"), "A: 数式 clip の追加を実行する");
        check(pump([&] { return editor->hasActiveFocus(); }), "A: 追加後に入力欄へ focus が移る");
        const auto id = controller.selectedMathClip().value("clipId").toString().toStdString();
        check(!id.empty() && controller.projectForTest().timelineClips.size() == before + 1,
              "A: 数式 clip が 1 本増える");
        // 入力欄に式を入れて Ctrl+Enter で確定する (製品の確定の経路)。
        editor->setProperty("text", QString::fromStdString(*want.source));
        QTest::keyClick(ui.window, Qt::Key_Return, Qt::ControlModifier);
        settle(300);
        const auto* clip = clipById(controller.projectForTest(), id);
        check(clip && clip->kind == project::TimelineClipKind::Math &&
                  clip->math.source == *want.source,
              "A: 入力欄の Ctrl+Enter で式を確定する: " + *want.source);
        check(clip && clip->track.kind == project::TrackKind::Video && clip->track.index == 0 &&
                  clip->timelineStartFrame == want.start &&
                  clip->sourceOutFrame - clip->sourceInFrame == kClipFrames,
              "A: 数式 clip は V1 の " + std::to_string(want.start) + " に 300 frame で置かれる");
        *want.id = id;
    }
    // 3 本に等しい不透明度 (エフェクトコントロールと同じ controller の操作)。
    for (const auto* id : {&out.e1, &out.e2, &out.e3})
        check(controller.setClipEffectValues(QString::fromStdString(*id),
                                             {{QStringLiteral("opacity"), kOpacityPercent}}, true),
              "A: 不透明度 80% を付ける");

    // --- 3. E1 に Write を付ける (数式の inspector の欄) ---
    check(controller.selectTimelineClip(QString::fromStdString(out.e1), false), "A: E1 を選ぶ");
    settle(300);
    auto* writeToggle = ui.item("mathWriteToggle");
    if (!writeToggle || !writeToggle->isVisible()) {
        check(false, "A: Write の欄が見えません");
    } else {
        ui.click(writeToggle);
        check(pump([&] { return controller.selectedMathClip().value("intro") == "write"; }),
              "A: Write の欄で Write を付ける");
        const auto* e1 = clipById(controller.projectForTest(), out.e1);
        check(e1 && e1->mathAnimation.intro == project::MathIntroKind::Write &&
                  e1->mathAnimation.introFrames == 60,
              "A: 既定の Write は 1 秒 (60 frame)");
        auto* seconds = ui.item("mathWriteSeconds");
        if (!seconds || !seconds->isVisible()) {
            check(false, "A: Write の長さの欄が見えません");
        } else {
            // 欄の数値の箱 (左の見出し 92 px の右) をダブルクリックして入力する。
            const QPoint box =
                seconds
                    ->mapToScene(QPointF(92 + (seconds->width() - 92) / 2, seconds->height() / 2))
                    .toPoint();
            QTest::mouseDClick(ui.window, Qt::LeftButton, {}, box);
            settle(200);
            typeText(ui.window, "1.5");
            QTest::keyClick(ui.window, Qt::Key_Return);
            settle(300);
        }
        e1 = clipById(controller.projectForTest(), out.e1);
        check(e1 && e1->mathAnimation.introFrames == kWriteFrames,
              "A: Write の長さ欄で 1.5 秒 (90 frame) にする");
    }

    // --- 4・5. 編集点から変形を作る ---
    auto* create = ui.item("mathTransformCreateButton");
    auto* alignment = ui.item("transitionAlignmentBox");
    auto* duration = ui.item("transitionDurationField");
    auto* inspector = ui.item("transitionInspector");
    auto* endpoints = ui.item("mathTransformEndpoints");
    if (!create || !alignment || !duration || !inspector || !endpoints) {
        check(false, "A: 変形の作成・長さの UI が製品の画面にありません");
        controller.shutdown();
        return false;
    }
    const auto createAt = [&](const std::string& outgoing, const std::string& incoming,
                              std::string& id) {
        check(controller.selectEditPoint(QString::fromStdString(outgoing), QStringLiteral("right")),
              "A: 編集点を選ぶ");
        settle(200);
        check(create->isVisible() && create->isEnabled(),
              "A: 数式 clip の編集点で「数式の変形を適用」を押せる");
        const auto undo = controller.undoDepthForTest();
        ui.click(create);
        const auto* placed = transitionBetween(controller.projectForTest(), outgoing, incoming);
        check(placed && placed->kind == project::TransitionKind::MathTransform &&
                  placed->framesBeforeCut == 0 && placed->framesAfterCut == 60 &&
                  controller.undoDepthForTest() == undo + 1,
              "A: ボタンで MathTransform (cut で開始 0 / 60) を Undo 1 回分で作る");
        id = placed ? placed->id : std::string{};
        check(!id.empty() && controller.selectedTransitionId().toStdString() == id,
              "A: 作った変形を選ぶ");
        settle(200);
    };
    const auto chooseCenter = [&] {
        // 配置の一覧の「中央」(index 0) を選ぶ (ComboBox の activated と同じ)。
        check(QMetaObject::invokeMethod(alignment, "activated", Q_ARG(int, 0)),
              "A: 配置に「中央」を選ぶ");
        settle(300);
    };
    createAt(out.e1, out.e2, out.t1);
    chooseCenter();
    {
        const auto* t1 = transitionBetween(controller.projectForTest(), out.e1, out.e2);
        check(t1 && t1->id == out.t1 && t1->kind == project::TransitionKind::MathTransform &&
                  t1->framesBeforeCut == kT1Before && t1->framesAfterCut == kT1After,
              "A: inspector の配置で E1→E2 を中央 (30 / 30) にする");
    }
    createAt(out.e2, out.e3, out.t2);
    QTest::mouseClick(
        ui.window, Qt::LeftButton, {},
        duration->mapToScene(QPointF(duration->width() / 2, duration->height() - 8)).toPoint());
    settle(200);
    typeText(ui.window, "0.5");
    QTest::keyClick(ui.window, Qt::Key_Return);
    settle(300);
    {
        const auto* t2 = transitionBetween(controller.projectForTest(), out.e2, out.e3);
        check(t2 && t2->id == out.t2 && t2->framesBeforeCut == 0 && t2->framesAfterCut == 30,
              "A: inspector の長さ欄で E2→E3 を 0.5 秒 (0 / 30) にする");
    }
    chooseCenter();
    {
        const auto* t2 = transitionBetween(controller.projectForTest(), out.e2, out.e3);
        check(t2 && t2->id == out.t2 && t2->kind == project::TransitionKind::MathTransform &&
                  t2->framesBeforeCut == kT2Before && t2->framesAfterCut == kT2After,
              "A: inspector の配置で E2→E3 を中央 (15 / 15) にする");
    }
    check(controller.projectForTest().timelineTransitions.size() == 2,
          "A: トランジションは 2 つだけ");

    // --- 7. timeline の帯と inspector の題 ---
    for (const auto& [id, from, to] :
         {std::tuple{out.t1, out.e1, out.e2}, std::tuple{out.t2, out.e2, out.e3}}) {
        check(controller.selectTransition(QString::fromStdString(id)), "A: 変形を選び直す");
        settle(300);
        auto* band = ui.item(("timelineTransition_" + id).c_str());
        auto* label = band ? band->findChild<QQuickItem*>(QStringLiteral("timelineTransitionLabel"))
                           : nullptr;
        auto* title = inspector->childItems().value(0);
        check(label && textOf(label) == QStringLiteral("数式の変形"),
              "A: timeline の帯は「数式の変形」: " + id);
        check(inspector->isVisible() && textOf(title) == QStringLiteral("数式の変形") &&
                  controller.selectedTransition().value("kind") == "math_transform",
              "A: inspector の題は「数式の変形」: " + id);
        const auto* source = clipById(controller.projectForTest(), from);
        const auto* target = clipById(controller.projectForTest(), to);
        const auto text = textOf(endpoints);
        check(source && target && endpoints->isVisible() &&
                  text.indexOf(QString::fromStdString(source->name)) >= 0 &&
                  text.indexOf(QString::fromStdString(target->name)) >
                      text.indexOf(QString::fromStdString(source->name)),
              "A: inspector の向きは 変形前 → 変形後: " + text.toStdString());
    }
    check(project::validateTimeline(controller.projectForTest()).success,
          "A: 作った Project は timeline の検証を通る");

    // --- 保存 (製品の保存) ---
    check(controller.dirty(), "A: 保存前は dirty");
    check(controller.saveProject() && !controller.dirty(), "A: saveProject で保存する");
    out.project = controller.projectForTest();
    out.savedBytes = readBytes(projectPath);
    check(!out.savedBytes.empty(), "A: 保存した file を読める");
    controller.shutdown();
    return true;
}

// 保存した JSON に派生の値 (照合・分け方・backend・cache) が無いこと。見つけた問題を返す
// (検査そのものの負の対照を同じ関数で行うため、check を直接呼ばない)。
std::vector<std::string> savedJsonFindings(const std::string& bytes, const Authored& authored) {
    std::vector<std::string> findings;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok)
            findings.push_back(what);
    };
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    const auto root = document.object();
    expect(root.value("schema_version").toInt() == project::kProjectSchemaVersion,
           "B: 保存した file は現行 schema");
    const QSet<QString> transitionKeys = {
        "id",  "outgoing_clip_id", "incoming_clip_id", "frames_before_cut", "frames_after_cut",
        "kind"};
    int mathTransforms = 0;
    QJsonArray transitions;
    for (const auto& name : root.keys())
        if (root.value(name).isArray() && name.contains("transition"))
            transitions = root.value(name).toArray();
    for (const auto& value : transitions) {
        const auto object = value.toObject();
        QSet<QString> keys;
        for (const auto& key : object.keys())
            keys.insert(key);
        if (keys != transitionKeys)
            note("transition の field: " + QStringList(keys.values()).join(",").toStdString());
        expect(keys == transitionKeys, "B: transition の field は ID・両端・前後・種類だけ");
        mathTransforms += object.value("kind").toString() == "math_transform";
    }
    expect(transitions.size() == 2 && mathTransforms == 2,
           "B: 保存した file のトランジションは 2 つとも math_transform");
    // 数式 clip の "math" と "math_animation" は P0 / P1 の field だけ。
    const QSet<QString> mathKeys = {"syntax", "source", "font_size", "color", "background_color"};
    const QSet<QString> animationKeys = {"intro", "intro_frames"};
    int mathClips = 0;
    QString derivedScope; // 数式 clip とトランジションの JSON (派生の値を探す範囲)
    for (const auto& value : root.value("timeline_clips").toArray()) {
        const auto clip = value.toObject();
        if (clip.value("kind").toString() != "math")
            continue;
        ++mathClips;
        QSet<QString> keys;
        for (const auto& key : clip.value("math").toObject().keys())
            keys.insert(key);
        expect(keys == mathKeys, "B: 数式 clip の math は syntax・source・font_size・色だけ");
        if (clip.contains("math_animation")) {
            QSet<QString> animation;
            for (const auto& key : clip.value("math_animation").toObject().keys())
                animation.insert(key);
            expect(animation == animationKeys, "B: math_animation は intro・intro_frames だけ");
        }
        derivedScope += QString::fromUtf8(QJsonDocument(clip).toJson());
    }
    for (const auto& value : transitions)
        derivedScope += QString::fromUtf8(QJsonDocument(value.toObject()).toJson());
    expect(mathClips == 3, "B: 保存した file の数式 clip は 3 本");
    // 既存の Manim script clip の一覧 (escape hatch、P0 以前からの field) は空のまま。
    expect(root.value("manim_assets").isArray() && root.value("manim_assets").toArray().isEmpty(),
           "B: Manim script clip の一覧 (manim_assets) は空");
    const std::string lower = derivedScope.toLower().toStdString();
    for (const char* token : {"segment", "match", "backend", "cache", "toolchain", "manim",
                              "provenance", "artifact", "sha256", ".a8", "transform/"})
        expect(lower.find(token) == std::string::npos,
               std::string("B: 数式 clip・トランジションの JSON に派生の値 (") + token +
                   ") が無い");
    expect(bytes.find(authored.t1) != std::string::npos &&
               bytes.find(authored.t2) != std::string::npos,
           "B: 保存した file に両方の変形の ID がある");
    return findings;
}

} // namespace

int runMathP28Acceptance(const std::filesystem::path& manim, const std::filesystem::path& work) {
    std::error_code error;
    if (std::filesystem::exists(work, error)) {
        std::fprintf(stderr, "作業 directory が既にあります (上書き・削除しません): %s\n",
                     work.string().c_str());
        return 2;
    }
    if (!std::filesystem::is_regular_file(manim, error)) {
        std::fprintf(stderr, "Manim の実行ファイルがありません: %s\n", manim.string().c_str());
        return 2;
    }
    if (!std::filesystem::is_regular_file(kFfmpeg, error) ||
        !std::filesystem::is_regular_file(kFfprobe, error)) {
        std::fprintf(stderr, "UCRT64 の ffmpeg / ffprobe がありません\n");
        return 2;
    }
    std::filesystem::create_directories(work, error);
    if (error) {
        std::fprintf(stderr, "作業 directory を作れません\n");
        return 2;
    }
    const auto projectPath = work / L"quadratic-p28.mvm";
    note("P2-8: 作業 directory " + work.string());

    // ---- A ----
    Authored authored;
    if (!authorThroughProductUi(projectPath, manim, authored)) {
        std::fprintf(stderr, "%d 検査中 %d 件失敗 (A で中断)\n", checks, failures);
        return failures ? 1 : 4;
    }
    // 作成時の cache を退避して保持する。開き直した Project から実 Manim で描かせるため。
    std::filesystem::rename(work / "cache", work / "authoring-cache", error);
    check(!error && !std::filesystem::exists(work / "cache"),
          "A: 作成時の cache を authoring-cache へ退避する (削除しない)");

    // ---- B. 開き直し ----
    {
        const auto findings = savedJsonFindings(authored.savedBytes, authored);
        for (const auto& finding : findings)
            note("FAIL の内容: " + finding);
        check(findings.empty(), "B: 保存した Project JSON に派生の値が無く、形が P2 の契約どおり");
        // 検査の負の対照: 派生の値を足した複写は検出される (空振りで通らない)。
        auto root =
            QJsonDocument::fromJson(QByteArray::fromStdString(authored.savedBytes)).object();
        auto transitions = root.value("timeline_transitions").toArray();
        auto first = transitions.at(0).toObject();
        first.insert("segments", QJsonArray{"x^2", " + "});
        transitions.replace(0, first);
        auto withSegments = root;
        withSegments.insert("timeline_transitions", transitions);
        auto clips = root.value("timeline_clips").toArray();
        auto clip = clips.at(0).toObject();
        auto math = clip.value("math").toObject();
        math.insert("backend", "manim-mathtex");
        clip.insert("math", math);
        clips.replace(0, clip);
        auto withBackend = root;
        withBackend.insert("timeline_clips", clips);
        for (const auto& mutated : {withSegments, withBackend})
            check(
                !savedJsonFindings(QJsonDocument(mutated).toJson().toStdString(), authored).empty(),
                "B: 検査の負の対照: 派生の値を足した JSON を検出する");
    }
    const auto loaded = project::loadProjectJson(projectPath);
    check(loaded.success, "B: 保存した Project を読める: " + loaded.error);
    if (!loaded.success)
        return 1;
    check(loaded.project.schemaVersion == project::kProjectSchemaVersion,
          "B: 読んだ Project は現行 schema");
    check(loaded.project == authored.project,
          "B: 読んだ Project は保存前の Project と等しい (式・Write・変形の ID・前後)");
    {
        const auto& p = loaded.project;
        const auto* e1 = clipById(p, authored.e1);
        const auto* e2 = clipById(p, authored.e2);
        const auto* e3 = clipById(p, authored.e3);
        check(e1 && e2 && e3 && e1->math.source == kE1 && e2->math.source == kE2 &&
                  e3->math.source == kE3,
              "B: E1 / E2 / E3 の式");
        check(e1 && e1->mathAnimation.intro == project::MathIntroKind::Write &&
                  e1->mathAnimation.introFrames == kWriteFrames && e2 &&
                  e2->mathAnimation.intro == project::MathIntroKind::None && e3 &&
                  e3->mathAnimation.intro == project::MathIntroKind::None,
              "B: E1 の Write (90 frame)、E2・E3 は Write 無し");
        const auto* t1 = transitionBetween(p, authored.e1, authored.e2);
        const auto* t2 = transitionBetween(p, authored.e2, authored.e3);
        check(t1 && t1->id == authored.t1 && t1->kind == project::TransitionKind::MathTransform &&
                  t1->framesBeforeCut == kT1Before && t1->framesAfterCut == kT1After,
              "B: E1→E2 は同じ ID の MathTransform (30 / 30)");
        check(t2 && t2->id == authored.t2 && t2->kind == project::TransitionKind::MathTransform &&
                  t2->framesBeforeCut == kT2Before && t2->framesAfterCut == kT2After,
              "B: E2→E3 は同じ ID の MathTransform (15 / 15)");
    }

    MvmController controller(projectPath, manim, loaded.project);
    check(controller.holdsProjectLock(), "B: 開き直した controller が Project lock を持つ");
    check(!controller.dirty(), "B: 開き直した直後は dirty でない");
    ProductWindow ui;
    std::string reason;
    if (!ui.open(controller, reason)) {
        note(reason);
        controller.shutdown();
        return 4;
    }
    check(pump([&] { return controller.previewReady(); }, 30000), "B: preview の準備");
    auto& cache = controller.mathRastersForTest();
    QElapsedTimer timer;
    timer.start();
    check(pump([&] { return cache.backendState() != MathRasterCache::BackendState::Checking; },
               180000) &&
              cache.backendState() == MathRasterCache::BackendState::Available,
          "B: 実 Manim の確認が通る: " + cache.backendMessage().toStdString());
    const auto toolchain = cache.toolchainText().toStdString();
    note("B: toolchain " + toolchain + " (" + std::to_string(timer.elapsed()) + " ms)");
    check(!controller.dirty(), "B: backend の確認の後も dirty でない");
    const QString t1Id = QString::fromStdString(authored.t1);
    const QString t2Id = QString::fromStdString(authored.t2);
    const auto transformState = [&](const QString& id) {
        controller.selectTransition(id);
        return controller.selectedTransition().value("transformState").toString();
    };
    const auto terminal = [](const QString& s) {
        return s == "ready" || s == "error" || s == "unavailable";
    };
    timer.restart();
    check(pump(
              [&] {
                  for (const auto* id : {&authored.e1, &authored.e2, &authored.e3})
                      if (!terminal(controller.mathClipData(QString::fromStdString(*id))
                                        .value("state")
                                        .toString()))
                          return false;
                  return terminal(controller.mathClipData(QString::fromStdString(authored.e1))
                                      .value("writeState")
                                      .toString()) &&
                         terminal(transformState(t1Id)) && terminal(transformState(t2Id));
              },
              900000),
          "B: 開き直した Project の静止・Write・変形の描画が終わる");
    note("B: 静止 3・Write 1・変形 2 の描画 " + std::to_string(timer.elapsed()) + " ms");
    for (const auto* id : {&authored.e1, &authored.e2, &authored.e3}) {
        const auto data = controller.mathClipData(QString::fromStdString(*id));
        check(data.value("state") == "ready" && !data.value("showingPrevious").toBool(),
              "B: 静止は ready (前の描画ではない): " +
                  data.value("message").toString().toStdString());
    }
    check(controller.mathClipData(QString::fromStdString(authored.e1)).value("writeState") ==
              "ready",
          "B: E1 の Write は ready");
    for (const auto& id : {t1Id, t2Id}) {
        check(
            transformState(id) == "ready",
            "B: 変形は ready: " +
                controller.selectedTransition().value("transformMessage").toString().toStdString());
        check(controller.selectedTransition().value("kind") == "math_transform",
              "B: 開き直した変形の kind は math_transform");
    }
    check(!controller.dirty() && controller.projectForTest() == loaded.project,
          "B: 描画は Project を変えない (dirty でない)");

    // --- provenance と現在の端点の identity (既存の cache の API) ---
    const auto& current = controller.projectForTest();

    struct TransformCase {
        std::string id;
        std::string outgoing;
        std::string incoming;
        qint64 start;
        qint64 frames;
        qint64 cut;
        mvm::math::MathTransformSpec spec;
        std::optional<mvm::app::MathTransformArtifact> artifact;
        QString key;
    };

    std::vector<TransformCase> transforms = {
        {authored.t1, authored.e1, authored.e2, kT1Start, kT1Frames, kE2Start, {}, {}, {}},
        {authored.t2, authored.e2, authored.e3, kT2Start, kT2Frames, kE3Start, {}, {}, {}},
    };
    const auto transformDirectory = cache.cacheDirectory() / "transform";
    for (auto& t : transforms) {
        const auto* transition = transitionBetween(current, t.outgoing, t.incoming);
        const auto* outgoing = clipById(current, t.outgoing);
        const auto* incoming = clipById(current, t.incoming);
        const auto spec = transition && outgoing && incoming
                              ? mvm::app::mathTransformSpecFor(*transition, *outgoing, *incoming)
                              : std::nullopt;
        check(spec && spec->frames == t.frames && spec->source.source == outgoing->math.source &&
                  spec->target.source == incoming->math.source,
              "B: 変形の spec は今の両端の式と区間の枚数 (" + std::to_string(t.frames) + ")");
        if (!spec)
            continue;
        t.spec = *spec;
        const auto window = mvm::app::mathTransformWindowFor(current, *transition);
        check(window && window->start == t.start && window->frames == t.frames,
              "B: 変形の区間は手で数えた値 " + std::to_string(t.start) + " + " +
                  std::to_string(t.frames));
        t.key = cache.transformKeyFor(*spec);
        t.artifact = cache.readyTransformForExport(*spec);
        check(!t.key.isEmpty() && t.artifact &&
                  t.artifact->frames.size() == static_cast<std::size_t>(t.frames),
              "B: readyTransformForExport が今の端点の artifact (" + std::to_string(t.frames) +
                  " 枚) を返す");
        const auto provenance = transformDirectory / (t.key.toStdString() + ".txt");
        const auto text = readBytes(provenance);
        note("B: " + t.id + " の provenance " + provenance.string() + "\n" + text);
        check(!text.empty() && text.find(t.key.toStdString()) != std::string::npos &&
                  text.find(cache.keyFor(spec->source).toStdString()) != std::string::npos &&
                  text.find(cache.keyFor(spec->target).toStdString()) != std::string::npos,
              "B: provenance は今の変形の key と両端の静止の key を持つ");
        if (std::filesystem::exists(work / "authoring-cache" / "math" / "quadratic-p28.mvm" /
                                    "transform" / (t.key.toStdString() + ".txt")))
            note("B: 作成時の cache にも同じ key の変形がある (key は決定的)");
        if (t.artifact) {
            bool a8 = true;
            for (const auto& frame : t.artifact->frames)
                a8 = a8 && frame.extension() == L".a8" &&
                     std::filesystem::file_size(frame, error) ==
                         static_cast<std::uintmax_t>(t.artifact->width) *
                             static_cast<std::uintmax_t>(t.artifact->height);
            check(a8, "B: 変形の frame は .a8 (幅 x 高さ byte)");
            note("B: " + t.id + " artifact " + std::to_string(t.artifact->width) + "x" +
                 std::to_string(t.artifact->height) + " 端点 (" +
                 std::to_string(t.artifact->sourceX) + "," + std::to_string(t.artifact->sourceY) +
                 ") → (" + std::to_string(t.artifact->targetX) + "," +
                 std::to_string(t.artifact->targetY) + ")");
        }
    }
    {
        const auto* e1 = clipById(current, authored.e1);
        const auto sequence = e1 ? mvm::app::mathSequenceSpecFor(*e1) : std::nullopt;
        const auto ready = sequence ? cache.readySequence(*sequence) : std::nullopt;
        check(sequence && sequence->frames == kWriteFrames && ready &&
                  ready->frames.size() == static_cast<std::size_t>(kWriteFrames),
              "B: E1 の Write の連番 (90 枚) が disk にある");
        for (const auto* id : {&authored.e1, &authored.e2, &authored.e3}) {
            const auto* clip = clipById(current, *id);
            check(clip && cache.readyArtifact(mvm::app::mathRenderSpecFor(clip->math)),
                  "B: 今の式の静止の artifact が disk にある");
        }
    }

    // 共有の時間の関数が手で数えた値と一致する (preview と書き出しが通す関数)。
    {
        using Frames = std::vector<std::pair<qint64, qint64>>;
        const auto* e1 = clipById(current, authored.e1);
        for (const auto& [frame, want] :
             Frames{{0, 0}, {1, 1}, {45, 45}, {89, 89}, {90, -1}, {269, -1}}) {
            const auto shown = e1 ? mvm::app::mathIntroFrameAt(*e1, 60, 1, frame) : std::nullopt;
            check(shown && *shown == want,
                  "B: mathIntroFrameAt(" + std::to_string(frame) + ") = " + std::to_string(want));
        }
        const mvm::app::MathTransformWindow w1{kT1Start, kT1Frames};
        const mvm::app::MathTransformWindow w2{kT2Start, kT2Frames};
        for (const auto& [frame, want] :
             Frames{{269, -1}, {270, 0}, {299, 29}, {300, 30}, {329, 59}, {330, -1}})
            check(mvm::app::mathTransformFrameAt(w1, frame) == want,
                  "B: T1 の mathTransformFrameAt(" + std::to_string(frame) + ")");
        for (const auto& [frame, want] :
             Frames{{584, -1}, {585, 0}, {599, 14}, {600, 15}, {614, 29}, {615, -1}})
            check(mvm::app::mathTransformFrameAt(w2, frame) == want,
                  "B: T2 の mathTransformFrameAt(" + std::to_string(frame) + ")");
    }

    // ---- C. 実 D3D11 preview ----
    auto observation = std::make_shared<Observation>();
    controller.setMathWriteObserverForTest(
        [observation](const std::string&, std::int64_t frame, std::int64_t state) {
            std::lock_guard lock(observation->mutex);
            observation->records.emplace_back("write", frame, state);
        });
    controller.setMathTransformObserverForTest(
        [observation](const std::string& transitionId, const std::string& clipId,
                      std::int64_t frame, std::int64_t shown) {
            std::lock_guard lock(observation->mutex);
            observation->records.emplace_back(transitionId + "/" + clipId, frame, shown);
        });
    const auto sameEffects = [](const mvm::preview::PreviewCompositionLayer& left,
                                const mvm::preview::PreviewCompositionLayer& right) {
        return left.destination == right.destination && left.sourceRect == right.sourceRect &&
               left.opacity == right.opacity && left.effectsEnabled == right.effectsEnabled &&
               left.rotationDegrees == right.rotationDegrees && !left.motion && !right.motion;
    };
    // 普通の静止 (区間の外、Write の後) の画素。
    const auto staticLayer = [&](qint64 frame, const char* what) {
        int layers = 0;
        const auto layer = presentedLayerAt(controller, frame, false, &layers);
        check(layer && layers == 1 &&
                  (!layer->stillAnimation || layer->stillAnimation->stateAt(frame) < 0),
              std::string("C: ") + what + " は普通の静止の layer 1 枚");
        return layer;
    };
    const auto plainA = staticLayer(150, "E1 の中央 (frame 150)");
    const auto plainB = staticLayer(450, "E2 の中央 (frame 450)");
    const auto plainC = staticLayer(750, "E3 の中央 (frame 750)");
    if (!plainA || !plainB || !plainC) {
        controller.shutdown();
        std::fprintf(stderr, "%d 検査中 %d 件失敗 (C で中断)\n", checks, failures);
        return 1;
    }
    const auto staticA = presentedPixels(*plainA, 150);
    const auto staticB = presentedPixels(*plainB, 450);
    const auto staticC = presentedPixels(*plainC, 750);
    check(std::fabs(plainA->opacity - 0.8F) < 1e-6F && sameEffects(*plainA, *plainB) &&
              sameEffects(*plainB, *plainC),
          "C: 3 本の静止の layer は等しい ClipEffects (不透明度 0.8)");

    // 見せた frame の記録 (engine の render thread の評価) が手で数えた値であることを待つ。
    const auto engineEvaluated = [&](const std::string& key, qint64 frame, std::int64_t want) {
        return pump([&] {
            for (const auto& [k, f, shown] : observation->snapshot())
                if (k == key && f == frame && shown == want)
                    return true;
            return false;
        });
    };

    // --- Write ---
    std::map<qint64, std::vector<std::uint8_t>> previewPixels;
    std::map<qint64, float> previewOpacity;
    {
        std::uint64_t previous = 0;
        for (const qint64 frame : {qint64{1}, qint64{10}, qint64{45}, qint64{89}}) {
            int layers = 0;
            const auto layer = presentedLayerAt(controller, frame, true, &layers);
            if (!layer) {
                check(false, "C: Write の frame " + std::to_string(frame) + " を提示する");
                continue;
            }
            check(engineEvaluated("write", frame, frame),
                  "C: engine の Write の frame は timeline の frame (" + std::to_string(frame) +
                      ")");
            const auto pixels = presentedPixels(*layer, frame);
            const auto coverage = alphaSum(pixels);
            note("C: Write frame " + std::to_string(frame) + " の被覆 " + std::to_string(coverage) +
                 " / 静止 " + std::to_string(alphaSum(staticA)));
            check(layers == 1 && sameEffects(*layer, *plainA),
                  "C: Write の layer は 1 枚で、ClipEffects は静止と同じ");
            check(coverage > previous && coverage <= alphaSum(staticA) &&
                      (frame == 89 || pixels != staticA),
                  "C: Write の被覆は進み具合とともに増え、静止を超えない");
            previous = coverage;
            previewPixels.emplace(frame, pixels);
            previewOpacity.emplace(frame, layer->opacity);
        }
    }
    for (const auto& [frame, plain] :
         std::vector<std::pair<qint64, const std::vector<std::uint8_t>*>>{
             {90, &staticA}, {269, &staticA}, {584, &staticB}}) {
        const auto layer = presentedLayerAt(controller, frame, false);
        check(layer && presentedPixels(*layer, frame) == *plain,
              "C: Write の後・変形の直前の frame " + std::to_string(frame) +
                  " は普通の静止と全画素一致");
        if (layer) {
            previewPixels.emplace(frame, presentedPixels(*layer, frame));
            previewOpacity.emplace(frame, layer->opacity);
        }
    }

    // --- 両方の変形 ---
    struct Phase {
        const TransformCase* t;
        const std::vector<std::uint8_t>* staticSource;
        const std::vector<std::uint8_t>* staticTarget;
        const mvm::preview::PreviewCompositionLayer* plainSource;
        const mvm::preview::PreviewCompositionLayer* plainTarget;
    };

    for (const Phase phase : {Phase{&transforms[0], &staticA, &staticB, &*plainA, &*plainB},
                              Phase{&transforms[1], &staticB, &staticC, &*plainB, &*plainC}}) {
        const auto& t = *phase.t;
        const qint64 start = t.start;
        const qint64 last = t.start + t.frames - 1;
        const qint64 beforeCut = t.cut - 1;
        const qint64 midBefore = t.start + (t.cut - t.start) / 2;
        const qint64 midAfter = t.cut + (last - t.cut) / 2;
        const auto source = presentedLayerAt(controller, midBefore, true);
        const auto target = presentedLayerAt(controller, t.cut, true);
        if (!source || !target) {
            check(false, "C: " + t.id + " の cut の前後の layer を提示する");
            continue;
        }
        check(engineEvaluated(t.id + "/" + t.outgoing, midBefore, midBefore - start) &&
                  engineEvaluated(t.id + "/" + t.incoming, t.cut, t.cut - start),
              "C: " + t.id +
                  ": engine の変形の frame は mathTransformFrameAt (cut の前は前の "
                  "layer、後は後ろの layer)");
        check(sameEffects(*source, *phase.plainSource) && sameEffects(*target, *phase.plainTarget),
              "C: " + t.id + ": 変形の layer の ClipEffects は普通の静止と同じ (1 回だけ)");
        // 前・後ろの layer の animation は区間のどの frame でも同じ画素 (A/B の受け渡し)。
        bool handoff = true;
        for (qint64 frame = start; frame <= last; ++frame)
            handoff = handoff && presentedPixels(*source, frame) == presentedPixels(*target, frame);
        check(handoff, "C: " + t.id + ": cut の前後の layer は区間の全 frame で同じ変形を見せる");
        // frame 0 は普通の source の静止と全画素一致。
        const auto layer0 = presentedLayerAt(controller, start, true);
        check(layer0 && engineEvaluated(t.id + "/" + t.outgoing, start, 0) &&
                  presentedPixels(*layer0, start) == *phase.staticSource,
              "C: " + t.id + ": 変形の frame 0 は普通の変形前の静止と全画素一致");
        // 最後の frame と直後の静止。artifact の位置は target の静止が B の静止に重なる位置。
        const auto lastLayer = presentedLayerAt(controller, last, true);
        check(lastLayer && engineEvaluated(t.id + "/" + t.incoming, last, t.frames - 1),
              "C: " + t.id + ": 最後の frame は変形の frame " + std::to_string(t.frames - 1));
        const auto afterLayer = presentedLayerAt(controller, last + 1, false);
        check(
            afterLayer && presentedPixels(*afterLayer, last + 1) == *phase.staticTarget &&
                (!afterLayer->stillAnimation || afterLayer->stillAnimation->stateAt(last + 1) < 0),
            "C: " + t.id + ": 変形の直後は普通の変形後の静止");
        if (t.artifact) {
            const auto sourceEntry = cache.request(t.spec.source);
            const auto targetEntry = cache.request(t.spec.target);
            if (sourceEntry.mask && targetEntry.mask) {
                const int ax = (kOutputWidth - sourceEntry.mask->width) / 2 - t.artifact->sourceX;
                const int ay = (kOutputHeight - sourceEntry.mask->height) / 2 - t.artifact->sourceY;
                const int bx = (kOutputWidth - targetEntry.mask->width) / 2 - t.artifact->targetX;
                const int by = (kOutputHeight - targetEntry.mask->height) / 2 - t.artifact->targetY;
                // 位置は 1 画素以内で動き、進み具合 1/2 以上で target の位置 (2 枚以上なら最後は
                // target)。
                const auto originAt = [&](int from, int to, qint64 index) {
                    return from != to && 2 * index >= t.frames ? to : from;
                };
                note("C: " + t.id + " 端点の左上 (" + std::to_string(ax) + "," +
                     std::to_string(ay) + ") → (" + std::to_string(bx) + "," + std::to_string(by) +
                     ")");
                check(std::abs(ax - bx) <= 1 && std::abs(ay - by) <= 1 &&
                          originAt(ax, bx, t.frames - 1) == bx &&
                          originAt(ay, by, t.frames - 1) == by,
                      "C: " + t.id +
                          ": 最後の frame の artifact は target の静止が B の静止に重なる位置 "
                          "(静止へ段差なく続く)");
            } else {
                check(false, "C: " + t.id + ": 両端の静止の mask");
            }
        }
        if (lastLayer && afterLayer)
            note("C: " + t.id + " 最後の frame と変形後の静止の違う画素 " +
                 std::to_string(differingPixels(presentedPixels(*lastLayer, last),
                                                presentedPixels(*afterLayer, last + 1))) +
                 " (途中の frame なので一致は期待しない、参考)");
        // 不透明度は合成で掛かり、patch の glyph は被覆 255 のまま (artifact に焼き込まない)。
        check(maxAlpha(presentedPixels(*target, midAfter)) == 255,
              "C: " + t.id + ": 変形の画素に不透明度を焼き込まない");
        for (const qint64 frame : {start, midBefore, beforeCut, t.cut, midAfter, last, last + 1}) {
            const bool animated = frame <= last;
            int layers = 0;
            const auto layer = presentedLayerAt(controller, frame, animated, &layers);
            check(layer && layers == 1, "C: " + t.id + ": frame " + std::to_string(frame) +
                                            " の合成は数式の layer 1 枚 (二重に重ねない)");
            if (layer) {
                previewPixels.emplace(frame, presentedPixels(*layer, frame));
                previewOpacity.emplace(frame, layer->opacity);
            }
        }
        controller.selectTransition(QString::fromStdString(t.id));
        check(controller.selectedTransition().value("transformPreview") == "ready",
              "C: " + t.id + ": inspector の preview の状態は ready (最新の変形を使う)");
    }

    // --- 通し再生 (mask は memory にある) ---
    {
        check(presentedLayerAt(controller, 0, true).has_value(), "C: 先頭で Write の mask を読む");
        observation->clear();
        check(controller.playTimeline(), "C: E1 → E2 → E3 を先頭から再生する");
        check(pump([&] { return controller.playheadFrame() >= kT2Start + kT2Frames + 30; }, 60000),
              "C: 再生が 2 つの変形を越える");
        controller.pauseTimeline();
        const auto records = observation->snapshot();
        std::size_t writeShown = 0, transformShown = 0;
        bool authoritative = true;
        std::set<std::string> seen;
        for (const auto& [key, frame, shown] : records) {
            if (key == "write") {
                authoritative = authoritative && shown == expectedWriteFrame(frame);
                writeShown += shown >= 0;
                continue;
            }
            const auto transitionId = key.substr(0, key.find('/'));
            authoritative =
                authoritative && shown == expectedTransformFrame(transitionId, authored.t1, frame);
            if (shown >= 0) {
                ++transformShown;
                seen.insert(key);
            }
        }
        note("C: 通し再生の記録 " + std::to_string(records.size()) + " 件 (Write " +
             std::to_string(writeShown) + "、変形 " + std::to_string(transformShown) + ")");
        check(writeShown > 0 && transformShown > 0 && authoritative,
              "C: 再生中の Write と変形の frame はすべて timeline の frame から決まる");
        check(seen.contains(authored.t1 + "/" + authored.e1) &&
                  seen.contains(authored.t1 + "/" + authored.e2) &&
                  seen.contains(authored.t2 + "/" + authored.e2) &&
                  seen.contains(authored.t2 + "/" + authored.e3),
              "C: 両方の変形を cut の前後の layer で見せる");
    }

    // --- mask が遅れて届く (P1.2 / P2-5 の保留の仕組み) ---
    const auto delayed = [&](qint64 from, qint64 releaseAt, qint64 until, const std::string& key,
                             const std::function<std::int64_t(std::int64_t)>& want,
                             qint64 windowEnd, const char* what) {
        // 上限を設定し直すと memory の mask は cache から外れる (P1.2 と同じ)。読み込みを保留する。
        cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
        cache.holdResidentLoadsForTest(true);
        check(seekWhenReady(controller, from) &&
                  waitPresented(
                      [&] {
                          return controller.previewPresentedLatest() &&
                                 cache.heldResidentLoadCountForTest() >= 1;
                      },
                      30000),
              std::string("C: ") + what + ": mask を memory に届けずに静止を見せる");
        observation->clear();
        const auto rebuilds = controller.playbackRebuildCount();
        check(controller.playTimeline(), std::string("C: ") + what + ": 再生する");
        check(pump([&] { return controller.playheadFrame() >= releaseAt; }, 30000),
              std::string("C: ") + what + ": 区間の途中まで進む");
        bool none = true;
        for (const auto& [k, f, shown] : observation->snapshot())
            none = none && !(k == key && shown >= 0);
        check(none, std::string("C: ") + what + ": mask が届く前は animation を見せない");
        const auto released = controller.playheadFrame();
        cache.holdResidentLoadsForTest(false);
        check(pump([&] { return controller.playheadFrame() >= until; }, 30000),
              std::string("C: ") + what + ": 一時停止せずに再生が進む");
        const bool stillPlaying = controller.playing();
        controller.pauseTimeline();
        std::int64_t first = std::numeric_limits<std::int64_t>::max();
        bool authoritative = true, restarted = false;
        std::size_t shownCount = 0;
        std::set<std::string> layers;
        for (const auto& [k, f, shown] : observation->snapshot()) {
            if (k.rfind(key, 0) != 0)
                continue;
            authoritative = authoritative && shown == want(f);
            if (shown >= 0) {
                ++shownCount;
                first = std::min(first, f);
                restarted = restarted || shown == 0;
                layers.insert(k);
            }
        }
        note(std::string("C: ") + what + ": 届けた位置 " + std::to_string(released) + "、記録 " +
             std::to_string(shownCount) + " 件、最初の frame " +
             (shownCount ? std::to_string(first) : std::string("-")));
        check(stillPlaying && controller.playbackRebuildCount() == rebuilds,
              std::string("C: ") + what + ": 届いても再生を止めず、組み直しもしない");
        check(shownCount > 0 && first >= released - 10 && first < windowEnd,
              std::string("C: ") + what + ": 届いた後の再生中の frame から見せる");
        check(authoritative && !restarted,
              std::string("C: ") + what + ": timeline の frame で見せ、frame 0 からやり直さない");
        return layers;
    };
    delayed(
        0, 30, 100, "write", [](std::int64_t f) { return expectedWriteFrame(f); }, kWriteFrames,
        "Write の遅延");
    {
        const auto layers = delayed(
            200, kT1Start + 10, kE2Start + 20, authored.t1,
            [&](std::int64_t f) { return expectedTransformFrame(authored.t1, authored.t1, f); },
            kT1Start + kT1Frames, "変形の遅延");
        check(layers.contains(authored.t1 + "/" + authored.e2),
              "C: 変形の遅延: cut の後も後ろの layer で同じ変形が続く");
    }

    // ---- D. 書き出し (映像のみ、製品の書き出し) ----
    // preview の mask を OverBudget にして、書き出しが memory に依らないことを示す。
    // P2-5 の native 試験 3 と同じ順: 変形を選び、上限を下げ、区間へ seek して提示を待つ。
    check(controller.selectTransition(t1Id), "D: E1→E2 の変形を選ぶ");
    cache.setResidentMemoryBudget(1);
    const bool overBudgetShown =
        seekWhenReady(controller, kE2Start + 10) &&
        waitPresented(
            [&] {
                return controller.previewPresentedLatest() &&
                       controller.selectedTransition().value("transformPreview") == "memory";
            },
            30000);
    if (!overBudgetShown) {
        const auto residency = cache.transformResidencyOf(transforms[0].spec);
        const auto submitted = stillLayerOf(controller.submittedCompositionForTest());
        note("D: 診断: playhead " + std::to_string(controller.playheadFrame()) + "、提示 " +
             std::to_string(controller.previewPresentedLatest()) + "、transformPreview '" +
             controller.selectedTransition().value("transformPreview").toString().toStdString() +
             "'、選択 " + controller.selectedTransitionId().toStdString() + "、residency " +
             std::to_string(static_cast<int>(residency.state)) + " " +
             residency.message.toStdString() + "、submitted の animation " +
             std::to_string(submitted && submitted->stillAnimation) + "、status " +
             controller.statusText().toStdString());
        diagnosePreview(controller, "D");
    }
    check(overBudgetShown, "D: 変形の preview は OverBudget (memory) を受け入れて cut で見せる");
    {
        const auto submitted = stillLayerOf(controller.submittedCompositionForTest());
        check(submitted && (!submitted->stillAnimation ||
                            submitted->stillAnimation->stateAt(kE2Start + 10) < 0),
              "D: OverBudget の間、提示した合成は変形を付けない (cut の静止)");
    }
    check(cache.transformResidencyOf(transforms[0].spec).state !=
              MathRasterCache::Residency::Resident,
          "D: 変形の mask は memory に無い");
    check(controller.selectedTransition().value("transformState") == "ready",
          "D: disk の変形は ready のまま");
    const auto treeBefore = listTree(cache.cacheDirectory());
    const auto video = work / L"quadratic-p28.mp4";
    timer.restart();
    check(controller.exportTimelineWithQuality(urlOf(video), QStringLiteral("high")),
          "D: 製品の書き出し (高画質) を開始する: " + controller.statusText().toStdString());
    check(waitExport(controller), "D: 書き出しが終わる");
    const auto status = controller.statusText().toStdString();
    note("D: 書き出しの status: " + status + " (" + std::to_string(timer.elapsed()) + " ms)");
    check(status.rfind("書き出しました", 0) == 0 && std::filesystem::is_regular_file(video),
          "D: 書き出しが成功し、MP4 がある");
    const auto treeAfter = listTree(cache.cacheDirectory());
    check(!treeBefore.empty() && treeBefore == treeAfter,
          "D: 書き出しは cache を書き換えない (Manim で描き直さない、" +
              std::to_string(treeBefore.size()) + " file)");
    check(!controller.dirty() && controller.projectForTest() == loaded.project,
          "D: 書き出しは Project を変えない");
    {
        QProcess probe;
        probe.start(QString::fromLatin1(kFfprobe),
                    {"-v", "error", "-count_frames", "-show_entries",
                     "stream=codec_type,codec_name,width,height,nb_read_frames", "-of", "csv=p=0",
                     QString::fromStdWString(video.wstring())});
        probe.waitForFinished(300000);
        const auto streams = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
        note("D: ffprobe: " + streams.toStdString());
        int videoStreams = 0, audioStreams = 0;
        bool frames900 = false;
        for (const auto& line : streams.split('\n')) {
            const auto fields = line.trimmed().split(',');
            if (fields.contains("video")) {
                ++videoStreams;
                frames900 = fields.contains("1920") && fields.contains("1080") &&
                            fields.contains(QString::number(kEnd));
            }
            audioStreams += fields.contains("audio");
        }
        check(videoStreams == 1 && frames900, "D: 映像は 1920x1080 の 900 frame");
        note("D: 音声の stream " + std::to_string(audioStreams) +
             " (Project に音声 clip は無い。音声の内容は P2 の受け入れに含めない)");
    }
    // 復号した frame と製品の preview の合成を比べる。
    std::map<qint64, QByteArray> decoded;
    for (const auto& [frame, expected] : previewPixels) {
        // Write の frame 1 は濃い被覆が P2-6 の非空の下限 (alpha > 128 が 100 画素超) に届かない
        // (初回 run で 61 画素)。下限を緩めず、書き出しの比較は frame 10 以降で行う。
        if (frame == 1) {
            const auto c = compareDecoded(QByteArray(static_cast<qsizetype>(expected.size()), 0),
                                          expected, previewOpacity.at(frame));
            note("D: frame 1 は書き出しの比較から外す (濃い被覆 " + std::to_string(c.visible) +
                 " 画素)");
            check(c.visible <= 100, "D: frame 1 を外す理由は P2-6 の非空の下限だけ");
            continue;
        }
        const auto bytes = decodeFrame(video, frame);
        check(bytes.has_value(), "D: frame " + std::to_string(frame) + " を復号できる");
        if (!bytes)
            continue;
        decoded.emplace(frame, *bytes);
        const double opacity = previewOpacity.at(frame);
        const auto c = compareDecoded(*bytes, expected, opacity);
        const auto twice = compareDecoded(*bytes, expected, opacity * opacity);
        const auto shifted = compareDecoded(*bytes, shiftedRight(expected, 16), opacity);
        note("D: frame " + std::to_string(frame) + " " + describe(c) +
             " / 二重の不透明度 誤差和=" + std::to_string(twice.totalError) +
             " / 16px ずらし 誤差和=" + std::to_string(shifted.totalError));
        check(c.passes(), "D: frame " + std::to_string(frame) + " は製品 preview と一致する");
        check(!shifted.passes() && c.totalError * 2 < shifted.totalError,
              "D: frame " + std::to_string(frame) + " の負の対照: 16px ずらした期待は通らない");
        check(c.totalError * 2 < twice.totalError,
              "D: frame " + std::to_string(frame) +
                  " の負の対照: 不透明度を二重に掛けた期待より近い");
    }
    // cut の前後の frame は、hard cut の対照 (その側の普通の静止) より変形の画素に近い。
    for (const auto& [frame, hardCut] :
         std::vector<std::pair<qint64, const std::vector<std::uint8_t>*>>{{285, &staticA},
                                                                          {299, &staticA},
                                                                          {300, &staticB},
                                                                          {314, &staticB},
                                                                          {592, &staticB},
                                                                          {599, &staticB},
                                                                          {600, &staticC},
                                                                          {607, &staticC}}) {
        if (!decoded.contains(frame) || !previewPixels.contains(frame)) {
            check(false, "D: cut の前後の frame " + std::to_string(frame) + " を比べる");
            continue;
        }
        const double opacity = previewOpacity.at(frame);
        const auto c = compareDecoded(decoded.at(frame), previewPixels.at(frame), opacity);
        const auto cut = compareDecoded(decoded.at(frame), *hardCut, opacity);
        note("D: frame " + std::to_string(frame) +
             " hard cut の対照 誤差和=" + std::to_string(cut.totalError));
        check(c.totalError * 2 < cut.totalError,
              "D: frame " + std::to_string(frame) + " は hard cut の対照より変形に近い");
    }
    // 変形の frame 0 は直前の普通の静止と同じ入力。lossy な符号化なので完全一致は要求しない。
    for (const auto& [a, b] : std::vector<std::pair<qint64, qint64>>{{269, 270}, {584, 585}}) {
        if (decoded.contains(a) && decoded.contains(b)) {
            std::size_t differing = 0;
            for (qsizetype at = 0; at < decoded.at(a).size(); ++at)
                differing += decoded.at(a)[at] != decoded.at(b)[at];
            note("D: 復号した frame " + std::to_string(a) + " と " + std::to_string(b) +
                 " の違う byte " + std::to_string(differing) + " (参考)");
        }
    }
    check(previewPixels.contains(269) && previewPixels.contains(270) &&
              previewPixels.at(269) == previewPixels.at(270) && previewPixels.contains(584) &&
              previewPixels.contains(585) && previewPixels.at(584) == previewPixels.at(585),
          "D: 書き出しの比較の基準: 変形 frame 0 と直前の静止は preview で全画素一致");

    // ---- E. 負例 ----
    const auto savedNow = [&] { return readBytes(projectPath) == authored.savedBytes; };
    const auto stillMathTransforms = [&](const project::Project& p) {
        const auto* a = transitionBetween(p, authored.e1, authored.e2);
        const auto* b = transitionBetween(p, authored.e2, authored.e3);
        return p.timelineTransitions.size() == 2 && a && b &&
               a->kind == project::TransitionKind::MathTransform &&
               b->kind == project::TransitionKind::MathTransform;
    };
    const auto expectRefused = [&](const std::filesystem::path& output, const char* what) {
        const bool started = controller.exportTimelineWithQuality(urlOf(output), "high");
        if (started)
            waitExport(controller);
        const auto message = controller.statusText().toStdString();
        note(std::string("E: ") + what + ": " + (started ? "開始後に" : "開始時に") + " " +
             message);
        check(message.rfind("書き出しました", 0) != 0 && !std::filesystem::exists(output),
              std::string("E: ") + what + ": 書き出しを拒否し、出力を作らない (hard cut しない)");
        check(controller.projectForTest() == loaded.project &&
                  stillMathTransforms(controller.projectForTest()) && savedNow(),
              std::string("E: ") + what + ": Project と保存した file を変えず、Blend にしない");
    };
    if (transforms[0].artifact) {
        const auto frame = transforms[0].artifact->frames[10];
        const auto original = readBytes(frame);
        // 1. frame の中身の破損。
        auto corrupt = original;
        corrupt[corrupt.size() / 2] = static_cast<char>(corrupt[corrupt.size() / 2] ^ 0x5A);
        check(writeBytes(frame, corrupt), "E: 変形の frame 10 を壊す");
        expectRefused(work / "negative-corrupt.mp4", "frame の破損");
        check(writeBytes(frame, original) && readBytes(frame) == original &&
                  cache.readyTransformForExport(transforms[0].spec).has_value(),
              "E: 壊した frame を元に戻すと、同じ artifact を再び検証できる");
        // 2. frame の欠落。
        const auto aside = std::filesystem::path(frame.wstring() + L".aside");
        std::filesystem::rename(frame, aside, error);
        check(!error, "E: 変形の frame 10 を退避する");
        expectRefused(work / "negative-missing.mp4", "frame の欠落");
        std::filesystem::rename(aside, frame, error);
        check(!error && readBytes(frame) == original &&
                  cache.readyTransformForExport(transforms[0].spec).has_value(),
              "E: 退避した frame を戻すと、同じ artifact を再び検証できる");
        check(listTree(cache.cacheDirectory()).size() == treeBefore.size(),
              "E: 破損・欠落の検査は cache の file を消さない・描き直さない");
    }
    // 3. 端点の式の変更 (古い key)。E2 を E1 と同じ式にする (静止は disk にある)。
    {
        const auto undo = controller.undoDepthForTest();
        check(controller.updateMathClip(QString::fromStdString(authored.e2),
                                        {{QStringLiteral("source"), QString::fromStdString(kE1)}}),
              "E: E2 の式を変える");
        const auto& edited = controller.projectForTest();
        const auto* e1 = clipById(edited, authored.e1);
        const auto* e2 = clipById(edited, authored.e2);
        const auto* t1 = transitionBetween(edited, authored.e1, authored.e2);
        const auto staleSpec =
            t1 && e1 && e2 ? mvm::app::mathTransformSpecFor(*t1, *e1, *e2) : std::nullopt;
        check(staleSpec && !cache.transformKeyFor(*staleSpec).isEmpty() &&
                  cache.transformKeyFor(*staleSpec) != transforms[0].key,
              "E: 端点が変わると変形の key が変わる");
        check(std::filesystem::is_regular_file(transformDirectory /
                                               (transforms[0].key.toStdString() + ".txt")) &&
                  transforms[0].artifact &&
                  std::filesystem::is_regular_file(transforms[0].artifact->frames.front()),
              "E: 古い key の変形の provenance と frame は disk に残っている");
        check(staleSpec && !cache.readyTransformForExport(*staleSpec).has_value(),
              "E: 今の端点の変形は書き出しに使える状態でない");
        note(std::string("E: 古い key の readyTransformForExport (参考): ") +
             (cache.readyTransformForExport(transforms[0].spec) ? "あり" : "なし"));
        check(t1 && t1->kind == project::TransitionKind::MathTransform,
              "E: 式を変えても変形は残る (端点は clip を参照する)");
        const auto output = work / "negative-stale.mp4";
        const bool started = controller.exportTimelineWithQuality(urlOf(output), "high");
        if (started)
            waitExport(controller);
        note("E: 古い key: " + controller.statusText().toStdString());
        check(!started && !std::filesystem::exists(output) &&
                  controller.statusText().contains(QStringLiteral("数式の変形")),
              "E: 古い変形を今の端点の書き出しに使わず、開始時に拒否する");
        check(controller.undoLastEdit() && controller.undoDepthForTest() == undo &&
                  controller.projectForTest() == loaded.project && savedNow(),
              "E: Undo で保存した Project に戻り、file は変わらない");
        check(pump([&] {
                  return cache.readyTransformForExport(transforms[0].spec).has_value() &&
                         cache.readyTransformForExport(transforms[1].spec).has_value();
              }),
              "E: 元の端点の変形は disk で検証できる");
    }
    // 4. backend の不在: 同じ Project の複写を、無い Manim で開く。
    {
        const auto directory = work / "negative-backend";
        std::filesystem::create_directories(directory, error);
        const auto copy = directory / L"quadratic-p28.mvm";
        std::filesystem::copy_file(projectPath, copy, error);
        check(!error && readBytes(copy) == authored.savedBytes, "E: 保存した Project を複写する");
        const auto copied = project::loadProjectJson(copy);
        check(copied.success, "E: 複写を読める");
        MvmController offline(copy, directory / "no-manim" / "manim.exe", copied.project);
        check(offline.holdsProjectLock(), "E: 複写の controller は lock を持つ");
        check(pump(
                  [&] {
                      return offline.mathRastersForTest().backendState() ==
                             MathRasterCache::BackendState::Unavailable;
                  },
                  60000),
              "E: Manim が無いと backend は利用不可");
        for (const auto& id : {t1Id, t2Id}) {
            check(offline.selectTransition(id), "E: 利用不可の controller で変形を選ぶ");
            settle(100);
            const auto shown = offline.selectedTransition();
            check(shown.value("kind") == "math_transform" &&
                      shown.value("transformState") == "unavailable" &&
                      shown.value("transformUnavailableReason") == "backend",
                  "E: backend の不在で変形は Project に残り、利用不可 (backend)");
        }
        const auto output = directory / "negative-backend.mp4";
        const bool started = offline.exportTimelineWithQuality(urlOf(output), "high");
        if (started)
            pump([&] { return !offline.exporting(); }, 300000);
        note("E: backend の不在: " + offline.statusText().toStdString());
        check(!started && !std::filesystem::exists(output), "E: backend の不在では書き出さない");
        check(offline.projectForTest() == copied.project && !offline.dirty() &&
                  stillMathTransforms(offline.projectForTest()) &&
                  readBytes(copy) == authored.savedBytes,
              "E: backend の不在は Project・file を変えず、Blend にしない");
        offline.shutdown();
    }
    check(savedNow() && !controller.dirty(), "E: 負例の後も保存した Project は変わらない");

    controller.setMathWriteObserverForTest({});
    controller.setMathTransformObserverForTest({});
    controller.shutdown();
    note("P2-8: toolchain " + toolchain);
    note("P2-8: Project " + projectPath.string());
    note("P2-8: MP4 " + video.string());
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
