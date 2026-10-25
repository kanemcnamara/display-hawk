#pragma once

#include <QByteArray>

// Decompresses a raw Snappy block (as used by HAP's second-stage compression).
// Returns false on malformed input; never reads or writes out of bounds.
bool snappyUncompress(const char *data, qsizetype size, QByteArray &out);

// Compresses into a raw Snappy block (greedy, 4-byte hash matches; fast rather than tight, as
// HAP's block-compressed textures only shrink so far anyway).
QByteArray snappyCompress(const char *data, qsizetype size);
