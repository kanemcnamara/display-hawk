#pragma once

#include "audio/AudioAnalyzer.h"

#include <QObject>

#include "render/RenderEngine.h"

class Document;
class ModulationHub;
class RenderEngine;

// Drives the show from an analysed audio input according to the project's audio-reactive links:
// opacity/master modulation goes to the renderer as multipliers (faders and undo history are left
// alone); beat links trigger columns or clips.
class AudioReactive : public QObject
{
    Q_OBJECT
public:
    AudioReactive(Document *doc, RenderEngine *engine, QObject *parent = nullptr);

    AudioAnalyzer *analyzer() const { return m_analyzer; }
    void setInputDevice(const QByteArray &deviceId); // empty = system default; remembered
    QByteArray inputDeviceId() const { return m_deviceId; }
    QString status() const { return m_status; }

    // Applies one frame of levels (from the analyser, or injected by tests).
    void apply(const AudioLevels &levels);
    // Modulation goes through the hub (mixed with LFOs) when set, else straight to the renderer.
    void setHub(ModulationHub *hub) { m_hub = hub; }

signals:
    void beatDetected(); // each detected beat (drives the tempo when following audio)

    void statusChanged();

private:
    void sync();
    void restartInput();

    Document *m_doc;
    RenderEngine *m_engine;
    AudioAnalyzer *m_analyzer;
    QByteArray m_deviceId;
    QString m_status;
    double m_beatPulse = 0.0; // "Beat" as a signal: jumps to 1 on a beat, decays quickly
    bool m_modulating = false;
    ModulationHub *m_hub = nullptr;
    void send(double master, const QVector<double> &layers, const QVector<EffectParamMod> &offsets);
};
