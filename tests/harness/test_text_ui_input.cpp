// 横書き文字ツールの直接入力を、実際の Main.qml へキーとマウスの event を送って検査する。
//
// 製品と同じ QML・controller・preview surface を D3D11 の window で起動し、
//   T -> モニターをクリック -> 入力 -> Ctrl+Enter で文字 clip ができる
//   Esc では何も作らない
//   選択ツールでドラッグすると位置が変わり、ドラッグ中だけ UI が文字を重ねる
//   Ctrl+Z で位置が戻る
//   入力欄の編集中は timeline の shortcut (Ctrl+C/V/D, M, I, O) が効かない
// を確かめる。IME の変換確定は OS の入力方式が要るのでここでは扱わない
// (docs/premiere-like-editing.md の手動確認手順を参照)。
#include "app/preview/preview_engine_rhi_item.h"
#include "app/text_raster.h"
#include "focus_release_filter.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media_import.h"
#include "mvm_controller.h"
#include "project/timeline_edit.h"
#include "test_media_fixture.h"
#include "trim_cursor.h"
#include "waveform_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

void pump(int milliseconds = 100) {
    pumpUntil([] { return false; }, milliseconds);
}

// QTest::keyClicks は QWidget 用なので、QWindow へは 1 文字ずつ送る。
void typeText(QWindow* window, const char* text) {
    for (const char* c = text; *c; ++c)
        QTest::keyClick(window, *c);
}

// popup の中身は overlay (window の root item の子) に置かれ、QObject の親子では
// 辿れないことがある。見た目の木を辿って探す。
void collectItems(QQuickItem* item, QList<QQuickItem*>& out) {
    out.push_back(item);
    for (QQuickItem* child : item->childItems())
        collectItems(child, out);
}

QList<QQuickItem*> visualItems(QQuickWindow* window) {
    QList<QQuickItem*> items;
    collectItems(window->contentItem(), items);
    return items;
}

QQuickItem* findVisualItem(QQuickWindow* window, const QString& name) {
    for (QQuickItem* item : visualItems(window))
        if (item->objectName() == name)
            return item;
    return nullptr;
}

int textClipCount(const mvm::app::MvmController& controller) {
    auto* model = controller.timelineModel();
    int count = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        const auto roles = model->roleNames();
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            if (role.value() == "clipKind" && model->data(index, role.key()).toString() == "text")
                ++count;
    }
    return count;
}

// track model の行 index の role 値。video track の目玉や audio の solo の確定を見る。
bool trackRole(QAbstractItemModel* model, int row, const char* roleName) {
    const auto roles = model->roleNames();
    for (auto role = roles.cbegin(); role != roles.cend(); ++role)
        if (role.value() == roleName)
            return model->data(model->index(row, 0), role.key()).toBool();
    return false;
}

QPoint itemCenter(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

qint64 transitionValue(const mvm::app::MvmController& controller, const char* key) {
    return controller.selectedTransition().value(QString::fromLatin1(key)).toLongLong();
}

// トランジションを選んだ直後に呼ぶ。エフェクトコントロールへ切り替わり、長さ欄の入力と
// ミニタイムラインのドラッグで長さが 1 undo ずつ変わり、A のドラッグでリップルトリムし、
// ルーラーを押すとそこへ scrub する。timeline ではトランジションの下側で clip の端を trim でき、
// 離れたトランジションは消える。各編集は undo して、呼ぶ前の状態 (Shift+D で置いた 30 / 30 を
// 選んだ状態) へ戻す。
void testTransitionInspector(QQuickWindow* window, mvm::app::MvmController& controller) {
    auto* inspector = findVisualItem(window, QStringLiteral("transitionInspector"));
    check(window->property("leftPanelTab").toInt() == 0 && inspector && inspector->isVisible(),
          "トランジションを押してもエフェクトコントロールに長さと配置を出しません");
    check(transitionValue(controller, "framesBeforeCut") == 30 &&
              transitionValue(controller, "framesAfterCut") == 30,
          "前提: Shift+D のトランジションが 30 / 30 ではありません");

    // 長さ欄は秒。1 回クリックして 0.5 を入力すると 60fps で 30 frame、配置 (中央) を保って 15 /
    // 15。 単位をフレームへ切り替えて 20 を入力すると 10 / 10。
    auto* durationField = findVisualItem(window, QStringLiteral("transitionDurationField"));
    auto* unitButton = findVisualItem(window, QStringLiteral("transitionDurationUnit"));
    check(durationField && durationField->isVisible() && unitButton,
          "トランジションの長さ欄がありません");
    check(controller.timelineFpsNum() == 60 && controller.timelineFpsDen() == 1,
          "前提: timeline が 60fps ではありません");
    const auto enterDuration = [&](const char* text) {
        QTest::mouseClick(
            window, Qt::LeftButton, {},
            durationField
                ->mapToScene(QPointF(durationField->width() / 2, durationField->height() - 8))
                .toPoint());
        pump();
        typeText(window, text);
        QTest::keyClick(window, Qt::Key_Return);
        pump(300);
    };
    if (durationField && unitButton) {
        enterDuration("0.5");
        check(transitionValue(controller, "framesBeforeCut") == 15 &&
                  transitionValue(controller, "framesAfterCut") == 15,
              "長さ欄に秒で入力してもトランジションを中央のまま 0.5 秒にしません");
        controller.undoLastEdit();
        pump(300);
        QTest::mouseClick(
            window, Qt::LeftButton, {},
            unitButton->mapToScene(QPointF(unitButton->width() / 2, unitButton->height() / 2))
                .toPoint());
        pump();
        enterDuration("20");
        check(transitionValue(controller, "framesBeforeCut") == 10 &&
                  transitionValue(controller, "framesAfterCut") == 10,
              "単位をフレームにして入力してもトランジションを 20 frame にしません");
        controller.undoLastEdit();
        pump(300);
        // 秒へ戻す (後の試験と製品の既定に合わせる)。
        QTest::mouseClick(
            window, Qt::LeftButton, {},
            unitButton->mapToScene(QPointF(unitButton->width() / 2, unitButton->height() / 2))
                .toPoint());
        pump();
    }

    // ルーラーには目盛りと timecode を書く。表示範囲は cut の前後 60 frame で、cut (60)
    // は文字の目盛り。
    auto* ruler = findVisualItem(window, QStringLiteral("transitionMiniRuler"));
    bool cutLabelled = false;
    if (ruler) {
        QList<QQuickItem*> rulerItems;
        collectItems(ruler, rulerItems);
        const QString cutText = controller.frameTimecode(transitionValue(controller, "cut"));
        for (QQuickItem* item : rulerItems)
            cutLabelled =
                cutLabelled || (item->isVisible() && item->property("text").toString() == cutText);
    }
    check(cutLabelled, "ミニタイムラインのルーラーに cut の timecode を書きません");

    // ミニタイムラインのトランジションの右端を右へドラッグすると、cut の後ろだけが延びる。
    auto* bar = findVisualItem(window, QStringLiteral("transitionMiniBar"));
    check(bar && bar->isVisible() && bar->width() > 4,
          "ミニタイムラインにトランジションを描きません");
    if (bar) {
        const QPoint grab = bar->mapToScene(QPointF(bar->width() - 1, bar->height() / 2)).toPoint();
        const QPoint delta(20, 0);
        QTest::mousePress(window, Qt::LeftButton, {}, grab);
        for (int step = 1; step <= 4; ++step)
            QTest::mouseMove(window, grab + delta * step / 4);
        QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
        pump(300);
        check(transitionValue(controller, "framesBeforeCut") == 30 &&
                  transitionValue(controller, "framesAfterCut") > 30,
              "ミニタイムラインの右端のドラッグで cut の後ろだけを延ばしません");
        check(controller.undoLastEdit(), "右端のドラッグを Undo できません");
        pump(300);
        check(transitionValue(controller, "framesBeforeCut") == 30 &&
                  transitionValue(controller, "framesAfterCut") == 30,
              "右端のドラッグが 1 回の Undo で戻りません");
    }

    // ミニタイムラインのルーラーで cut の位置を押すと、timeline の再生ヘッドがそこへ動く。
    auto* lane = findVisualItem(window, QStringLiteral("transitionMiniTimeline"));
    auto* playhead = findVisualItem(window, QStringLiteral("transitionMiniPlayhead"));
    check(lane && playhead, "ミニタイムラインの再生ヘッドがありません");
    if (lane && playhead && bar) {
        const qint64 cut = transitionValue(controller, "cut");
        controller.seekTimelineFrame(cut - 20);
        pump(300);
        // cut は表示範囲の中央にある。
        QTest::mouseClick(window, Qt::LeftButton, {},
                          lane->mapToScene(QPointF(lane->width() / 2, 8)).toPoint());
        pump(300);
        check(std::llabs(controller.playheadFrame() - cut) <= 1 && playhead->isVisible() &&
                  std::abs(playhead->x() - lane->width() / 2) <= 2,
              "ミニタイムラインを押しても再生ヘッドがそこへ動きません");
    }

    // ミニタイムラインの A を左へドラッグすると A の終端のリップルトリム。cut が手前へ来て、
    // トランジションは残る (選んだまま)。
    auto* clipA = findVisualItem(window, QStringLiteral("transitionMiniClipA"));
    check(clipA && clipA->isVisible(), "ミニタイムラインに A の行がありません");
    if (clipA) {
        const qint64 cutBefore = transitionValue(controller, "cut");
        const QPoint grab =
            clipA->mapToScene(QPointF(clipA->width() / 4, clipA->height() / 2)).toPoint();
        const QPoint delta(-20, 0);
        QTest::mousePress(window, Qt::LeftButton, {}, grab);
        for (int step = 1; step <= 4; ++step)
            QTest::mouseMove(window, grab + delta * step / 4);
        QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
        pump(300);
        check(!controller.selectedTransitionId().isEmpty() &&
                  transitionValue(controller, "cut") < cutBefore &&
                  transitionValue(controller, "incomingStart") ==
                      transitionValue(controller, "cut"),
              "ミニタイムラインの A のドラッグでリップルトリムしません");
        check(controller.undoLastEdit(), "A のドラッグを Undo できません");
        pump(300);
        check(transitionValue(controller, "cut") == cutBefore,
              "A のドラッグが 1 回の Undo で戻りません");
    }

    // ミニタイムラインの B を右へドラッグすると、離す前から B の先頭 (とトランジション)
    // が付いてくる。 離すと B の先頭のリップルトリムで、cut は動かず B の終端が手前へ来る。
    auto* clipB = findVisualItem(window, QStringLiteral("transitionMiniClipB"));
    check(clipB && clipB->isVisible() && bar, "ミニタイムラインに B の行がありません");
    if (clipB && bar) {
        const qint64 cutBefore = transitionValue(controller, "cut");
        const qint64 endBefore = transitionValue(controller, "incomingEnd");
        const qreal barXBefore = bar->x();
        const QPoint grab =
            clipB->mapToScene(QPointF(clipB->width() * 3 / 4, clipB->height() / 2)).toPoint();
        const QPoint delta(20, 0);
        QTest::mousePress(window, Qt::LeftButton, {}, grab);
        for (int step = 1; step <= 4; ++step)
            QTest::mouseMove(window, grab + delta * step / 4);
        pump();
        check(bar->x() > barXBefore + 5, "ミニタイムラインの B のドラッグ中に表示が動きません");
        QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
        pump(300);
        check(transitionValue(controller, "cut") == cutBefore &&
                  transitionValue(controller, "incomingEnd") < endBefore &&
                  !controller.selectedTransitionId().isEmpty(),
              "ミニタイムラインの B のドラッグで B の先頭をリップルトリムしません");
        check(controller.undoLastEdit(), "B のドラッグを Undo できません");
        pump(300);
        check(transitionValue(controller, "incomingEnd") == endBefore,
              "B のドラッグが 1 回の Undo で戻りません");
    }

    // timeline のトランジションは clip の縦中央の低い帯で、下側では cut の端を掴める。
    // cut の端は incoming の先頭 (outgoing の上に重なる)。右へ trim すると接しなくなり、
    // トランジションは消える。
    const QString transitionId = controller.selectedTransitionId();
    auto* drawn = findVisualItem(window, QStringLiteral("timelineTransition_") + transitionId);
    auto* outgoing = findVisualItem(window, QStringLiteral("timelineClip_video"));
    check(drawn && outgoing && drawn->height() < outgoing->height() * 0.7,
          "timeline のトランジションが track の高さを覆っています");
    if (drawn && outgoing) {
        const qreal drawnCenter = drawn->mapToScene(QPointF(0, drawn->height() / 2)).y();
        const qreal clipCenter = outgoing->mapToScene(QPointF(0, outgoing->height() / 2)).y();
        check(std::abs(drawnCenter - clipCenter) <= 1.5,
              "timeline のトランジションが clip の縦中央にありません");
    }
    if (drawn && outgoing) {
        // 再生ヘッド (上のルーラーで cut へ動かした) が端の上に重ならないよう離す。
        controller.seekTimelineFrame(0);
        pump(300);
        const QPoint grab =
            outgoing->mapToScene(QPointF(outgoing->width() - 2, outgoing->height() - 5)).toPoint();
        const QPoint delta(20, 0);
        QTest::mousePress(window, Qt::LeftButton, {}, grab);
        for (int step = 1; step <= 4; ++step)
            QTest::mouseMove(window, grab + delta * step / 4);
        QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
        pump(300);
        check(
            controller.timelineTransitions().isEmpty() &&
                controller.selectedTransitionId().isEmpty(),
            "トランジションの下側で clip の端を trim できないか、離れたトランジションが残りました");
        controller.undoLastEdit();
        pump(300);
        check(controller.timelineTransitions().size() == 1 &&
                  controller.selectTransition(transitionId),
              "trim の Undo でトランジションが戻りません");
        pump();
    }
}

} // namespace

