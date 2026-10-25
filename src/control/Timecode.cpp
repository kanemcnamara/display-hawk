#include "control/Timecode.h"

#include "control/Midi.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QtMath>

namespace {

double rateFromCode(int code)
{
    switch (code) {
    case 0: return 24.0;
    case 1: return 25.0;
    case 2: return 29.97;
    default: return 30.0;
    }
}

// Snaps a measured frame rate to the standard one it is nearest.
double snapFps(double fps)
{
    double best = 25.0;
    for (double r : {24.0, 25.0, 29.97, 30.0})
        if (qAbs(r - fps) < qAbs(best - fps))
            best = r;
    return best;
}

} // namespace

// ---------------------------------------------------------------- MTC

bool MtcDecoder::feed(quint8 data, double *seconds, double *fps)
{
    const int piece = (data >> 4) & 7;
    m_pieces[piece] = data & 0x0F;
    if (piece == 0)
        m_seen = 0;
    m_seen |= 1 << piece;
    if (piece != 7 || m_seen != 0xFF)
        return false;
    const int frames = m_pieces[0] | (m_pieces[1] & 1) << 4;
    const int secs = m_pieces[2] | (m_pieces[3] & 3) << 4;
    const int mins = m_pieces[4] | (m_pieces[5] & 3) << 4;
    const int hours = m_pieces[6] | (m_pieces[7] & 1) << 4;
    const double rate = rateFromCode((m_pieces[7] >> 1) & 3);
    // The eight messages take two frames to send: the time they carry started two frames ago.
    *seconds = hours * 3600.0 + mins * 60.0 + secs + (frames + 2) / rate;
    *fps = rate;
    return true;
}

// ---------------------------------------------------------------- LTC

bool LtcDecoder::feed(const float *samples, int count, double *seconds, double *fps)
{
    m_decoded = false;
    for (int i = 0; i < count; ++i) {
        m_sinceEdge += 1.0;
        // A zero crossing with a little hysteresis so noise near zero does not count.
        const float v = samples[i];
        const bool high = m_high ? v > -0.05f : v > 0.05f;
        if (high == m_high)
            continue;
        m_high = high;
        const double interval = m_sinceEdge;
        m_sinceEdge = 0;
        if (interval < 3)
            continue; // glitch
        if (m_period <= 0) { // first edges: assume this was a whole bit
            m_period = interval;
            continue;
        }
        // Biphase mark: a whole bit without an edge in the middle is 0; two half bits are 1.
        if (interval < m_period * 0.75) {
            if (m_halfPending) {
                m_halfPending = false;
                bit(1);
                m_period = m_period * 0.9 + interval * 2.0 * 0.1;
            } else {
                m_halfPending = true;
            }
        } else {
            m_halfPending = false;
            bit(0);
            m_period = m_period * 0.9 + interval * 0.1;
        }
    }
    if (m_decoded) {
        *seconds = m_seconds;
        *fps = m_fps;
    }
    return m_decoded;
}

void LtcDecoder::bit(int value)
{
    // The last 80 bits, oldest first.
    for (int i = 0; i < 79; ++i)
        m_bits[i] = m_bits[i + 1];
    m_bits[79] = quint8(value);
    ++m_count;
    if (m_count < 80)
        return;
    // Sync word (bits 64..79, in the order sent): 0011 1111 1111 1101.
    static const quint8 sync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
    for (int i = 0; i < 16; ++i)
        if (m_bits[64 + i] != sync[i])
            return;
    auto field = [this](int first, int bits) {
        int v = 0;
        for (int b = 0; b < bits; ++b)
            v |= m_bits[first + b] << b;
        return v;
    };
    const int frames = field(0, 4) + 10 * field(8, 2);
    const int secs = field(16, 4) + 10 * field(24, 3);
    const int mins = field(32, 4) + 10 * field(40, 3);
    const int hours = field(48, 4) + 10 * field(56, 2);
    m_fps = snapFps(m_rate / qMax(1.0, m_period) / 80.0);
    // Decoded at the end of its frame: the time now is one frame on.
    m_seconds = hours * 3600.0 + mins * 60.0 + secs + (frames + 1) / m_fps;
    m_decoded = true;
}

