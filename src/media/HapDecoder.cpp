#include "media/HapDecoder.h"

#include "media/Snappy.h"

#include <QString>
#include <QVector>

// HAP frame layout (https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md):
// a frame is a "section": a 4-byte header (24-bit little-endian size, 8-bit type), or an 8-byte
// header when those 24 bits are zero (then a 32-bit size follows). The type's high nibble is the
// second-stage compressor (0xA none, 0xB Snappy, 0xC chunked "complex"), the low nibble the
// texture format. Type 0x0D holds several images (Hap Q Alpha: YCoCg + alpha).

namespace {

struct Section {
    const unsigned char *data = nullptr;
    quint32 size = 0;
    quint8 type = 0;
    qsizetype headerSize = 0;
};

bool readSection(const unsigned char *p, qsizetype avail, Section &s)
{
    if (avail < 4)
        return false;
    s.size = quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16;
    s.type = p[3];
    s.headerSize = 4;
    if (s.size == 0) {
        if (avail < 8)
            return false;
        s.size = quint32(p[4]) | quint32(p[5]) << 8 | quint32(p[6]) << 16 | quint32(p[7]) << 24;
        s.headerSize = 8;
    }
    s.data = p + s.headerSize;
    return qsizetype(s.size) <= avail - s.headerSize;
}

quint32 le32(const unsigned char *p)
{
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool decompress(quint8 compressor, const unsigned char *data, qsizetype size, QByteArray &out)
{
    if (compressor == 0x0A) {
        out.append(reinterpret_cast<const char *>(data), size);
        return true;
    }
    if (compressor == 0x0B) {
        QByteArray chunk;
        if (!snappyUncompress(reinterpret_cast<const char *>(data), size, chunk))
            return false;
        out.append(chunk);
        return true;
    }
    return false;
}

// Decodes one texture section into raw blocks; reports its texture-format nibble.
bool decodeTexture(const Section &s, QByteArray &blocks, quint8 &format, QString *error)
{
    format = s.type & 0x0F;
    const quint8 compressor = s.type & 0xF0;
    blocks.clear();
    if (compressor == 0xA0 || compressor == 0xB0) {
        if (!decompress(compressor >> 4, s.data, s.size, blocks))
            return fail(error, QStringLiteral("corrupt Snappy data"));
        return true;
    }
    if (compressor != 0xC0)
        return fail(error, QStringLiteral("unknown compressor 0x%1").arg(compressor, 2, 16, QLatin1Char('0')));

    // Complex: a decode-instructions container, then the chunk data it describes.
    Section instr;
    if (!readSection(s.data, s.size, instr) || instr.type != 0x01)
        return fail(error, QStringLiteral("missing decode instructions"));
    QByteArray compressors;
    QVector<quint32> sizes, offsets;
    for (qsizetype p = 0; p < qsizetype(instr.size);) {
        Section sub;
        if (!readSection(instr.data + p, instr.size - p, sub))
            return fail(error, QStringLiteral("corrupt decode instructions"));
        if (sub.type == 0x02) {
            compressors = QByteArray(reinterpret_cast<const char *>(sub.data), sub.size);
        } else if (sub.type == 0x03 || sub.type == 0x04) {
            QVector<quint32> &v = sub.type == 0x03 ? sizes : offsets;
            for (quint32 i = 0; i + 4 <= sub.size; i += 4)
                v.append(le32(sub.data + i));
        }
        p += sub.headerSize + sub.size;
    }
    if (compressors.isEmpty() || sizes.size() != compressors.size()
        || (!offsets.isEmpty() && offsets.size() != sizes.size()))
        return fail(error, QStringLiteral("inconsistent chunk tables"));

    const unsigned char *frameData = instr.data + instr.size;
    const qsizetype frameSize = qsizetype(s.size) - instr.headerSize - instr.size;
    quint32 running = 0;
    for (int i = 0; i < compressors.size(); ++i) {
        const quint32 offset = offsets.isEmpty() ? running : offsets[i];
        running += sizes[i];
        if (qsizetype(offset) + qsizetype(sizes[i]) > frameSize)
            return fail(error, QStringLiteral("chunk outside frame"));
        if (!decompress(quint8(compressors[i]), frameData + offset, sizes[i], blocks))
            return fail(error, QStringLiteral("corrupt chunk %1").arg(i));
    }
    return true;
}

qsizetype expectedBytes(const QSize &size, int bytesPerBlock)
{
    return qsizetype((size.width() + 3) / 4) * ((size.height() + 3) / 4) * bytesPerBlock;
}

} // namespace

bool decodeHapFrame(const QByteArray &sample, const QSize &size, HapFrame &out, QString *error)
{
    out = HapFrame();
    out.size = size;
    const auto *p = reinterpret_cast<const unsigned char *>(sample.constData());
    Section top;
    if (!readSection(p, sample.size(), top))
        return fail(error, QStringLiteral("truncated frame"));

    if ((top.type & 0x0F) == 0x0D) {
        // Several images: Hap Q Alpha stores the YCoCg colour image and a BC4 alpha image.
        for (qsizetype q = 0; q < qsizetype(top.size);) {
            Section img;
            if (!readSection(top.data + q, top.size - q, img))
                return fail(error, QStringLiteral("corrupt multi-image frame"));
            QByteArray blocks;
            quint8 format = 0;
            if (!decodeTexture(img, blocks, format, error))
                return false;
            if (format == 0x0F)
                out.color = blocks;
            else if (format == 0x01)
                out.alpha = blocks;
            q += img.headerSize + img.size;
        }
        if (out.color.size() != expectedBytes(size, 16) || out.alpha.size() != expectedBytes(size, 8))
            return fail(error, QStringLiteral("unexpected Hap Q Alpha image sizes"));
        out.kind = HapFrame::YCoCgAlpha;
        return true;
    }

    quint8 format = 0;
    if (!decodeTexture(top, out.color, format, error))
        return false;
    int bytesPerBlock = 16;
    switch (format) {
    case 0x0B: out.kind = HapFrame::BC1; bytesPerBlock = 8; break;
    case 0x0E: out.kind = HapFrame::BC3; break;
    case 0x0F: out.kind = HapFrame::YCoCg; break;
    case 0x0C: out.kind = HapFrame::BC7; break;
    default:
        return fail(error, QStringLiteral("unsupported HAP texture format 0x%1").arg(format, 2, 16, QLatin1Char('0')));
    }
    if (out.color.size() != expectedBytes(size, bytesPerBlock)) {
        out.kind = HapFrame::None;
        return fail(error, QStringLiteral("texture data is %1 bytes, expected %2")
                               .arg(out.color.size())
                               .arg(expectedBytes(size, bytesPerBlock)));
    }
    return true;
}
