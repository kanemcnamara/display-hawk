#pragma once

#include "model/Project.h"
#include "render/RenderEngine.h"

#include <QObject>
#include <QVector>

class Document;
class TempoClock;
class QTimer;

// Combines everything that modulates the show at render time (audio-reactive links and LFOs)
// and hands the result to the renderer: opacity and master multipliers multiply, effect
// parameter offsets add up on top of each parameter's set value. Faders and undo stay untouched.
class ModulationHub : public QObject
{
    Q_OBJECT
public:
    ModulationHub(Document *doc, RenderEngine *engine, TempoClock *clock, QObject *parent = nullptr);

    // Audio-reactive contribution: master and per-layer multipliers, and effect parameter offsets
    // (EffectParamMod::value is the amount added). (1, {}, {}) = none.
    void setAudio(double master, const QVector<double> &layers, const QVector<EffectParamMod> &effectOffsets);

    // One LFO's value now, -1..1.
    static double wave(LfoShape shape, double cyclePosition);

private:
    void update();

    Document *m_doc;
    RenderEngine *m_engine;
    TempoClock *m_clock;
    QTimer *m_timer;
    double m_audioMaster = 1.0;
    QVector<double> m_audioLayers;
    QVector<EffectParamMod> m_audioEffects;
    bool m_sentAnything = false;
};
