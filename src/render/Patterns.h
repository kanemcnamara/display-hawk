#pragma once

#include "model/Project.h"

#include <QImage>

namespace Patterns {

// Test pattern sized to the content target (whole composition or one wall).
QImage generate(const Project &project, QSize size);

// Full-composition image with every LED wall's panels coloured, outlined and numbered.
QImage panelMap(const Project &project, bool transparentBackground);

} // namespace Patterns
