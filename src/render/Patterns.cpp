#include "render/Patterns.h"

#include <QPainter>
#include <QtMath>

namespace {

QFont pixelFont(int px, bool bold = true)
{
    QFont f; // platform UI font: Segoe UI on Windows, SF on macOS
    f.setPixelSize(qMax(6, px));
    f.setBold(bold);
    return f;
}

void drawLabel(QPainter &p, const QRect &r, const QString &text, int px)
{
    p.setFont(pixelFont(px));
    p.setPen(QColor(0, 0, 0, 200));
    p.drawText(r.translated(1, 1), Qt::AlignCenter, text);
    p.setPen(Qt::white);
    p.drawText(r, Qt::AlignCenter, text);
}

QImage grid(QSize size, int spacing)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    const int w = size.width(), h = size.height();

    for (int x = 0, i = 0; x < w; x += spacing, ++i) {
        p.setPen(i % 4 == 0 ? QColor(170, 170, 170) : QColor(70, 70, 70));
        p.drawLine(x, 0, x, h - 1);
    }
    for (int y = 0, i = 0; y < h; y += spacing, ++i) {
        p.setPen(i % 4 == 0 ? QColor(170, 170, 170) : QColor(70, 70, 70));
        p.drawLine(0, y, w - 1, y);
    }

    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 200, 255), 2));
    p.drawLine(0, 0, w - 1, h - 1);
    p.drawLine(w - 1, 0, 0, h - 1);
    const int r = qMin(w, h) / 2 - 2;
    p.setPen(QPen(QColor(255, 200, 0), 2));
    p.drawEllipse(QPoint(w / 2, h / 2), r, r);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.setPen(QPen(Qt::red, 1));
    p.drawLine(w / 2, 0, w / 2, h - 1);
    p.drawLine(0, h / 2, w - 1, h / 2);
    p.setPen(Qt::white);
    p.drawRect(0, 0, w - 1, h - 1);

    const int fontPx = qMax(10, qMin(w, h) / 12);
    const QRect labelRect(0, h / 2 - fontPx * 2, w, fontPx * 2 - 4);
    p.fillRect(QRect(w / 2 - fontPx * 4, labelRect.y(), fontPx * 8, labelRect.height()), QColor(0, 0, 0, 180));
    drawLabel(p, labelRect, QStringLiteral("%1 x %2").arg(w).arg(h), fontPx);
    return img;
}

QImage colorBars(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    const QColor bars[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue, Qt::black};
    const int w = size.width(), h = size.height();
    const int barH = h * 3 / 4;
    for (int i = 0; i < 8; ++i) {
        const int x0 = w * i / 8, x1 = w * (i + 1) / 8;
        p.fillRect(QRect(x0, 0, x1 - x0, barH), bars[i]);
    }
    // Grey steps for checking gamma and low-end colour on LED.
    const int steps = 11;
    for (int i = 0; i < steps; ++i) {
        const int x0 = w * i / steps, x1 = w * (i + 1) / steps;
        const int v = 255 * i / (steps - 1);
        p.fillRect(QRect(x0, barH, x1 - x0, h - barH), QColor(v, v, v));
    }
    return img;
}

QImage gradient(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    const QColor ends[] = {Qt::red, Qt::green, Qt::blue, Qt::white};
    const int w = size.width(), h = size.height();
    for (int i = 0; i < 4; ++i) {
        const int y0 = h * i / 4, y1 = h * (i + 1) / 4;
        QLinearGradient g(0, 0, w, 0);
        g.setColorAt(0, Qt::black);
        g.setColorAt(1, ends[i]);
        p.fillRect(QRect(0, y0, w, y1 - y0), g);
    }
    return img;
}

QImage checker(QSize size, int spacing)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    for (int y = 0, j = 0; y < size.height(); y += spacing, ++j)
        for (int x = 0, i = 0; x < size.width(); x += spacing, ++i)
            if ((i + j) % 2 == 0)
                p.fillRect(QRect(x, y, spacing, spacing), Qt::white);
    return img;
}

} // namespace

