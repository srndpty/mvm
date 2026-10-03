#include "app/text_raster.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QTextLayout>

namespace mvm::app {
namespace {

// 描画と定位置の計算が同じ寸法を使うよう、文字の組み方をここに集める。
struct TextLayout {
    QFont font;
    QStringList lines;
    qreal maxWidth = 0;
    qreal padding = 0;
    qreal lineSpacing = 0;
    qreal ascent = 0;
    QSizeF block;
};

bool layoutText(const project::TextClipData& data, TextLayout& layout, QString& error) {
    error.clear();
    const QString family = QString::fromStdString(data.fontFamily);
    if (family == QStringLiteral("Meiryo") && !QFontDatabase::hasFamily(family))
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/meiryo.ttc"));
    if (!QFontDatabase::hasFamily(family)) {
        error = QStringLiteral("指定フォントが見つかりません: ") + family;
        return false;
    }
    if (data.content.empty()) {
        error = QStringLiteral("文字画像の本文が空です");
        return false;
    }
    layout.font = QFont(family);
    layout.font.setPixelSize(data.fontSize);
    layout.font.setBold(data.bold);
    const QFontMetricsF metrics(layout.font);
    layout.lines = QString::fromStdString(data.content).split(u'\n');
    layout.maxWidth = 0;
    for (const QString& line : layout.lines)
        layout.maxWidth = std::max(layout.maxWidth, metrics.horizontalAdvance(line));
    layout.padding = data.outlineWidth + 2;
    layout.lineSpacing = metrics.lineSpacing();
    layout.ascent = metrics.ascent();
    layout.block =
        QSizeF(layout.maxWidth + 2 * layout.padding,
               static_cast<qreal>(layout.lines.size()) * layout.lineSpacing + 2 * layout.padding);
    return true;
}

} // namespace

QImage renderTextRaster(const project::TextClipData& data, int width, int height, QString& error) {
    TextLayout layout;
    if (!layoutText(data, layout, error))
        return {};
    if (width <= 0 || height <= 0) {
        error = QStringLiteral("文字画像の寸法または本文が不正です");
        return {};
    }
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
    painter.fillRect(QRectF(QPointF(data.x, data.y), layout.block), background);
    const QColor foreground(QString::fromStdString(data.color));
    const QColor outline(QString::fromStdString(data.outlineColor));
    const QFontMetricsF metrics(layout.font);
    for (qsizetype index = 0; index < layout.lines.size(); ++index) {
        const QString& line = layout.lines[index];
        qreal offset = 0;
        if (data.alignment == "center")
            offset = (layout.maxWidth - metrics.horizontalAdvance(line)) / 2;
        else if (data.alignment == "right")
            offset = layout.maxWidth - metrics.horizontalAdvance(line);
        QPainterPath path;
        path.addText(QPointF(data.x + layout.padding + offset,
                             data.y + layout.padding + layout.ascent +
                                 static_cast<qreal>(index) * layout.lineSpacing),
                     layout.font, line);
        if (data.outlineWidth > 0)
            painter.strokePath(path, QPen(outline, data.outlineWidth * 2.0, Qt::SolidLine,
                                          Qt::RoundCap, Qt::RoundJoin));
        painter.fillPath(path, foreground);
    }
    painter.end();
    return image;
}

namespace {
// 字幕を折り返して画面の下へ置いた文字データを作る。描画 (renderSubtitleRaster) と、
// 描画せずに収まるかだけを見る検査 (checkSubtitleLayout) が同じ計算を使う。
bool placeSubtitle(const project::SubtitleCue& cue, const project::SubtitleStyle& style, int width,
                   int height, project::TextClipData& data, QString& error) {
    data.fontFamily = style.fontFamily;
    data.fontSize = style.fontSize;
    data.bold = style.bold;
    data.color = style.color;
    data.outlineColor = style.outlineColor;
    data.outlineWidth = style.outlineWidth;
    data.backgroundColor = style.backgroundColor;
    data.alignment = style.alignment;
    data.content = cue.content;
    TextLayout checked;
    if (!layoutText(data, checked, error))
        return false;
    const auto available = width * (1.0 - 2 * style.sideMargin) - 2 * checked.padding;
    if (available <= 0) {
        error = QStringLiteral("字幕の余白が描画幅を超えています");
        return false;
    }
    QStringList wrapped;
    for (const auto& paragraph : QString::fromStdString(cue.content).split(u'\n')) {
        if (paragraph.isEmpty()) {
            wrapped.append(QString());
            continue;
        }
        QTextLayout layout(paragraph, checked.font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(available);
            wrapped.append(paragraph.mid(line.textStart(), line.textLength()));
        }
        layout.endLayout();
    }
    data.content = wrapped.join(u'\n').toStdString();
    const auto block = textBlockSize(data, error);
    if (!error.isEmpty())
        return false;
    if (block.height() > height * (1.0 - style.bottomMargin)) {
        error =
            QStringLiteral("字幕が画面の高さを超えています。本文・サイズ・余白を調整してください");
        return false;
    }
    double x = width * style.sideMargin;
    if (style.alignment == "center")
        x = (width - block.width()) / 2;
    else if (style.alignment == "right")
        x = width * (1.0 - style.sideMargin) - block.width();
    data.x = std::max(0, static_cast<int>(std::lround(x)));
    data.y = std::max(
        0, static_cast<int>(std::lround(height * (1.0 - style.bottomMargin) - block.height())));
    return true;
}
} // namespace

QImage renderSubtitleRaster(const project::SubtitleCue& cue, const project::SubtitleStyle& style,
                            int width, int height, QString& error) {
    project::TextClipData data;
    if (!placeSubtitle(cue, style, width, height, data, error))
        return {};
    return renderTextRaster(data, width, height, error);
}

bool checkSubtitleLayout(const project::SubtitleCue& cue, const project::SubtitleStyle& style,
                         int width, int height, QString& error) {
    project::TextClipData data;
    if (width <= 0 || height <= 0) {
        error = QStringLiteral("文字画像の寸法または本文が不正です");
        return false;
    }
    return placeSubtitle(cue, style, width, height, data, error);
}

QSizeF textBlockSize(const project::TextClipData& data, QString& error) {
    TextLayout layout;
    if (!layoutText(data, layout, error))
        return {};
    return layout.block;
}

TextPresetPlacement textPresetPlacement(const project::TextClipData& data, int width, int height,
                                        const std::string& alignment) {
    TextPresetPlacement result;
    if (width <= 0 || height <= 0) {
        result.error = QStringLiteral("出力の寸法が不正です");
        return result;
    }
    if (alignment != "left" && alignment != "center" && alignment != "right") {
        result.error = QStringLiteral("未知の揃えです: ") + QString::fromStdString(alignment);
        return result;
    }
    const QSizeF block = textBlockSize(data, result.error);
    if (block.isEmpty())
        return result;
    const double side = width * kTextPresetSideRatio;
    double x = side;
    if (alignment == "center")
        x = (width - block.width()) / 2.0;
    else if (alignment == "right")
        x = width - side - block.width();
    const double y = height * (1.0 - kTextPresetBottomRatio) - block.height();
    // 画面より大きな文字は左上を画面内に留める。Project の x / y は 0 以上である。
    result.x = std::max(0, static_cast<int>(std::lround(x)));
    result.y = std::max(0, static_cast<int>(std::lround(y)));
    result.success = true;
    return result;
}

} // namespace mvm::app
