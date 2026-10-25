#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantList>

// Open Sound Control 1.0 messages: an address and typed arguments (int32 'i', float32 'f',
// string 's', blob 'b', true/false 'T'/'F', and 'd' doubles from some senders).
struct OscMessage {
    QString address;
    QVariantList args;

    // The first argument as a number (bool/int/float), or `fallback` when there is none.
    double number(double fallback = 1.0) const;
    QString toString() const;
};

namespace Osc {

// Parses a UDP datagram: a single message or a (possibly nested) bundle. Malformed parts are skipped.
QList<OscMessage> parse(const QByteArray &datagram);

// Encodes one message (ints, doubles as float32, bools as T/F, strings).
QByteArray encode(const OscMessage &message);

} // namespace Osc
