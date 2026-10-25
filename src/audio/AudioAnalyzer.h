#pragma once

#include <QAudioDevice>
#include <QObject>
#include <QVector>

#include <complex>

class QAudioSource;
class QIODevice;

// Levels from an audio input, each 0..1 after automatic gain: overall level, bass (20-250 Hz),
// mid (250 Hz-2 kHz) and treble (2-12 kHz), plus beat onsets detected on the bass.
struct AudioLevels {
    double level = 0.0;
    double bass = 0.0;
    double mid = 0.0;
    double treble = 0.0;
    bool beat = false; // a beat started in this analysis frame
};

// Analyses an audio input (microphone, line-in) in ~21 ms hops: a 1024-point FFT with a Hann
// window and 50% overlap at 48 kHz. Runs on the GUI thread; the work is tiny.
class AudioAnalyzer : public QObject
{
    Q_OBJECT
public:
    explicit AudioAnalyzer(QObject *parent = nullptr);
    ~AudioAnalyzer() override;

    bool start(const QAudioDevice &device, QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_source != nullptr; }
    QString deviceName() const { return m_deviceName; }

    void setSensitivity(double gain) { m_gain = gain; }
    AudioLevels levels() const { return m_levels; }

    // Feeds mono float samples directly (tests, or other sources). Emits levels() per frame.
    void process(const float *samples, int count, int sampleRate);

signals:
    void levelsChanged(const AudioLevels &levels);

private:
    void readInput();
    void analyse(int sampleRate);

    QAudioSource *m_source = nullptr;
    QIODevice *m_input = nullptr;
    QString m_deviceName;
    int m_channels = 1;
    int m_sampleRate = 48000;
    double m_gain = 1.0;

    QVector<float> m_buffer;          // mono samples waiting for analysis
    QVector<std::complex<float>> m_fft;
    QVector<float> m_window;

    // Adaptive normalisation per signal: slowly decaying peak; envelopes for smooth motion.
    double m_peak[4] = {1e-6, 1e-6, 1e-6, 1e-6};
    double m_env[4] = {0, 0, 0, 0};
    // Beat detection: bass energy against its average over the last second.
    QVector<double> m_bassHistory;
    int m_historyPos = 0;
    qint64 m_samplesSinceBeat = 1 << 30; // counted in samples, so injected audio is timed correctly
    AudioLevels m_levels;
};

Q_DECLARE_METATYPE(AudioLevels)