namespace Patterns {

QImage generate(const Project &project, const Clip &c, QSize size)
{
    size = size.expandedTo(QSize(1, 1));
    switch (c.pattern) {
    case TestPattern::Grid: return grid(size, qMax(4, c.gridSpacing));
    case TestPattern::ColorBars: return colorBars(size);
    case TestPattern::Gradient: return gradient(size);
    case TestPattern::Checker: return checker(size, qMax(1, c.gridSpacing));
    case TestPattern::PanelMap: return panelMap(project, false);
    case TestPattern::MovingBars: {
        // The bars themselves are drawn live by the renderer; this still frame is for thumbnails.
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::black);
        QPainter p(&img);
        const int bar = qMax(4, size.width() / 64);
        p.fillRect(QRect(size.width() / 3, 0, bar, size.height()), Qt::white);
        p.fillRect(QRect(0, size.height() * 2 / 3, size.width(), qMax(4, size.height() / 36)), Qt::white);
        return img;
    }
    case TestPattern::Solid: {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(c.solidColor);
        return img;
    }
    }
    return {};
}

QImage panelMap(const Project &project, bool transparentBackground)
{
    QImage img(project.compSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(transparentBackground ? QColor(Qt::transparent) : QColor(20, 20, 20));
    QPainter p(&img);

    for (const LedWall &w : project.walls) {
        const int fontPx = qMin(w.panelPxW, w.panelPxH) / 4;
        int number = 1; // only fitted cabinets are numbered
        for (int r = 0; r < w.rows; ++r) {
            for (int c = 0; c < w.cols; ++c) {
                const QRect pr = w.panelRect(c, r);
                if (!w.panelEnabled(c, r)) {
                    // Not fitted: dark, crossed out.
                    if (!transparentBackground)
                        p.fillRect(pr, QColor(12, 12, 12));
                    p.setPen(QPen(QColor(120, 120, 120, 200), 1, Qt::DashLine));
                    p.drawRect(pr.adjusted(0, 0, -1, -1));
                    p.drawLine(pr.topLeft(), pr.bottomRight());
                    p.drawLine(pr.topRight(), pr.bottomLeft());
                    continue;
                }
                ++number;
                if (!transparentBackground) {
                    const QColor fill = (r + c) % 2 == 0 ? w.color.darker(160) : w.color.darker(260);
                    p.fillRect(pr, fill);
                }
                p.setPen(QColor(255, 255, 255, transparentBackground ? 200 : 255));
                p.drawRect(pr.adjusted(0, 0, -1, -1));
                if (fontPx >= 6) {
                    const QRect top = pr.adjusted(0, 0, 0, -pr.height() / 3);
                    const QRect bottom = pr.adjusted(0, pr.height() * 2 / 3 - fontPx / 2, 0, 0);
                    drawLabel(p, top, QString::number(number - 1), fontPx);
                    p.setFont(pixelFont(fontPx / 2, false));
                    p.setPen(QColor(230, 230, 230));
                    p.drawText(bottom, Qt::AlignHCenter | Qt::AlignTop,
                               QStringLiteral("C%1 R%2").arg(c + 1).arg(r + 1));
                }
            }
        }
        // Wall outline and name.
        p.setPen(QPen(w.color.lighter(130), 2));
        p.drawRect(w.rect().adjusted(1, 1, -1, -1));
        const int nameFont = qBound(10, w.pixelHeight() / 10, 96);
        p.setFont(pixelFont(nameFont));
        const QString label = QStringLiteral("%1  %2x%3  P%4")
                                  .arg(w.name)
                                  .arg(w.pixelWidth())
                                  .arg(w.pixelHeight())
                                  .arg(w.pitchMm(), 0, 'f', 2);
        const QRect nameRect(w.x + 6, w.y + 4, w.pixelWidth() - 12, nameFont + 8);
        p.setPen(QColor(0, 0, 0, 220));
        p.drawText(nameRect.translated(2, 2), Qt::AlignLeft | Qt::AlignTop, label);
        p.setPen(Qt::white);
        p.drawText(nameRect, Qt::AlignLeft | Qt::AlignTop, label);
    }
    return img;
}

QImage panelMask(const Project &project)
{
    QImage img(project.compSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    for (const LedWall &w : project.walls)
        for (int r = 0; r < w.rows; ++r)
            for (int c = 0; c < w.cols; ++c)
                if (w.panelEnabled(c, r))
                    p.fillRect(w.panelRect(c, r), Qt::white);
    return img;
}

} // namespace Patterns
