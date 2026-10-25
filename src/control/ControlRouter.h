#pragma once

#include "control/Midi.h"
#include "control/Osc.h"
#include "model/Project.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>

class Document;
class QUdpSocket;

// Turns OSC messages and MIDI messages into document actions (trigger clips/columns, master,
// blackout, layer opacity/bypass/solo, outputs live), and runs MIDI learn for control mappings.
//
// OSC addresses (layers and columns count from 1; layer 1 is the bottom layer):
//   /dh/master <0..1>              /dh/blackout [0|1]      /dh/live [0|1]
//   /dh/column/<c>/trigger         /dh/layer/<l>/clip/<c>/trigger
//   /dh/layer/<l>/opacity <0..1>   /dh/layer/<l>/bypass [0|1]   /dh/layer/<l>/solo [0|1]
//   /dh/layer/<l>/clear           /dh/layer/<l>/volume <0..1>
//   /dh/audio/master <0..1>        /dh/audio/mute [0|1]
// Toggles without an argument flip; triggers ignore a 0 argument (button release).
class ControlRouter : public QObject
{
    Q_OBJECT
public:
    explicit ControlRouter(Document *doc, QObject *parent = nullptr);
    ~ControlRouter() override;

    // OSC over UDP. port 0 picks a free port (see oscPort()). Remembered in the app settings
    // unless `remember` is false.
    // localOnly listens on 127.0.0.1 only (tests; avoids the OS firewall prompt).
    bool setOsc(bool enabled, int port, QString *error = nullptr, bool remember = true, bool localOnly = false);
    bool oscEnabled() const { return m_socket != nullptr; }
    int oscPort() const;

    MidiInput *midi() const { return m_midi; }
    void setMidiDevices(const QStringList &names); // opens them and remembers the choice

    // The next MIDI note or controller is assigned to mapping `index`.
    void startLearn(int index);
    void cancelLearn();
    int learningIndex() const { return m_learn; }

    void handleOsc(const OscMessage &message, const QString &from = QString());
    void handleMidi(const MidiMessage &message);

    // DMX input (Art-Net on UDP 6454 or sACN on UDP 5568) while the project's DMX patch is enabled.
    bool artNetListening() const { return m_artnet != nullptr; }
    bool sacnListening() const { return m_sacn != nullptr; }
    QString artNetStatus() const;
    QByteArray lastDmx() const { return m_dmxPrev; } // the patched universe, as last received
    void setArtNetLocalOnly(bool localOnly);         // tests: 127.0.0.1 only (Art-Net and sACN)
    void handleArtNet(const QByteArray &packet, const QHostAddress &from);
    void handleSacn(const QByteArray &packet, const QHostAddress &from);

    static QString oscReference();
    static QStringList dmxChannelNames(const DmxPatch &patch);

signals:
    void activity(const QString &line);  // for the monitor
    void learned(int index);
    void learnStateChanged();
    void liveRequested(int state);       // -1 toggle, 0 off, 1 on
    void oscStateChanged();

private:
    enum class Mode { Press, Value };
    void apply(ControlAction action, int layer, int column, double value, Mode mode, int param = 0);
    void readDatagrams();
    void syncArtNet();
    void readArtNet();
    void readSacn();
    void closeDmxInputs();
    void handleDmx(int universe, const QByteArray &data, const QString &from);

    Document *m_doc;
    QUdpSocket *m_socket = nullptr;
    MidiInput *m_midi;
    int m_learn = -1;
    QHash<int, int> m_lastCC; // (channel << 8 | controller) -> last value, for trigger edges

    QUdpSocket *m_artnet = nullptr;
    QUdpSocket *m_sacn = nullptr;
    int m_sacnGroup = -1;         // universe whose multicast group is joined
    int m_sacnPriority = -1;      // highest priority heard recently for our universe
    QElapsedTimer m_sacnPriorityClock;
    bool m_artnetLocalOnly = false;
    QByteArray m_dmxPrev;       // previous values of the patched universe (change detection)
    quint64 m_dmxPatchKey = 0;  // patch the previous values belong to
    QElapsedTimer m_dmxClock;   // since the last packet for our universe
    QString m_dmxFrom;
    qint64 m_dmxPackets = 0;
};
