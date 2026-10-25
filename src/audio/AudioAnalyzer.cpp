#include "audio/AudioAnalyzer.h"

#include <QAudioFormat>
#include <QAudioSource>
#include <QtMath>

namespace {

constexpr int kFftSize = 1024;
constexpr int kHop = kFftSize / 2;
constexpr int kHistory = 43; // ~1 s of hops at 48 kHz
constexpr int kMinBeatGapMs = 250;

// In-place iterative radix-2 FFT.
void fft(QVector<std::complex<float>> &a)
{
    const int n = int(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * float(M_PI) / float(len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace

AudioAnalyzer::AudioAnalyzer(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<AudioLevels>();
    m_window.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        m_window[i] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * i / (kFftSize - 1)));
    m_fft.resize(kFftSize);
    m_bassHistory.fill(0.0, kHistory);
}

AudioAnalyzer::~AudioAnalyzer()
{
    stop();
}

bool AudioAnalyzer::start(const QAudioDevice &device, QString *error)
{
    stop();
    if (device.isNull()) {
        if (error)
            *error = tr("No audio input device");
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(qBound(1, device.preferredFormat().channelCount(), 2));
    format.setSampleFormat(QAudioFormat::Float);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format))
            format.setSampleFormat(QAudioFormat::Int16);
    }
    m_channels = format.channelCount();
    m_sampleRate = format.sampleRate();
    m_source = new QAudioSource(device, format, this);
    m_source->setBufferSize(format.bytesForDuration(40000)); // 40 ms: responsive without dropouts
    m_input = m_source->start();
    if (!m_input) {
        if (error)
            *error = tr("Could not open %1").arg(device.description());
        delete m_source;
        m_source = nullptr;
        return false;
    }
    m_deviceName = device.description();
    connect(m_input, &QIODevice::readyRead, this, &AudioAnalyzer::readInput);
    qInfo("Audio-reactive input: %s, %d Hz, %d ch", qPrintable(m_deviceName), m_sampleRate, m_channels);
    return true;
}

void AudioAnalyzer::stop()
{
    if (m_source) {
        m_source->stop();
        delete m_source;
    }
    m_source = nullptr;
    m_input = nullptr;
    m_buffer.clear();
    m_levels = AudioLevels();
    emit levelsChanged(m_levels);
}

void AudioAnalyzer::readInput()
{
    const QByteArray data = m_input->readAll();
    const QAudioFormat f = m_source->format();
    const int frameBytes = f.bytesPerFrame();
    if (frameBytes <= 0)
        return;
    const int frames = int(data.size() / frameBytes);
    QVector<float> mono(frames);
    for (int i = 0; i < frames; ++i) {
        const char *frame = data.constData() + i * frameBytes;
        float sum = 0.0f;
        for (int c = 0; c < m_channels; ++c)
            sum += f.normalizedSampleValue(frame + c * f.bytesPerSample());
        mono[i] = sum / float(m_channels);
    }
    process(mono.constData(), frames, m_sampleRate);
}

void AudioAnalyzer::process(const float *samples, int count, int sampleRate)
{
    for (int i = 0; i < count; ++i)
        m_buffer.append(samples[i] * float(m_gain));
    while (m_buffer.size() >= kFftSize) {
        analyse(sampleRate);
        m_buffer.remove(0, kHop);
    }
}

void AudioAnalyzer::analyse(int sampleRate)
{
    double rms = 0.0;
    for (int i = 0; i < kFftSize; ++i) {
        rms += double(m_buffer[i]) * m_buffer[i];
        m_fft[i] = std::complex<float>(m_buffer[i] * m_window[i], 0.0f);
    }
    rms = std::sqrt(rms / kFftSize);
    fft(m_fft);

    auto band = [&](double lo, double hi) {
        const int a = qMax(1, int(lo * kFftSize / sampleRate));
        const int b = qMin(kFftSize / 2 - 1, int(hi * kFftSize / sampleRate));
        double e = 0.0;
        for (int k = a; k <= b; ++k)
            e += std::norm(m_fft[k]);
        return std::sqrt(e / qMax(1, b - a + 1));
    };
    const double raw[4] = {rms, band(20, 250), band(250, 2000), band(2000, 12000)};

    // Normalise each signal against its own recent peak (decays ~0.5% per frame, about 3 dB/s),
    // with a floor so silence reads as zero rather than amplified noise.
    static const double floors[4] = {0.003, 0.02, 0.01, 0.005};
    double norm[4];
    for (int s = 0; s < 4; ++s) {
        m_peak[s] = qMax(raw[s], qMax(m_peak[s] * 0.995, floors[s]));
        norm[s] = qBound(0.0, raw[s] / m_peak[s], 1.0);
        // Fast attack, slower release: punchy but not flickery.
        const double k = norm[s] > m_env[s] ? 0.6 : 0.15;
        m_env[s] += (norm[s] - m_env[s]) * k;
    }

    // Beat: bass energy clearly above its one-second average, at most every 250 ms.
    const double bassEnergy = raw[1] * raw[1];
    double average = 0.0;
    for (double h : std::as_const(m_bassHistory))
        average += h;
    average /= kHistory;
    m_bassHistory[m_historyPos] = bassEnergy;
    m_historyPos = (m_historyPos + 1) % kHistory;
    const bool loudEnough = raw[1] > floors[1];
    m_samplesSinceBeat += kHop;
    bool beat = false;
    if (loudEnough && bassEnergy > 1.6 * average && m_samplesSinceBeat >= qint64(sampleRate) * kMinBeatGapMs / 1000) {
        beat = true;
        m_samplesSinceBeat = 0;
    }

    m_levels = {m_env[0], m_env[1], m_env[2], m_env[3], beat};
    emit levelsChanged(m_levels);
}
