#include "render/TextRenderer.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace TextRenderer {

namespace {

QFont fontFor(const Clip &clip, double px)
{
    QFont f(clip.fontFamily.isEmpty() ? QStringLiteral("Arial") : clip.fontFamily);
    f.setPixelSize(qMax(1, qRound(px)));
    f.setBold(clip.bold);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

// Lines of `text` wrapped to `width` (0 = no wrapping) in font `f`.
QStringList wrap(const QString &text, const QFont &f, double width)
{
    QStringList out;
    const QFontMetricsF fm(f);
    for (const QString &para : text.split(QLatin1Char('\n'))) {
        if (width <= 0) {
            out << para;
            continue;
        }
        QString line;
        for (const QString &word : para.split(QLatin1Char(' '))) {
            const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate) > width) {
                out << line;
                line = word;
            } else {
                line = candidate;
            }
        }
        out << line;
    }
    return out;
}

QSizeF blockSize(const QStringList &lines, const QFont &f)
{
    const QFontMetricsF fm(f);
    double w = 0;
    for (const QString &l : lines)
        w = qMax(w, fm.horizontalAdvance(l));
    return QSizeF(w, fm.lineSpacing() * lines.size());
}

// Draws the lines into `rect` aligned horizontally, with the outline under the fill.
void drawLines(QPainter &p, const Clip &clip, const QFont &f, const QStringList &lines, const QRectF &rect)
{
    const QFontMetricsF fm(f);
    QPainterPath path;
    double y = rect.top() + fm.ascent();
    for (const QString &l : lines) {
        const double w = fm.horizontalAdvance(l);
        const double x = clip.textAlign == 0 ? rect.left() : clip.textAlign == 2 ? rect.right() - w : rect.center().x() - w / 2;
        path.addText(QPointF(x, y), f, l);
        y += fm.lineSpacing();
    }
    p.setRenderHint(QPainter::Antialiasing);
    if (clip.outlineWidth > 0) {
        p.setPen(QPen(clip.outlineColor, clip.outlineWidth * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(clip.solidColor);
    p.drawPath(path);
}

} // namespace

QByteArray key(const Clip &clip, const QSize &target)
{
    QByteArray k;
    k += clip.text.toUtf8() + '\x1f' + clip.fontFamily.toUtf8() + '\x1f';
    k += QByteArray::number(clip.fontSize) + (clip.bold ? "b" : "n") + clip.solidColor.name(QColor::HexArgb).toLatin1();
    k += clip.outlineColor.name(QColor::HexArgb).toLatin1() + QByteArray::number(clip.outlineWidth);
    k += clip.textBackground.name(QColor::HexArgb).toLatin1() + QByteArray::number(clip.textAlign);
    k += QByteArray::number(int(clip.textMotion)) + QByteArray::number(target.width()) + 'x' + QByteArray::number(target.height());
    return k;
}

QImage render(const Clip &clip, const QSize &target)
{
    const QSize area = target.expandedTo(QSize(1, 1));
    const double margin = area.height() * 0.04;
    const bool sideways = clip.textMotion == TextMotion::ScrollLeft || clip.textMotion == TextMotion::ScrollRight;
    const bool vertical = clip.textMotion == TextMotion::ScrollUp || clip.textMotion == TextMotion::ScrollDown;

    if (sideways) {
        // One line, as tall as the area (font size 0), then a gap of one area height.
        const QString line = QString(clip.text).replace(QLatin1Char('\n'), QStringLiteral("   "));
        const QFont f = fontFor(clip, clip.fontSize > 0 ? clip.fontSize : area.height() * 0.75);
        const QFontMetricsF fm(f);
        const int w = qMax(1, qCeil(fm.horizontalAdvance(line) + area.height()));
        QImage img(qMin(w, 16384), area.height(), QImage::Format_ARGB32_Premultiplied);
        img.fill(clip.textBackground);
        QPainter p(&img);
        const double top = (area.height() - fm.height()) / 2.0;
        drawLines(p, clip, f, {line}, QRectF(area.height() / 2.0, top, w - area.height(), fm.height()));
        return img;
    }
    if (vertical) {
        // The block wrapped to the width, then a gap of one area height.
        const QFont f = fontFor(clip, clip.fontSize > 0 ? clip.fontSize : area.height() / 8.0);
        const QStringList lines = wrap(clip.text, f, area.width() - 2 * margin);
        const QSizeF block = blockSize(lines, f);
        const int h = qMin(16384, qCeil(block.height() + area.height()));
        QImage img(area.width(), h, QImage::Format_ARGB32_Premultiplied);
        img.fill(clip.textBackground);
        QPainter p(&img);
        drawLines(p, clip, f, lines, QRectF(margin, area.height() / 2.0, area.width() - 2 * margin, block.height()));
        return img;
    }

    // Still: the largest size that fits (or the size given), centred vertically.
    const QRectF box(margin, margin, area.width() - 2 * margin, area.height() - 2 * margin);
    double px = clip.fontSize;
    QStringList lines;
    if (px <= 0) {
        double lo = 4, hi = area.height();
        for (int i = 0; i < 18; ++i) { // binary search on the font size
            const double mid = (lo + hi) / 2;
            const QFont f = fontFor(clip, mid);
            const QStringList l = wrap(clip.text, f, box.width());
            const QSizeF s = blockSize(l, f);
            if (s.width() <= box.width() && s.height() <= box.height())
                lo = mid;
            else
                hi = mid;
        }
        px = lo;
    }
    const QFont f = fontFor(clip, px);
    lines = wrap(clip.text, f, box.width());
    const QSizeF block = blockSize(lines, f);
    QImage img(area, QImage::Format_ARGB32_Premultiplied);
    img.fill(clip.textBackground);
    QPainter p(&img);
    drawLines(p, clip, f, lines, QRectF(box.left(), box.center().y() - block.height() / 2, box.width(), block.height()));
    return img;
}

} // namespace TextRenderer
