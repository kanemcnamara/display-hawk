#include "control/Sacn.h"

#include <QtEndian>
#include <cstring>

namespace Sacn {

namespace {
// Root layer packet identifier "ASC-E1.17\0\0\0".
const char kAcnId[12] = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', '\0', '\0', '\0'};
constexpr quint32 kVectorRootData = 0x00000004;
constexpr quint32 kVectorFramingData = 0x00000002;
constexpr quint8 kVectorDmpSetProperty = 0x02;
constexpr int kDataOffset = 126; // first DMX value, after the start code at 125

quint16 be16(const QByteArray &d, int i) { return qFromBigEndian<quint16>(d.constData() + i); }
quint32 be32(const QByteArray &d, int i) { return qFromBigEndian<quint32>(d.constData() + i); }
void put16(QByteArray &d, int i, quint16 v) { qToBigEndian(v, d.data() + i); }
void put32(QByteArray &d, int i, quint32 v) { qToBigEndian(v, d.data() + i); }
} // namespace

bool parse(const QByteArray &d, Packet &out)
{
    if (d.size() < kDataOffset || std::memcmp(d.constData() + 4, kAcnId, 12) != 0)
        return false;
    if (be32(d, 18) != kVectorRootData || be32(d, 40) != kVectorFramingData || quint8(d[117]) != kVectorDmpSetProperty)
        return false;
    const int count = be16(d, 123); // property values, including the start code
    if (count < 1 || count > 513 || 125 + count > d.size() || quint8(d[125]) != 0) // only null start code (DMX levels)
        return false;
    out.cid = d.mid(22, 16);
    out.sourceName = QString::fromUtf8(d.constData() + 44, int(qstrnlen(d.constData() + 44, 64)));
    out.priority = quint8(d[108]);
    out.sequence = quint8(d[111]);
    const quint8 options = quint8(d[112]);
    out.preview = options & 0x80;
    out.terminated = options & 0x40;
    out.universe = be16(d, 113);
    out.data = d.mid(kDataOffset, count - 1);
    return out.universe >= 1 && out.universe <= 63999;
}

QByteArray build(int universe, const QByteArray &data, int priority, int sequence, const QString &sourceName,
                 bool terminated)
{
    const QByteArray dmx = data.left(512);
    const int count = int(dmx.size()) + 1;
    QByteArray d(125 + count, '\0');
    // Root layer
    put16(d, 0, 0x0010);
    put16(d, 2, 0x0000);
    std::memcpy(d.data() + 4, kAcnId, 12);
    put16(d, 16, quint16(0x7000 | (d.size() - 16)));
    put32(d, 18, kVectorRootData);
    std::memcpy(d.data() + 22, "DisplayHawkTest!", 16); // CID
    // Framing layer
    put16(d, 38, quint16(0x7000 | (d.size() - 38)));
    put32(d, 40, kVectorFramingData);
    const QByteArray name = sourceName.toUtf8().left(63);
    std::memcpy(d.data() + 44, name.constData(), size_t(name.size()));
    d[108] = char(priority);
    d[111] = char(sequence);
    d[112] = char(terminated ? 0x40 : 0x00);
    put16(d, 113, quint16(universe));
    // DMP layer
    put16(d, 115, quint16(0x7000 | (d.size() - 115)));
    d[117] = char(kVectorDmpSetProperty);
    d[118] = char(0xA1);
    put16(d, 119, 0x0000);
    put16(d, 121, 0x0001);
    put16(d, 123, quint16(count));
    d[125] = 0; // start code
    std::memcpy(d.data() + kDataOffset, dmx.constData(), size_t(dmx.size()));
    return d;
}

QHostAddress multicastGroup(int universe)
{
    return QHostAddress(QStringLiteral("239.255.%1.%2").arg((universe >> 8) & 0xFF).arg(universe & 0xFF));
}

} // namespace Sacn
