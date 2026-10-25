#include "control/ControlRouter.h"

#include "control/ArtNet.h"
#include "control/Sacn.h"
#include "model/Document.h"
#include "render/Effects.h"

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
    case ControlAction::LayerVolume:
    case ControlAction::AudioMaster:
    case ControlAction::EffectParam:
        return Kind::Continuous;
    case ControlAction::AudioMute:
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
    else if (p[1] == QLatin1String("audio") && p.value(2) == QLatin1String("master") && hasArg)
        apply(ControlAction::AudioMaster, 0, 0, value, Mode::Value);
    else if (p[1] == QLatin1String("audio") && p.value(2) == QLatin1String("mute"))
        apply(ControlAction::AudioMute, 0, 0, value, toggleMode);
    else if (p[1] == QLatin1String("tempo") && p.value(2) == QLatin1String("tap") && value != 0.0)
        apply(ControlAction::TapTempo, 0, 0, 1.0, Mode::Press);
    else if (p[1] == QLatin1String("tempo") && p.value(2) == QLatin1String("resync") && value != 0.0)
        apply(ControlAction::TempoResync, 0, 0, 1.0, Mode::Press);
    else if (p[1] == QLatin1String("tempo") && p.value(2) == QLatin1String("bpm") && hasArg)
        m_doc->setBpm(value); // in BPM, not 0..1
    else if (p[1] == QLatin1String("comp") && p.value(2) == QLatin1String("effect") && p.size() >= 5 && hasArg)
        apply(ControlAction::EffectParam, -1, index(3), value, Mode::Value, index(4));
    else if (p[1] == QLatin1String("column") && p.value(3) == QLatin1String("trigger") && value != 0.0)
        apply(ControlAction::TriggerColumn, 0, index(2), 1.0, Mode::Press);
    else if (p[1] == QLatin1String("layer") && p.size() >= 4) {
        const int layer = index(2);
        const QString what = p[3];
        if (what == QLatin1String("clip") && p.value(5) == QLatin1String("trigger") && value != 0.0)
            apply(ControlAction::TriggerClip, layer, index(4), 1.0, Mode::Press);
        else if (what == QLatin1String("clip") && p.value(5) == QLatin1String("trigger") && hasArg)
            m_doc->releaseClip(layer, index(4)); // 0 = released (piano clips)
        else if (what == QLatin1String("opacity") && hasArg)
            apply(ControlAction::LayerOpacity, layer, 0, value, Mode::Value);
        else if (what == QLatin1String("volume") && hasArg)
            apply(ControlAction::LayerVolume, layer, 0, value, Mode::Value);
        else if (what == QLatin1String("bypass"))
            apply(ControlAction::LayerBypass, layer, 0, value, toggleMode);
        else if (what == QLatin1String("solo"))
            apply(ControlAction::LayerSolo, layer, 0, value, toggleMode);
        else if (what == QLatin1String("clear") && value != 0.0)
            apply(ControlAction::ClearLayer, layer, 0, 1.0, Mode::Press);
        else if (what == QLatin1String("effect") && p.size() >= 6 && hasArg)
            apply(ControlAction::EffectParam, layer, index(4), value, Mode::Value, index(5));
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
                else if (m.data2 < 64 && previous >= 64 && c.action == ControlAction::TriggerClip)
                    m_doc->releaseClip(c.layer, c.column); // piano clips
            } else {
                apply(c.action, c.layer, c.column, v, Mode::Value, c.param);
            }
        } else if (m.isNoteOn()) {
            if (kind == Kind::Continuous)
                apply(c.action, c.layer, c.column, m.data2 / 127.0, Mode::Value, c.param);
            else
                apply(c.action, c.layer, c.column, 1.0, Mode::Press); // a press flips a toggle
        } else if (m.isNoteOff() && c.action == ControlAction::TriggerClip) {
            m_doc->releaseClip(c.layer, c.column); // piano clips play while the key is held
        }
    }
}

void ControlRouter::apply(ControlAction action, int layer, int column, double value, Mode mode, int param)
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
    case ControlAction::LayerVolume:
        if (layerOk)
            m_doc->setLayerVolume(layer, value);
        break;
    case ControlAction::AudioMaster:
        m_doc->setAudioMaster(value);
        break;
    case ControlAction::AudioMute:
        m_doc->setAudioMuted(mode == Mode::Press ? !p.audioMuted : on);
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
    case ControlAction::TapTempo:
        emit tapTempoRequested();
        break;
    case ControlAction::TempoResync:
        emit tempoResyncRequested();
        break;
    case ControlAction::EffectParam: {
        // 0..1 from the controller spans the parameter's range; choices step through.
        const QVector<EffectInstance> *chain = layer < 0 ? &p.compEffects : (layerOk ? &p.layers[layer].effects : nullptr);
        if (!chain || column < 0 || column >= chain->size())
            break;
        const EffectDef *def = effectDef((*chain)[column].type);
        if (!def || param < 0 || param >= def->params.size())
            break;
        const EffectParamDef &pd = def->params[param];
        double v = pd.min + qBound(0.0, value, 1.0) * (pd.max - pd.min);
        if (!pd.choices.isEmpty())
            v = qRound(v);
        m_doc->setEffectParam(layer, column, param, v);
        break;
    }
    }
}

