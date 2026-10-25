#pragma once

#include "control/Midi.h"
#include "control/Osc.h"
#include "model/Project.h"

#include <QHash>
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
//   /dh/layer/<l>/clear
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

    static QString oscReference();

signals:
    void activity(const QString &line);  // for the monitor
    void learned(int index);
    void learnStateChanged();
    void liveRequested(int state);       // -1 toggle, 0 off, 1 on
    void oscStateChanged();

private:
    enum class Mode { Press, Value };
    void apply(ControlAction action, int layer, int column, double value, Mode mode);
    void readDatagrams();

    Document *m_doc;
    QUdpSocket *m_socket = nullptr;
    MidiInput *m_midi;
    int m_learn = -1;
    QHash<int, int> m_lastCC; // (channel << 8 | controller) -> last value, for trigger edges
};
