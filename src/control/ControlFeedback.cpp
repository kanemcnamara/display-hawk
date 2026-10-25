#include "control/ControlFeedback.h"

#include "control/ControlRouter.h"
#include "control/Midi.h"
#include "control/Osc.h"
#include "model/Document.h"
#include "render/Effects.h"

#include <QSettings>
#include <QTimer>

namespace {

int midiKey(const ControlMapping &c)
{
    return (c.midiIsCC ? 1 : 0) << 16 | c.midiChannel << 8 | c.midiNumber;
}

int scaled(double v)
{
    return qBound(0, qRound(v * 127.0), 127);
}

// An effect parameter's value as 0..1 of its range (as the controller sent it), or -1.
double effectParamNormalized(const Project &p, int layer, int effect, int param)
{
    const QVector<EffectInstance> *chain =
        layer < 0 ? &p.compEffects : (layer < p.layers.size() ? &p.layers[layer].effects : nullptr);
    if (!chain || effect < 0 || effect >= chain->size())
        return -1.0;
    const EffectInstance &e = (*chain)[effect];
    const EffectDef *def = effectDef(e.type);
    if (!def || param < 0 || param >= def->params.size())
        return -1.0;
    const EffectParamDef &pd = def->params[param];
    const double v = param < e.params.size() ? e.params[param] : pd.def;
    return pd.max > pd.min ? qBound(0.0, (v - pd.min) / (pd.max - pd.min), 1.0) : 0.0;
}

QVariant flag(bool on)
{
    return QVariant(on ? 1.0 : 0.0);
}

} // namespace

ControlFeedback::ControlFeedback(Document *doc, ControlRouter *router, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_router(router)
    , m_midiOut(new MidiOutput(this))
    , m_timer(new QTimer(this))
{
    // Coalesced: a fader drag changes the document many times a frame.
    m_timer->setSingleShot(true);
    m_timer->setInterval(15);
    connect(m_timer, &QTimer::timeout, this, &ControlFeedback::refresh);
    connect(m_doc, &Document::changed, this, &ControlFeedback::schedule);
    connect(m_router, &ControlRouter::midiDevicesChanged, this, &ControlFeedback::syncDefaultOutputs);
    connect(m_router, &ControlRouter::oscClientAdded, this, [this](const QHostAddress &client) {
        if (m_oscEnabled && m_oscHost.isEmpty())
            sendOscTo(oscState(m_doc->project(), m_live), client); // a new client gets everything
    });

    QSettings s;
    if (s.contains("midi/outputs"))
        m_midiOut->setOpenDevices(s.value("midi/outputs").toStringList());
    else
        syncDefaultOutputs();
    m_oscEnabled = s.value("osc/feedback", false).toBool();
    m_oscPort = s.value("osc/feedbackPort", 9000).toInt();
    m_oscHost = s.value("osc/feedbackHost").toString();
    schedule();
}

bool ControlFeedback::midiOutputsChosen() const
{
    return QSettings().contains("midi/outputs");
}

void ControlFeedback::setMidiOutputs(const QStringList &names)
{
    QSettings().setValue("midi/outputs", names);
    m_midiOut->setOpenDevices(names);
    resendAll();
}

void ControlFeedback::syncDefaultOutputs()
{
    if (midiOutputsChosen())
        return;
    // Controllers usually name their input and output alike ("APC40 mkII" in and out).
    const QStringList inputs = m_router->midi()->openDevices();
    QStringList names;
    for (const QString &out : m_midiOut->availableDevices())
        if (inputs.contains(out))
            names << out;
    if (names != m_midiOut->openDevices()) {
        m_midiOut->setOpenDevices(names);
        resendAll();
    }
}

void ControlFeedback::setOsc(bool enabled, int port, const QString &host, bool remember)
{
    m_oscEnabled = enabled;
    m_oscPort = qBound(1, port, 65535);
    m_oscHost = host.trimmed();
    if (remember) {
        QSettings s;
        s.setValue("osc/feedback", enabled);
        s.setValue("osc/feedbackPort", m_oscPort);
        s.setValue("osc/feedbackHost", m_oscHost);
    }
    m_oscSent.clear();
    schedule();
}

void ControlFeedback::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    schedule();
}

