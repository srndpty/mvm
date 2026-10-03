#include <cstdio>
#include <functional>
#include <memory>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

class KeyframeUiController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList keyframeChannels READ channels NOTIFY changed)
public:
    QVariantList channels() const {
        return {QVariantMap{{"name", "positionX"},
                            {"label", "位置 X"},
                            {"value", -10},
                            {"minimum", -1000},
                            {"maximum", 1000},
                            {"animated", animated},
                            {"atKey", true},
                            {"frame", 0},
                            {"duration", 101},
                            {"editable", editable},
                            {"keys", QVariantList{QVariantMap{{"frame", 0},
                                                              {"value", -10},
                                                              {"interpolation", 0},
                                                              {"control1", 1.0 / 3},
                                                              {"control2", 2.0 / 3}},
                                                  QVariantMap{{"frame", 100},
                                                              {"value", 10},
                                                              {"interpolation", 1},
                                                              {"control1", 0.0},
                                                              {"control2", 1.0 / 3}}}}}};
    }

    Q_INVOKABLE bool setEffectAnimation(const QString& name, bool enabled) {
        lastAction = "animation:" + name;
        animated = enabled;
        emit changed();
        return true;
    }

    Q_INVOKABLE bool toggleEffectKey(const QString& name) {
        lastAction = "key:" + name;
        return true;
    }

    Q_INVOKABLE bool seekEffectKey(const QString& name, int direction) {
        lastAction = "seek:" + name + QString::number(direction);
        return true;
    }

    Q_INVOKABLE bool setEffectValue(const QString&, double, bool) { return true; }

    Q_INVOKABLE bool setEffectValues(const QVariantMap&, bool) { return true; }

    Q_INVOKABLE bool cancelEffectPreview() {
        lastAction = "cancel";
        return true;
    }

    Q_INVOKABLE bool moveEffectKey(const QString&, qint64, qint64 to, bool commit) {
        lastAction = commit ? "moveCommit" : "movePreview";
        movedTo = to;
        return true;
    }

    Q_INVOKABLE bool editEffectKey(const QString&, qint64, qint64, double, bool commit) {
        lastAction = commit ? "graphCommit" : "graphPreview";
        return true;
    }

    Q_INVOKABLE bool setEffectSpline(const QString&, qint64, double, double, bool commit) {
        lastAction = commit ? "splineCommit" : "splinePreview";
        return true;
    }

    Q_INVOKABLE bool copyEffectKeys(const QString&, const QVariantList&, bool cut) {
        lastAction = cut ? "cut" : "copy";
        return true;
    }

    Q_INVOKABLE bool deleteEffectKeys(const QString&, const QVariantList&) {
        lastAction = "delete";
        return true;
    }

    Q_INVOKABLE bool pasteEffectKeys(const QString&) {
        lastAction = "paste";
        return true;
    }

    qint64 movedTo = 0;

    Q_INVOKABLE bool seekEffectFrame(qint64 frame, bool scrub = false) {
        lastAction = scrub ? "scrub" : "seekFrame";
        movedTo = frame;
        return true;
    }

    Q_INVOKABLE void beginScrub() { ++scrubStarts; }

    Q_INVOKABLE void endScrub() { ++scrubEnds; }

    int scrubStarts = 0;
    int scrubEnds = 0;

    Q_INVOKABLE bool setEffectInterpolation(const QString&, qint64, int) { return true; }

    bool animated = true;
    bool editable = true;
    QString lastAction;
