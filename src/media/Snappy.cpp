#include "media/Snappy.h"

#include <cstring>

// Format: a varint with the uncompressed length, then elements whose tag's low two bits select a
// literal (0) or a back-reference copy with a 1, 2 or 4 byte offset (1, 2, 3).
bool snappyUncompress(const char *data, qsizetype size, QByteArray &out)
{
    const auto *in = reinterpret_cast<const unsigned char *>(data);
    qsizetype pos = 0;
    quint64 length = 0;
    for (int shift = 0;; shift += 7) {
        if (pos >= size || shift > 35)
            return false;
        const unsigned char b = in[pos++];
        length |= quint64(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
    }
    if (length > (1ull << 31))
        return false;
    out.resize(qsizetype(length));
    char *o = out.data();
    qsizetype op = 0;
    const qsizetype olen = qsizetype(length);

    auto le = [&](int bytes, quint32 &value) {
        if (pos + bytes > size)
            return false;
        value = 0;
        for (int i = 0; i < bytes; ++i)
            value |= quint32(in[pos + i]) << (8 * i);
        pos += bytes;
        return true;
    };
    auto copy = [&](quint32 offset, qsizetype len) {
        if (offset == 0 || offset > quint32(op) || op + len > olen)
            return false;
        // Byte by byte: a copy may overlap its own output (run-length style).
        const char *from = o + op - offset;
        for (qsizetype i = 0; i < len; ++i)
            o[op + i] = from[i];
        op += len;
        return true;
    };

    while (pos < size) {
        const unsigned char tag = in[pos++];
        quint32 v = 0;
        switch (tag & 3) {
        case 0: { // literal
            qsizetype len = tag >> 2;
            if (len >= 60) {
                if (!le(int(len) - 59, v))
                    return false;
                len = v;
            }
            len += 1;
            if (pos + len > size || op + len > olen)
                return false;
            std::memcpy(o + op, in + pos, size_t(len));
            pos += len;
            op += len;
            break;
        }
        case 1: { // copy, 11-bit offset
            if (pos >= size)
                return false;
            const quint32 offset = (quint32(tag >> 5) << 8) | in[pos++];
            if (!copy(offset, ((tag >> 2) & 7) + 4))
                return false;
            break;
        }
        case 2: // copy, 16-bit offset
            if (!le(2, v) || !copy(v, (tag >> 2) + 1))
                return false;
            break;
        case 3: // copy, 32-bit offset
            if (!le(4, v) || !copy(v, (tag >> 2) + 1))
                return false;
            break;
        }
    }
    return op == olen;
}
