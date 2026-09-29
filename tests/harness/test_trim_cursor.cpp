// clip の端のカーソル (TrimCursor)。画像のブラケットと矢印の向き、pointer の位置による形の
// 切り替え、押している間の保持、override cursor の出し入れを確かめる。
// 期待値は仕様 (premiere と同じく端の線の左は <-]、右は [->。右端: 内側 <-] / 外側 [->、左端: 内側
// [-> / 外側 <-]) から直接書き、描画の 座標定数は使わない (hot spot
// に対する左右の画素の有無だけを見る)。

#include "trim_cursor.h"

#include <cstdio>
#include <cstdlib>

#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QQuickItem>
#include <QQuickWindow>

namespace {

int gFailures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "NG: %s\n", message);
    }
}

// hot spot の行と腕の行で、hot spot の左右どちらに画素があるか。
struct Shape {
    bool arrowLeft = false;
    bool arrowRight = false;
    bool armsLeft = false;  // "]"
    bool armsRight = false; // "["
    bool colored = false;
};

bool opaque(const QImage& image, int x, int y) {
    return x >= 0 && y >= 0 && x < image.width() && y < image.height() &&
           image.pixelColor(x, y).alpha() >= 128;
}

Shape shapeOf(const QCursor& cursor, const QColor& color) {
    Shape shape;
    const QImage image = cursor.pixmap().toImage().convertToFormat(QImage::Format_ARGB32);
    const int hotX = cursor.hotSpot().x();
    const int hotY = cursor.hotSpot().y();
    for (int x = 0; x < image.width(); ++x) {
        if (!opaque(image, x, hotY))
            continue;
        if (x < hotX - 6)
            shape.arrowLeft = true;
        if (x > hotX + 6)
            shape.arrowRight = true;
        const QColor pixel = image.pixelColor(x, hotY);
        if (std::abs(pixel.red() - color.red()) < 40 &&
            std::abs(pixel.green() - color.green()) < 40 &&
            std::abs(pixel.blue() - color.blue()) < 40)
            shape.colored = true;
    }
    // 腕は縦線の上端付近。hot spot から 9 px 上の行を中心に上下 1 px を見る。
    for (int dy = -1; dy <= 1; ++dy) {
        const int y = hotY - 9 + dy;
        for (int dx = 3; dx <= 4; ++dx) {
            shape.armsLeft = shape.armsLeft || opaque(image, hotX - dx, y);
            shape.armsRight = shape.armsRight || opaque(image, hotX + dx, y);
        }
    }
    return shape;
}

QCursor cursorOf(const char* kind, const QColor& color) {
    QCursor cursor;
    mvm::app::TrimCursor::cursorForKind(QString::fromLatin1(kind), color, cursor);
    return cursor;
}

void moveTo(QQuickWindow& window, QPointF scene) {
    QMouseEvent move(QEvent::MouseMove, scene, scene, window.mapToGlobal(scene), Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &move);
}