int countClipDelegates(QQuickWindow* window) {
    int count = 0;
    for (QQuickItem* item : visualItems(window))
        if (item->objectName().startsWith(QStringLiteral("timelineClip_")))
            ++count;
    return count;
}

// clip が多い timeline でも、clip の delegate は表示範囲 (± 余白) の分しか作らない。
// 全 clip 分を作ると 10,000 clip で 10,000 個の delegate (操作・波形・メニュー付き) になる。
// 編集しても delegate を作り直さず、スクロール先の clip は作る。
int checkLargeTimelineDelegates(const mvm::project::Project& base,
                                const std::filesystem::path& projectPath) {
    constexpr int kClips = 10000;
    auto project = base;
    const auto source = project.timelineClips.front();
    project.timelineClips.clear();
    for (int index = 0; index < kClips; ++index) {
        auto clip = source;
        clip.id = "many-" + std::to_string(index);
        clip.name = clip.id;
        clip.sourceInFrame = 0;
        clip.sourceOutFrame = 30;
        clip.timelineStartFrame = static_cast<std::int64_t>(index) * 30;
        project.timelineClips.push_back(clip);
    }
    mvm::app::MvmController controller(projectPath, {}, project);
    mvm::app::WaveformCache waveformCache;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
         {QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}});
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    auto* window = engine.rootObjects().isEmpty()
                       ? nullptr
                       : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* flick =
        window ? window->findChild<QQuickItem*>(QStringLiteral("timelineFlick")) : nullptr;
    if (!window || !flick) {
        std::fprintf(stderr, "FAIL: 大きな timeline の window がありません\n");
        controller.shutdown();
        return 3;
    }
    check(QTest::qWaitForWindowExposed(window), "window が表示されません");
    pump(500);
    check(controller.timelineModel()->rowCount() == kClips, "前提: 10,000 clip を読み込めません");
    const int initial = countClipDelegates(window);
    // 表示幅 (最大でも画面幅程度) の 3 倍に掛かる clip の数で収まる。clip は 30 frame で、
    // 最小倍率でも 1 clip は数 px 以上ある。
    check(initial > 0 && initial < 1000, "clip の delegate が表示範囲に比例する数に収まりません");
    check(findVisualItem(window, QStringLiteral("timelineClip_many-0")) != nullptr,
          "表示範囲の先頭の clip に delegate がありません");

    // 編集 (表示範囲の clip の trim) で delegate を作り直さない。
    QQuickItem* before = findVisualItem(window, QStringLiteral("timelineClip_many-1"));
    check(before &&
              controller.trimClip(QStringLiteral("many-1"), QStringLiteral("right"), -5, false),
          "前提: 表示範囲の clip を trim できません");
    pump(200);
    check(findVisualItem(window, QStringLiteral("timelineClip_many-1")) == before,
          "編集で clip の delegate を作り直しました");

    // 末尾へスクロールすると末尾の clip の delegate ができ、先頭の clip の delegate は消える。
    const qreal contentWidth = flick->property("contentWidth").toReal();
    flick->setProperty("contentX", std::max<qreal>(0, contentWidth - flick->width()));
    pump(500);
    const QString last = QStringLiteral("timelineClip_many-%1").arg(kClips - 1);
    check(findVisualItem(window, last) != nullptr, "スクロール先の clip に delegate がありません");
    check(findVisualItem(window, QStringLiteral("timelineClip_many-0")) == nullptr,
          "表示範囲から外れた clip の delegate が残っています");
    check(countClipDelegates(window) < 1000,
          "スクロールした後の delegate が表示範囲に収まりません");
    std::printf("large timeline: clip %d、delegate 初期 %d / スクロール後 %d\n", kClips, initial,
                countClipDelegates(window));
    controller.shutdown();
    return 0;
}

int countTextLayers(QQuickWindow* window) {
    int count = 0;
    for (QQuickItem* item : visualItems(window))
        if (item->objectName().startsWith(QStringLiteral("textLayer_")))
            ++count;
    return count;
}

// 字幕のように短い文字 clip が多くても、preview の文字 layer (preview 全面の delegate) は
// 再生位置に掛かる文字 clip の分しか作らない。全文字 clip 分を作ると 10,000 個になる。
int checkLargeTextOverlayDelegates(const std::filesystem::path& projectPath) {
    constexpr int kTexts = 10000;
    auto project = mvm::project::createDefaultProject();
    for (int index = 0; index < kTexts; ++index) {
        mvm::project::TimelineClip line;
        line.kind = mvm::project::TimelineClipKind::Text;
        line.id = "line-" + std::to_string(index);
        line.name = line.id;
        line.sourceFpsNum = project.timelineFpsNum;
        line.sourceFpsDen = project.timelineFpsDen;
        line.sourceFrameCount = 30;
        line.sourceOutFrame = 30;
        line.timelineStartFrame = static_cast<std::int64_t>(index) * 30;
        line.track = {mvm::project::TrackKind::Video, 0};
        line.text.content = line.id;
        project.timelineClips.push_back(std::move(line));
    }
    mvm::app::MvmController controller(projectPath, {}, project);
    mvm::app::WaveformCache waveformCache;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
         {QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}});
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    auto* window = engine.rootObjects().isEmpty()
                       ? nullptr
                       : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) {
        std::fprintf(stderr, "FAIL: 文字 clip の多い timeline の window がありません\n");
        controller.shutdown();
        return 3;
    }
    check(QTest::qWaitForWindowExposed(window), "window が表示されません");
    pump(500);
    check(controller.timelineModel()->rowCount() == kTexts,
          "前提: 10,000 個の文字 clip を読み込めません");
    const int initial = countTextLayers(window);
    check(initial >= 1 && initial <= 2 &&
              findVisualItem(window, QStringLiteral("textLayer_line-0")) != nullptr,
          "文字 layer の delegate が再生位置に掛かる文字 clip の数に収まりません");
    // 再生位置を動かすと、その位置の文字 clip の layer に入れ替わる。
    const qint64 target = 500 * 30 + 5;
    // 文字だけの Project では preview の seek が受理されなくても、再生位置 (表示) は動く。
    controller.seekTimelineFrame(target);
    const bool sought = pumpUntil([&] { return controller.playheadFrame() == target; }, 5000);
    pump(200);
    check(sought, "前提: 文字 clip の多い timeline で再生位置を動かせません");
    check(countTextLayers(window) <= 2 &&
              findVisualItem(window, QStringLiteral("textLayer_line-500")) != nullptr &&
              findVisualItem(window, QStringLiteral("textLayer_line-0")) == nullptr,
          "再生位置を動かした後の文字 layer が再生位置に掛かる文字 clip に入れ替わりません");
    std::printf("large text overlay: 文字 clip %d、文字 layer 初期 %d / 移動後 %d\n", kTexts,
                initial, countTextLayers(window));
    controller.shutdown();
    return 0;
}

