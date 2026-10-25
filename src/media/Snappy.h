#pragma once

#include <QByteArray>

// Decompresses a raw Snappy block (as used by HAP's second-stage compression).
// Returns false on malformed input; never reads or writes out of bounds.
bool snappyUncompress(const char *data, qsizetype size, QByteArray &out);