QString overrideKind(const mvm::app::TrimCursor& cursor) {
    return QGuiApplication::overrideCursor() ? cursor.shownKind() : QString();
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QColor red(QStringLiteral("#e8413c"));

    // 画像: ブラケットは端の種類、矢印は pointer のある側。
    const auto inInner = shapeOf(cursorOf("inInner", red), red);   // [->
    const auto inOuter = shapeOf(cursorOf("inOuter", red), red);   // <-]
    const auto outInner = shapeOf(cursorOf("outInner", red), red); // <-]
    const auto outOuter = shapeOf(cursorOf("outOuter", red), red); // [->
    check(inInner.armsRight && !inInner.armsLeft && inInner.arrowRight && !inInner.arrowLeft,
          "左端の内側が [-> になっていません");
    check(inOuter.armsLeft && !inOuter.armsRight && inOuter.arrowLeft && !inOuter.arrowRight,
          "左端の外側が <-] になっていません");
    check(outInner.armsLeft && !outInner.armsRight && outInner.arrowLeft && !outInner.arrowRight,
          "右端の内側が <-] になっていません");
    check(outOuter.armsRight && !outOuter.armsLeft && outOuter.arrowRight && !outOuter.arrowLeft,
          "右端の外側が [-> になっていません");
    check(inInner.colored && outOuter.colored, "カーソルが指定した色で描かれていません");
    // 対照: 別の色を指定すると赤としては検出されない (色の検査が空振りしない)。
    check(!shapeOf(cursorOf("outInner", QColor(Qt::blue)), red).colored,
          "色の検査が指定色を区別していません");
    QCursor unknown;
    check(!mvm::app::TrimCursor::cursorForKind(QStringLiteral("bogus"), red, unknown),
          "不明な形の名前でカーソルを作りました");

    // pointer の位置で形を決める。帯 (x 100〜116) の中央が端の線。
    QQuickWindow window;
    window.resize(400, 200);
    {
        // QML と同じく、作ってから親へ付ける (コンストラクタの親では window の監視が始まらない)。
        auto* right = new mvm::app::TrimCursor;
        right->setParentItem(window.contentItem());
        right->setPosition({100, 50});
        right->setSize({16, 40});
        right->setEdge(QStringLiteral("out"));
        right->setMode(QStringLiteral("trim"));
        moveTo(window, {50, 70});
        check(QGuiApplication::overrideCursor() == nullptr, "帯の外でカーソルを出しました");
        moveTo(window, {104, 70});
        check(overrideKind(*right) == QStringLiteral("outInner"),
              "右端の線の左 (内側) で <-] になりません");
        moveTo(window, {112, 70});
        check(overrideKind(*right) == QStringLiteral("outOuter"),
              "右端の線の右 (外側) で [-> になりません");
        // 押している間は範囲の外へ出ても形を保つ。
        right->setHeld(true);
        moveTo(window, {300, 70});
        check(overrideKind(*right) == QStringLiteral("outOuter"),
              "押している間に範囲の外で形が変わりました");
        right->setHeld(false);
        check(QGuiApplication::overrideCursor() == nullptr,
              "離した後、範囲の外でカーソルが残ります");

        right->setEdge(QStringLiteral("in"));
        moveTo(window, {112, 70});
        check(overrideKind(*right) == QStringLiteral("inInner"),
              "左端の線の右 (内側) で [-> になりません");
        moveTo(window, {104, 70});
        check(overrideKind(*right) == QStringLiteral("inOuter"),
              "左端の線の左 (外側) で <-] になりません");
        right->setMode(QStringLiteral("split"));
        check(QGuiApplication::overrideCursor() &&
                  QGuiApplication::overrideCursor()->shape() == Qt::SplitHCursor,
              "ローリングで SplitHCursor になりません");
        right->setMode(QString());
        check(QGuiApplication::overrideCursor() == nullptr, "mode を外してもカーソルが残ります");
        right->setMode(QStringLiteral("trim"));

        // 隣の端へ移ると新しい端の形になり、古い端が外しても新しい端の形は残る。
        auto* next = new mvm::app::TrimCursor;
        next->setParentItem(window.contentItem());
        next->setPosition({200, 50});
        next->setSize({16, 40});
        next->setEdge(QStringLiteral("out"));
        next->setMode(QStringLiteral("trim"));
        moveTo(window, {104, 70});
        check(overrideKind(*right) == QStringLiteral("inOuter"), "前提: 最初の端の形が出ません");
        moveTo(window, {204, 70});
        check(overrideKind(*next) == QStringLiteral("outInner") && right->shownKind().isEmpty(),
              "隣の端へ移ったときに新しい端の形になりません");

        // 出している途中で破棄しても override cursor を残さない。
        delete next;
        check(QGuiApplication::overrideCursor() == nullptr,
              "出している途中で破棄すると override cursor が残ります");
        delete right;
    }

    std::fprintf(stderr, "trim cursor: %s\n", gFailures == 0 ? "PASS" : "FAIL");
    return gFailures == 0 ? 0 : 1;
}
