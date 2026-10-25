#include "control/ArtNet.h"

#include <cstring>

namespace ArtNet {

namespace {
const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
constexpr int kProtocolVersion = 14;

void putText(QByteArray &p, int offset, int size, const QString &text)
{
    const QByteArray latin = text.toLatin1().left(size - 1); // NUL-terminated, fixed size
    std::memcpy(p.data() + offset, latin.constData(), size_t(latin.size()));
}
} // namespace

quint16 opcode(const QByteArray &p)
{
    if (p.size() < 10 || std::memcmp(p.constData(), kId, 8) != 0)
        return 0;
    return quint16(quint8(p[8]) | quint8(p[9]) << 8); // little-endian
}

bool parseDmx(const QByteArray &p, Dmx &out)
{
    if (opcode(p) != kOpDmx || p.size() < 18)
        return false;
    out.sequence = quint8(p[12]);
    out.universe = (quint8(p[15]) & 0x7F) << 8 | quint8(p[14]); // Net, then SubUni
    const int length = quint8(p[16]) << 8 | quint8(p[17]);      // big-endian
    if (length < 2 || length > 512 || 18 + length > p.size())
        return false;
    out.data = p.mid(18, length);
    return true;
}

QByteArray buildDmx(int universe, const QByteArray &data, int sequence)
{
    QByteArray dmx = data.left(512);
    if (dmx.size() % 2)
        dmx += '\0'; // length must be even
    QByteArray p(18, '\0');
    std::memcpy(p.data(), kId, 8);
    p[8] = char(kOpDmx & 0xFF);
    p[9] = char(kOpDmx >> 8);
    p[10] = 0;
    p[11] = char(kProtocolVersion);
    p[12] = char(sequence);
    p[14] = char(universe & 0xFF);
    p[15] = char((universe >> 8) & 0x7F);
    p[16] = char(dmx.size() >> 8);
    p[17] = char(dmx.size() & 0xFF);
    return p + dmx;
}

QByteArray buildPoll()
{
    QByteArray p(14, '\0');
    std::memcpy(p.data(), kId, 8);
    p[8] = char(kOpPoll & 0xFF);
    p[9] = char(kOpPoll >> 8);
    p[11] = char(kProtocolVersion);
    return p;
}

QByteArray buildPollReply(const QHostAddress &ip, int universe, const QString &shortName, const QString &longName,
                          const QString &report)
{
    QByteArray p(239, '\0');
    std::memcpy(p.data(), kId, 8);
    p[8] = char(kOpPollReply & 0xFF);
    p[9] = char(kOpPollReply >> 8);
    const quint32 v4 = ip.toIPv4Address();
    p[10] = char(v4 >> 24);
    p[11] = char(v4 >> 16);
    p[12] = char(v4 >> 8);
    p[13] = char(v4);
    p[14] = char(kPort & 0xFF); // port, little-endian
    p[15] = char(kPort >> 8);
    p[16] = 0;                  // firmware version
    p[17] = 1;
    p[18] = char((universe >> 8) & 0x7F); // NetSwitch
    p[19] = char((universe >> 4) & 0x0F); // SubSwitch
    p[23] = char(0xD0);                   // Status1: indicators normal, port address set by network
    putText(p, 26, 18, shortName);
    putText(p, 44, 64, longName);
    putText(p, 108, 64, report);
    p[173] = 1;                           // NumPorts (lo)
    p[174] = char(0x80);                  // PortTypes[0]: can output DMX from the network (we consume it)
    p[182] = char(0x80);                  // GoodOutput[0]: data being transmitted
    p[190] = char(universe & 0x0F);       // SwOut[0]
    p[200] = 0x00;                        // Style: StNode
    std::memcpy(p.data() + 207, p.constData() + 10, 4); // BindIp: this node itself
    p[211] = 1;                           // BindIndex
    p[212] = 0x08;                        // Status2: supports 15-bit Port-Address
    return p;
}

QString universeText(int universe)
{
    return QStringLiteral("%1.%2.%3 (%4)")
        .arg((universe >> 8) & 0x7F)
        .arg((universe >> 4) & 0x0F)
        .arg(universe & 0x0F)
        .arg(universe);
}

} // namespace ArtNet