signals:
    void changed();
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickStyle::setStyle("Basic");
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(MVM_KEYFRAME_QML)));
    KeyframeUiController controller;
    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{"mvmController", QVariant::fromValue(&controller)}}));
    if (!object) {
        std::fprintf(stderr, "キー編集UIを作れません: %s\n", qPrintable(component.errorString()));
        return 1;
    }
    auto* item = qobject_cast<QQuickItem*>(object.get());
    if (!item)
        return 1;
    QQuickWindow window;
    window.resize(600, 300);
    item->setParentItem(window.contentItem());
    item->setWidth(600);
    window.show();
    QCoreApplication::processEvents();
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "失敗: %s\n", message);
            ++failures;
        }
    };
    const std::function<QQuickItem*(QQuickItem*, const QString&)> findItem =
        [&](QQuickItem* parent, const QString& name) -> QQuickItem* {
        if (parent->objectName() == name)
            return parent;
        for (auto* child : parent->childItems())
            if (auto* found = findItem(child, name))
                return found;
        return nullptr;
    };
    const auto click = [&](const char* name) {
        auto* button = findItem(item, QString::fromLatin1(name));
        check(button && button->property("enabled").toBool(), "キー編集のボタンが有効になりません");
        if (button)
            QMetaObject::invokeMethod(button, "clicked");
    };
    auto* field = findItem(item, QStringLiteral("value_positionX"));
    check(field && field->height() <= 24 && field->property("inlineLabelWidth").toDouble() > 0,
          "数値欄が1行に収まりません");
    if (field) {
        const QPoint point =
            field->mapToScene(QPointF(field->width() - 20, field->height() / 2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, {}, point);
        QCoreApplication::processEvents();
        check(window.activeFocusItem() && window.activeFocusItem()->property("text").isValid(),
              "シングルクリックで数値編集になりません");
        QTest::keyClick(&window, Qt::Key_Escape);
    }
    auto* key = findItem(item, QStringLiteral("laneKey_positionX_0"));
    check(key != nullptr, "前提: 小タイムラインにキーがありません");
    if (key) {
        const QPoint point =
            key->mapToScene(QPointF(key->width() / 2 + 2, key->height() / 2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, {}, point);
        check(item->property("selectedFrames").toList().contains(0),
              "クリックしたキーが選択されません");
        auto* second = findItem(item, QStringLiteral("laneKey_positionX_1"));
        check(second != nullptr, "前提: 複数選択の2個目のキーがありません");
        if (second) {
            const auto secondPoint = second->mapToScene(QPointF(4, second->height() / 2)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::ControlModifier, secondPoint);
            check(item->property("selectedFrames").toList().size() == 2,
                  "Ctrl+クリックで複数キーを選択できません");
            QTest::mouseClick(&window, Qt::LeftButton, Qt::ControlModifier, secondPoint);
            check(item->property("selectedFrames").toList() == QVariantList{0},
                  "Ctrl+クリックでキーの選択を外せません");
        }
        QTest::mousePress(&window, Qt::LeftButton, {}, point);
        QTest::mouseMove(&window, point + QPoint(60, 35));
        check(controller.lastAction == "movePreview", "範囲外のドラッグでキー移動が中断されました");
        QTest::keyClick(&window, Qt::Key_Escape);
        QTest::mouseRelease(&window, Qt::LeftButton, {}, point + QPoint(60, 35));
        check(controller.lastAction == "cancel", "Esc後にキー移動を確定しました");
    }
    auto* lane = findItem(item, QStringLiteral("lane_positionX"));
    check(lane != nullptr, "前提: 小タイムラインがありません");
    if (lane) {
        const auto start = lane->mapToScene(QPointF(lane->width() / 2, 9)).toPoint();
        const auto end = lane->mapToScene(QPointF(lane->width() * 0.75, 9)).toPoint();
        const auto selected = item->property("selectedFrames").toList();
        QTest::mouseClick(&window, Qt::LeftButton, {}, start);
        check(controller.lastAction == "seekFrame" && controller.movedTo == 50 &&
                  controller.scrubStarts == 0,
              "小タイムラインのクリックで直接シークしません");
        QTest::mousePress(&window, Qt::LeftButton, {}, start);
        QTest::mouseMove(&window, end);
        check(controller.lastAction == "scrub" && controller.movedTo == 75,
              "小タイムラインの空白ドラッグでスクラブしません");
        QTest::mouseRelease(&window, Qt::LeftButton, {}, end);
        check(controller.scrubStarts == 1 && controller.scrubEnds == 1 &&
                  item->property("selectedFrames").toList() == selected &&
                  item->property("marquee").isNull(),
              "スクラブが終了しないか、小タイムラインで矩形選択しています");
    }
    click("graphToggle_positionX");
    QTest::qWait(50);
    auto* graph = findItem(item, QStringLiteral("graph_positionX"));
    check(graph && graph->isVisible(), "カーブ欄を開けません");
    auto* handle = findItem(item, QStringLiteral("curveHandle_positionX_0_0"));
    check(handle && handle->isVisible(), "選択キーの区間ハンドルがありません");
    if (handle) {
        const auto point = handle->mapToScene(QPointF(4, 4)).toPoint();
        QTest::mousePress(&window, Qt::LeftButton, {}, point);
        QTest::mouseMove(&window, point + QPoint(0, -8));
        check(controller.lastAction == "splinePreview", "ハンドルのドラッグを曲線編集へ渡しません");
        QTest::mouseRelease(&window, Qt::LeftButton, {}, point + QPoint(0, -8));
        check(controller.lastAction == "splineCommit", "ハンドルのドラッグを確定しません");
    }
    if (graph) {
        const QPoint start = graph->mapToScene(QPointF(0, graph->height() - 3)).toPoint();
        const QPoint end = graph->mapToScene(QPointF(graph->width() + 5, -3)).toPoint();
        QTest::mousePress(&window, Qt::LeftButton, {}, start);
        QTest::mouseMove(&window, end);
        QTest::mouseRelease(&window, Qt::LeftButton, {}, end);
        const auto frames = item->property("selectedFrames").toList();
        check(frames.contains(0) && frames.contains(100),
              "グラフの空白ドラッグで複数キーを矩形選択しません");
        QMetaObject::invokeMethod(item, "copyKeys", Q_ARG(QVariant, QVariant(false)));
        check(controller.lastAction == "copy", "矩形選択したキーをコピーしません");
        QMetaObject::invokeMethod(item, "deleteKeys");
        check(window.activeFocusItem() == item, "キー削除後に編集パネルのフォーカスを維持しません");
        check(controller.lastAction == "delete" &&
                  item->property("selectedFrames").toList().isEmpty(),
              "矩形選択したキーを削除しません");
    }
    QTest::qWait(30);
    check(window.grabWindow().save(QStringLiteral("keyframe-ui-preview.png")),
          "UIの表示確認用画像を保存できません");
    click("key_positionX");
    check(controller.lastAction == "key:positionX", "キーの追加削除を対象項目へ渡しません");
    click("next_positionX");
    check(controller.lastAction == "seek:positionX1", "次のキーへの移動を渡しません");
    click("previous_positionX");
    check(controller.lastAction == "seek:positionX-1", "前のキーへの移動を渡しません");
    click("animation_positionX");
    check(!controller.animated && controller.lastAction == "animation:positionX",
          "ストップウォッチをオフにできません");
    controller.editable = false;
    emit controller.changed();
    QCoreApplication::processEvents();
    const auto* button = findItem(item, QStringLiteral("key_positionX"));
    check(button && !button->property("enabled").toBool(),
          "再生中・クリップ外のキー編集を無効にしません");
    item->setParentItem(nullptr);
    return failures == 0 ? 0 : 1;
}

#include "test_keyframe_ui.moc"
