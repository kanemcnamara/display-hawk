#include "control/Midi.h"

#include <chrono>

#include <QMetaObject>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <mmsystem.h>
#elif defined(Q_OS_MACOS)
#include <CoreMIDI/CoreMIDI.h>
#endif

QString MidiMessage::toString() const
{
    QString kind;
    switch (type()) {
    case 0x80: kind = QStringLiteral("Note off"); break;
    case 0x90: kind = data2 ? QStringLiteral("Note on") : QStringLiteral("Note off"); break;
    case 0xB0: kind = QStringLiteral("CC"); break;
    case 0xC0: kind = QStringLiteral("Program"); break;
    case 0xE0: kind = QStringLiteral("Pitch bend"); break;
    default: kind = QStringLiteral("0x%1").arg(status, 2, 16, QLatin1Char('0')); break;
    }
    return QStringLiteral("%1  %2 %3 = %4  ch %5").arg(device, kind).arg(data1).arg(data2).arg(channel() + 1);
}

namespace {

// Splits raw MIDI bytes into channel messages (running status supported, system messages skipped)
// and posts each to the GUI thread.
void dispatch(MidiInput *owner, const QString &device, const quint8 *bytes, int length, quint8 &running)
{
    for (int i = 0; i < length;) {
        quint8 b = bytes[i];
        if (b >= 0xF8) { // real-time (clock, start, stop) may arrive anywhere, even mid-message
            const qint64 ns = MidiInput::nowNs();
            QMetaObject::invokeMethod(owner, [owner, b, ns] { emit owner->realtime(b, ns); }, Qt::QueuedConnection);
            ++i;
            continue;
        }
        if (b >= 0xF0) { // other system messages: not used
            ++i;
            continue;
        }
        if (b & 0x80) {
            running = b;
            ++i;
        }
        if (!running)
            return;
        const int type = running & 0xF0;
        const int dataBytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (i + dataBytes > length)
            return;
        MidiMessage m{running, bytes[i], quint8(dataBytes == 2 ? bytes[i + 1] : 0), device};
        i += dataBytes;
        // Queued with the owner as context: Qt drops the call if the owner is gone by then.
        QMetaObject::invokeMethod(owner, [owner, m] { emit owner->message(m); }, Qt::QueuedConnection);
    }
}

} // namespace

qint64 MidiInput::nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if defined(Q_OS_WIN)

struct MidiInput::Backend {
    struct Open {
        HMIDIIN handle = nullptr;
        QString name;
        MidiInput *owner = nullptr;
        quint8 running = 0;
    };
    QList<Open *> open;

    static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR)
    {
        if (msg != MIM_DATA)
            return;
        auto *o = reinterpret_cast<Open *>(instance);
        const quint8 bytes[3] = {quint8(p1 & 0xFF), quint8((p1 >> 8) & 0xFF), quint8((p1 >> 16) & 0xFF)};
        dispatch(o->owner, o->name, bytes, 3, o->running);
    }

    static QStringList devices()
    {
        QStringList names;
        for (UINT i = 0, n = midiInGetNumDevs(); i < n; ++i) {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
                names << QString::fromWCharArray(caps.szPname);
        }
        return names;
    }

    void closeAll()
    {
        for (Open *o : std::as_const(open)) {
            midiInStop(o->handle);
            midiInReset(o->handle);
            midiInClose(o->handle);
            delete o;
        }
        open.clear();
    }

    void openByName(MidiInput *owner, const QStringList &names)
    {
        closeAll();
        const QStringList all = devices();
        for (int i = 0; i < all.size(); ++i) {
            if (!names.contains(all[i]))
                continue;
            auto *o = new Open{nullptr, all[i], owner, 0};
            if (midiInOpen(&o->handle, UINT(i), DWORD_PTR(&callback), DWORD_PTR(o), CALLBACK_FUNCTION)
                != MMSYSERR_NOERROR) {
                qWarning("MIDI: could not open %s", qPrintable(all[i]));
                delete o;
                continue;
            }
            midiInStart(o->handle);
            open << o;
        }
    }

    QStringList openNames() const
    {
        QStringList n;
        for (const Open *o : open)
            n << o->name;
        return n;
    }
};

#elif defined(Q_OS_MACOS)

struct MidiInput::Backend {
    struct Source {
        QString name;
        MidiInput *owner = nullptr;
        quint8 running = 0;
    };
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    QList<Source *> open;

    static QString endpointName(MIDIEndpointRef e)
    {
        CFStringRef name = nullptr;
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name) != noErr || !name)
            return QStringLiteral("MIDI source");
        const QString s = QString::fromCFString(name);
        CFRelease(name);
        return s;
    }

    static void readProc(const MIDIPacketList *list, void *, void *srcRef)
    {
        auto *src = static_cast<Source *>(srcRef);
        const MIDIPacket *p = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            dispatch(src->owner, src->name, p->data, p->length, src->running);
            p = MIDIPacketNext(p);
        }
    }

    static QStringList devices()
    {
        QStringList names;
        for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i)
            names << endpointName(MIDIGetSource(i));
        return names;
    }

    void closeAll()
    {
        if (port) {
            for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i)
                MIDIPortDisconnectSource(port, MIDIGetSource(i));
        }
        qDeleteAll(open);
        open.clear();
    }

    void openByName(MidiInput *owner, const QStringList &names)
    {
        closeAll();
        if (!client && MIDIClientCreate(CFSTR("Display Hawk"), nullptr, nullptr, &client) != noErr)
            return;
        if (!port && MIDIInputPortCreate(client, CFSTR("Display Hawk input"), &readProc, nullptr, &port) != noErr)
            return;
        for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i) {
            const MIDIEndpointRef e = MIDIGetSource(i);
            const QString name = endpointName(e);
            if (!names.contains(name))
                continue;
            auto *s = new Source{name, owner, 0};
            if (MIDIPortConnectSource(port, e, s) == noErr)
                open << s;
            else
                delete s;
        }
    }

    QStringList openNames() const
    {
        QStringList n;
        for (const Source *s : open)
            n << s->name;
        return n;
    }

    ~Backend()
    {
        closeAll();
        if (port)
            MIDIPortDispose(port);
        if (client)
            MIDIClientDispose(client);
    }
};

#else

// Other platforms: no MIDI yet.
struct MidiInput::Backend {
    static QStringList devices() { return {}; }
    void closeAll() {}
    void openByName(MidiInput *, const QStringList &) {}
    QStringList openNames() const { return {}; }
};

#endif

MidiInput::MidiInput(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Backend>())
{
    qRegisterMetaType<MidiMessage>();
}

MidiInput::~MidiInput()
{
    d->closeAll();
}

QStringList MidiInput::availableDevices() const
{
    return Backend::devices();
}

void MidiInput::setOpenDevices(const QStringList &names)
{
    d->openByName(this, names);
}

QStringList MidiInput::openDevices() const
{
    return d->openNames();
}