void ControlFeedback::beat(int beatInBar)
{
    if (m_oscEnabled) // beats are events, not state: sent as they happen
        sendOscTo({{QStringLiteral("/dh/tempo/beat"), QVariant(double(beatInBar + 1))}}, QHostAddress());
    bool tapMapped = false;
    for (const ControlMapping &c : m_doc->project().controls)
        tapMapped = tapMapped || (c.midiSet && c.action == ControlAction::TapTempo);
    if (!tapMapped)
        return;
    m_beatOn = true;
    refresh();
    QTimer::singleShot(100, this, [this] {
        m_beatOn = false;
        refresh();
    });
}

void ControlFeedback::resendAll()
{
    m_midiSent.clear();
    m_oscSent.clear();
    schedule();
}

void ControlFeedback::schedule()
{
    if (!m_timer->isActive())
        m_timer->start();
}

QHash<int, int> ControlFeedback::midiState(const Project &p, bool live, bool beatOn)
{
    const MidiFeedback &f = p.midiFeedback;
    QHash<int, int> out;
    for (const ControlMapping &c : p.controls) {
        if (!c.midiSet)
            continue;
        const bool layerOk = c.layer >= 0 && c.layer < p.layers.size();
        const Layer *l = layerOk ? &p.layers[c.layer] : nullptr;
        auto onOff = [&f](bool on) { return on ? f.activeValue : f.offValue; };
        int v = f.offValue;
        switch (c.action) {
        case ControlAction::TriggerClip:
            if (l && c.column >= 0 && c.column < l->clips.size() && !l->clips[c.column].isEmpty())
                v = l->activeColumn == c.column ? f.activeValue : f.loadedValue;
            break;
        case ControlAction::TriggerColumn: {
            bool any = false, playing = false;
            for (const Layer &layer : p.layers) {
                any = any || (c.column < layer.clips.size() && !layer.clips[c.column].isEmpty());
                playing = playing || layer.activeColumn == c.column;
            }
            v = playing ? f.activeValue : (any ? f.loadedValue : f.offValue);
            break;
        }
        case ControlAction::ClearLayer:
            v = l && l->activeColumn >= 0 ? f.loadedValue : f.offValue; // something to clear
            break;
        case ControlAction::TapTempo:
            v = onOff(beatOn);
            break;
        case ControlAction::Blackout: v = onOff(p.blackout); break;
        case ControlAction::OutputsLive: v = onOff(live); break;
        case ControlAction::AudioMute: v = onOff(p.audioMuted); break;
        case ControlAction::LayerBypass: v = onOff(l && l->bypass); break;
        case ControlAction::LayerSolo: v = onOff(l && l->solo); break;
        // Continuous: the position, for motor faders and LED rings.
        case ControlAction::Master: v = scaled(p.master); break;
        case ControlAction::AudioMaster: v = scaled(p.audioMaster); break;
        case ControlAction::Crossfader: v = scaled(p.crossfader); break;
        case ControlAction::LayerOpacity: v = l ? scaled(l->opacity) : 0; break;
        case ControlAction::LayerVolume: v = l ? scaled(l->volume) : 0; break;
        case ControlAction::EffectParam: {
            const double n = effectParamNormalized(p, c.layer, c.column, c.param);
            v = n < 0 ? 0 : scaled(n);
            break;
        }
        case ControlAction::TempoResync:
        case ControlAction::CueGo:
        case ControlAction::CueBack:
            break;
        }
        // One control mapped to several actions shows the strongest.
        const int key = midiKey(c);
        out.insert(key, qMax(out.value(key, 0), qBound(0, v, 127)));
    }
    return out;
}

