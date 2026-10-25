#include "render/Patterns.h"

#include <QPainter>
#include "model/CabinetMapping.h"
#include <QPainterPath>
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
        // Drawn unrotated, through the wall's rotation.
        p.setTransform(w.transform());
        p.setRenderHint(QPainter::Antialiasing, w.rotated());
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
    p.resetTransform();
    return img;
}

QImage outputOverlay(const Project &project, const Screen &screen)
{
    QImage img(QSize(screen.width, screen.height).expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const int line = qMax(2, screen.width / 640);
    for (const Slice &sl : screen.slices) {
        if (!sl.enabled)
            continue;
        const QColor c = sl.port > 0 ? CabinetMapping::portColor(sl.port) : QColor(255, 160, 40);
        // The outline as warped; warped slices also get an 8 x 8 alignment grid.
        QPolygonF quad;
        const int steps = sl.isMeshed() ? 32 : 1;
        for (int side = 0; side < 4; ++side)
            for (int k = 0; k < steps; ++k) {
                const double f = double(k) / steps;
                const QPointF st[] = {{f, 0}, {1, f}, {1 - f, 1}, {0, 1 - f}};
                quad << sl.outputPoint(st[side].x(), st[side].y());
            }
        p.setPen(QPen(c, line));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(quad);
        if (sl.isMeshed() || sl.isWarped()) {
            p.setPen(QPen(QColor(255, 255, 255, 150), qMax(1, line / 2)));
            for (int g = 1; g < 8; ++g) {
                QPolygonF across, down;
                for (int k = 0; k <= 32; ++k) {
                    across << sl.outputPoint(k / 32.0, g / 8.0);
                    down << sl.outputPoint(g / 8.0, k / 32.0);
                }
                p.drawPolyline(across);
                p.drawPolyline(down);
            }
        }
        const QRectF box = quad.boundingRect();
        const QString title = sl.isCabinet() ? QStringLiteral("C%1 R%2").arg(sl.cabinet.x() + 1).arg(sl.cabinet.y() + 1)
                                             : sl.name;
        const QString sub = sl.port > 0 ? QStringLiteral("P%1").arg(sl.port) : QString();
        const int fontPx = qBound(8, int(qMin(box.width() / qMax<qsizetype>(4, title.size()), box.height() / 4)), 120);
        p.setFont(pixelFont(fontPx));
        const QRectF titleRect(box.x(), box.center().y() - fontPx, box.width(), fontPx * 1.3);
        p.setPen(QColor(0, 0, 0, 220));
        p.drawText(titleRect.translated(2, 2), Qt::AlignCenter, title);
        p.setPen(Qt::white);
        p.drawText(titleRect, Qt::AlignCenter, title);
        if (!sub.isEmpty()) {
            p.setFont(pixelFont(qMax(8, fontPx * 3 / 4)));
            p.setPen(c);
            p.drawText(QRectF(box.x(), titleRect.bottom(), box.width(), fontPx * 1.1), Qt::AlignCenter, sub);
        }
    }
    // Port routes: where each port's data enters and the order it runs through the cabinets.
    for (const CabinetMapping::PortRoute &route : CabinetMapping::portRoutes(screen)) {
        p.setPen(QPen(route.color, line * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(route.points);
        p.setBrush(route.color);
        for (int i = 1; i < route.points.size(); ++i) {
            const QPointF a = route.points[i - 1], b = route.points[i];
            const QPointF d = b - a;
            const double len = qSqrt(d.x() * d.x() + d.y() * d.y());
            if (len < 1)
                continue;
            const QPointF dir = d / len, n(-dir.y(), dir.x()), mid = (a + b) / 2;
            const double s = line * 5;
            p.drawPolygon(QPolygonF({mid + dir * s, mid - dir * s * 0.8 + n * s * 0.8, mid - dir * s * 0.8 - n * s * 0.8}));
        }
        if (!route.points.isEmpty()) {
            p.drawEllipse(route.points.first(), line * 6, line * 6); // where the cable comes in
        }
        p.setBrush(Qt::NoBrush);
    }
    return img;
}

namespace {

// Three passes of a running-sum box blur approximate a Gaussian; radius in pixels.
void boxBlur(QImage &img, int radius)
{
    if (radius < 1)
        return;
    const int w = img.width(), h = img.height();
    QVector<int> line(qMax(w, h));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) { // horizontal
            uchar *row = img.scanLine(y);
            int sum = 0;
            for (int x = -radius; x <= radius; ++x)
                sum += row[qBound(0, x, w - 1)];
            for (int x = 0; x < w; ++x) {
                line[x] = sum / (2 * radius + 1);
                sum += row[qMin(w - 1, x + radius + 1)] - row[qMax(0, x - radius)];
            }
            for (int x = 0; x < w; ++x)
                row[x] = uchar(line[x]);
        }
        for (int x = 0; x < w; ++x) { // vertical
            int sum = 0;
            for (int y = -radius; y <= radius; ++y)
                sum += img.scanLine(qBound(0, y, h - 1))[x];
            for (int y = 0; y < h; ++y) {
                line[y] = sum / (2 * radius + 1);
                sum += img.scanLine(qMin(h - 1, y + radius + 1))[x] - img.scanLine(qMax(0, y - radius))[x];
            }
            for (int y = 0; y < h; ++y)
                img.scanLine(y)[x] = uchar(line[y]);
        }
    }
}

} // namespace

QImage maskOverlay(const Screen &screen)
{
    const QSize size = QSize(screen.width, screen.height).expandedTo(QSize(1, 1));
    QImage blackout(size, QImage::Format_Grayscale8); // 255 = blacked out
    blackout.fill(0);
    bool any = false;
    for (const ProjectionMask &m : screen.masks) {
        if (!m.enabled || m.points.size() < 3)
            continue;
        any = true;
        QImage shape(size, QImage::Format_Grayscale8);
        shape.fill(m.invert ? 255 : 0);
        {
            QPainter p(&shape);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(m.invert ? Qt::black : Qt::white);
            p.drawPolygon(m.points);
        }
        // Three box passes of radius r spread an edge over about 3r pixels.
        boxBlur(shape, qRound(m.feather / 3.0));
        for (int y = 0; y < size.height(); ++y) {
            uchar *dst = blackout.scanLine(y);
            const uchar *src = shape.constScanLine(y);
            for (int x = 0; x < size.width(); ++x)
                dst[x] = qMax(dst[x], src[x]);
        }
    }
    if (!any)
        return {};
    QImage out(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *a = blackout.constScanLine(y);
        uchar *o = out.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = 0;
            o[4 * x + 3] = a[x];
        }
    }
    return out;
}

QImage panelMask(const Project &project)
{
    QImage img(project.compSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    for (const LedWall &w : project.walls) {
        // One path per wall, so a rotated wall's antialiased edge has no seams between cabinets.
        QPainterPath path;
        for (int r = 0; r < w.rows; ++r)
            for (int c = 0; c < w.cols; ++c)
                if (w.panelEnabled(c, r))
                    path.addRect(w.panelRect(c, r));
        p.setRenderHint(QPainter::Antialiasing, w.rotated());
        p.fillPath(w.transform().map(path.simplified()), Qt::white);
    }
    return img;
}

} // namespace Patterns
