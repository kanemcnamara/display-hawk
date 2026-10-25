#include "model/CabinetMapping.h"

#include <QSet>

namespace CabinetMapping {

QVector<QPoint> chainOrder(const LedWall &wall, const CabinetLayout &layout)
{
    QVector<QPoint> out;
    const bool fromRight = layout.startCorner == 1 || layout.startCorner == 3;
    const bool fromBottom = layout.startCorner == 2 || layout.startCorner == 3;
    const bool rows = layout.order == CabinetOrder::Rows || layout.order == CabinetOrder::RowsSerpentine;
    const bool serpentine = layout.order == CabinetOrder::RowsSerpentine || layout.order == CabinetOrder::ColumnsSerpentine;
    const int lines = rows ? wall.rows : wall.cols;
    const int along = rows ? wall.cols : wall.rows;
    for (int li = 0; li < lines; ++li) {
        const int line = (rows ? fromBottom : fromRight) ? lines - 1 - li : li;
        bool forward = !(rows ? fromRight : fromBottom);
        if (serpentine && li % 2 == 1)
            forward = !forward;
        for (int ai = 0; ai < along; ++ai) {
            const int a = forward ? ai : along - 1 - ai;
            const QPoint cab = rows ? QPoint(a, line) : QPoint(line, a);
            if (wall.panelEnabled(cab.x(), cab.y()))
                out.append(cab);
        }
    }
    return out;
}

QVector<Slice> generate(const Project &project, const Screen &screen, const CabinetLayout &layout,
                        const QVector<Slice> &previous)
{
    QVector<Slice> out;
    if (layout.wall < 0 || layout.wall >= project.walls.size())
        return out;
    const LedWall &w = project.walls[layout.wall];
    const int rot = layout.rotation & 3;
    const bool quarter = rot % 2 == 1;
    const int cw = quarter ? w.panelPxH : w.panelPxW; // one cabinet on the output
    const int ch = quarter ? w.panelPxW : w.panelPxH;
    const qint64 cabinetPixels = qint64(w.panelPxW) * w.panelPxH;
    const int perPort = layout.portCapacity > 0 ? int(qMax<qint64>(1, layout.portCapacity / qMax<qint64>(1, cabinetPixels)))
                                                : INT_MAX;

    const QVector<QPoint> chain = chainOrder(w, layout);
    int curX = layout.x, curY = layout.y;
    int lastPort = -1;
    for (int i = 0; i < chain.size(); ++i) {
        const QPoint cab = chain[i];
        const int port = layout.firstPort + i / perPort;
        QPoint pos;
        if (layout.arrange == CabinetArrange::WallShape) {
            // The whole wall turns with its cabinets: rotate the grid position, then mirror it.
            int c = cab.x(), r = cab.y();
            int cols = w.cols, rows = w.rows;
            if (rot == 1) {
                c = w.rows - 1 - cab.y();
                r = cab.x();
            } else if (rot == 2) {
                c = w.cols - 1 - cab.x();
                r = w.rows - 1 - cab.y();
            } else if (rot == 3) {
                c = cab.y();
                r = w.cols - 1 - cab.x();
            }
            if (quarter)
                std::swap(cols, rows);
            if (layout.flipH)
                c = cols - 1 - c;
            if (layout.flipV)
                r = rows - 1 - r;
            pos = QPoint(layout.x + c * (cw + layout.gap), layout.y + r * (ch + layout.gap));
        } else if (layout.arrange == CabinetArrange::PackRows) {
            const bool portBreak = layout.newLinePerPort && lastPort >= 0 && port != lastPort;
            if (curX > layout.x && (curX + cw > screen.width || portBreak)) {
                curX = layout.x;
                curY += ch + layout.gap;
            }
            pos = QPoint(curX, curY);
            curX += cw + layout.gap;
        } else {
            const bool portBreak = layout.newLinePerPort && lastPort >= 0 && port != lastPort;
            if (curY > layout.y && (curY + ch > screen.height || portBreak)) {
                curY = layout.y;
                curX += cw + layout.gap;
            }
            pos = QPoint(curX, curY);
            curY += ch + layout.gap;
        }
        lastPort = port;

        Slice sl;
        for (const Slice &p : previous) {
            if (p.wall == layout.wall && p.cabinet == cab) {
                sl = p; // keeps enabled, angle, brightness and colour correction
                break;
            }
        }
        sl.name = QStringLiteral("%1 C%2 R%3").arg(w.name).arg(cab.x() + 1).arg(cab.y() + 1);
        sl.input = w.panelRect(cab.x(), cab.y());
        sl.output = QRect(pos, QSize(cw, ch));
        sl.rotation = rot;
        sl.flipH = layout.flipH;
        sl.flipV = layout.flipV;
        sl.wall = layout.wall;
        sl.cabinet = cab;
        sl.port = layout.portCapacity > 0 ? port : 0;
        out.append(sl);
    }
    return out;
}

void regenerate(Project &project, int wall)
{
    for (Screen &s : project.screens) {
        for (const CabinetLayout &layout : std::as_const(s.cabinetLayouts)) {
            if (wall >= 0 && layout.wall != wall)
                continue;
            QVector<Slice> previous;
            int insertAt = -1;
            for (int i = 0; i < s.slices.size();) {
                if (s.slices[i].isCabinet() && s.slices[i].wall == layout.wall) {
                    if (insertAt < 0)
                        insertAt = i;
                    previous.append(s.slices.takeAt(i));
                } else {
                    ++i;
                }
            }
            if (insertAt < 0)
                insertAt = int(s.slices.size());
            const QVector<Slice> fresh = generate(project, s, layout, previous);
            for (int i = 0; i < fresh.size(); ++i)
                s.slices.insert(insertAt + i, fresh[i]);
        }
    }
}

Summary summarize(const Screen &screen, int wall)
{
    Summary sum;
    QSet<int> ports;
    const QRect bounds(0, 0, screen.width, screen.height);
    for (const Slice &sl : screen.slices) {
        if (!sl.isCabinet() || sl.wall != wall)
            continue;
        ++sum.cabinets;
        if (sl.port > 0)
            ports.insert(sl.port);
        if (!bounds.contains(sl.output))
            ++sum.outside;
    }
    sum.ports = int(ports.size());
    return sum;
}

QColor portColor(int port)
{
    // Golden-angle hues: neighbouring ports get clearly different colours.
    return QColor::fromHsv(int((port - 1) * 137.508) % 360, 200, 255);
}

QVector<PortRoute> portRoutes(const Screen &screen)
{
    QVector<PortRoute> routes;
    for (const Slice &sl : screen.slices) {
        if (sl.port <= 0 || !sl.enabled)
            continue;
        PortRoute *route = nullptr;
        for (PortRoute &r : routes)
            if (r.port == sl.port)
                route = &r;
        if (!route) {
            routes.append({sl.port, portColor(sl.port), {}});
            route = &routes.last();
        }
        route->points.append(QRectF(sl.output).center());
    }
    return routes;
}

} // namespace CabinetMapping
