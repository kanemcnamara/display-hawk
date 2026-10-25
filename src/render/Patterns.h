#pragma once

#include "model/Project.h"

#include <QImage>

namespace Patterns {

// A clip's test pattern at the given size (the PanelMap pattern is always composition-sized).
QImage generate(const Project &project, const Clip &clip, QSize size);

// Full-composition image with every LED wall's panels coloured, outlined and numbered.
QImage panelMap(const Project &project, bool transparentBackground);

} // namespace Patterns
