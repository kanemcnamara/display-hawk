#pragma once

#include <QObject>
#include <QStringList>

#include <memory>

struct MidiMessage {
    quint8 status = 0; // includes the channel in the low nibble
    quint8 data1 = 0;  // note or controller number
    quint8 data2 = 0;  // velocity or value
    QString device;

    int type() const { return status & 0xF0; }
    int channel() const { return status & 0x0F; }
    bool isNoteOn() const { return type() == 0x90 && data2 > 0; }
    bool isNoteOff() const { return type() == 0x80 || (type() == 0x90 && data2 == 0); }
    bool isCC() const { return type() == 0xB0; }
    QString toString() const;
};

// MIDI input from every enabled device, through the OS's own API (WinMM on Windows, CoreMIDI on
// macOS). Messages are delivered on the GUI thread.
class MidiInput : public QObject
{
    Q_OBJECT
public:
    explicit MidiInput(QObject *parent = nullptr);
    ~MidiInput() override;

    QStringList availableDevices() const;
    // Opens exactly these devices (by name); devices not present are ignored.
    void setOpenDevices(const QStringList &names);
    QStringList openDevices() const;

signals:
    void message(const MidiMessage &message);
    // System real-time bytes (0xF8 clock, 0xFA start, 0xFB continue, 0xFC stop) with the time they
    // arrived, in nanoseconds on the steady clock (taken on the driver thread, before queueing).
    void realtime(quint8 byte, qint64 ns);
    // MIDI timecode quarter frame (0xF1 and its data byte).
    void timecodeQuarterFrame(quint8 data);

public:
    static qint64 nowNs(); // the steady clock the timestamps above use
    struct Backend;

private:
    std::unique_ptr<Backend> d;
};

Q_DECLARE_METATYPE(MidiMessage)