// ---------------------------------------------------------------- Art-Net

void ControlRouter::setArtNetLocalOnly(bool localOnly)
{
    m_artnetLocalOnly = localOnly;
    closeDmxInputs();
    syncArtNet();
}

void ControlRouter::closeDmxInputs()
{
    if (!m_artnet && !m_sacn)
        return;
    delete m_artnet;
    m_artnet = nullptr;
    delete m_sacn;
    m_sacn = nullptr;
    m_sacnGroup = -1;
    m_dmxPrev.clear();
    qInfo("DMX input off");
    emit oscStateChanged();
}

void ControlRouter::syncArtNet()
{
    const DmxPatch &patch = m_doc->project().dmx;
    const bool artnet = patch.enabled && patch.protocol == DmxProtocol::ArtNet;
    const bool sacn = patch.enabled && patch.protocol == DmxProtocol::Sacn;
    if ((m_artnet && !artnet) || (m_sacn && !sacn))
        closeDmxInputs();

    if (sacn) {
        if (!m_sacn) {
            m_sacn = new QUdpSocket(this);
            if (!m_sacn->bind(m_artnetLocalOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4, Sacn::kPort,
                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
                const QString err = m_sacn->errorString();
                qWarning("sACN: cannot listen on UDP %d: %s", Sacn::kPort, qPrintable(err));
                emit activity(QStringLiteral("sACN: cannot listen on UDP %1: %2").arg(Sacn::kPort).arg(err));
                delete m_sacn;
                m_sacn = nullptr;
                emit oscStateChanged();
                return;
            }
            connect(m_sacn, &QUdpSocket::readyRead, this, &ControlRouter::readSacn);
            qInfo("sACN listening on UDP %d", Sacn::kPort);
            emit oscStateChanged();
        }
        // Join the universe's multicast group on every interface (unicast works regardless).
        if (m_sacnGroup != patch.universe && !m_artnetLocalOnly) {
            for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
                if (!(iface.flags() & QNetworkInterface::CanMulticast) || !(iface.flags() & QNetworkInterface::IsUp))
                    continue;
                if (m_sacnGroup > 0)
                    m_sacn->leaveMulticastGroup(Sacn::multicastGroup(m_sacnGroup), iface);
                m_sacn->joinMulticastGroup(Sacn::multicastGroup(patch.universe), iface);
            }
            m_sacnGroup = patch.universe;
        }
        return;
    }

    if (!artnet || m_artnet)
        return;
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

void ControlRouter::readSacn()
{
    while (m_sacn && m_sacn->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_sacn->receiveDatagram();
        handleSacn(d.data(), d.senderAddress());
    }
}

void ControlRouter::handleSacn(const QByteArray &datagram, const QHostAddress &from)
{
    Sacn::Packet p;
    if (!Sacn::parse(datagram, p) || p.preview || p.universe != m_doc->project().dmx.universe)
        return;
    // Several sources can send one universe; the highest priority heard in the last 3 s wins.
    if (!m_sacnPriorityClock.isValid() || m_sacnPriorityClock.elapsed() > 3000)
        m_sacnPriority = -1;
    if (p.priority < m_sacnPriority)
        return;
    if (p.terminated) {
        m_sacnPriority = -1; // let a lower-priority backup take over straight away
        return;
    }
    m_sacnPriority = p.priority;
    m_sacnPriorityClock.start();
    handleDmx(p.universe, p.data,
              QStringLiteral("%1 \"%2\" priority %3").arg(from.toString(), p.sourceName).arg(p.priority));
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
                          .arg(m_doc->project().dmx.protocol == DmxProtocol::Sacn
                                   ? QStringLiteral("sACN universe %1").arg(universe)
                                   : QStringLiteral("Art-Net ") + ArtNet::universeText(universe),
                               from,
                               baseline ? QStringLiteral(" (first packet: faders only)") : QString(),
                               changes.join(QStringLiteral(", "))));
    m_dmxPrev = data;
}

QString ControlRouter::artNetStatus() const
{
    const DmxPatch &patch = m_doc->project().dmx;
    const bool sacn = patch.protocol == DmxProtocol::Sacn;
    const int port = sacn ? Sacn::kPort : ArtNet::kPort;
    const QString universe = sacn ? QString::number(patch.universe) : ArtNet::universeText(patch.universe);
    if (!patch.enabled)
        return tr("DMX input is off.");
    if (!(sacn ? m_sacn : m_artnet))
        return tr("Not listening (UDP %1 unavailable).").arg(port);
    if (!m_dmxClock.isValid())
        return tr("Listening on UDP %1, waiting for universe %2.").arg(port).arg(universe);
    const qint64 age = m_dmxClock.elapsed();
    return tr("Receiving universe %1 from %2 (%3 packets, last %4 ms ago).")
        .arg(universe, m_dmxFrom)
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
        "<tt>/dh/layer/2/solo</tt> [0|1] &nbsp; <tt>/dh/layer/2/clear</tt> &nbsp; <tt>/dh/layer/2/volume</tt> f 0..1<br>"
        "<tt>/dh/audio/master</tt> f 0..1 &nbsp; <tt>/dh/audio/mute</tt> [0|1]<br>"
        "Toggles without an argument flip. Triggers ignore 0 (button release).");
}
