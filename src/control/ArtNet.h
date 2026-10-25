#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

// Art-Net 4 (UDP port 6454): enough to receive DMX and be discovered by consoles.
namespace ArtNet {

constexpr quint16 kPort = 6454;
constexpr quint16 kOpPoll = 0x2000;
constexpr quint16 kOpPollReply = 0x2100;
constexpr quint16 kOpDmx = 0x5000;

// The opcode of an Art-Net packet, or 0 when it is not one.
quint16 opcode(const QByteArray &packet);

struct Dmx {
    int universe = 0; // 15-bit Port-Address: Net << 8 | Sub-Net << 4 | Universe
    int sequence = 0;
    QByteArray data;  // up to 512 channel values
};
bool parseDmx(const QByteArray &packet, Dmx &out);

// An ArtDmx packet (used by the self-test and handy for scripts).
QByteArray buildDmx(int universe, const QByteArray &data, int sequence = 0);
QByteArray buildPoll();

// ArtPollReply announcing one output port (DMX out of the network into Display Hawk) on `universe`.
QByteArray buildPollReply(const QHostAddress &ip, int universe, const QString &shortName, const QString &longName,
                          const QString &report);

QString universeText(int universe); // "net.subnet.universe (n)"

} // namespace ArtNet
