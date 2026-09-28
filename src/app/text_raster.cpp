#include "app/text_raster.h"

#include <algorithm>

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>

namespace mvm::app {

QImage renderTextRaster(const project::TextClipData& data, int width, int height, QString& error) {
    error.clear();
    const QString family = QString::fromStdString(data.fontFamily);
    if (family == QStringLiteral("Meiryo") && !QFontDatabase::hasFamily(family))
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/meiryo.ttc"));
    if (!QFontDatabase::hasFamily(family)) {
        error = QStringLiteral("指定フォントが見つかりません: ") + family;
        return {};
    }
    if (width <= 0 || height <= 0 || data.content.empty()) {
        error = QStringLiteral("文字画像の寸法または本文が不正です");
        return {};
    }
    QFont font(family);
    font.setPixelSize(data.fontSize);
    font.setBold(data.bold);
    QFontMetricsF metrics(font);
    const QStringList lines = QString::fromStdString(data.content).split(u'\n');
    qreal maxWidth = 0;
    for (const QString& line : lines)
        maxWidth = std::max(maxWidth, metrics.horizontalAdvance(line));
    const qreal padding = data.outlineWidth + 2;
    const qreal blockWidth = maxWidth + 2 * padding;
    const qreal blockHeight =
        static_cast<qreal>(lines.size()) * metrics.lineSpacing() + 2 * padding;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        error = QStringLiteral("文字画像を確保できません");
        return {};
    }
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QColor background(QString::fromStdString(data.backgroundColor));
    painter.fillRect(QRectF(data.x, data.y, blockWidth, blockHeight), background);
    const QColor foreground(QString::fromStdString(data.color));
    const QColor outline(QString::fromStdString(data.outlineColor));
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QString& line = lines[index];
        qreal offset = 0;
        if (data.alignment == "center")
            offset = (maxWidth - metrics.horizontalAdvance(line)) / 2;
        else if (data.alignment == "right")
            offset = maxWidth - metrics.horizontalAdvance(line);
        QPainterPath path;
        path.addText(QPointF(data.x + padding + offset,
                             data.y + padding + metrics.ascent() +
                                 static_cast<qreal>(index) * metrics.lineSpacing()),
                     font, line);
        if (data.outlineWidth > 0)
            painter.strokePath(path, QPen(outline, data.outlineWidth * 2.0, Qt::SolidLine,
                                          Qt::RoundCap, Qt::RoundJoin));
        painter.fillPath(path, foreground);
    }
    painter.end();
    return image;
}

} // namespace mvm::app
