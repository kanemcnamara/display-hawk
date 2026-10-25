#pragma once

#include "model/Project.h"

#include <QColor>
#include <QPolygonF>

// Cabinet layouts: one slice per fitted cabinet of a wall, placed on a screen and chained onto
// processor ports. Pure functions over the project; Document applies the results.
namespace CabinetMapping {

// The wall's fitted cabinets in chain order (column, row).
QVector<QPoint> chainOrder(const LedWall &wall, const CabinetLayout &layout);

// The cabinet slices a layout produces; they sample at the wall's angle. Per-cabinet settings
// (enabled, brightness, colour correction) are carried over from `previous` slices of the same cabinets.
QVector<Slice> generate(const Project &project, const Screen &screen, const CabinetLayout &layout,
                        const QVector<Slice> &previous = {});

// Re-generates every layout of every screen that uses `wall` (all walls when -1), keeping each
// screen's other slices in place.
void regenerate(Project &project, int wall = -1);

struct Summary {
    int cabinets = 0;
    int ports = 0;
    int outside = 0; // cabinets that do not fit on the screen
};
Summary summarize(const Screen &screen, int wall);

// The path each processor port takes through a screen's cabinets: slice centres on the output,
// in slice (chain) order, one colour per port.
struct PortRoute {
    int port = 0;
    QColor color;
    QPolygonF points;
};
QVector<PortRoute> portRoutes(const Screen &screen);
QColor portColor(int port);

} // namespace CabinetMapping
