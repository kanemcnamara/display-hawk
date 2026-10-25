#include "ui/ReportExporter.h"

#include "model/Project.h"
#include "render/Patterns.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QTextStream>
#include <QtMath>
#include <numeric>

namespace ReportExporter {

namespace {

// A 1G port on common LED processors carries roughly 650k pixels at 60 Hz, 8-bit.
constexpr double kPixelsPerGigPort = 655360.0;

QString rectText(const QRect &r)
{
    return QStringLiteral("%1, %2  %3 x %4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QString flipText(const Slice &s)
{
    QStringList f;
    if (s.flipH)
        f << QStringLiteral("H");
    if (s.flipV)
        f << QStringLiteral("V");
    return f.isEmpty() ? QStringLiteral("-") : f.join(QLatin1Char('+'));
}

QString colourText(const Slice &s)
{
    if (!s.hasColorCorrection())
        return QStringLiteral("-");
    return QStringLiteral("RGB %1/%2/%3%  gamma %4  black %5%")
        .arg(qRound(s.gainR * 100))
        .arg(qRound(s.gainG * 100))
        .arg(qRound(s.gainB * 100))
        .arg(s.gamma, 0, 'f', 2)
        .arg(s.blackLevel * 100, 0, 'f', 1);
}

// Draws a simple table; returns the y below it.
int drawTable(QPainter &p, int x, int y, const QVector<int> &widths, const QStringList &header,
              const QVector<QStringList> &rows)
{
    const int rowH = p.fontMetrics().height() + 10;
    const int total = std::accumulate(widths.begin(), widths.end(), 0);
    p.fillRect(QRect(x, y, total, rowH), QColor(230, 232, 236));
    QFont bold = p.font();
    bold.setBold(true);
    auto drawRow = [&](const QStringList &cells, int ry) {
        int cx = x;
        for (int i = 0; i < widths.size(); ++i) {
            const QRect cell(cx + 6, ry, widths[i] - 12, rowH);
            p.drawText(cell, Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(cells.value(i), Qt::ElideRight, cell.width()));
            cx += widths[i];
        }
    };
    const QFont normal = p.font();
    p.setFont(bold);
    drawRow(header, y);
    p.setFont(normal);
    y += rowH;
    for (int r = 0; r < rows.size(); ++r, y += rowH) {
        if (r % 2)
            p.fillRect(QRect(x, y, total, rowH), QColor(246, 247, 249));
        drawRow(rows[r], y);
    }
    p.setPen(QColor(200, 200, 205));
    p.drawRect(QRect(x, y - rowH * (int(rows.size()) + 1), total, rowH * (int(rows.size()) + 1)));
    p.setPen(Qt::black);
    return y;
}

void drawImageFitted(QPainter &p, const QImage &img, const QRect &area)
{
    const QSize s = img.size().scaled(area.size(), Qt::KeepAspectRatio);
    const QRect r(area.x() + (area.width() - s.width()) / 2, area.y(), s.width(), s.height());
    p.drawImage(r, img);
    p.setPen(QColor(150, 150, 150));
    p.drawRect(r);
    p.setPen(Qt::black);
}

} // namespace

QImage screenLayout(const Project &project, const Screen &screen, const QImage &panelMap)
{
    QImage img(QSize(screen.width, screen.height).expandedTo(QSize(1, 1)), QImage::Format_RGB32);
    img.fill(QColor(8, 8, 10));
    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const Slice &sl : screen.slices) {
        if (!sl.enabled || sl.input.isEmpty() || sl.output.isEmpty())
            continue;
        // Same mapping as the GPU: rotate the content clockwise, then flip in output space.
        const QImage part = panelMap.copy(sl.input);
        const bool quarter = sl.rotation & 1;
        const QSizeF drawn = quarter ? QSizeF(sl.output.height(), sl.output.width()) : QSizeF(sl.output.size());
        p.save();
        p.translate(QRectF(sl.output).center());
        p.scale(sl.flipH ? -1 : 1, sl.flipV ? -1 : 1);
        p.rotate(90.0 * (sl.rotation & 3));
        p.drawImage(QRectF(-drawn.width() / 2, -drawn.height() / 2, drawn.width(), drawn.height()), part);
        p.restore();

        p.setPen(QPen(QColor(255, 160, 40), qMax(2, screen.width / 640)));
        p.drawRect(QRect(sl.output).adjusted(0, 0, -1, -1));
        QFont f = p.font();
        f.setPixelSize(qBound(12, sl.output.height() / 14, 48));
        f.setBold(true);
        p.setFont(f);
        const QString label = QStringLiteral("%1   in %2   out %3").arg(sl.name, rectText(sl.input), rectText(sl.output));
        // Bottom-left on a dark band, clear of the panel map's own wall title at the top.
        const QRect lr = QRect(sl.output).adjusted(8, 6, -8, -6);
        const QRect band = p.fontMetrics().boundingRect(lr, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, label);
        p.fillRect(band.adjusted(-6, -3, 6, 3), QColor(0, 0, 0, 190));
        p.setPen(QColor(255, 200, 120));
        p.drawText(lr, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, label);
    }
    Q_UNUSED(project);
    return img;
}

QStringList exportReport(const Project &project, const QString &dir, const QString &baseName, QString *error)
{
    QStringList written;
    QDir out(dir);
    if (!out.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QObject::tr("Cannot create folder %1").arg(dir);
        return {};
    }
    auto path = [&](const QString &suffix) { return out.filePath(baseName + suffix); };

    const QImage panelMap = Patterns::panelMap(project, false);
    if (panelMap.save(path(QStringLiteral("-panel-map.png"))))
        written << path(QStringLiteral("-panel-map.png"));
    QVector<QImage> layouts;
    for (int i = 0; i < project.screens.size(); ++i) {
        layouts.append(screenLayout(project, project.screens[i], panelMap));
        const QString file = path(QStringLiteral("-screen-%1.png").arg(i + 1));
        if (layouts.last().save(file))
            written << file;
    }

    // CSV of every slice: what goes where, for spreadsheets and processor setup.
    {
        QFile csv(path(QStringLiteral("-slices.csv")));
        if (csv.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream s(&csv);
            s << "screen,slice,enabled,in_x,in_y,in_w,in_h,out_x,out_y,out_w,out_h,rotation_deg,flip_h,flip_v,"
                 "brightness,gain_r,gain_g,gain_b,gamma,black_level\n";
            auto q = [](const QString &v) { return QStringLiteral("\"%1\"").arg(QString(v).replace('"', "\"\"")); };
            for (const Screen &sc : project.screens) {
                for (const Slice &sl : sc.slices) {
                    s << q(sc.name) << ',' << q(sl.name) << ',' << int(sl.enabled) << ',' << sl.input.x() << ','
                      << sl.input.y() << ',' << sl.input.width() << ',' << sl.input.height() << ',' << sl.output.x()
                      << ',' << sl.output.y() << ',' << sl.output.width() << ',' << sl.output.height() << ','
                      << sl.rotation * 90 << ',' << int(sl.flipH) << ',' << int(sl.flipV) << ',' << sl.brightness
                      << ',' << sl.gainR << ',' << sl.gainG << ',' << sl.gainB << ',' << sl.gamma << ','
                      << sl.blackLevel << '\n';
                }
            }
            written << csv.fileName();
        }
    }

    // PDF: A4 landscape at 150 dpi, drawn in device pixels.
    const QString pdfPath = path(QStringLiteral("-report.pdf"));
    QPdfWriter pdf(pdfPath);
    pdf.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape, QMarginsF(12, 12, 12, 12),
                                  QPageLayout::Millimeter));
    pdf.setResolution(150);
    pdf.setTitle(QStringLiteral("%1 - LED setup report").arg(baseName));
    pdf.setCreator(QStringLiteral("Display Hawk"));
    QPainter p;
    if (!p.begin(&pdf)) {
        if (error)
            *error = QObject::tr("Cannot write %1").arg(pdfPath);
        return written;
    }
    const QRect page = p.viewport();
    const int w = page.width();
    QFont body(QStringLiteral("Helvetica"));
    body.setPointSizeF(9);
    QFont title = body;
    title.setPointSizeF(18);
    title.setBold(true);
    QFont heading = body;
    heading.setPointSizeF(12);
    heading.setBold(true);

