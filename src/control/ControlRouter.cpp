#include "control/ControlRouter.h"

#include "model/Document.h"

#include <QNetworkDatagram>
#include <QSettings>
#include <QUdpSocket>

namespace {

enum class Kind { Trigger, Toggle, Continuous };

Kind kindOf(ControlAction a)
{
    switch (a) {
    case ControlAction::Master:
    case ControlAction::LayerOpacity:
        return Kind::Continuous;
    case ControlAction::Blackout:
    case ControlAction::OutputsLive:
    case ControlAction::LayerBypass:
    case ControlAction::LayerSolo:
        return Kind::Toggle;
    default:
        return Kind::Trigger;
    }
}

} // namespace

ControlRouter::ControlRouter(Document *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_midi(new MidiInput(this))
{
    connect(m_midi, &MidiInput::message, this, &ControlRouter::handleMidi);
    QSettings s;
    // MIDI devices default to all of them; OSC stays off until enabled (it opens a network port).
    m_midi->setOpenDevices(s.contains("midi/devices") ? s.value("midi/devices").toStringList()
                                                      : m_midi->availableDevices());
    if (s.value("osc/enabled", false).toBool())
        setOsc(true, s.value("osc/port", 7400).toInt(), nullptr, false);
}

ControlRouter::~ControlRouter() = default;

bool ControlRouter::setOsc(bool enabled, int port, QString *error, bool remember, bool localOnly)
{
    delete m_socket;
    m_socket = nullptr;
    bool ok = true;
    if (enabled) {
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(localOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4, quint16(port))) {
            if (error)
                *error = m_socket->errorString();
            qWarning("OSC: cannot listen on port %d: %s", port, qPrintable(m_socket->errorString()));
            delete m_socket;
            m_socket = nullptr;
            ok = false;
        } else {
            connect(m_socket, &QUdpSocket::readyRead, this, &ControlRouter::readDatagrams);
            qInfo("OSC listening on UDP port %d", oscPort());
        }
    }
    if (remember) {
        QSettings s;
        s.setValue("osc/enabled", enabled && ok);
        s.setValue("osc/port", port);
    }
    emit oscStateChanged();
    return ok;
}

int ControlRouter::oscPort() const
{
    return m_socket ? m_socket->localPort() : QSettings().value("osc/port", 7400).toInt();
}

void ControlRouter::setMidiDevices(const QStringList &names)
{
    m_midi->setOpenDevices(names);
    QSettings().setValue("midi/devices", names);
}

void ControlRouter::startLearn(int index)
{
    m_learn = index;
    emit learnStateChanged();
}

void ControlRouter::cancelLearn()
{
    m_learn = -1;
    emit learnStateChanged();
}

void ControlRouter::readDatagrams()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_socket->receiveDatagram();
        const QString from = d.senderAddress().toString();
        for (const OscMessage &m : Osc::parse(d.data()))
            handleOsc(m, from);
    }
}

void ControlRouter::handleOsc(const OscMessage &m, const QString &from)
{
    emit activity(QStringLiteral("OSC %1  %2").arg(from.isEmpty() ? QStringLiteral("local") : from, m.toString()));
    const QStringList p = m.address.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (p.size() < 2 || p[0] != QLatin1String("dh"))
        return;
    const bool hasArg = !m.args.isEmpty();
    const double value = m.number(1.0);
    // Toggles take an explicit state when given one, and flip otherwise.
    const Mode toggleMode = hasArg ? Mode::Value : Mode::Press;
    auto index = [&p](int i) { return i < p.size() ? p[i].toInt() - 1 : -1; };

    if (p[1] == QLatin1String("master") && hasArg)
        apply(ControlAction::Master, 0, 0, value, Mode::Value);
    else if (p[1] == QLatin1String("blackout"))
        apply(ControlAction::Blackout, 0, 0, value, toggleMode);
    else if (p[1] == QLatin1String("live"))
        apply(ControlAction::OutputsLive, 0, 0, value, toggleMode);
    else if (p[1] == QLatin1String("column") && p.value(3) == QLatin1String("trigger") && value != 0.0)
        apply(ControlAction::TriggerColumn, 0, index(2), 1.0, Mode::Press);
    else if (p[1] == QLatin1String("layer") && p.size() >= 4) {
        const int layer = index(2);
        const QString what = p[3];
        if (what == QLatin1String("clip") && p.value(5) == QLatin1String("trigger") && value != 0.0)
            apply(ControlAction::TriggerClip, layer, index(4), 1.0, Mode::Press);
        else if (what == QLatin1String("opacity") && hasArg)
            apply(ControlAction::LayerOpacity, layer, 0, value, Mode::Value);
        else if (what == QLatin1String("bypass"))
            apply(ControlAction::LayerBypass, layer, 0, value, toggleMode);
        else if (what == QLatin1String("solo"))
            apply(ControlAction::LayerSolo, layer, 0, value, toggleMode);
        else if (what == QLatin1String("clear") && value != 0.0)
            apply(ControlAction::ClearLayer, layer, 0, 1.0, Mode::Press);
    }
}

