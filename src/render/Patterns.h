#pragma once

#include "model/Project.h"

#include <QImage>

namespace Patterns {

// A clip's test pattern at the given size (the PanelMap pattern is always composition-sized).
QImage generate(const Project &project, const Clip &clip, QSize size);

// Full-composition image with every LED wall's panels coloured, outlined and numbered.
QImage panelMap(const Project &project, bool transparentBackground);

// White where an enabled LED panel is, black elsewhere (multiplied over the composition).
QImage panelMask(const Project &project);

// Drawn over a screen's output while identifying slices: each slice's outline and name (cabinet
// column/row and port), and each port's chain route. Transparent elsewhere; screen-sized.
QImage outputOverlay(const Project &project, const Screen &screen);

} // namespace Patterns