    auto pageTitle = [&](const QString &text, const QString &sub) {
        p.setFont(title);
        p.drawText(QRect(0, 0, w, 60), Qt::AlignLeft | Qt::AlignTop, text);
        p.setFont(body);
        p.setPen(QColor(90, 90, 90));
        p.drawText(QRect(0, 52, w, 30), Qt::AlignLeft | Qt::AlignTop, sub);
        p.setPen(Qt::black);
        return 100;
    };

    // Page 1: summary.
    int y = pageTitle(QStringLiteral("%1 - LED setup report").arg(baseName),
                      QStringLiteral("Display Hawk  |  %1  |  composition %2 x %3")
                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("d MMM yyyy, HH:mm")))
                          .arg(project.compWidth)
                          .arg(project.compHeight));
    p.setFont(heading);
    p.drawText(QRect(0, y, w, 30), Qt::AlignLeft, QObject::tr("LED walls"));
    y += 36;
    p.setFont(body);
    QVector<QStringList> wallRows;
    qint64 totalPixels = 0;
    double totalArea = 0;
    for (const LedWall &wall : project.walls) {
        totalPixels += wall.totalPixels();
        totalArea += wall.widthM() * wall.heightM();
        const int fitted = wall.enabledPanelCount();
        wallRows.append({wall.name,
                         QStringLiteral("%1 x %2%3").arg(wall.cols).arg(wall.rows)
                             .arg(fitted == wall.cols * wall.rows ? QString() : QStringLiteral(" (%1 fitted)").arg(fitted)),
                         QStringLiteral("%1 x %2 px").arg(wall.panelPxW).arg(wall.panelPxH),
                         QStringLiteral("%1 x %2 mm").arg(wall.panelMmW).arg(wall.panelMmH),
                         QStringLiteral("%1 mm").arg(wall.pitchMm(), 0, 'f', 2),
                         QStringLiteral("%1 x %2").arg(wall.pixelWidth()).arg(wall.pixelHeight()),
                         QStringLiteral("%1 x %2 m").arg(wall.widthM(), 0, 'f', 2).arg(wall.heightM(), 0, 'f', 2),
                         QStringLiteral("%1, %2").arg(wall.x).arg(wall.y),
                         QString::number(qCeil(wall.totalPixels() / kPixelsPerGigPort))});
    }
    const QVector<int> wallCols{int(w * 0.16), int(w * 0.11), int(w * 0.1), int(w * 0.12), int(w * 0.08),
                                int(w * 0.11), int(w * 0.12), int(w * 0.1), int(w * 0.1)};
    y = drawTable(p, 0, y,
                  wallCols,
                  {QObject::tr("Wall"), QObject::tr("Panels"), QObject::tr("Panel pixels"), QObject::tr("Panel size"),
                   QObject::tr("Pitch"), QObject::tr("Resolution"), QObject::tr("Physical size"),
                   QObject::tr("Comp. position"), QObject::tr("Est. 1G ports")},
                  wallRows);
    y += 16;
    p.drawText(QRect(0, y, w, 24), Qt::AlignLeft,
               QObject::tr("Total: %1 driven pixels, %2 m², ~%3 x 1G processor ports (650k px/port at 60 Hz, 8-bit)")
                   .arg(QLocale().toString(totalPixels))
                   .arg(totalArea, 0, 'f', 2)
                   .arg(qCeil(totalPixels / kPixelsPerGigPort)));
    y += 56;

    p.setFont(heading);
    p.drawText(QRect(0, y, w, 30), Qt::AlignLeft, QObject::tr("Screens (video outputs)"));
    y += 36;
    p.setFont(body);
    QVector<QStringList> screenRows;
    for (const Screen &sc : project.screens) {
        screenRows.append({sc.name, QStringLiteral("%1 x %2").arg(sc.width).arg(sc.height),
                           sc.display.isEmpty() ? QObject::tr("none") : sc.display,
                           QString::number(sc.slices.size()), sc.enabled ? QObject::tr("yes") : QObject::tr("no")});
    }
    drawTable(p, 0, y, {int(w * 0.25), int(w * 0.15), int(w * 0.3), int(w * 0.1), int(w * 0.1)},
              {QObject::tr("Screen"), QObject::tr("Resolution"), QObject::tr("Output device"), QObject::tr("Slices"),
               QObject::tr("Enabled")},
              screenRows);

    // Page 2: the panel map.
    pdf.newPage();
    y = pageTitle(QObject::tr("Panel map"),
                  QObject::tr("Composition %1 x %2. Panels are numbered left to right, top to bottom; crossed-out "
                              "cabinets are not fitted.")
                      .arg(project.compWidth)
                      .arg(project.compHeight));
    drawImageFitted(p, panelMap, QRect(0, y, w, page.height() - y));

    // One page per screen: what its processor receives, and the slice table.
    for (int i = 0; i < project.screens.size(); ++i) {
        const Screen &sc = project.screens[i];
        pdf.newPage();
        y = pageTitle(QObject::tr("%1 - processor input").arg(sc.name),
                      QObject::tr("%1 x %2 output. Each slice shows the panel map content the processor receives "
                                  "there.")
                          .arg(sc.width)
                          .arg(sc.height));
        const int tableRows = int(sc.slices.size()) + 1;
        const int tableH = tableRows * (p.fontMetrics().height() + 10);
        drawImageFitted(p, layouts[i], QRect(0, y, w, page.height() - y - tableH - 24));
        QVector<QStringList> sliceRows;
        for (const Slice &sl : sc.slices) {
            sliceRows.append({sl.name, rectText(sl.input), rectText(sl.output),
                              QStringLiteral("%1°").arg(sl.rotation * 90), flipText(sl),
                              QStringLiteral("%1%").arg(qRound(sl.brightness * 100)), colourText(sl),
                              sl.enabled ? QString() : QObject::tr("disabled")});
        }
        drawTable(p, 0, page.height() - tableH,
                  {int(w * 0.16), int(w * 0.17), int(w * 0.17), int(w * 0.07), int(w * 0.06), int(w * 0.08),
                   int(w * 0.21), int(w * 0.08)},
                  {QObject::tr("Slice"), QObject::tr("Input (composition)"), QObject::tr("Output (screen)"),
                   QObject::tr("Rotate"), QObject::tr("Flip"), QObject::tr("Bright."), QObject::tr("Colour"),
                   QString()},
                  sliceRows);
    }
    p.end();
    written << pdfPath;
    return written;
}

} // namespace ReportExporter