void ControlRouter::handleMidi(const MidiMessage &m)
{
    emit activity(QStringLiteral("MIDI %1").arg(m.toString()));
    if (!m.isNoteOn() && !m.isNoteOff() && !m.isCC())
        return;

    QVector<ControlMapping> &controls = m_doc->project().controls;
    if (m_learn >= 0 && (m.isNoteOn() || m.isCC())) {
        if (m_learn < controls.size()) {
            ControlMapping &c = controls[m_learn];
            c.midiSet = true;
            c.midiIsCC = m.isCC();
            c.midiChannel = m.channel();
            c.midiNumber = m.data1;
            const int index = m_learn;
            m_learn = -1;
            m_doc->notifyChanged(true);
            emit learned(index);
            emit learnStateChanged();
        }
        return;
    }

    const int ccKey = m.channel() << 8 | m.data1;
    const int previous = m.isCC() ? m_lastCC.value(ccKey, 0) : 0;
    if (m.isCC())
        m_lastCC.insert(ccKey, m.data2);

    // Copy: an action may change the project (and the mapping list with it).
    const QVector<ControlMapping> mappings = controls;
    for (const ControlMapping &c : mappings) {
        if (!c.midiSet || c.midiChannel != m.channel() || c.midiNumber != m.data1 || c.midiIsCC != m.isCC())
            continue;
        const Kind kind = kindOf(c.action);
        if (m.isCC()) {
            const double v = m.data2 / 127.0;
            if (kind == Kind::Trigger) {
                if (m.data2 >= 64 && previous < 64) // fire once as the control goes past halfway
                    apply(c.action, c.layer, c.column, 1.0, Mode::Press);
            } else {
                apply(c.action, c.layer, c.column, v, Mode::Value);
            }
        } else if (m.isNoteOn()) {
            if (kind == Kind::Continuous)
                apply(c.action, c.layer, c.column, m.data2 / 127.0, Mode::Value);
            else
                apply(c.action, c.layer, c.column, 1.0, Mode::Press); // a press flips a toggle
        }
    }
}

void ControlRouter::apply(ControlAction action, int layer, int column, double value, Mode mode)
{
    const Project &p = m_doc->project();
    const bool layerOk = layer >= 0 && layer < p.layers.size();
    const bool on = value >= 0.5;
    switch (action) {
    case ControlAction::Master:
        m_doc->setMaster(value);
        break;
    case ControlAction::Blackout:
        m_doc->setBlackout(mode == Mode::Press ? !p.blackout : on);
        break;
    case ControlAction::OutputsLive:
        emit liveRequested(mode == Mode::Press ? -1 : (on ? 1 : 0));
        break;
    case ControlAction::TriggerColumn:
        m_doc->triggerColumn(column);
        break;
    case ControlAction::TriggerClip:
        m_doc->triggerClip(layer, column);
        break;
    case ControlAction::ClearLayer:
        m_doc->clearLayer(layer);
        break;
    case ControlAction::LayerOpacity:
        if (layerOk)
            m_doc->setLayerPlayback(layer, value, p.layers[layer].bypass, p.layers[layer].solo);
        break;
    case ControlAction::LayerBypass:
        if (layerOk) {
            const Layer &l = p.layers[layer];
            m_doc->setLayerPlayback(layer, l.opacity, mode == Mode::Press ? !l.bypass : on, l.solo);
        }
        break;
    case ControlAction::LayerSolo:
        if (layerOk) {
            const Layer &l = p.layers[layer];
            m_doc->setLayerPlayback(layer, l.opacity, l.bypass, mode == Mode::Press ? !l.solo : on);
        }
        break;
    }
}

QString ControlRouter::oscReference()
{
    return QStringLiteral(
        "<b>OSC addresses</b> (layers and columns count from 1; layer 1 is the bottom layer)<br>"
        "<tt>/dh/master</tt> f 0..1 &nbsp; <tt>/dh/blackout</tt> [0|1] &nbsp; <tt>/dh/live</tt> [0|1]<br>"
        "<tt>/dh/column/3/trigger</tt><br>"
        "<tt>/dh/layer/2/clip/4/trigger</tt><br>"
        "<tt>/dh/layer/2/opacity</tt> f 0..1 &nbsp; <tt>/dh/layer/2/bypass</tt> [0|1] &nbsp; "
        "<tt>/dh/layer/2/solo</tt> [0|1] &nbsp; <tt>/dh/layer/2/clear</tt><br>"
        "Toggles without an argument flip. Triggers ignore 0 (button release).");
}