QVector<QPair<QString, QVariant>> ControlFeedback::oscState(const Project &p, bool live)
{
    QVector<QPair<QString, QVariant>> out;
    auto add = [&out](const QString &address, const QVariant &v) { out.append({address, v}); };
    add(QStringLiteral("/dh/master"), p.master);
    add(QStringLiteral("/dh/blackout"), flag(p.blackout));
    add(QStringLiteral("/dh/live"), flag(live));
    add(QStringLiteral("/dh/crossfader"), p.crossfader);
    add(QStringLiteral("/dh/audio/master"), p.audioMaster);
    add(QStringLiteral("/dh/audio/mute"), flag(p.audioMuted));
    add(QStringLiteral("/dh/tempo/bpm"), p.bpm);
    for (int c = 0; c < p.columns; ++c) {
        bool playing = false;
        for (const Layer &l : p.layers)
            playing = playing || l.activeColumn == c;
        add(QStringLiteral("/dh/column/%1/trigger").arg(c + 1), flag(playing));
    }
    for (int i = 0; i < p.layers.size(); ++i) {
        const Layer &l = p.layers[i];
        const QString base = QStringLiteral("/dh/layer/%1/").arg(i + 1);
        add(base + QStringLiteral("name"), l.name);
        add(base + QStringLiteral("opacity"), l.opacity);
        add(base + QStringLiteral("volume"), l.volume);
        add(base + QStringLiteral("bypass"), flag(l.bypass));
        add(base + QStringLiteral("solo"), flag(l.solo));
        for (int c = 0; c < l.clips.size(); ++c) {
            const QString clip = base + QStringLiteral("clip/%1/").arg(c + 1);
            add(clip + QStringLiteral("trigger"), flag(l.activeColumn == c));
            add(clip + QStringLiteral("name"), l.clips[c].isEmpty() ? QString() : l.clips[c].displayName());
        }
    }
    return out;
}

QList<QHostAddress> ControlFeedback::oscTargets() const
{
    if (!m_oscHost.isEmpty())
        return {QHostAddress(m_oscHost)};
    return m_router->oscClients();
}

void ControlFeedback::sendOscTo(const QVector<QPair<QString, QVariant>> &messages, const QHostAddress &host)
{
    const QList<QHostAddress> targets = host.isNull() ? oscTargets() : QList<QHostAddress>{host};
    if (targets.isEmpty() || messages.isEmpty())
        return;
    for (const auto &m : messages) {
        const QByteArray datagram = Osc::encode(OscMessage{m.first, {m.second}});
        for (const QHostAddress &t : targets)
            if (!t.isNull())
                m_router->sendOsc(datagram, t, quint16(m_oscPort));
    }
}

void ControlFeedback::refresh()
{
    const Project &p = m_doc->project();
    if (p.midiFeedback.enabled && !m_midiOut->openDevices().isEmpty()) {
        QHash<int, int> state = midiState(p, m_live, m_beatOn);
        // Controls no longer mapped go dark.
        for (auto it = m_midiSent.cbegin(); it != m_midiSent.cend(); ++it)
            if (!state.contains(it.key()) && it.value() != 0)
                state.insert(it.key(), 0);
        for (auto it = state.cbegin(); it != state.cend(); ++it) {
            if (m_midiSent.value(it.key(), -1) == it.value())
                continue;
            const bool cc = it.key() >> 16;
            const quint8 status = quint8((cc ? 0xB0 : 0x90) | ((it.key() >> 8) & 0x0F));
            m_midiOut->send(status, quint8(it.key() & 0x7F), quint8(it.value()));
        }
        m_midiSent = state;
    } else {
        m_midiSent.clear();
    }

    if (!m_oscEnabled)
        return;
    QVector<QPair<QString, QVariant>> changes;
    for (const auto &m : oscState(p, m_live)) {
        auto it = m_oscSent.find(m.first);
        if (it != m_oscSent.end() && *it == m.second)
            continue;
        m_oscSent.insert(m.first, m.second);
        changes.append(m);
    }
    sendOscTo(changes, QHostAddress());
}
