#include "media/Snappy.h"

#include <cstring>
#include <vector>

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

namespace {

void putLiteral(QByteArray &out, const char *data, qsizetype length)
{
    while (length > 0) {
        const qsizetype n = qMin<qsizetype>(length, 1 << 16);
        const quint32 m = quint32(n - 1);
        if (m < 60) {
            out.append(char(m << 2));
        } else if (m < 256) {
            out.append(char(60 << 2));
            out.append(char(m));
        } else {
            out.append(char(61 << 2));
            out.append(char(m & 0xFF));
            out.append(char(m >> 8));
        }
        out.append(data, n);
        data += n;
        length -= n;
    }
}

void putCopy(QByteArray &out, quint32 offset, qsizetype length)
{
    // 2-byte-offset copies carry 1..64 bytes; long matches become several (none shorter than 4).
    while (length > 0) {
        qsizetype n = qMin<qsizetype>(length, 64);
        if (length - n > 0 && length - n < 4)
            n = length - 4;
        out.append(char(((n - 1) << 2) | 2));
        out.append(char(offset & 0xFF));
        out.append(char(offset >> 8));
        length -= n;
    }
}

} // namespace

QByteArray snappyCompress(const char *data, qsizetype size)
{
    QByteArray out;
    out.reserve(size + size / 6 + 32);
    for (quint64 v = quint64(size);; v >>= 7) { // varint: the uncompressed length
        if (v < 0x80) {
            out.append(char(v));
            break;
        }
        out.append(char((v & 0x7F) | 0x80));
    }
    const auto *in = reinterpret_cast<const unsigned char *>(data);
    constexpr int kHashBits = 14;
    std::vector<qint32> table(size_t(1) << kHashBits, -1);
    auto load32 = [in](qsizetype i) {
        quint32 v;
        std::memcpy(&v, in + i, 4);
        return v;
    };
    auto hash = [](quint32 v) { return (v * 0x1E35A7BDu) >> (32 - kHashBits); };
    qsizetype pos = 0, literalStart = 0, misses = 0;
    while (pos + 4 <= size) {
        const quint32 v = load32(pos);
        const quint32 h = hash(v);
        const qsizetype candidate = table[h];
        table[h] = qint32(pos);
        if (candidate >= 0 && pos - candidate < 65536 && load32(candidate) == v) {
            qsizetype length = 4;
            while (pos + length < size && in[candidate + length] == in[pos + length])
                ++length;
            putLiteral(out, data + literalStart, pos - literalStart);
            putCopy(out, quint32(pos - candidate), length);
            pos += length;
            literalStart = pos;
            misses = 0;
        } else {
            pos += 1 + (misses++ >> 5); // skip ahead faster through incompressible data
        }
    }
    putLiteral(out, data + literalStart, size - literalStart);
    return out;
}