// ---------------------------------------------------------------- input

TimecodeInput::TimecodeInput(MidiInput *midi, QObject *parent)
    : QObject(parent)
    , m_midi(midi)
{
    connect(m_midi, &MidiInput::timecodeQuarterFrame, this, &TimecodeInput::onQuarterFrame);
}

TimecodeInput::~TimecodeInput()
{
    setSource(Source::Off);
}

void TimecodeInput::setSource(Source source, const QByteArray &audioDevice)
{
    if (m_audio) {
        m_audio->stop();
        delete m_audio;
        m_audio = nullptr;
        m_audioIo = nullptr;
    }
    m_source = source;
    m_seconds = -1;
    m_status = source == Source::Off ? tr("Off") : source == Source::Mtc ? tr("Waiting for MIDI timecode") : QString();
    if (source == Source::Ltc) {
        QAudioDevice device = QMediaDevices::defaultAudioInput();
        for (const QAudioDevice &d : QMediaDevices::audioInputs())
            if (d.id() == audioDevice)
                device = d;
        if (device.isNull() || qEnvironmentVariableIsSet("DISPLAYHAWK_NO_AUDIO_INPUT")) {
            m_status = tr("No audio input for LTC");
        } else {
            QAudioFormat format;
            format.setSampleRate(48000);
            format.setChannelCount(1);
            format.setSampleFormat(QAudioFormat::Float);
            if (!device.isFormatSupported(format))
                format = device.preferredFormat();
            m_audioRate = format.sampleRate();
            m_ltc = LtcDecoder(m_audioRate);
            m_audio = new QAudioSource(device, format, this);
            m_audio->setBufferSize(m_audioRate / 50 * format.bytesPerFrame()); // 20 ms
            m_audioIo = m_audio->start();
            connect(m_audioIo, &QIODevice::readyRead, this, &TimecodeInput::onAudio);
            m_status = tr("Listening for LTC on %1").arg(device.description());
        }
    }
    emit statusChanged();
}

double TimecodeInput::seconds() const
{
    if (m_seconds < 0)
        return -1;
    return running() ? m_seconds + m_since.elapsed() / 1000.0 : m_seconds; // stopped: hold
}

bool TimecodeInput::running() const
{
    return m_seconds >= 0 && m_since.isValid() && m_since.elapsed() < 500;
}

void TimecodeInput::deliver(double seconds, double fps)
{
    const bool wasRunning = running();
    m_seconds = seconds;
    m_fps = fps;
    m_since.start();
    if (!wasRunning) {
        m_status = tr("Receiving %1 fps").arg(fps, 0, 'f', fps == 29.97 ? 2 : 0);
        emit statusChanged();
    }
    emit timecode(seconds, fps);
}

void TimecodeInput::onQuarterFrame(quint8 data)
{
    double s, fps;
    if (m_source == Source::Mtc && m_mtc.feed(data, &s, &fps))
        deliver(s, fps);
}

void TimecodeInput::feedLtc(const float *samples, int count, int sampleRate)
{
    if (sampleRate != m_audioRate) {
        m_audioRate = sampleRate;
        m_ltc = LtcDecoder(sampleRate);
    }
    double s, fps;
    if (m_ltc.feed(samples, count, &s, &fps))
        deliver(s, fps);
}

void TimecodeInput::onAudio()
{
    if (!m_audioIo || !m_audio)
        return;
    const QAudioFormat f = m_audio->format();
    const QByteArray data = m_audioIo->readAll();
    const int frames = data.size() / qMax(1, f.bytesPerFrame());
    QVector<float> mono(frames);
    for (int i = 0; i < frames; ++i)
        mono[i] = f.normalizedSampleValue(data.constData() + i * f.bytesPerFrame()); // first channel
    feedLtc(mono.constData(), frames, f.sampleRate());
}
