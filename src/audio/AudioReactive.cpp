#include "audio/AudioReactive.h"

#include "model/Document.h"
#include "control/ModulationHub.h"
#include "render/Effects.h"
#include "render/RenderEngine.h"

#include <QMediaDevices>
#include <QSettings>

AudioReactive::AudioReactive(Document *doc, RenderEngine *engine, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_engine(engine)
    , m_analyzer(new AudioAnalyzer(this))
{
    m_deviceId = QSettings().value(QStringLiteral("audio/input")).toByteArray();
    connect(m_analyzer, &AudioAnalyzer::levelsChanged, this, &AudioReactive::apply);
    connect(m_doc, &Document::changed, this, &AudioReactive::sync);
    sync();
}

void AudioReactive::setInputDevice(const QByteArray &deviceId)
{
    m_deviceId = deviceId;
    QSettings().setValue(QStringLiteral("audio/input"), deviceId);
    if (m_analyzer->isRunning())
        restartInput();
}

void AudioReactive::restartInput()
{
    if (qEnvironmentVariableIsSet("DISPLAYHAWK_NO_AUDIO_INPUT")) { // self-test: levels are injected
        m_status = tr("Audio input disabled (DISPLAYHAWK_NO_AUDIO_INPUT)");
        emit statusChanged();
        return;
    }
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    for (const QAudioDevice &d : QMediaDevices::audioInputs())
        if (d.id() == m_deviceId)
            device = d;
    QString error;
    m_status = m_analyzer->start(device, &error) ? tr("Listening to %1").arg(device.description()) : error;
    emit statusChanged();
}

void AudioReactive::sync()
{
    const AudioReactiveSettings &ar = m_doc->project().audioReactive;
    m_analyzer->setSensitivity(ar.sensitivity);
    // Listening is needed for the links, and for the tempo when it follows the audio's beats.
    const bool wanted = ar.enabled || m_doc->project().tempoSource == TempoSource::AudioBeats;
    if (wanted && !m_analyzer->isRunning()) {
        restartInput();
    } else if (!wanted && m_analyzer->isRunning()) {
        m_analyzer->stop();
        m_status = tr("Off");
        emit statusChanged();
    }
    if (!ar.enabled && m_modulating) {
        send(1.0, {}, {}); // back to exactly the faders
        m_modulating = false;
    }
}

void AudioReactive::send(double master, const QVector<double> &layers, const QVector<EffectParamMod> &offsets)
{
    if (m_hub) {
        m_hub->setAudio(master, layers, offsets);
        return;
    }
    // Without a hub, offsets become absolute values here.
    QVector<EffectParamMod> absolute = offsets;
    const Project &p = m_doc->project();
    for (EffectParamMod &m : absolute) {
        const QVector<EffectInstance> &chain = m.layer < 0 ? p.compEffects : p.layers[m.layer].effects;
        m.value += effectParam(chain[m.effect], m.param);
    }
    m_engine->setModulation(master, layers, absolute);
}

void AudioReactive::apply(const AudioLevels &levels)
{
    const Project &p = m_doc->project();
    if (levels.beat)
        emit beatDetected(); // the tempo can follow the music even without audio-reactive links
    if (!p.audioReactive.enabled)
        return;
    m_beatPulse = levels.beat ? 1.0 : m_beatPulse * 0.82;
    auto value = [&](AudioSignal s) {
        switch (s) {
        case AudioSignal::Level: return levels.level;
        case AudioSignal::Bass: return levels.bass;
        case AudioSignal::Mid: return levels.mid;
        case AudioSignal::Treble: return levels.treble;
        case AudioSignal::Beat: return m_beatPulse;
        }
        return 0.0;
    };

    // Modulation: each link scales its target between (1 - amount) and 1; several links multiply.
    double master = 1.0;
    QVector<double> layers(p.layers.size(), 1.0);
    QVector<EffectParamMod> effects;
    bool beatColumn = false;
    QVector<int> beatClipLayers;
    for (const AudioLink &link : p.audioReactive.links) {
        if (audioTargetIsBeat(link.target)) {
            if (levels.beat) {
                if (link.target == AudioTarget::NextColumn)
                    beatColumn = true;
                else if (link.layer < p.layers.size())
                    beatClipLayers << link.layer;
            }
            continue;
        }
        if (link.target == AudioTarget::EffectParam) {
            // From the set value towards the parameter's maximum; several links add up.
            const QVector<EffectInstance> *chain = link.layer < 0 ? &p.compEffects
                                                   : (link.layer < p.layers.size() ? &p.layers[link.layer].effects : nullptr);
            if (!chain || link.effect < 0 || link.effect >= chain->size())
                continue;
            const EffectInstance &e = (*chain)[link.effect];
            const EffectDef *def = effectDef(e.type);
            if (!def || link.param < 0 || link.param >= def->params.size())
                continue;
            // An offset on top of the set value (the hub adds offsets and keeps the result in range).
            const double base = effectParam(e, link.param);
            const double push = link.amount * value(link.signal) * (def->params[link.param].max - base);
            effects.append({link.layer < 0 ? -1 : link.layer, link.effect, link.param, push});
            continue;
        }
        const double factor = 1.0 - link.amount + link.amount * value(link.signal);
        if (link.target == AudioTarget::Master)
            master *= factor;
        else if (link.layer >= 0 && link.layer < layers.size())
            layers[link.layer] *= factor;
    }
    send(master, layers, effects);
    m_modulating = true;

    // Beat triggers: step to the next column / clip that has something in it.
    if (beatColumn && p.columns > 0) {
        int current = -1;
        for (const Layer &l : p.layers)
            current = qMax(current, l.activeColumn);
        for (int step = 1; step <= p.columns; ++step) {
            const int c = (current + step) % p.columns;
            bool any = false;
            for (const Layer &l : p.layers)
                any = any || !l.clips.value(c).isEmpty();
            if (any) {
                m_doc->triggerColumn(c);
                break;
            }
        }
    }
    for (int li : std::as_const(beatClipLayers)) {
        const Layer &l = m_doc->project().layers[li];
        for (int step = 1; step <= l.clips.size(); ++step) {
            const int c = (qMax(-1, l.activeColumn) + step) % int(l.clips.size());
            if (!l.clips[c].isEmpty()) {
                m_doc->triggerClip(li, c);
                break;
            }
        }
    }
}
