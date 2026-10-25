#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

// Streaming ACN (ANSI E1.31): DMX over UDP port 5568, multicast 239.255.<hi>.<lo> per universe.
namespace Sacn {

constexpr quint16 kPort = 5568;

struct Packet {
    int universe = 0;     // 1..63999
    int priority = 100;   // 0..200, higher wins
    int sequence = 0;
    bool preview = false;    // visualiser data: not for live output
    bool terminated = false; // the source is stopping
    QString sourceName;
    QByteArray cid;          // 16-byte component id of the source
    QByteArray data;         // DMX channel values (start code removed)
};

// Parses an E1.31 data packet (null start code only). Returns false for anything else.
bool parse(const QByteArray &datagram, Packet &out);

QByteArray build(int universe, const QByteArray &data, int priority = 100, int sequence = 0,
                 const QString &sourceName = QStringLiteral("Display Hawk"), bool terminated = false);

QHostAddress multicastGroup(int universe);

} // namespace Sacn
