#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>
#include <array>

class MidiInput;
class QAudioSource;
class QIODevice;

// MIDI timecode: eight quarter-frame messages carry one time (every two frames).
class MtcDecoder
{
public:
    // Returns true when a complete time has been assembled; *seconds is then the current time
    // (the message describes the time two frames ago) and *fps its rate.
    bool feed(quint8 data, double *seconds, double *fps);

private:
    std::array<int, 8> m_pieces{};
    int m_seen = 0; // bit mask of pieces received since the last 0
};

// SMPTE linear timecode from audio samples (biphase mark code, 80 bits a frame).
class LtcDecoder
{
public:
    explicit LtcDecoder(int sampleRate = 48000) : m_rate(sampleRate) {}
    // Feeds mono samples (-1..1). Returns true when at least one frame was decoded in this block;
    // *seconds is the latest time (at the end of its frame) and *fps its rate.
    bool feed(const float *samples, int count, double *seconds, double *fps);

private:
    void bit(int value);

    int m_rate;
    bool m_high = false;
    double m_sinceEdge = 0;   // samples since the last zero crossing
    double m_period = 0;      // estimated length of one bit, in samples
    bool m_halfPending = false;
    std::array<quint8, 80> m_bits{};
    int m_count = 0;
    bool m_decoded = false;
    double m_seconds = -1;
    double m_fps = 25;
};

// The show's timecode input: MTC from the MIDI inputs, or LTC from an audio input.
class TimecodeInput : public QObject
{
    Q_OBJECT
public:
    TimecodeInput(MidiInput *midi, QObject *parent = nullptr);
    ~TimecodeInput() override;

    enum class Source { Off, Mtc, Ltc };
    void setSource(Source source, const QByteArray &audioDevice = {});
    Source source() const { return m_source; }
    double seconds() const; // the latest time, run on by the clock since it arrived; -1 = none
    double fps() const { return m_fps; }
    bool running() const; // time arrived within the last half second
    QString status() const { return m_status; }

    // Tests: feed audio straight into the LTC decoder.
    void feedLtc(const float *samples, int count, int sampleRate);

signals:
    void timecode(double seconds, double fps);
    void statusChanged();

private:
    void onQuarterFrame(quint8 data);
    void onAudio();
    void deliver(double seconds, double fps);

    MidiInput *m_midi;
    Source m_source = Source::Off;
    MtcDecoder m_mtc;
    LtcDecoder m_ltc;
    QAudioSource *m_audio = nullptr;
    QIODevice *m_audioIo = nullptr;
    int m_audioRate = 48000;
    QElapsedTimer m_since;
    double m_seconds = -1;
    double m_fps = 25;
    QString m_status;
};
