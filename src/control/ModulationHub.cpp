#include "control/ModulationHub.h"

#include "control/TempoClock.h"
#include "model/Document.h"
#include "render/Effects.h"

#include <QTimer>
#include <QtMath>

ModulationHub::ModulationHub(Document *doc, RenderEngine *engine, TempoClock *clock, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_engine(engine)
    , m_clock(clock)
    , m_timer(new QTimer(this))
{
    // LFOs move every frame; at 60 Hz the renderer gets a fresh value for each frame.
    m_timer->setInterval(16);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &ModulationHub::update);
    m_timer->start();
}

void ModulationHub::setAudio(double master, const QVector<double> &layers, const QVector<EffectParamMod> &effectOffsets)
{
    m_audioMaster = master;
    m_audioLayers = layers;
    m_audioEffects = effectOffsets;
    update();
}

double ModulationHub::wave(LfoShape shape, double t)
{
    const double f = t - qFloor(t);
    switch (shape) {
    case LfoShape::Sine: return qSin(2.0 * M_PI * f);
    case LfoShape::Triangle: return 1.0 - 4.0 * qAbs(f - 0.5);
    case LfoShape::SawUp: return 2.0 * f - 1.0;
    case LfoShape::SawDown: return 1.0 - 2.0 * f;
    case LfoShape::Square: return f < 0.5 ? 1.0 : -1.0;
    case LfoShape::Random: { // a new random level every cycle, the same for the same cycle
        quint32 h = quint32(qint64(qFloor(t)) * 2654435761u) ^ 0x9E3779B9u;
        h ^= h >> 15;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        return (h % 20001) / 10000.0 - 1.0;
    }
    }
    return 0.0;
}

void ModulationHub::update()
{
    const Project &p = m_doc->project();
    double master = m_audioMaster;
    QVector<double> layers(p.layers.size(), 1.0);
    for (int i = 0; i < layers.size() && i < m_audioLayers.size(); ++i)
        layers[i] = m_audioLayers[i];
    QVector<EffectParamMod> offsets = m_audioEffects; // parameter units

    auto chainOf = [&p](int layer) -> const QVector<EffectInstance> * {
        if (layer < 0)
            return &p.compEffects;
        return layer < p.layers.size() ? &p.layers[layer].effects : nullptr;
    };
    const double beats = m_clock->beats();
    for (const Lfo &l : p.lfos) {
        if (!l.enabled || l.amount <= 0.0)
            continue;
        const double w = wave(l.shape, beats / qMax(0.0625, l.beats) + l.phase);
        if (l.target == AudioTarget::EffectParam) {
            // Swings the parameter around its set value, by up to half its range either way.
            const QVector<EffectInstance> *chain = chainOf(l.layer);
            if (!chain || l.effect < 0 || l.effect >= chain->size())
                continue;
            const EffectDef *def = effectDef((*chain)[l.effect].type);
            if (!def || l.param < 0 || l.param >= def->params.size())
                continue;
            const EffectParamDef &pd = def->params[l.param];
            offsets.append({l.layer < 0 ? -1 : l.layer, l.effect, l.param, w * l.amount * (pd.max - pd.min) / 2.0});
        } else {
            // Between (1 - amount) and 1, like audio links: the wave dips the fader, never boosts it.
            const double factor = 1.0 - l.amount * (1.0 - (w + 1.0) / 2.0);
            if (l.target == AudioTarget::Master)
                master *= factor;
            else if (l.layer >= 0 && l.layer < layers.size())
                layers[l.layer] *= factor;
        }
    }

    // Offsets for the same parameter add up; the result is the set value plus them, in range.
    QVector<EffectParamMod> effects;
    for (const EffectParamMod &o : std::as_const(offsets)) {
        EffectParamMod *same = nullptr;
        for (EffectParamMod &e : effects)
            if (e.layer == o.layer && e.effect == o.effect && e.param == o.param)
                same = &e;
        if (same)
            same->value += o.value;
        else
            effects.append(o);
    }
    for (EffectParamMod &e : effects) {
        const QVector<EffectInstance> *chain = chainOf(e.layer);
        const EffectDef *def = chain && e.effect < chain->size() ? effectDef((*chain)[e.effect].type) : nullptr;
        if (!def || e.param >= def->params.size())
            continue;
        const EffectParamDef &pd = def->params[e.param];
        e.value = qBound(pd.min, effectParam((*chain)[e.effect], e.param) + e.value, pd.max);
    }

    bool layersPlain = true;
    for (double f : std::as_const(layers))
        layersPlain = layersPlain && f == 1.0;
    const bool anything = master != 1.0 || !layersPlain || !effects.isEmpty();
    if (anything) {
        m_engine->setModulation(master, layers, effects);
        m_sentAnything = true;
    } else if (m_sentAnything) {
        m_engine->setModulation(1.0, {}, {}); // back to exactly the faders
        m_sentAnything = false;
    }
}
