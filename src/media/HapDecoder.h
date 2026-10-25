#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QSize>

// One decoded HAP frame: GPU block-compressed texture data, ready to upload as-is.
struct HapFrame {
    enum Kind {
        None,
        BC1,        // Hap: RGB DXT1
        BC3,        // Hap Alpha: RGBA DXT5
        BC7,        // Hap R: RGBA BPTC
        YCoCg,      // Hap Q: scaled YCoCg in DXT5, converted on the GPU
        YCoCgAlpha, // Hap Q Alpha: Hap Q plus a BC4 alpha image
    };
    Kind kind = None;
    QSize size;       // picture size; textures are padded to multiples of 4
    QByteArray color; // BC1/BC3/BC7 blocks
    QByteArray alpha; // BC4 blocks (YCoCgAlpha only)
    int index = -1;   // frame number in the file

    bool isValid() const { return kind != None; }
    QSize paddedSize() const { return QSize((size.width() + 3) & ~3, (size.height() + 3) & ~3); }
};
Q_DECLARE_METATYPE(HapFrame)

// Parses one HAP sample (as stored in the QuickTime file) for a picture of the given size.
bool decodeHapFrame(const QByteArray &sample, const QSize &size, HapFrame &out, QString *error = nullptr);