// 製品のミキサーパネルと実際の音声出力を接続して検査する。
int checkAudioMixerPanel(const std::filesystem::path& projectPath) {
    auto project = mvm::project::createDefaultProject();
    project.audioTracks.push_back({"A2", false});
    project.audioTracks.push_back({"A3", false});
    mvm::project::TimelineClip clip;
    clip.id = "mixer-audio";
    clip.name = "音声";
    clip.kind = mvm::project::TimelineClipKind::Audio;
    clip.track = {mvm::project::TrackKind::Audio, 0};
    clip.mediaPath = std::filesystem::path(MVM_TEXT_TEST_VIDEO).parent_path() / "wav_48k.wav";
    clip.sourceFpsNum = 60;
    clip.sourceFrameCount = 300;
    clip.sourceOutFrame = 150;
    project.timelineClips.push_back(clip);
    auto nextClip = clip;
    nextClip.id = "mixer-next-audio";
    nextClip.timelineStartFrame = 150;
    project.timelineClips.push_back(nextClip);
    mvm::test::attachFixtureMedia(project);
    mvm::app::MvmController controller(projectPath, {}, project);
    if (!controller.holdsProjectLock()) {
        std::fprintf(stderr, "失敗: %s\n", controller.statusText().toUtf8().constData());
        controller.shutdown();
        return 3;
    }
    controller.setMasterVolume(0.0);
    mvm::app::WaveformCache cache;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
         {QStringLiteral("waveformCache"), QVariant::fromValue(&cache)}});
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    auto* window = engine.rootObjects().isEmpty()
                       ? nullptr
                       : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) {
        controller.shutdown();
        return 3;
    }
    window->setProperty("leftPanelWidth", 500);
    window->setProperty("leftPanelTab", 2);
    auto* surface =
        window->findChild<mvm::app::PreviewEngineRhiItem*>(QStringLiteral("previewSurface"));
    controller.attachPreview(surface);
    check(QTest::qWaitForWindowExposed(window), "window が表示されません");
    check(pumpUntil([&] { return controller.previewReady(); }, 30000),
          "ミキサー試験のプレビューを準備できません");
    pump(100);
    auto* strip = findVisualItem(window, QStringLiteral("audioMixerTrack0"));
    check(strip && strip->isVisible() &&
              findVisualItem(window, QStringLiteral("audioMixerMaster")) &&
              findVisualItem(window, QStringLiteral("previewMasterMixer")),
          "製品のミキサーと共通マスターを表示できません");
    const auto role = [&](const char* name) {
        auto* model = controller.audioTrackModel();
        const auto names = model->roleNames();
        for (auto i = names.cbegin(); i != names.cend(); ++i)
            if (i.value() == name)
                return model->data(model->index(0, 0), i.key());
        return QVariant{};
    };
    const auto before = controller.undoDepthForTest();
    check(controller.setAudioTrackMix(0, 6, -0.5, false) &&
              controller.undoDepthForTest() == before && role("mixerGainDb").toDouble() == 6,
          "ドラッグ途中で履歴を作るか、表示へ制御値を反映できません");
    controller.cancelAudioTrackMix(0);
    check(role("mixerGainDb").toDouble() == 0, "ドラッグ取消で音量が戻りません");
    check(controller.setAudioTrackMix(0, 6, -0.5) && controller.undoDepthForTest() == before + 1,
          "ドラッグ確定が1回の履歴になりません");
    check(!controller.setAudioTrackMix(0, 16, 0) && !controller.setAudioTrackMix(-1, 0, 0) &&
              role("mixerPan").toDouble() == -0.5,
          "不正な制御値でミキサーを変更しました");
    check(controller.setAudioMixerName(0, QStringLiteral("ナレーション")) &&
              role("mixerName").toString() == QStringLiteral("ナレーション") &&
              role("trackName").toString() == QStringLiteral("A1"),
          "ミキサー名を変えるとタイムライン名も変わります");
    check(controller.setTrackMuted("audio", 0, true) && role("trackMuted").toBool(),
          "Muteがタイムラインと同期しません");
    check(controller.setTrackMuted("audio", 0, false) &&
              controller.setTrackSolo("audio", 0, true) && role("trackSolo").toBool(),
          "Soloがタイムラインと同期しません");
    check(controller.setTrackSolo("audio", 0, false), "Soloを解除できません");
    // パンの右端: マスターは無音でもトラックの右メーターだけが動く。
    check(controller.setAudioTrackMix(0, 0, 1), "右パンを設定できません");
    check(pumpUntil([&] {
              return controller.previewEngineForTest()->status().state ==
                     mvm::preview::PreviewEngineState::ReadyPaused;
          }),
          "ミキサー試験の音声準備が完了しません");
    if (!controller.playTimeline()) {
        std::fprintf(stderr, "失敗: ミキサー再生: %s\n",
                     controller.statusText().toUtf8().constData());
        controller.shutdown();
        return 1;
    }
    check(pumpUntil([&] { return controller.audioTrackMeter(0).value("right").toDouble() > -60; },
                    5000),
          "実際に消費したトラックPCMのメーターが動きません");
    check(controller.audioTrackMeter(0).value("left").toDouble() <= -96 &&
              controller.audioMeterDbLeft() <= -96 && controller.audioMeterDbRight() <= -96,
          "右パンまたはマスター無音がメーターへ反映されません");
    check(controller.setAudioTrackMix(0, 0, -1) && controller.playing(),
          "再生中のパン変更で停止しました");
    check(pumpUntil(
              [&] {
                  return controller.audioTrackMeter(0).value("left").toDouble() > -60 &&
                         controller.audioTrackMeter(0).value("right").toDouble() <= -96;
              },
              3000),
          "既存の再生経路へ左パンを反映できません");
    if (strip)
        check(strip->property("pan").toDouble() == -1,
              "製品パネルのパンがcontrollerと同期しません");
    check(controller.setAudioTrackMix(0, 15, -1), "+15 dBを設定できません");
    check(pumpUntil([&] { return controller.audioTrackMeter(0).value("clipped").toBool(); }, 3000),
          "実音声の0 dB超過を保持しません");
    check(pumpUntil([&] { return controller.preparedPlaybackSourceCountForTest() > 0; }, 2000),
          "次の音声境界のsourceを先読みできません");
    check(pumpUntil([&] { return controller.playheadFrame() >= 140; }, 4000),
          "先読み後に境界直前まで再生できません");
    const auto prepared = controller.preparedPlaybackSourceCountForTest();
    const auto stale = controller.playbackStalePreparationCount();
    check(prepared > 0 && controller.setAudioTrackMix(0, 3, -0.5) &&
              controller.setAudioMixerName(0, QStringLiteral("境界前の名前")) &&
              controller.preparedPlaybackSourceCountForTest() == prepared &&
              controller.playbackStalePreparationCount() == stale,
          "ミキサー値・名前の確定で先読みsourceを破棄しました");
    check(pumpUntil([&] { return controller.playheadFrame() >= 160; }, 3000) &&
              controller.playbackRebuildCount() == 0,
          "ミキサー確定後の境界通過で再生を組み直しました");
    check(controller.setAudioTrackMix(0, 15, -1) &&
              controller.setAudioMixerName(0, QStringLiteral("ナレーション")),
          "境界試験後のミキサー設定を戻せません");
    const auto screenshot = qEnvironmentVariable("MVM_MIXER_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        pump(200);
        check(window->grabWindow().save(screenshot), "ミキサーの表示画像を保存できません");
    }
    check(controller.pauseTimeline(), "ミキサー試験の再生を停止できません");
    controller.clearAudioTrackClip(0);
    check(!controller.audioTrackMeter(0).value("clipped").toBool(),
          "実音声のクリップ表示を解除できません");
    check(controller.saveProject(), "ミキサー設定を保存できません");
    const auto restored = mvm::project::loadProjectJson(projectPath);
    check(restored.success && restored.project.audioTracks[0].mixerName == "ナレーション" &&
              restored.project.audioTracks[0].mixerPan == -1,
          "確定したミキサー設定を読み戻せません");
    check(controller.undoLastEdit() && controller.undoLastEdit() &&
              role("mixerGainDb").toDouble() == 3,
          "ミキサー音量をUndoできません");
    check(controller.redoLastEdit() && role("mixerGainDb").toDouble() == 15,
          "ミキサー音量をRedoできません");
    check(pumpUntil([&] {
              return controller.previewEngineForTest()->status().state ==
                     mvm::preview::PreviewEngineState::ReadyPaused;
          }) &&
              controller.seekTimelineFrame(0) && pumpUntil([&] {
                  return controller.previewEngineForTest()->status().state ==
                         mvm::preview::PreviewEngineState::ReadyPaused;
              }) &&
              controller.shuttleRight() && controller.shuttleRight(),
          "no-op検査の2倍シャトルを開始できません");
    if (controller.shuttleRate() != 2)
        std::fprintf(stderr, "シャトル開始時の状態: %s\n",
                     controller.statusText().toUtf8().constData());
    const auto shuttle = controller.shuttleRate();
    check(shuttle == 2 && controller.setAudioTrackMix(0, 15, -1) &&
              controller.shuttleRate() == shuttle,
          "変更のないミキサー確定でシャトルを停止しました");
    controller.pauseTimeline();
    controller.beginScrub();
    check(controller.scrubAudioSnapshotForTest().sessionVolume == 0,
          "スクラブ開始時のマスター音量が違います");
    controller.setMasterVolume(0.1);
    check(std::abs(controller.scrubAudioSnapshotForTest().sessionVolume - 0.1F) < 1e-6F,
          "スクラブ中のマスター変更がendpointへ反映されません");
    check(controller.setAudioTrackMix(0, 0, 1, false), "スクラブ中のパン変更を受理しません");
    check(pumpUntil(
              [&] {
                  const auto meter = controller.scrubAudioSnapshotForTest();
                  return meter.meterPeakRight > 0.001F && meter.meterPeakLeft < 0.0001F;
              },
              3000),
          "スクラブの実PCMへ変更後の右パンを反映しません");
    controller.cancelAudioTrackMix(0);
    check(pumpUntil(
              [&] {
                  const auto meter = controller.scrubAudioSnapshotForTest();
                  return meter.meterPeakLeft > 0.001F && meter.meterPeakRight < 0.0001F;
              },
              3000),
          "スクラブのPCMへ取り消し後の左パンを反映しません");
    controller.endScrub();
    controller.setMasterVolume(0);
    auto oldBus = controller.audioMixerBusForTest(1);
    oldBus->clipped.store(true);
    oldBus->peakLeft.store(0.7F);
    check(controller.removeTrack("audio", 1) && controller.audioMixerBusForTest(1) != oldBus &&
              !controller.audioTrackMeter(1).value("clipped").toBool() &&
              controller.audioTrackMeter(1).value("left").toDouble() <= -96,
          "トラック削除後に別トラックのclip latch・ピークを引き継ぎました");
    check(controller.undoLastEdit(), "構造変更を取り消せません");
    check(controller.saveProject(), "切替前の変更を保存できません");
    const auto otherProjectPath = projectPath.parent_path() / "other-mixer.mvm";
    std::filesystem::copy_file(projectPath, otherProjectPath);
    oldBus = controller.audioMixerBusForTest(0);
    oldBus->clipped.store(true);
    check(controller.openProject(
              QUrl::fromLocalFile(QString::fromStdWString(otherProjectPath.wstring()))) &&
              controller.audioMixerBusForTest(0) != oldBus &&
              !controller.audioTrackMeter(0).value("clipped").toBool(),
          "同じトラック数のProject切替でclip latchを引き継ぎました");
    controller.shutdown();
    if (!failures)
        std::puts("製品ミキサーの表示・履歴・同期・保存と、実音声の再生中パンを確認しました");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "FAIL: MLT runtime を初期化できません\n");
        return 3;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::fprintf(stderr, "FAIL: 一時 directory を作れません\n");
        return 3;
    }

    // 製品の表示設定を変更せず、検査ごとに独立した設定へ保存する。
    application.setOrganizationName(QStringLiteral("mvm-test"));
    application.setApplicationName(QStringLiteral("project-panel"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());

    if (application.arguments().contains(QStringLiteral("--audio-mixer"))) {
        const int mixerResult =
            checkAudioMixerPanel(directory.filePath(QStringLiteral("mixer.mvm")).toStdWString());
        mvm_mlt_runtime_shutdown();
        return mixerResult;
    }

    // V1 に映像を置く。文字は映像の上 (V2) に置かれ、engine が合成する経路を通る。
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip video;
    video.id = "video";
    video.name = "video";
    video.mediaPath = MVM_TEXT_TEST_VIDEO;
    video.sourceFpsNum = 60;
    video.sourceFpsDen = 1;
    video.sourceFrameCount = 300;
    video.sourceOutFrame = 120;
    project.timelineClips.push_back(video);
    mvm::test::attachFixtureMedia(project);
    // この試験はパネルの素材を置き直す。使用中の素材は時間軸が実物と違うと置けないので、
    // 素材の値は実際のファイルを調べた値にする (id・名前は fixture のまま)。
    {
        const auto probed = mvm::app::probeMediaFile(video.mediaPath);
        if (!probed.success) {
            std::fprintf(stderr, "FAIL: 試験の動画を調べられません: %s\n", probed.error.c_str());
            return 3;
        }
        auto& item = project.mediaItems.front();
        const auto id = item.id;
        const auto name = item.name;
        item = probed.item;
        item.id = id;
        item.name = name;
        item.mediaPath = video.mediaPath;
    }
    const std::filesystem::path projectPath =
        directory.filePath(QStringLiteral("ui.mvm")).toStdWString();

    int exitCode = 0;
    {
        mvm::app::MvmController controller(projectPath, {}, project);
        mvm::app::WaveformCache waveformCache;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
             {QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}});
        engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        auto* window = engine.rootObjects().isEmpty()
                           ? nullptr
                           : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* surface = window ? window->findChild<mvm::app::PreviewEngineRhiItem*>(
                                     QStringLiteral("previewSurface"))
                               : nullptr;
        auto* host =
            window ? window->findChild<QQuickItem*>(QStringLiteral("previewHost")) : nullptr;
        if (!window || !surface || !host) {
            std::fprintf(stderr, "FAIL: window / preview surface / preview host がありません\n");
            controller.shutdown();
            return 3;
        }
        window->installEventFilter(new mvm::app::FocusReleaseFilter(window));
        controller.attachPreview(surface);
        // どの経路で抜けても controller.shutdown() を通す。通さずに破棄すると
        // preview engine の teardown が QML engine の破棄と競合する。
        // key / mouse event は active な window にしか届かない (非 active になると Qt Quick は
        // activeFocusItem を外す)。検査中に他の操作で前面を奪われた run は、製品の失敗と
        // 混ぜずに PROTOCOL_INVALID として終える (AGENTS.md の Interactive measurement protocol)。
        bool activationLost = false;
        QObject::connect(window, &QWindow::activeChanged, window, [&] {
            if (!window->isActive())
                activationLost = true;
        });
        const auto run = [&]() -> int {
            window->requestActivate();
            if (!QTest::qWaitForWindowExposed(window) || !QTest::qWaitForWindowActive(window)) {
                std::fprintf(stderr, "PROTOCOL_INVALID: window が前面になりません。検査中は"
                                     "他の window を操作しないでください\n");
                return 4;
            }
            activationLost = false;
            if (!pumpUntil([&] { return controller.previewReady(); }, 30000)) {
                std::fprintf(stderr, "FAIL: preview が準備できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            // 起動直後は初回 seek の完了待ちで Seeking のことがある。受理されるまで再試行する。
            // seek の要求は毎回 stateChanged を出すので、間隔を空けて再試行する。
            const auto seekAccepted = [&] {
                for (int attempt = 0; attempt < 60; ++attempt) {
                    if (controller.seekTimelineFrame(10))
                        return true;
                    pump(500);
                }
                return false;
            };
            if (!seekAccepted()) {
                std::fprintf(stderr, "FAIL: 映像のある frame へ seek できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            pump(300);
            check(controller.previewVideoAtPlayhead(), "前提: playhead に映像がありません");

            // 0. 枠のドラッグのように effect を続けて変えると、先の変更の seek が終わる前に次が
            //    来る。途中の変更は捨ててよいが、最後の値 (C) は必ず preview に出る。
            {
                const QString videoId = QStringLiteral("video");
                controller.selectTimelineClips({videoId});
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                const QVariantMap a{{QStringLiteral("scaleX"), 90.0},
                                    {QStringLiteral("scaleY"), 90.0}};
                const QVariantMap b{{QStringLiteral("scaleX"), 70.0},
                                    {QStringLiteral("scaleY"), 70.0}};
                const QVariantMap c{{QStringLiteral("scaleX"), 50.0},
                                    {QStringLiteral("scaleY"), 40.0}};
                // event loop を回さずに 3 回続けて渡す (A の seek の途中で B・C が来る)。
                const bool accepted = controller.setClipEffectValues(videoId, a, false) &&
                                      controller.setClipEffectValues(videoId, b, false) &&
                                      controller.setClipEffectValues(videoId, c, false);
                check(accepted && !controller.previewPresentedLatest(),
                      "前提: 連続した effect 変更が seek の途中に重なっていません");
                // C: 幅 50%・高さ 40% を中央に置く。
                const auto showsC = [&] {
                    const auto destination = controller.submittedLayerDestination(videoId);
                    return destination && std::abs(destination->x() - 0.25) < 1e-4 &&
                           std::abs(destination->y() - 0.3) < 1e-4 &&
                           std::abs(destination->width() - 0.5) < 1e-4 &&
                           std::abs(destination->height() - 0.4) < 1e-4;
                };
                pumpUntil([&] { return controller.previewPresentedLatest() && showsC(); }, 10000);
                check(controller.previewPresentedLatest() && showsC(),
                      "seek の途中に重ねた最後の effect が preview に出ません");
                controller.cancelEffectPreview();
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);

                // 再生位置 (10) から離れた frame 200 へ素材を置く。置いた clip が current になり、
                // 再生位置も current も再生位置の clip (V1) へ戻らない。
                const qint64 playheadBefore = controller.playheadFrame();
                const int placedIndex = controller.clipCount();
                controller.addMediaItemsToTimelineAt({QStringLiteral("fixture-media-0")},
                                                     QStringLiteral("video"), 1, 200);
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                check(controller.clipCount() > placedIndex &&
                          controller.playheadFrame() == playheadBefore &&
                          controller.currentClipIndex() == placedIndex,
                      "離れた位置へ置いたclipがcurrentにならない、または再生位置が動きました");
                controller.undoLastEdit();
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);

                // Ctrl+K: 選択が無ければ、seek が選んだ再生ヘッド位置の current clip を切る。
                controller.selectTimelineClips({});
                check(seekAccepted(), "前提: Ctrl+K 試験の seek が受理されません");
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                const int clipsBeforeSplit = controller.clipCount();
                check(controller.currentClipIndex() >= 0,
                      "前提: seek で再生ヘッド位置の clip が current になりません");
                QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
                pumpUntil([&] { return controller.clipCount() == clipsBeforeSplit + 1; }, 10000);
                check(controller.clipCount() == clipsBeforeSplit + 1,
                      "Ctrl+K で選択の無いときに current clip を再生ヘッドで分割しません");
                controller.undoLastEdit();
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);

                // 編集点: cut の端を押すと編集点を選び、Shift+D でクロスディゾルブを置く。
                // トランジションを押して選び、Delete で消す。
                check(controller.splitClipAt(QStringLiteral("video"), 60, false, true),
                      "前提: 編集点の試験で clip を分割できません");
                pump(300);
                auto* leftHalf = findVisualItem(window, QStringLiteral("timelineClip_video"));
                check(leftHalf != nullptr, "前提: timeline の clip がありません");
                if (leftHalf) {
                    QTest::mouseClick(
                        window, Qt::LeftButton, {},
                        leftHalf->mapToScene(QPointF(leftHalf->width() - 3, leftHalf->height() / 2))
                            .toPoint());
                    pump();
                    check(controller.selectedEditPoint()
                                  .value(QStringLiteral("frame"))
                                  .toLongLong() == 60,
                          "clip の端を押しても編集点を選びません");
                    QTest::keyClick(window, Qt::Key_D, Qt::ShiftModifier);
                    pump(300);
                    const auto transitions = controller.timelineTransitions();
                    check(transitions.size() == 1,
                          "編集点で Shift+D を押してもトランジションを置きません");
                    if (transitions.size() == 1) {
                        const QString id = transitions.front()
                                               .toMap()
                                               .value(QStringLiteral("transitionId"))
                                               .toString();
                        auto* drawn =
                            findVisualItem(window, QStringLiteral("timelineTransition_") + id);
                        check(drawn && drawn->isVisible() && drawn->width() > 0,
                              "置いたトランジションを timeline に描きません");
                        controller.selectTimelineClips({});
                        pump();
                        // 選択を外しても delegate は作り直さない (同じ item のまま)。
                        check(findVisualItem(window, QStringLiteral("timelineTransition_") + id) ==
                                  drawn,
                              "clip の選択を変えただけでトランジションの表示を作り直しました");
                        // プロジェクトのタブを開いておき、押したらエフェクトコントロールへ戻ることを見る。
                        window->setProperty("leftPanelTab", 1);
                        if (drawn) {
                            QTest::mouseClick(
                                window, Qt::LeftButton, {},
                                drawn->mapToScene(QPointF(drawn->width() / 2, drawn->height() / 2))
                                    .toPoint());
                            pump();
                        }
                        check(controller.selectedTransitionId() == id,
                              "トランジションを押しても選びません");
                        testTransitionInspector(window, controller);
                        QTest::keyClick(window, Qt::Key_Delete);
                        pump(300);
                        check(controller.timelineTransitions().isEmpty() &&
                                  controller.clipCount() > 1,
                              "選んだトランジションを Delete で消しません (clip を消した)");
                        controller.undoLastEdit(); // Delete
                        controller.undoLastEdit(); // Shift+D
                    }
                }
                controller.undoLastEdit(); // 分割
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);

                // clip 移動の吸着: V2 の [200, 500) を V1 の clip の終端 (120) の数 frame 手前まで
                // ドラッグして離すと、ちょうど 120 から始まる。吸着が無ければ離した位置になる。
                {
                    const int placedRow = controller.clipCount();
                    controller.addMediaItemsToTimelineAt({QStringLiteral("fixture-media-0")},
                                                         QStringLiteral("video"), 1, 200);
                    pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                    pump(300);
                    auto* model = controller.timelineModel();
                    const int startRole = model->roleNames().key("timelineStartFrame", -1);
                    const QString placedId = model->clipIdAt(placedRow);
                    auto* placed =
                        findVisualItem(window, QStringLiteral("timelineClip_") + placedId);
                    const auto startOf = [&] {
                        return model->data(model->index(placedRow, 0), startRole).toLongLong();
                    };
                    check(placed && startRole >= 0 && startOf() == 200,
                          "前提: 吸着の試験の clip を置けません");
                    if (placed && startRole >= 0) {
                        const int durationRole =
                            model->roleNames().key("timelineDurationFrames", -1);
                        const double durationFrames =
                            model->data(model->index(placedRow, 0), durationRole).toDouble();
                        const double pixelsPerFrame =
                            placed->width() / std::max(1.0, durationFrames);
                        // 吸着の距離は 8 px。ちょうどの位置から 6 px 以内で、1 frame 以上ずれた量。
                        const int shortFrames = std::max(1, static_cast<int>(6.0 / pixelsPerFrame));
                        check(shortFrames * pixelsPerFrame <= 8.0,
                              "前提: 拡大率が大きく、吸着の距離の中で frame をずらせません");
                        const QPoint grab =
                            placed->mapToScene(QPointF(30, placed->height() / 2)).toPoint();
                        const QPoint delta(
                            static_cast<int>(std::lround((-80 + shortFrames) * pixelsPerFrame)), 0);
                        QTest::mousePress(window, Qt::LeftButton, {}, grab);
                        for (int step = 1; step <= 8; ++step)
                            QTest::mouseMove(window, grab + delta * step / 8);
                        QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
                        pump(300);
                        std::printf("吸着: %.2f px/frame、%d frame 手前で離して start=%lld\n",
                                    pixelsPerFrame, shortFrames, static_cast<long long>(startOf()));
                        check(startOf() == 120,
                              "clip を別の clip の終端の近くで離しても端に吸着しません");
                    }
                    controller.undoLastEdit(); // 移動
                    controller.undoLastEdit(); // 配置
                    pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                }
            }

            const auto scenePoint = [host](double fx, double fy) {
                return host->mapToScene(QPointF(host->width() * fx, host->height() * fy)).toPoint();
            };
            const int outputWidth = controller.property("outputWidth").toInt();
            const int outputHeight = controller.property("outputHeight").toInt();

            // 1. T で文字ツール、クリック、入力、Ctrl+Enter で確定。
            QTest::keyClick(window, Qt::Key_T);
            pump();
            check(window->property("timelineTool").toString() == QStringLiteral("text"),
                  "T で文字ツールになりません");
            QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.25, 0.25));
            pump();
            check(window->property("textEditing").toBool(), "クリックで文字の入力を始めません");
            typeText(window, "ABC");
            QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
            pumpUntil([&] { return textClipCount(controller) == 1; });
            check(textClipCount(controller) == 1, "Ctrl+Enter で文字 clip ができません");
            const QVariantMap created = controller.selectedTextClip();
            const QString clipId = created.value(QStringLiteral("clipId")).toString();
            check(created.value(QStringLiteral("content")).toString() == QStringLiteral("ABC"),
                  "入力した本文が保存されていません");
            const int createdX = created.value(QStringLiteral("x")).toInt();
            const int createdY = created.value(QStringLiteral("y")).toInt();
            check(std::abs(createdX - outputWidth / 4) <= 2 &&
                      std::abs(createdY - outputHeight / 4) <= 2,
                  "クリックした位置が文字の左上になっていません");
            check(controller.textOverlayClip().isEmpty(),
                  "確定後も文字を engine の合成から外したままです");

            // 2. Esc は破棄する。
            QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.6, 0.7));
            pump();
            check(window->property("textEditing").toBool(), "2 回目のクリックで入力を始めません");
            typeText(window, "XYZ");
            QTest::keyClick(window, Qt::Key_Escape);
            pump(300);
            check(!window->property("textEditing").toBool(), "Esc で入力を終えません");
            check(textClipCount(controller) == 1, "Esc で文字 clip を作りました");

            // 3. 選択ツールで文字をドラッグする。文字の画素を探して掴む。
            //    再生位置は文字 clip の先頭 (10) から離しておき、掴んでも動かないことを見る。
            constexpr qint64 kDragFrame = 40;
            bool movedPlayhead = false;
            for (int attempt = 0; attempt < 60 && !movedPlayhead; ++attempt) {
                movedPlayhead = controller.seekTimelineFrame(kDragFrame);
                if (!movedPlayhead)
                    pump(500);
            }
            check(movedPlayhead, "前提: 再生位置を動かせません");
            pump(300);
            QTest::keyClick(window, Qt::Key_V);
            pump();
            check(window->property("timelineTool").toString() == QStringLiteral("select"),
                  "V で選択ツールになりません");
            // QML と同じ換算 (floor(local * output / host)) で、押す scene 座標と
            // その上下左右 1 画素がすべて文字に当たる点を探す。縁を掴むと丸めで外れる。
            const auto hitsText = [&](QPoint scene) {
                const QPointF local = host->mapFromScene(QPointF(scene));
                const int px =
                    static_cast<int>(std::floor(local.x() * outputWidth / host->width()));
                const int py =
                    static_cast<int>(std::floor(local.y() * outputHeight / host->height()));
                return controller.textClipAt(px, py) == clipId;
            };
            const QPoint origin = scenePoint(static_cast<double>(createdX) / outputWidth,
                                             static_cast<double>(createdY) / outputHeight);
            QPoint grab;
            bool found = false;
            for (int dy = 0; dy < 60 && !found; ++dy)
                for (int dx = 0; dx < 120 && !found; ++dx) {
                    const QPoint candidate = origin + QPoint(dx, dy);
                    if (hitsText(candidate) && hitsText(candidate + QPoint(1, 0)) &&
                        hitsText(candidate - QPoint(1, 0)) && hitsText(candidate + QPoint(0, 1)) &&
                        hitsText(candidate - QPoint(0, 1))) {
                        grab = candidate;
                        found = true;
                    }
                }
            check(found, "掴める文字の画素が見つかりません");
            if (found) {
                const QPoint delta(40, 20);
                QTest::mousePress(window, Qt::LeftButton, {}, grab);
                pump(50);
                check(controller.textOverlayClip() == clipId,
                      "ドラッグ中の文字を engine の合成から外していません");
                for (int step = 1; step <= 4; ++step) {
                    QTest::mouseMove(window, grab + delta * step / 4);
                    pump(30);
                }
                QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
                pump(300);
                check(controller.textOverlayClip().isEmpty(),
                      "ドラッグ後も文字を engine の合成から外したままです");
                check(controller.property("playheadFrame").toLongLong() == kDragFrame,
                      "文字を掴むと再生位置が clip の先頭へ動きました");
                // 離した直後は位置の保存で seek 中である。そこへ合成を出し直して拒否されると、
                // 文字を外したままの composition が残り、文字が消えていた。
                pump(500);
                if (controller.statusText().contains(QStringLiteral("失敗")))
                    std::fprintf(stderr, "status: %s\n",
                                 controller.statusText().toUtf8().constData());
                check(!controller.statusText().contains(QStringLiteral("失敗")),
                      "ドラッグ後の preview 更新が失敗しました (文字が消える)");
                const QVariantMap moved = controller.textClipData(clipId);
                const int expectedX =
                    createdX +
                    static_cast<int>(std::lround(delta.x() * outputWidth / host->width()));
                const int expectedY =
                    createdY +
                    static_cast<int>(std::lround(delta.y() * outputHeight / host->height()));
                const int movedX = moved.value(QStringLiteral("x")).toInt();
                const int movedY = moved.value(QStringLiteral("y")).toInt();
                if (std::abs(movedX - expectedX) > 2 || std::abs(movedY - expectedY) > 2)
                    std::fprintf(stderr, "移動後 %d,%d / 期待 %d,%d / 作成時 %d,%d\n", movedX,
                                 movedY, expectedX, expectedY, createdX, createdY);
                check(std::abs(movedX - expectedX) <= 2 && std::abs(movedY - expectedY) <= 2,
                      "ドラッグした分だけ文字が動いていません");

                // 4. Ctrl+Z で位置が戻る。
                QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
                pumpUntil([&] {
                    return controller.textClipData(clipId).value(QStringLiteral("x")).toInt() ==
                           createdX;
                });
                const QVariantMap undone = controller.textClipData(clipId);
                check(undone.value(QStringLiteral("x")).toInt() == createdX &&
                          undone.value(QStringLiteral("y")).toInt() == createdY,
                      "Ctrl+Z で文字の位置が戻りません");

                // 5. 字形の無い所 (字間や字の内側) でも、文字の範囲の中なら掴める。
                const QRect bounds = controller.textClipBounds(clipId);
                // 期待値は製品の cache を使わず、保存された書式から描き直した画像で決める。
                const QVariantMap data = controller.textClipData(clipId);
                mvm::project::TextClipData text;
                text.content = data.value(QStringLiteral("content")).toString().toStdString();
                text.fontFamily = data.value(QStringLiteral("fontFamily")).toString().toStdString();
                text.fontSize = data.value(QStringLiteral("fontSize")).toInt();
                text.x = data.value(QStringLiteral("x")).toInt();
                text.y = data.value(QStringLiteral("y")).toInt();
                text.color = data.value(QStringLiteral("color")).toString().toStdString();
                text.bold = data.value(QStringLiteral("bold")).toBool();
                text.alignment = data.value(QStringLiteral("alignment")).toString().toStdString();
                text.outlineColor =
                    data.value(QStringLiteral("outlineColor")).toString().toStdString();
                text.outlineWidth = data.value(QStringLiteral("outlineWidth")).toInt();
                text.backgroundColor =
                    data.value(QStringLiteral("backgroundColor")).toString().toStdString();
                QString rasterError;
                const QImage raster =
                    mvm::app::renderTextRaster(text, outputWidth, outputHeight, rasterError);
                check(!raster.isNull(), "前提: 文字画像を描けません");
                QPoint gap;
                bool gapFound = false;
                for (int dy = 0; dy < 60 && !gapFound; ++dy)
                    for (int dx = 0; dx < 200 && !gapFound; ++dx) {
                        const QPoint candidate = origin + QPoint(dx, dy);
                        const QPointF local = host->mapFromScene(QPointF(candidate));
                        const int px =
                            static_cast<int>(std::floor(local.x() * outputWidth / host->width()));
                        const int py =
                            static_cast<int>(std::floor(local.y() * outputHeight / host->height()));
                        const QRect inner = bounds.adjusted(4, 4, -4, -4);
                        if (inner.contains(px, py) && !raster.isNull() &&
                            qAlpha(raster.pixel(px, py)) == 0) {
                            gap = candidate;
                            gapFound = true;
                        }
                    }
                check(gapFound, "前提: 文字の範囲の中に透明な画素が見つかりません");
                if (gapFound) {
                    QTest::mousePress(window, Qt::LeftButton, {}, gap);
                    pump(50);
                    check(controller.textOverlayClip() == clipId,
                          "字形の無い所では文字を掴めません");
                    QTest::mouseRelease(window, Qt::LeftButton, {}, gap);
                    pump(100);
                }
            }

            // 6. inspector の X を左右ドラッグすると、離す前から preview の文字が動き、
            //    Project は離したときにだけ変わる。
            auto* xField = window->findChild<QQuickItem*>(QStringLiteral("textNumberField_x"));
            check(xField != nullptr, "前提: inspector の X 欄がありません");
            if (xField) {
                const QRect before = controller.textClipBounds(clipId);
                const int savedX =
                    controller.textClipData(clipId).value(QStringLiteral("x")).toInt();
                const QPoint start =
                    xField->mapToScene(QPointF(xField->width() * 0.7, xField->height() / 2))
                        .toPoint();
                QTest::mousePress(window, Qt::LeftButton, {}, start);
                for (int step = 1; step <= 5; ++step) {
                    QTest::mouseMove(window, start + QPoint(step * 10, 0));
                    pump(30);
                }
                pump(200);
                const QRect during = controller.textClipBounds(clipId);
                check(during.x() > before.x(), "X のドラッグ中に preview の文字が動きません");
                check(controller.textClipData(clipId).value(QStringLiteral("x")).toInt() == savedX,
                      "X のドラッグ中に Project を書き換えました");
                QTest::mouseRelease(window, Qt::LeftButton, {}, start + QPoint(50, 0));
                pump(300);
                check(controller.textClipData(clipId).value(QStringLiteral("x")).toInt() > savedX,
                      "X のドラッグを離しても Project に保存されません");
            }

            // 8. 定位置ボタン。押すと下 15% の中央へ置き、X / Y
            // で動かした後に押しても同じ所へ戻る。
            auto* centerButton =
                window->findChild<QQuickItem*>(QStringLiteral("textPlaceButton_center"));
            check(centerButton != nullptr, "前提: 定位置ボタンがありません");
            if (centerButton) {
                const QPoint buttonPoint =
                    centerButton
                        ->mapToScene(QPointF(centerButton->width() / 2, centerButton->height() / 2))
                        .toPoint();
                QVariantMap data = controller.textClipData(clipId);
                mvm::project::TextClipData text;
                text.content = data.value(QStringLiteral("content")).toString().toStdString();
                text.fontFamily = data.value(QStringLiteral("fontFamily")).toString().toStdString();
                text.fontSize = data.value(QStringLiteral("fontSize")).toInt();
                text.bold = data.value(QStringLiteral("bold")).toBool();
                text.outlineWidth = data.value(QStringLiteral("outlineWidth")).toInt();
                text.alignment = "center";
                const auto expected =
                    mvm::app::textPresetPlacement(text, outputWidth, outputHeight, "center");
                check(expected.success, "前提: 定位置を求められません");
                for (int round = 0; round < 2; ++round) {
                    QTest::mouseClick(window, Qt::LeftButton, {}, buttonPoint);
                    pump(300);
                    data = controller.textClipData(clipId);
                    check(data.value(QStringLiteral("x")).toInt() == expected.x &&
                              data.value(QStringLiteral("y")).toInt() == expected.y &&
                              data.value(QStringLiteral("alignment")).toString() ==
                                  QStringLiteral("center"),
                          "定位置ボタンで中央下へ置かれません");
                    // 2 回目の前に位置をずらす。押し直すと定位置へ戻ること。
                    controller.updateTextClip(clipId,
                                              {{QStringLiteral("x"), 5}, {QStringLiteral("y"), 7}});
                    pump(300);
                }
            }

            const auto clickItem = [&](QQuickItem* item) {
                QTest::mouseClick(
                    window, Qt::LeftButton, {},
                    item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
            };
            const auto textValue = [&](const char* key) {
                return controller.textClipData(clipId).value(QString::fromLatin1(key)).toString();
            };

            // 9. 色は見本の横に非 modal で開き、選んでいる途中の色を preview へ反映する。
            //    Esc では Project を変えずに戻し、OK で確定する。
            auto* swatch = window->findChild<QQuickItem*>(QStringLiteral("textColorSwatch_color"));
            auto* picker = window->findChild<QObject*>(QStringLiteral("textColorPicker"));
            check(swatch && picker, "前提: 文字色の見本または color picker がありません");
            if (swatch && picker) {
                const QString originalColor = textValue("color");
                clickItem(swatch);
                pump(300);
                check(picker->property("visible").toBool(),
                      "色の見本を押しても color picker が開きません");
                check(!picker->property("modal").toBool() && !picker->property("dim").toBool(),
                      "color picker が背景を暗くしています");
                check(picker->property("x").toReal() >= swatch->width(),
                      "color picker が見本の横に出ていません");
                auto* yellow = findVisualItem(window, QStringLiteral("colorPreset_#FFFFE600"));
                check(yellow != nullptr, "前提: 色の候補がありません");
                if (yellow) {
                    const int serialBefore = controller.textPreviewSerial();
                    clickItem(yellow);
                    pump(300);
                    check(controller.textPreviewSerial() > serialBefore,
                          "選んでいる途中の色を preview へ反映しません");
                    check(textValue("color") == originalColor, "確定前に文字色を保存しました");
                    // 止まらずにドラッグし続けている間も、一定間隔で描き直す (throttle)。
                    // 止まるのを待つ実装では、離すまで 1 回も描き直されない。
                    auto* colorField = findVisualItem(window, QStringLiteral("colorPickerField"));
                    check(colorField != nullptr, "前提: 色の面がありません");
                    if (colorField) {
                        const QPoint from = colorField
                                                ->mapToScene(QPointF(colorField->width() * 0.2,
                                                                     colorField->height() * 0.2))
                                                .toPoint();
                        QTest::mousePress(window, Qt::LeftButton, {}, from);
                        pump(30);
                        const int serialDragStart = controller.textPreviewSerial();
                        // 実際のマウスに近い 10ms 間隔で約 1.2 秒動かし続ける。
                        for (int step = 1; step <= 120; ++step) {
                            QTest::mouseMove(window, from + QPoint(step, step / 2));
                            pump(10);
                        }
                        const int redrawsDuringDrag =
                            controller.textPreviewSerial() - serialDragStart;
                        QTest::mouseRelease(window, Qt::LeftButton, {}, from + QPoint(120, 60));
                        pump(300);
                        if (redrawsDuringDrag < 3)
                            std::fprintf(stderr, "ドラッグ中の描き直し: %d 回\n",
                                         redrawsDuringDrag);
                        check(redrawsDuringDrag >= 3,
                              "色の面をドラッグしている間に preview が描き直されません");
                    }
                    const int serialPicked = controller.textPreviewSerial();
                    QTest::keyClick(window, Qt::Key_Escape);
                    pump(300);
                    check(!picker->property("visible").toBool(),
                          "Esc で color picker が閉じません");
                    check(textValue("color") == originalColor, "Esc で文字色を保存しました");
                    check(controller.textPreviewSerial() > serialPicked,
                          "Esc で preview を元の色へ戻していません");

                    clickItem(swatch);
                    pump(300);
                    clickItem(yellow);
                    pump(200);
                    auto* ok = findVisualItem(window, QStringLiteral("colorPickerOk"));
                    check(ok != nullptr, "前提: OK ボタンがありません");
                    if (ok)
                        clickItem(ok);
                    pump(300);
                    check(textValue("color") == QStringLiteral("#FFFFE600"),
                          "OK で文字色を保存しません");
                }
            }

            // 10. フォント一覧で hover した書体を preview へ仮に反映し、選ばずに閉じれば戻す。
            auto* fontBox = window->findChild<QQuickItem*>(QStringLiteral("textFontBox"));
            check(fontBox != nullptr, "前提: フォントの一覧がありません");
            if (fontBox) {
                const QString originalFont = textValue("fontFamily");
                clickItem(fontBox);
                pump(300);
                QQuickItem* candidate = nullptr;
                for (QQuickItem* item : visualItems(window)) {
                    const QVariant family = item->property("modelData");
                    if (!item->inherits("QQuickItemDelegate") || !item->isVisible() ||
                        !family.isValid() || family.toString() == originalFont ||
                        item->width() <= 0)
                        continue;
                    // ListView は表示範囲の外にも delegate を作る。見えている行だけを使う。
                    QQuickItem* list = item->parentItem();
                    while (list && !list->inherits("QQuickListView"))
                        list = list->parentItem();
                    const QPointF center =
                        item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                    if (list && list->mapRectToScene(QRectF(0, 0, list->width(), list->height()))
                                    .adjusted(0, 4, 0, -4)
                                    .contains(center)) {
                        candidate = item;
                        break;
                    }
                }
                check(candidate != nullptr, "前提: hover できる別の書体が一覧にありません");
                if (candidate) {
                    const int serialBefore = controller.textPreviewSerial();
                    const QPoint over =
                        candidate
                            ->mapToScene(QPointF(candidate->width() / 2, candidate->height() / 2))
                            .toPoint();
                    QTest::mouseMove(window, over - QPoint(0, 1));
                    QTest::mouseMove(window, over);
                    pump(400);
                    check(controller.textPreviewSerial() > serialBefore,
                          "hover した書体を preview へ反映しません");
                    check(textValue("fontFamily") == originalFont,
                          "hover だけで書体を保存しました");
                    const int serialHovered = controller.textPreviewSerial();
                    QTest::keyClick(window, Qt::Key_Escape);
                    pump(300);
                    auto* fontPopup = fontBox->property("popup").value<QObject*>();
                    check(fontPopup && !fontPopup->property("visible").toBool(),
                          "Esc でフォントの一覧が閉じません");
                    check(textValue("fontFamily") == originalFont,
                          "閉じただけで書体を保存しました");
                    check(controller.textPreviewSerial() > serialHovered,
                          "一覧を閉じても preview を元の書体へ戻していません");
                }
            }

            // 7. inspector の本文欄に focus を移しても、preview を押せば単キー操作が戻る。
            auto* contentEditor =
                window->findChild<QQuickItem*>(QStringLiteral("textContentEditor"));
            check(contentEditor && contentEditor->isVisible(),
                  "前提: inspector の本文欄がありません");
            if (contentEditor && contentEditor->isVisible()) {
                const QPoint editorPoint = contentEditor
                                               ->mapToScene(QPointF(contentEditor->width() / 2,
                                                                    contentEditor->height() / 2))
                                               .toPoint();
                QTest::mouseClick(window, Qt::LeftButton, {}, editorPoint);
                pump();
                check(window->property("keyboardFocusTakesKeys").toBool(),
                      "前提: 本文欄を押しても入力 focus になりません");
                QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.9, 0.9));
                pump();
                check(!window->property("keyboardFocusTakesKeys").toBool(),
                      "preview を押しても本文欄が focus を持ち続け、単キー操作が止まったままです");
                // 選択ツールで preview の映像を押すと、その映像 (V1) が選ばれて枠が出る。
                check(controller.transformClipId() == QStringLiteral("video"),
                      "preview で押した映像が選ばれません");
                QTest::keyClick(window, Qt::Key_T);
                pump();
                check(window->property("timelineTool").toString() == QStringLiteral("text"),
                      "本文欄の後で T が効きません");
                // 以降は文字の inspector を使うので、文字 clip を選び直す (再生位置は動かない)。
                controller.selectTimelineClips({clipId});
                pump();

                // 11. 本文欄の入力中は、timeline の shortcut (Ctrl+C/V/X/D, M, I, O) が
                //     Action へ流れず、文字入力側だけが受ける。
                const QString savedContent = textValue("content");
                const int clipsBefore = controller.clipCount();
                const qint64 inBefore = controller.inFrame();
                const qint64 outBefore = controller.outFrame();
                const int markersBefore = static_cast<int>(controller.timelineMarkers().size());
                QTest::mouseClick(window, Qt::LeftButton, {}, editorPoint);
                pump();
                check(window->property("keyboardFocusTakesKeys").toBool(),
                      "前提: 本文欄を押し直しても入力 focus になりません");
                typeText(window, "mio");
                pump();
                const QString typed = contentEditor->property("text").toString();
                check(typed.contains(QStringLiteral("mio")), "M / I / O が本文欄へ入力されません");
                check(static_cast<int>(controller.timelineMarkers().size()) == markersBefore &&
                          controller.inFrame() == inBefore && controller.outFrame() == outBefore,
                      "入力中の M / I / O で timeline のマーカー・イン・アウトが変わりました");
                QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
                pump();
                check(QGuiApplication::clipboard()->text() == typed,
                      "入力中の Ctrl+C が本文をコピーしません");
                QTest::keyClick(window, Qt::Key_D, Qt::ControlModifier);
                QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
                pump(300);
                check(contentEditor->property("text").toString() == typed,
                      "入力中の Ctrl+V が本文へ貼り付けられません");
                check(controller.clipCount() == clipsBefore,
                      "入力中の Ctrl+D / Ctrl+V で timeline の clip が増えました");
                // Esc で入力を取り消す。timeline の clipboard
                // は空のままなので貼り付けは拒否される。
                QTest::keyClick(window, Qt::Key_Escape);
                pump(300);
                check(!window->property("keyboardFocusTakesKeys").toBool() &&
                          textValue("content") == savedContent,
                      "Esc で本文の入力を取り消せません");
                check(!controller.pasteClips() && controller.clipCount() == clipsBefore,
                      "入力中の Ctrl+C が timeline の clip をコピーしました");
            }

            // 12. Delete の宛先は最後に押した場所で決まる。プロジェクトパネルに素材の選択が
            //     残っていても、timeline の clip を押した後の Delete は clip を消す。
            {
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::keyClick(window, Qt::Key_V);
                window->setProperty("leftPanelTab", 1);
                pump(300);
                auto* search = findVisualItem(window, QStringLiteral("mediaBinSearch"));
                auto* splitter = findVisualItem(window, QStringLiteral("projectPanelSplitter"));
                check(search && splitter, "検索欄または幅調整ハンドルがありません");
                if (search && splitter) {
                    QTest::mouseClick(window, Qt::LeftButton, {}, itemCenter(search));
                    typeText(window, "mio");
                    check(controller.mediaBinModel()->filterText() == QStringLiteral("mio") &&
                              controller.mediaBinModel()->rowCount() == 0,
                          "検索欄の入力で素材を絞り込めません");
                    QTest::keyClick(window, Qt::Key_Escape);
                    check(controller.mediaBinModel()->filterText().isEmpty() &&
                              controller.mediaBinModel()->rowCount() > 0,
                          "Esc で検索を解除できません");
                    check(window->property("leftPanelWidth").toReal() == 560,
                          "パネル幅の初期値が広がっていません");
                    const QPoint handle = itemCenter(splitter);
                    QTest::mousePress(window, Qt::LeftButton, {}, handle);
                    QTest::mouseMove(window, handle + QPoint(40, 0));
                    QTest::mouseRelease(window, Qt::LeftButton, {}, handle + QPoint(40, 0));
                    const auto width = window->property("leftPanelWidth").toReal();
                    check(width > 560, "ドラッグでパネル幅を広げられません");
                    check(pumpUntil(
                              [&] {
                                  QSettings saved;
                                  saved.sync();
                                  return saved.value(QStringLiteral("workspace/leftPanelWidth"))
                                             .toReal() == width;
                              },
                              3000),
                          "調整したパネル幅が設定ファイルへ保存されません");
                }
                const QString mediaId = QStringLiteral("fixture-media-0");
                const auto center = [](QQuickItem* item) {
                    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                        .toPoint();
                };
                const auto hasMedia = [&] {
                    return controller.mediaBinModel()->rowOfEntry(mediaId) >= 0;
                };
                auto* panel = findVisualItem(window, QStringLiteral("projectPanel"));
                auto* dialog = window->findChild<QObject*>(QStringLiteral("mediaBinRemoveDialog"));
                const auto dialogOpen = [&] {
                    return dialog && dialog->property("visible").toBool();
                };
                const auto mediaRow = [&] {
                    return findVisualItem(window, QStringLiteral("mediaBinRow_") + mediaId);
                };
                auto* clipItem = findVisualItem(window, QStringLiteral("timelineClip_video"));
                check(panel && mediaRow() && clipItem && dialog,
                      "前提: プロジェクトパネルの素材・timeline の clip・削除確認がありません");
                if (panel && mediaRow() && clipItem && dialog) {
                    // a. 素材 A を選び、timeline の clip B を押して Delete。B だけが消え、
                    //    素材の削除確認は出ない。
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(mediaRow()));
                    pump();
                    check(panel->property("selectedIds").toList().size() == 1,
                          "前提: プロジェクトパネルの素材を選べません");
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(clipItem));
                    pump();
                    const int clipsBefore = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(
                        controller.clipCount() < clipsBefore && !dialogOpen() && hasMedia(),
                        "素材を選んだ後に timeline の clip を押しても Delete が clip を消しません");
                    controller.undoLastEdit();
                    pump(300);

                    // b. プロジェクトパネルへ戻って素材 A を押し、Delete。素材の削除確認が出る
                    //    (使っている clip も消えるため)。閉じれば何も消えない。
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(mediaRow()));
                    pump();
                    const int clipsBeforePanel = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(dialogOpen() && controller.clipCount() == clipsBeforePanel,
                          "プロジェクトパネルの素材を選んで Delete しても削除確認が出ません");
                    QMetaObject::invokeMethod(dialog, "close");
                    pump(300);
                    check(!dialogOpen() && hasMedia() && controller.clipCount() == clipsBeforePanel,
                          "削除確認を閉じても素材か clip が消えました");

                    // c. パネルの空白 (行のすぐ下。一覧の下端は横スクロールバー) を押して素材の
                    //    選択を外し (focus はパネルのまま)、Delete。選択中の timeline の clip
                    //    が消える。
                    controller.selectTimelineClips({QStringLiteral("video")});
                    pump();
                    auto* row = mediaRow();
                    QTest::mouseClick(window, Qt::LeftButton, {},
                                      row->mapToScene(QPointF(10, row->height() + 30)).toPoint());
                    pump();
                    check(panel->property("selectedIds").toList().isEmpty(),
                          "前提: パネルの空白を押しても素材の選択が外れません");
                    const int clipsBeforeEmpty = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(
                        controller.clipCount() < clipsBeforeEmpty && !dialogOpen() && hasMedia(),
                        "パネルで素材を選んでいないときの Delete が timeline の clip を消しません");
                    controller.undoLastEdit();
                    pump(300);
                }
            }
            // 13. clip の端は、クリックせずに hover するだけでカーソルが変わる。右端は内側 <-] /
            //     外側 [->、左端は内側 [-> / 外側 <-] (線の左は <-]、右は
            //     [->)。押している間は範囲の外へ出ても形を保ち、
            //     端から離れれば元に戻る。左に空きのある文字 clip (frame 10 から) で試す。
            {
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::keyClick(window, Qt::Key_V);
                pump(200);
                auto* clipItem = findVisualItem(window, QStringLiteral("timelineClip_") + clipId);
                check(clipItem != nullptr && clipItem->x() > 10,
                      "前提: 左に空きのある timeline の clip がありません");
                if (clipItem) {
                    const QColor red(QStringLiteral("#e8413c"));
                    const auto isCursor = [&](const char* kind) {
                        QCursor expected;
                        const QCursor* shown = QGuiApplication::overrideCursor();
                        return mvm::app::TrimCursor::cursorForKind(QString::fromLatin1(kind), red,
                                                                   expected) &&
                               shown && shown->shape() == Qt::BitmapCursor &&
                               shown->hotSpot() == expected.hotSpot() &&
                               shown->pixmap().toImage() == expected.pixmap().toImage();
                    };
                    // clip の幅は timeline の表示倍率の再計算で後から変わる。位置は毎回、現在の
                    // 幅から決める (fromRight なら右端からの距離)。
                    const auto pointAt = [&](double offset, bool fromRight) {
                        const double localX = fromRight ? clipItem->width() + offset : offset;
                        return clipItem->mapToScene(QPointF(localX, clipItem->height() / 2))
                            .toPoint();
                    };
                    const auto hover = [&](double offset, bool fromRight) {
                        const QPoint point = pointAt(offset, fromRight);
                        QTest::mouseMove(window, point - QPoint(0, 1));
                        QTest::mouseMove(window, point);
                        pump(100);
                    };
                    pump(300);
                    hover(clipItem->width() / 2, false);
                    check(QGuiApplication::overrideCursor() == nullptr,
                          "clip の中央でも端のカーソルが出ています");
                    hover(-3, true);
                    check(isCursor("outInner"), "右端の内側を hover しても <-] になりません");
                    hover(3, true);
                    check(isCursor("outOuter"), "右端の外側を hover しても [-> になりません");
                    hover(3, false);
                    check(isCursor("inInner"), "左端の内側を hover しても [-> になりません");
                    hover(-3, false);
                    check(isCursor("inOuter"), "左端の外側を hover しても <-] になりません");

                    // 押したまま右端から大きく外へ動かしても、押した側の形を保つ。元の位置で離す。
                    const QPoint edgePress = pointAt(-3, true);
                    hover(-3, true);
                    QTest::mousePress(window, Qt::LeftButton, {}, edgePress);
                    pump(50);
                    QTest::mouseMove(window, edgePress + QPoint(60, 0));
                    pump(100);
                    check(isCursor("outInner"), "右端を押したまま動かすと <-] が保たれません");
                    QTest::mouseMove(window, edgePress);
                    pump(50);
                    QTest::mouseRelease(window, Qt::LeftButton, {}, edgePress);
                    pump(200);

                    hover(clipItem->width() / 2, false);
                    check(QGuiApplication::overrideCursor() == nullptr,
                          "端から離れても端のカーソルが残ります");
                }
            }
            // 14. 速度の直接入力は、文字列全体が数値のときだけ確定する。"50foo" は拒否して
            //     入力欄を開いたまま古い値を残す (以前は parseFloat が 50 と読み、読めない
            //     入力でも黙って閉じて古い値で適用していた)。
            {
                QMetaObject::invokeMethod(window, "openSpeedDurationDialog",
                                          Q_ARG(QVariant, QVariant(QStringLiteral("video"))));
                pump(300);
                auto* field = findVisualItem(window, QStringLiteral("speedDurationSpeedField"));
                check(field != nullptr && field->isVisible(), "前提: 速度の入力欄がありません");
                if (field) {
                    QMetaObject::invokeMethod(field, "beginEditing");
                    pump();
                    QQuickItem* editor = nullptr;
                    QList<QQuickItem*> children;
                    collectItems(field, children);
                    for (QQuickItem* child : children)
                        if (child->inherits("QQuickTextField"))
                            editor = child;
                    check(editor && editor->isVisible(), "前提: 速度の直接入力欄が開きません");
                    const auto commit = [&](const char* text) {
                        editor->setProperty("text", QString::fromUtf8(text));
                        QVariant accepted;
                        QMetaObject::invokeMethod(field, "commitEditing",
                                                  Q_RETURN_ARG(QVariant, accepted));
                        pump();
                        return accepted.toBool();
                    };
                    if (editor) {
                        const double before = field->property("value").toDouble();
                        check(!commit("50foo") && editor->isVisible() &&
                                  field->property("value").toDouble() == before,
                              "数値でない速度入力を受理した、または入力欄を閉じました");
                        check(!commit("") && editor->isVisible(), "空の速度入力を受理しました");
                        check(commit("50") && !editor->isVisible() &&
                                  field->property("value").toDouble() == 50.0,
                              "数値の速度入力を確定できません");
                        QMetaObject::invokeMethod(field, "beginEditing");
                        pump();
                        check(commit("75 %") && field->property("value").toDouble() == 75.0,
                              "単位付きの速度入力を確定できません");
                    }
                }
                // 直接入力を確定した後も focus はダイアログの中に残り、Esc で閉じる (以前は
                // window へ focus を返していたので Esc が届かず、ダイアログが開いたまま残った)。
                QTest::keyClick(window, Qt::Key_Escape);
                pumpUntil([&] { return !field || !field->isVisible(); }, 3000);
                check(!field || !field->isVisible(),
                      "速度の直接入力を確定した後に Esc でダイアログが閉じません");
            }
            // 15. トラックヘッダ: video の目玉を押して上の track までドラッグすると、通った
            //     track がすべて非表示になる (Photoshop のレイヤーの目玉)。途中では確定せず見た目
            //     だけ変え、離したときに 1 undo で確定する。audio の S は押すと solo になる。
            {
                // modal の背景が残っていると最初の press がダイアログを閉じるのに使われ、
                // 目玉の検査が空振りする。前の手順のダイアログが閉じていることを前提として見る。
                const auto modalOpen = [&] {
                    for (QQuickItem* item : visualItems(window))
                        if (item->parentItem() && item->parentItem()->inherits("QQuickOverlay") &&
                            item->isVisible() && item->width() >= window->width() &&
                            item->height() >= window->height())
                            return true;
                    return false;
                };
                check(!modalOpen(), "前提: modal のダイアログが閉じません");
                auto* videoTracks = controller.videoTrackModel();
                const int top = controller.videoTrackCount() - 1;
                auto* bottomEye = findVisualItem(window, QStringLiteral("trackEye_video_0"));
                auto* topEye = findVisualItem(window, QStringLiteral("trackEye_video_%1").arg(top));
                auto* topIcon =
                    findVisualItem(window, QStringLiteral("trackEyeIcon_video_%1").arg(top));
                check(top >= 1 && bottomEye && topEye && topIcon && bottomEye->isVisible(),
                      "前提: video track 2 本以上の目玉がありません");
                bool allShown = true;
                for (int index = 0; index <= top; ++index)
                    allShown = allShown && !trackRole(videoTracks, index, "trackMuted");
                check(allShown, "前提: video track が非表示になっています");
                // 目玉は track 名のすぐ右に詰めて置く (track 名と目玉の間を空けない)。
                check(bottomEye && bottomEye->x() < 40, "目玉が track 名のすぐ右にありません");
                const QPointer<QQuickItem> eyeBeforeEdits = bottomEye;
                if (top >= 1 && bottomEye && topEye && topIcon) {
                    const QPoint from = itemCenter(bottomEye);
                    const QPoint to = itemCenter(topEye);
                    QTest::mousePress(window, Qt::LeftButton, {}, from);
                    pump(50);
                    for (int step = 1; step <= 6; ++step) {
                        QTest::mouseMove(window, from + (to - from) * step / 6);
                        pump(30);
                    }
                    check(topIcon->property("hidden").toBool() &&
                              !trackRole(videoTracks, top, "trackMuted"),
                          "ドラッグ中に通った track の目玉を変えない、または途中で確定しました");
                    QTest::mouseRelease(window, Qt::LeftButton, {}, to);
                    pump(300);
                    bool allHidden = true;
                    for (int index = 0; index <= top; ++index)
                        allHidden = allHidden && trackRole(videoTracks, index, "trackMuted");
                    check(allHidden, "目玉のドラッグで通った video track を非表示にしません");
                    controller.undoLastEdit();
                    pump(300);
                    bool restored = true;
                    for (int index = 0; index <= top; ++index)
                        restored = restored && !trackRole(videoTracks, index, "trackMuted");
                    check(restored, "目玉のドラッグ塗りが 1 回の undo で戻りません");
                }

                auto* audioTracks = controller.audioTrackModel();
                auto* solo = findVisualItem(window, QStringLiteral("trackSolo_audio_0"));
                check(solo && solo->isVisible(), "audio track の S がありません");
                if (solo) {
                    QTest::mouseClick(window, Qt::LeftButton, {}, itemCenter(solo));
                    pump(300);
                    check(trackRole(audioTracks, 0, "trackSolo"), "S を押しても solo になりません");
                    // 押すたびにヘッダを作り直すと、目玉が一瞬消えてちらつく。行数が変わらない
                    // 編集では同じ item のまま値だけが変わる。
                    check(findVisualItem(window, QStringLiteral("trackSolo_audio_0")) == solo &&
                              eyeBeforeEdits &&
                              findVisualItem(window, QStringLiteral("trackEye_video_0")) ==
                                  eyeBeforeEdits.data(),
                          "目玉・M・S の切り替えでトラックヘッダを作り直しています");
                    controller.undoLastEdit();
                    pump(300);
                }
            }
            // 16. トラックの削除は常設のボタンではなく、ヘッダの右クリックメニューから行う。
            {
                check(findVisualItem(window, QStringLiteral("trackHeaderMenuRemove")) == nullptr ||
                          !findVisualItem(window, QStringLiteral("trackHeaderMenuRemove"))
                               ->isVisible(),
                      "前提: トラックヘッダのメニューが開いています");
                const int before = controller.videoTrackCount();
                check(controller.addTrack(QStringLiteral("video")) &&
                          controller.videoTrackCount() == before + 1,
                      "前提: 削除する video track を足せません");
                pump(300);
                auto* area = findVisualItem(
                    window, QStringLiteral("trackHeaderContextArea_video_%1").arg(before));
                check(area && area->isVisible(), "トラックヘッダの右クリックの受け口がありません");
                if (area) {
                    const QPoint point =
                        area->mapToScene(QPointF(10, area->height() / 2)).toPoint();
                    QTest::mouseClick(window, Qt::RightButton, {}, point);
                    QQuickItem* remove = nullptr;
                    pumpUntil(
                        [&] {
                            remove =
                                findVisualItem(window, QStringLiteral("trackHeaderMenuRemove"));
                            return remove && remove->isVisible() && remove->height() > 0;
                        },
                        3000);
                    check(remove && remove->isVisible(),
                          "トラックヘッダを右クリックしても削除のメニューが出ません");
                    if (remove && remove->isVisible()) {
                        pump(200);
                        QTest::mouseClick(window, Qt::LeftButton, {}, itemCenter(remove));
                        pumpUntil([&] { return controller.videoTrackCount() == before; }, 3000);
                        check(controller.videoTrackCount() == before,
                              "メニューの「このトラックを削除」で track が消えません");
                    }
                }
                if (controller.videoTrackCount() != before)
                    controller.undoLastEdit();
                pump(300);
            }
            // 17. 再生中に目玉を切り替えても再生は止まらない。隠すと次の tick で layer から外れ、
            //     戻すと足りない source を組み直して、その位置から再生を続けて映す。
            {
                QVariantList allVideo;
                for (int index = 0; index < controller.videoTrackCount(); ++index)
                    allVideo.push_back(index);
                check(seekAccepted(), "前提: 再生中の目玉の試験の seek が受理されません");
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                check(controller.previewVideoAtPlayhead(), "前提: playhead に映像がありません");
                const auto presentedLayers = [&] {
                    return controller.lastPresentedFrameForTest().layerCount;
                };
                check(controller.playTimeline(), "前提: 再生を始められません");
                pumpUntil([&] { return controller.playing() && presentedLayers() >= 1; }, 5000);
                check(controller.playing() && presentedLayers() >= 1,
                      "前提: 再生中に映像が出ません");
                check(controller.setTracksMuted(QStringLiteral("video"), allVideo, true) &&
                          controller.playing(),
                      "再生中に目玉で隠すと再生が止まります");
                pumpUntil([&] { return presentedLayers() == 0; }, 2000);
                check(controller.playing() && presentedLayers() == 0,
                      "再生中に隠した track が再生を続けたまま layer から外れません");
                check(controller.setTracksMuted(QStringLiteral("video"), allVideo, false) &&
                          controller.playing(),
                      "再生中に目玉で表示すると再生が止まります");
                pumpUntil([&] { return presentedLayers() >= 1; }, 3000);
                check(controller.playing() && presentedLayers() >= 1,
                      "再生中に表示へ戻した track が再生を続けたまま映りません");
                controller.pauseTimeline();
                pump(300);
            }
            // 18. 受理されない編集・書き出しは再生を止めない。候補の検証が先、transport の停止は
            //     commit が確定してから。Undo 履歴と未保存状態も変えない。
            {
                check(controller.playTimeline(),
                      "前提: 受理されない操作の試験で再生を始められません");
                pumpUntil([&] { return controller.playing(); }, 5000);
                check(controller.playing(), "前提: 受理されない操作の試験で再生中になりません");
                const auto undoDepth = controller.undoDepthForTest();
                const bool dirtyBefore = controller.dirty();
                check(!controller.trimClip(QStringLiteral("no-such-clip"), QStringLiteral("right"),
                                           -1, false),
                      "前提: 存在しない clip の trim を受理しました");
                check(controller.playing(), "存在しない clip の trim で再生が止まりました");
                check(!controller.exportTimeline(
                          QUrl(QStringLiteral("https://example.invalid/a.mp4"))),
                      "前提: ローカルでない書き出し先を受理しました");
                check(controller.playing(),
                      "ローカルでない書き出し先の書き出しで再生が止まりました");
                check(controller.undoDepthForTest() == undoDepth &&
                          controller.dirty() == dirtyBefore,
                      "受理されない操作が Undo 履歴または未保存状態を変えました");
                controller.pauseTimeline();
                pump(300);
            }
            return failures == 0 ? 0 : 1;
        };
        exitCode = run();
        if (activationLost) {
            // 途中で event が届かなかった可能性があるので、FAIL の有無にかかわらず判定しない。
            std::fprintf(stderr,
                         "PROTOCOL_INVALID: 検査中に window が非アクティブになりました "
                         "(FAIL %d 件は判定に使えません)。検査中は他の window を操作"
                         "しないでください\n",
                         failures);
            exitCode = 4;
        }
        controller.shutdown();
    }
    if (exitCode == 0)
        exitCode = checkLargeTimelineDelegates(
            project, directory.filePath(QStringLiteral("large.mvm")).toStdWString());
    if (exitCode == 0)
        exitCode = checkLargeTextOverlayDelegates(
            directory.filePath(QStringLiteral("large-text.mvm")).toStdWString());
    if (exitCode == 0 && failures != 0)
        exitCode = 1;
    mvm_mlt_runtime_shutdown();
    if (exitCode == 0)
        std::puts("文字ツールの直接入力 (作成・Esc・ドラッグ・範囲で掴む・Undo・focus 解放) "
                  "・入力中の shortcut 抑止を確認しました");
    return exitCode;
}
