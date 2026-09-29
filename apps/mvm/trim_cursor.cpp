#include "trim_cursor.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QQuickWindow>

namespace mvm::app {

namespace {
constexpr int kCursorSize = 32;
// ブラケットの縦線の x。矢印が右を向くならブラケットを左寄り、左を向くなら右寄りに置く。
constexpr int kBracketLeftX = 10;
constexpr int kBracketRightX = kCursorSize - 1 - kBracketLeftX;
constexpr int kCenterY = kCursorSize / 2;

// override cursor は application に 1 つだけ積み、最後に出した TrimCursor を持ち主とする。
// 隣の端へ移るとき「新しい端が出す -> 古い端が外す」の順に来ても、古い端が新しい端の
// カーソルを外さないようにする (持ち主でなければ何もしない)。GUI thread だけから触る。
TrimCursor* gOwner = nullptr;

// popup (dialog・menu) の上か。overlay は window 全面を覆うので、開いている popup の範囲だけ見る。
bool onOverlayPopup(QQuickWindow* window, const QPointF& scenePosition) {
    for (QQuickItem* child : window->contentItem()->childItems()) {
        if (!child->inherits("QQuickOverlay"))
            continue;
        for (QQuickItem* popup : child->childItems()) {
            if (popup->isVisible() && popup->contains(popup->mapFromScene(scenePosition)))
                return true;
        }
    }
    return false;
}
} // namespace

TrimCursor::TrimCursor(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, false);
    setAcceptedMouseButtons(Qt::NoButton);
}

TrimCursor::~TrimCursor() {
    watch(nullptr);
    show(QString());
}

void TrimCursor::setEdge(const QString& edge) {
    if (edge_ == edge)
        return;
    edge_ = edge;
    if (!held_)
        show(kindAt(lastScenePosition_));
    Q_EMIT edgeChanged();
}

void TrimCursor::setMode(const QString& mode) {
    if (mode_ == mode)
        return;
    mode_ = mode;
    if (!held_)
        show(kindAt(lastScenePosition_));
    Q_EMIT modeChanged();
}

void TrimCursor::setColor(const QColor& color) {
    if (color_ == color)
        return;
    color_ = color;
    // 色だけ変えて出し直す。
    const QString kind = shownKind_;
    show(QString());
    show(kind);
    Q_EMIT colorChanged();
}

void TrimCursor::setHeld(bool held) {
    if (held_ == held)
        return;
    held_ = held;
    // 押している間は押した時点の形を保つ。離したら今の pointer の位置で決め直す。
    if (!held_)
        show(kindAt(lastScenePosition_));
    Q_EMIT heldChanged();
}

void TrimCursor::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange)
        watch(value.window);
    // 隠れた (ツールが変わって端のハンドルが消えた等) ら外す。
    if (change == ItemVisibleHasChanged && !value.boolValue && !held_)
        show(QString());
}

void TrimCursor::watch(QQuickWindow* window) {
    if (window_ == window)
        return;
    if (window_)
        window_->removeEventFilter(this);
    window_ = window;
    if (window_)
        window_->installEventFilter(this);
}

QString TrimCursor::kindAt(const QPointF& scenePosition) const {
    if (!window_ || !isVisible() || !isEnabled() || mode_.isEmpty())
        return {};
    const QPointF local = mapFromScene(scenePosition);
    if (!contains(local) || onOverlayPopup(window_, scenePosition))
        return {};
    if (mode_ == QStringLiteral("split") || mode_ == QStringLiteral("sizeHor"))
        return mode_;
    // 端の線は帯の中央。左端 (in) は線の右が clip の内側、右端 (out) は線の左が内側。
    const bool rightOfLine = local.x() >= width() / 2;
    if (edge_ == QStringLiteral("in"))
        return rightOfLine ? QStringLiteral("inInner") : QStringLiteral("inOuter");
    return rightOfLine ? QStringLiteral("outOuter") : QStringLiteral("outInner");
}

bool TrimCursor::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_)
        return QQuickItem::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
        lastScenePosition_ = static_cast<const QMouseEvent*>(event)->scenePosition();
        if (!held_)
            show(kindAt(lastScenePosition_));
        break;
    case QEvent::Leave:
        lastScenePosition_ = QPointF(-1e9, -1e9);
        if (!held_)
            show(QString());
        break;
    default:
        break;
    }
    return QQuickItem::eventFilter(watched, event);
}

void TrimCursor::show(const QString& kind) {
    QCursor cursor;
    if (!cursorForKind(kind, color_, cursor)) {
        if (gOwner == this) {
            gOwner = nullptr;
            QGuiApplication::restoreOverrideCursor();
        }
    } else if (gOwner == this && kind == shownKind_) {
        return;
    } else if (gOwner) {
        QGuiApplication::changeOverrideCursor(cursor);
        gOwner = this;
    } else {
        QGuiApplication::setOverrideCursor(cursor);
        gOwner = this;
    }
    const QString shown = gOwner == this ? kind : QString();
    if (shownKind_ != shown) {
        shownKind_ = shown;
        Q_EMIT shownKindChanged();
    }
}

QCursor TrimCursor::bracketCursor(bool openBracket, bool arrowRight, const QColor& color) {
    QPixmap pixmap(kCursorSize, kCursorSize);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int bracketX = arrowRight ? kBracketLeftX : kBracketRightX;
    // "[" の腕は右へ、"]" の腕は左へ伸びる。矢印は向いている側へ伸びる。
    const int arms = openBracket ? 1 : -1;
    const int toward = arrowRight ? 1 : -1;
    QPainterPath path;
    path.moveTo(bracketX + arms * 5, kCenterY - 9);
    path.lineTo(bracketX, kCenterY - 9);
    path.lineTo(bracketX, kCenterY + 9);
    path.lineTo(bracketX + arms * 5, kCenterY + 9);
    const int tipX = bracketX + toward * 15;
    path.moveTo(bracketX + toward * 3, kCenterY);
    path.lineTo(tipX, kCenterY);
    path.moveTo(tipX - toward * 5, kCenterY - 5);
    path.lineTo(tipX, kCenterY);
    path.lineTo(tipX - toward * 5, kCenterY + 5);

    // どの背景の上でも見えるよう、暗い縁取りの上に色の線を重ねる。
    painter.setPen(QPen(QColor(20, 20, 20, 230), 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.end();
    return QCursor(pixmap, bracketX, kCenterY);
}

bool TrimCursor::cursorForKind(const QString& kind, const QColor& color, QCursor& cursor) {
    // premiere と同じく、形は端の線のどちら側かだけで決まる: 線の左は <-]、右は [->。
    // (左端の外側と右端の内側は線の左、左端の内側と右端の外側は線の右)
    if (kind == QStringLiteral("inInner") || kind == QStringLiteral("outOuter"))
        cursor = bracketCursor(true, true, color);
    else if (kind == QStringLiteral("inOuter") || kind == QStringLiteral("outInner"))
        cursor = bracketCursor(false, false, color);
    else if (kind == QStringLiteral("split"))
        cursor = QCursor(Qt::SplitHCursor);
    else if (kind == QStringLiteral("sizeHor"))
        cursor = QCursor(Qt::SizeHorCursor);
    else
        return false;
    return true;
}

} // namespace mvm::app
