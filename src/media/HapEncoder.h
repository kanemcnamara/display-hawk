#pragma once

#include "media/HapDecoder.h"

#include <QByteArray>
#include <QImage>
#include <QString>

class QThreadPool;

// HAP encoding: block-compresses pictures into the GPU texture formats HAP stores, and packs
// them into HAP frames (Snappy-compressed, in chunks that a player can decompress in parallel).
namespace Hap {

enum class Format {
    Hap,      // RGB DXT1: smallest files, good quality
    HapAlpha, // RGBA DXT5: keeps transparency
    HapQ,     // scaled YCoCg DXT5: best quality, twice the size of Hap
};
QString formatName(Format f);
QByteArray fourcc(Format f); // the QuickTime codec id: "Hap1", "Hap5", "HapY"

// Block-compresses an image (any size: edges are repeated out to whole 4x4 blocks) into the
// format's texture data. Rows of blocks are shared out over `pool` when given.
QByteArray compressTexture(const QImage &image, Format f, QThreadPool *pool = nullptr);

// One HAP frame as stored in the movie: the texture in `chunks` Snappy chunks (a chunk that
// does not shrink is stored as is).
QByteArray makeFrame(const QByteArray &texture, Format f, int chunks = 1, QThreadPool *pool = nullptr);

// Decodes block-compressed texture data back into a picture on the CPU (tests, thumbnails).
QImage textureToImage(const HapFrame &frame);

} // namespace Hap
