#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QVariant>
#include <QVector>

class ControlRouter;
class Document;
class MidiOutput;
class QTimer;
struct Project;

// Sends the show's state back to controllers, so they show what is going on:
//  - MIDI: every learned control gets a note velocity / CC value for its state (clip empty, loaded or
//    playing; toggles on/off; fader positions for motor faders and LED rings; tap tempo flashes on
//    the beat), using the project's feedback values, to the chosen MIDI outputs.
//  - OSC: the same addresses ControlRouter accepts (/dh/master, /dh/layer/1/clip/2/trigger, ...),
//    plus layer and clip names, to the clients that send us OSC (at the feedback port) or a fixed
//    host. Only changes are sent; a new client gets everything.
class ControlFeedback : public QObject
{
    Q_OBJECT
public:
    ControlFeedback(Document *doc, ControlRouter *router, QObject *parent = nullptr);

    MidiOutput *midiOutput() const { return m_midiOut; }
    // Chosen outputs (remembered). Until chosen, outputs named like the open MIDI inputs are used.
    void setMidiOutputs(const QStringList &names);
    bool midiOutputsChosen() const;

    // OSC feedback: on/off, the port clients listen on, and a fixed host (empty = reply to clients).
    void setOsc(bool enabled, int port, const QString &host, bool remember = true);
    bool oscEnabled() const { return m_oscEnabled; }
    int oscPort() const { return m_oscPort; }
    QString oscHost() const { return m_oscHost; }

    // State the document does not hold.
    void setLive(bool live);
    void beat(int beatInBar);

    // What each learned MIDI control should show: key (cc << 16 | channel << 8 | number) -> 0..127.
    static QHash<int, int> midiState(const Project &p, bool live, bool beatOn);
    // The OSC state, address -> value (float or string).
    static QVector<QPair<QString, QVariant>> oscState(const Project &p, bool live);

    // Sends everything again (after reconnecting a controller).
    void resendAll();

private:
    void schedule();
    void refresh();
    void syncDefaultOutputs();
    void sendOscTo(const QVector<QPair<QString, QVariant>> &messages, const QHostAddress &host);
    QList<QHostAddress> oscTargets() const;

    Document *m_doc;
    ControlRouter *m_router;
    MidiOutput *m_midiOut;
    QTimer *m_timer;
    bool m_live = false;
    bool m_beatOn = false;
    QHash<int, int> m_midiSent;
    QHash<QString, QVariant> m_oscSent;
    bool m_oscEnabled = false;
    int m_oscPort = 9000;
    QString m_oscHost;
};
