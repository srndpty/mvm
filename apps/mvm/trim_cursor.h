#ifndef MVM_APPS_MVM_TRIM_CURSOR_H
#define MVM_APPS_MVM_TRIM_CURSOR_H

#include <QColor>
#include <QCursor>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace mvm::app {

// clip の端を掴むときのカーソル (premiere と同じ、色付きのブラケットと矢印)。
//
// この item の範囲 (端の線をまたぐ帯。線は中央) に pointer がある間と、held (親の MouseArea を
// 押している) の間だけ、application の override cursor を出す。
//
// timeline の clip では、item のカーソル (MouseArea の cursorShape を含む) も hover
// (containsMouse) も window から届かなかった。そのため Qt の hover とカーソル探索には頼らず、
// window へ届く mouse の移動を event filter で見て、自分の範囲かを判定する。
//
// 形: ブラケットは端の種類 (edge "in" = 左端 "[" / "out" = 右端 "]")、矢印は pointer のある側を
// 向く。右端の内側 <-]・外側 ]->、左端の内側 [->・外側 <-[。mode が "split" / "sizeHor" なら
// Qt の標準形 (ローリング / レート調整)、"" なら出さない。
class TrimCursor : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TrimCursor)
    Q_PROPERTY(QString edge READ edge WRITE setEdge NOTIFY edgeChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(bool held READ held WRITE setHeld NOTIFY heldChanged)
    // 今出している形 (inInner / inOuter / outInner / outOuter / split / sizeHor)。出していなければ空。
    Q_PROPERTY(QString shownKind READ shownKind NOTIFY shownKindChanged)

public:
    explicit TrimCursor(QQuickItem* parent = nullptr);
    ~TrimCursor() override;

    QString edge() const { return edge_; }
    void setEdge(const QString& edge);
    QString mode() const { return mode_; }
    void setMode(const QString& mode);
    QColor color() const { return color_; }
    void setColor(const QColor& color);
    bool held() const { return held_; }
    void setHeld(bool held);
    QString shownKind() const { return shownKind_; }

    // ブラケットのカーソル画像。hot spot はブラケットの縦線 (clip の端) に置く。
    // openBracket: "[" (in 点) か "]" (out 点)。arrowRight: 矢印が右を向くか。
    static QCursor bracketCursor(bool openBracket, bool arrowRight, const QColor& color);
    // 形の名前に対応するカーソル。不明・空なら false。
    static bool cursorForKind(const QString& kind, const QColor& color, QCursor& cursor);

Q_SIGNALS:
    void edgeChanged();
    void modeChanged();
    void colorChanged();
    void heldChanged();
    void shownKindChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void watch(QQuickWindow* window);
    // scene 座標の pointer に対して出す形。範囲外・popup の上・無効なら空。
    QString kindAt(const QPointF& scenePosition) const;
    void show(const QString& kind);

    QString edge_ = QStringLiteral("out");
    QString mode_;
    QColor color_{QStringLiteral("#e8413c")};
    bool held_ = false;
    QString shownKind_;
    QPointF lastScenePosition_{-1e9, -1e9};
    QPointer<QQuickWindow> window_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_TRIM_CURSOR_H
