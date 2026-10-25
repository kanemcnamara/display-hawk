#include "control/ControlRouter.h"

#include "control/ArtNet.h"
#include "model/Document.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
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
    // Art-Net follows the project: on while its DMX patch is enabled.
    connect(m_doc, &Document::changed, this, &ControlRouter::syncArtNet);
    syncArtNet();
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

// ---------------------------------------------------------------- Art-Net

void ControlRouter::setArtNetLocalOnly(bool localOnly)
{
    m_artnetLocalOnly = localOnly;
    delete m_artnet;
    m_artnet = nullptr;
    syncArtNet();
}

void ControlRouter::syncArtNet()
{
    const bool wanted = m_doc->project().dmx.enabled;
    if (wanted == (m_artnet != nullptr))
        return;
    if (!wanted) {
        delete m_artnet;
        m_artnet = nullptr;
        m_dmxPrev.clear();
        qInfo("Art-Net input off");
        emit oscStateChanged();
        return;
    }
    m_artnet = new QUdpSocket(this);
    // Shared, so other Art-Net software on this machine can listen too.
    if (!m_artnet->bind(m_artnetLocalOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4, ArtNet::kPort,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        const QString err = m_artnet->errorString();
        qWarning("Art-Net: cannot listen on UDP %d: %s", ArtNet::kPort, qPrintable(err));
        emit activity(QStringLiteral("Art-Net: cannot listen on UDP %1: %2").arg(ArtNet::kPort).arg(err));
        delete m_artnet;
        m_artnet = nullptr;
    } else {
        connect(m_artnet, &QUdpSocket::readyRead, this, &ControlRouter::readArtNet);
        qInfo("Art-Net listening on UDP %d", ArtNet::kPort);
    }
    emit oscStateChanged();
}

void ControlRouter::readArtNet()
{
    while (m_artnet && m_artnet->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_artnet->receiveDatagram();
        handleArtNet(d.data(), d.senderAddress());
    }
}

void ControlRouter::handleArtNet(const QByteArray &packet, const QHostAddress &from)
{
    const quint16 op = ArtNet::opcode(packet);
    if (op == ArtNet::kOpPoll && m_artnet) {
        // Answer discovery from the interface on the poller's subnet, so consoles list us.
        QHostAddress self = QHostAddress::LocalHost;
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces())
            for (const QNetworkAddressEntry &e : iface.addressEntries())
                if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.ip().isLoopback()
                    && from.isInSubnet(e.ip(), e.prefixLength()))
                    self = e.ip();
        const DmxPatch &patch = m_doc->project().dmx;
        const QByteArray reply = ArtNet::buildPollReply(
            self, patch.universe, QStringLiteral("Display Hawk"), QStringLiteral("Display Hawk media server"),
            QStringLiteral("#0001 [%1] DMX %2 at %3").arg(m_dmxPackets, 4, 10, QLatin1Char('0'))
                .arg(ArtNet::universeText(patch.universe)).arg(patch.address));
        m_artnet->writeDatagram(reply, from, ArtNet::kPort);
        return;
    }
    ArtNet::Dmx dmx;
    if (op == ArtNet::kOpDmx && ArtNet::parseDmx(packet, dmx))
        handleDmx(dmx.universe, dmx.data, from.toString());
}

void ControlRouter::handleDmx(int universe, const QByteArray &data, const QString &from)
{
    const DmxPatch patch = m_doc->project().dmx;
    if (!patch.enabled || universe != patch.universe)
        return;
    ++m_dmxPackets;
    // A changed patch starts from a fresh baseline: old values belong to other channels.
    const quint64 key = quint64(patch.universe) << 32 | quint64(patch.address) << 16 | quint64(patch.layers);
    if (key != m_dmxPatchKey)
        m_dmxPrev.clear();
    m_dmxPatchKey = key;
    // A desk appearing (or returning after 10 s of silence) sets faders but fires no triggers: its
    // current clip and column channels become the starting point. Some desks only send on change
    // plus a keep-alive every ~4 s, so the silence window must be longer than that.
    const bool baseline = m_dmxPrev.isEmpty() || !m_dmxClock.isValid() || m_dmxClock.elapsed() > 10000;
    m_dmxClock.start();
    m_dmxFrom = from;

    auto value = [&](const QByteArray &d, int ch) {
        const int i = patch.address - 1 + ch;
        return i < d.size() ? int(quint8(d[i])) : -1;
    };
    QStringList changes;
    const QStringList names = dmxChannelNames(patch);
    auto changed = [&](int ch) {
        const int now = value(data, ch);
        if (now < 0 || (!m_dmxPrev.isEmpty() && value(m_dmxPrev, ch) == now))
            return false;
        changes << QStringLiteral("%1=%2").arg(names.value(ch)).arg(now);
        return true;
    };

    // Value changes only, so the desk and the Display Hawk UI can both be used.
    if (changed(0))
        apply(ControlAction::Master, 0, 0, value(data, 0) / 255.0, Mode::Value);
    if (changed(1))
        apply(ControlAction::Blackout, 0, 0, value(data, 1) / 255.0, Mode::Value);
    if (changed(2) && !baseline && value(data, 2) > 0)
        apply(ControlAction::TriggerColumn, 0, value(data, 2) - 1, 1.0, Mode::Press);
    for (int l = 0; l < patch.layers; ++l) {
        const int base = 3 + 3 * l;
        if (changed(base) && !baseline) {
            const int v = value(data, base);
            if (v == 0)
                apply(ControlAction::ClearLayer, l, 0, 1.0, Mode::Press);
            else
                apply(ControlAction::TriggerClip, l, v - 1, 1.0, Mode::Press);
        }
        if (changed(base + 1))
            apply(ControlAction::LayerOpacity, l, 0, value(data, base + 1) / 255.0, Mode::Value);
        if (changed(base + 2))
            apply(ControlAction::LayerBypass, l, 0, value(data, base + 2) / 255.0, Mode::Value);
    }
    if (!changes.isEmpty())
        emit activity(QStringLiteral("DMX %1 from %2%3: %4")
                          .arg(ArtNet::universeText(universe), from,
                               baseline ? QStringLiteral(" (first packet: faders only)") : QString(),
                               changes.join(QStringLiteral(", "))));
    m_dmxPrev = data;
}

QString ControlRouter::artNetStatus() const
{
    const DmxPatch &patch = m_doc->project().dmx;
    if (!patch.enabled)
        return tr("Art-Net input is off.");
    if (!m_artnet)
        return tr("Not listening (UDP %1 unavailable).").arg(ArtNet::kPort);
    if (!m_dmxClock.isValid())
        return tr("Listening on UDP %1, waiting for universe %2.").arg(ArtNet::kPort).arg(ArtNet::universeText(patch.universe));
    const qint64 age = m_dmxClock.elapsed();
    return tr("Receiving universe %1 from %2 (%3 packets, last %4 ms ago).")
        .arg(ArtNet::universeText(patch.universe), m_dmxFrom)
        .arg(m_dmxPackets)
        .arg(age);
}

QStringList ControlRouter::dmxChannelNames(const DmxPatch &patch)
{
    QStringList names{QStringLiteral("Master"), QStringLiteral("Blackout"), QStringLiteral("Column")};
    for (int l = 1; l <= patch.layers; ++l)
        names << QStringLiteral("L%1 clip").arg(l) << QStringLiteral("L%1 opacity").arg(l)
              << QStringLiteral("L%1 bypass").arg(l);
    return names;
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
