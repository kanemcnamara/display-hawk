#pragma once

#include <QObject>
#include <QVector>

class Document;
class QTimer;

// The show's beat: a running beat count at the project's BPM, kept in phase when the tempo changes.
// The tempo is tapped, or followed from MIDI clock or the audio beat detector (the project's tempo
// source). Times are nanoseconds on MidiInput::nowNs()'s steady clock, so MIDI timestamps line up.
class TempoClock : public QObject
{
    Q_OBJECT
public:
    explicit TempoClock(Document *doc, QObject *parent = nullptr);

    static qint64 nowNs();
    double beats() const { return beatsAt(nowNs()); } // continuous: 4.5 = half way through beat 5
    double beatsAt(qint64 ns) const;
    double bpm() const { return m_bpm; }
    // Milliseconds until the next multiple of `beats` (1 = next beat, 4 = next bar).
    int msUntil(int beats) const;

public slots:
    void tap(qint64 ns = -1);    // sets the tempo from recent taps; each tap is a beat
    void resync(qint64 ns = -1); // now is the first beat of a bar
    void nudge(double beats);    // shift the phase (positive = later)
    void midiRealtime(quint8 byte, qint64 ns);
    void audioBeat(qint64 ns = -1);

signals:
    void beat(int beatInBar); // 0..3, emitted as each beat starts

private:
    void syncFromProject();
    void rebase(qint64 ns, double beatsAtNs);
    void setTempo(double bpm, qint64 ns);

    Document *m_doc;
    QTimer *m_poll;
    double m_bpm = 120.0;
    qint64 m_originNs = 0;
    double m_originBeats = 0.0;
    qint64 m_lastBeat = -1;
    QVector<qint64> m_taps;
    QVector<qint64> m_midiTicks;
    int m_midiTickCount = 0;
    QVector<qint64> m_audioBeats;
};
