#include "control/Osc.h"

#include <QtEndian>
#include <cstring>

namespace {

// OSC strings are NUL-terminated and padded to a multiple of 4 bytes.
bool readString(const QByteArray &d, qsizetype &pos, QString &out)
{
    const qsizetype end = d.indexOf('\0', pos);
    if (end < 0)
        return false;
    out = QString::fromUtf8(d.constData() + pos, end - pos);
    pos = (end + 4) & ~qsizetype(3);
    return pos <= d.size();
}

bool readInt(const QByteArray &d, qsizetype &pos, qint32 &out)
{
    if (pos + 4 > d.size())
        return false;
    out = qFromBigEndian<qint32>(d.constData() + pos);
    pos += 4;
    return true;
}

void parseInto(const QByteArray &d, QList<OscMessage> &out, int depth)
{
    if (depth > 8 || d.isEmpty())
        return;
    qsizetype pos = 0;
    if (d.startsWith("#bundle")) {
        pos = 16; // "#bundle\0" + 8-byte time tag (ignored: everything happens now)
        while (pos + 4 <= d.size()) {
            qint32 size = 0;
            if (!readInt(d, pos, size) || size < 0 || pos + size > d.size())
                return;
            parseInto(d.mid(pos, size), out, depth + 1);
            pos += size;
        }
        return;
    }

    OscMessage m;
    QString tags;
    if (!readString(d, pos, m.address) || !m.address.startsWith(QLatin1Char('/')))
        return;
    if (pos < d.size() && !readString(d, pos, tags))
        return;
    for (int i = 1; i < tags.size(); ++i) { // tags[0] is ','
        switch (tags[i].toLatin1()) {
        case 'i': {
            qint32 v = 0;
            if (!readInt(d, pos, v))
                return;
            m.args << v;
            break;
        }
        case 'f': {
            qint32 bits = 0;
            if (!readInt(d, pos, bits))
                return;
            float f;
            std::memcpy(&f, &bits, 4);
            m.args << double(f);
            break;
        }
        case 'd': {
            if (pos + 8 > d.size())
                return;
            const quint64 bits = qFromBigEndian<quint64>(d.constData() + pos);
            double v;
            std::memcpy(&v, &bits, 8);
            m.args << v;
            pos += 8;
            break;
        }
        case 's': {
            QString s;
            if (!readString(d, pos, s))
                return;
            m.args << s;
            break;
        }
        case 'b': {
            qint32 size = 0;
            if (!readInt(d, pos, size) || size < 0 || pos + size > d.size())
                return;
            m.args << d.mid(pos, size);
            pos += (size + 3) & ~3;
            break;
        }
        case 'T': m.args << true; break;
        case 'F': m.args << false; break;
        default: break; // N, I and unknown tags carry no data we use
        }
    }
    out.append(m);
}

void writeString(QByteArray &out, const QByteArray &s)
{
    out += s;
    out += '\0';
    while (out.size() % 4)
        out += '\0';
}

void writeInt(QByteArray &out, qint32 v)
{
    char buf[4];
    qToBigEndian(v, buf);
    out.append(buf, 4);
}

} // namespace

double OscMessage::number(double fallback) const
{
    if (args.isEmpty())
        return fallback;
    const QVariant &v = args.first();
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? 1.0 : 0.0;
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : fallback;
}

QString OscMessage::toString() const
{
    QStringList parts{address};
    for (const QVariant &a : args)
        parts << (a.typeId() == QMetaType::QString ? QStringLiteral("\"%1\"").arg(a.toString()) : a.toString());
    return parts.join(QLatin1Char(' '));
}

namespace Osc {

QList<OscMessage> parse(const QByteArray &datagram)
{
    QList<OscMessage> out;
    parseInto(datagram, out, 0);
    return out;
}

QByteArray encode(const OscMessage &message)
{
    QByteArray out;
    writeString(out, message.address.toUtf8());
    QByteArray tags(",");
    QByteArray data;
    for (const QVariant &a : message.args) {
        switch (a.typeId()) {
        case QMetaType::Bool:
            tags += a.toBool() ? 'T' : 'F';
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
            tags += 'i';
            writeInt(data, qint32(a.toLongLong()));
            break;
        case QMetaType::Double:
        case QMetaType::Float: {
            tags += 'f';
            const float f = a.toFloat();
            qint32 bits;
            std::memcpy(&bits, &f, 4);
            writeInt(data, bits);
            break;
        }
        default:
            tags += 's';
            writeString(data, a.toString().toUtf8());
            break;
        }
    }
    writeString(out, tags);
    return out + data;
}

} // namespace Osc
