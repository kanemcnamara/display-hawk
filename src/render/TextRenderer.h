#pragma once

#include "model/Project.h"

#include <QImage>

namespace TextRenderer {

// The picture of a text clip for a target area of `target` pixels (straight alpha):
// still/blinking text: the whole area, laid out and (font size 0) as large as fits;
// scrolling left/right: one line, as tall as the area, plus a gap, to be repeated sideways;
// scrolling up/down: the text block as wide as the area, plus a gap, to be repeated vertically.
QImage render(const Clip &clip, const QSize &target);

// A key that changes whenever the picture would.
QByteArray key(const Clip &clip, const QSize &target);

} // namespace TextRenderer
